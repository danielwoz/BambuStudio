// Bambu Bridge — Winsock / POSIX socket shim.
//
// Lets server/*.cpp and router/*.cpp keep their POSIX-style `::accept`,
// `::send`, `::recv`, `::select`, `::setsockopt`, `::bind` calls verbatim.
// On Linux these expand to the system calls; on Windows they expand to
// Winsock equivalents with the type/flag adjustments folded in.
//
// IMPORTANT: this header must be included BEFORE any <windows.h> so that
// <winsock2.h> wins the include race. Every server/router TU that touches
// sockets includes THIS header first (in place of the old <sys/socket.h>
// cluster).
#pragma once

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  // WSAPoll() and `struct pollfd` require Windows Vista+ (0x0600). Raise an
  // unset/too-low target so the bridge's non-blocking-connect probes
  // (LocalControlTunnel, LanUploadSink port-6000) see them.
  #if !defined(_WIN32_WINNT) || (_WIN32_WINNT < 0x0600)
    #undef _WIN32_WINNT
    #define _WIN32_WINNT 0x0601
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <mswsock.h>
  #include <windows.h>
  #pragma comment(lib, "ws2_32.lib")

  #include <cstdint>

  // ---- POSIX-style type aliases --------------------------------------
  // Winsock uses int where POSIX uses socklen_t; SSIZE_T for ssize_t.
  using bridge_socklen_t = int;
  #ifndef _SSIZE_T_DEFINED
    using ssize_t = SSIZE_T;
    #define _SSIZE_T_DEFINED
  #endif

  // socklen_t is NOT defined by ws2tcpip.h in all SDKs; provide it if missing.
  // ws2tcpip.h DOES define socklen_t (as int) on modern SDKs, so guard it.
  #ifndef BAMBU_HAVE_SOCKLEN_T
    // Modern Windows SDK already typedefs socklen_t in ws2tcpip.h. If your
    // SDK lacks it, define BAMBU_NEED_SOCKLEN_T at compile time.
    #ifdef BAMBU_NEED_SOCKLEN_T
      using socklen_t = int;
    #endif
  #endif

  // ---- Flag / constant shims -----------------------------------------
  // Winsock has no SIGPIPE, so MSG_NOSIGNAL is a no-op.
  #ifndef MSG_NOSIGNAL
    #define MSG_NOSIGNAL 0
  #endif
  // POSIX shutdown() constants → Winsock SD_* equivalents.
  #ifndef SHUT_RD
    #define SHUT_RD   SD_RECEIVE
  #endif
  #ifndef SHUT_WR
    #define SHUT_WR   SD_SEND
  #endif
  #ifndef SHUT_RDWR
    #define SHUT_RDWR SD_BOTH
  #endif

  // ---- Close --------------------------------------------------------
  inline int bambu_close_socket(SOCKET s) { return ::closesocket(s); }

  // ---- Errno translation --------------------------------------------
  // After a failed socket call, POSIX reads errno; Winsock reads
  // WSAGetLastError(). bambu_last_socket_error() returns the latter on
  // Windows and errno on POSIX, so call-site error handling can branch on
  // a single accessor. Common codes are aliased to their POSIX spelling.
  inline int bambu_last_socket_error() { return ::WSAGetLastError(); }
  #ifndef EWOULDBLOCK_WIN_ALIASED
    #define EWOULDBLOCK_WIN_ALIASED 1
    // Only alias if the CRT hasn't already (it usually has, to small ints).
    #ifndef EWOULDBLOCK
      #define EWOULDBLOCK WSAEWOULDBLOCK
    #endif
    #ifndef ECONNRESET
      #define ECONNRESET WSAECONNRESET
    #endif
  #endif

  // ---- Non-blocking toggle (replaces MSG_DONTWAIT / fcntl O_NONBLOCK) -
  inline int bambu_set_nonblocking(SOCKET s, bool nonblocking) {
      u_long mode = nonblocking ? 1u : 0u;
      return ::ioctlsocket(s, FIONBIO, &mode);
  }
  // FIONREAD pending-bytes peek (replaces ::ioctl(fd, FIONREAD, &n)).
  inline int bambu_bytes_available(SOCKET s, unsigned long* out) {
      return ::ioctlsocket(s, FIONREAD, out);
  }

  // ---- Timeout shims -------------------------------------------------
  // POSIX takes `struct timeval`; Winsock takes a DWORD millisecond count.
  inline int bambu_set_recv_timeout_ms(SOCKET s, DWORD ms) {
      return ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                          reinterpret_cast<const char*>(&ms), sizeof(ms));
  }
  inline int bambu_set_send_timeout_ms(SOCKET s, DWORD ms) {
      return ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO,
                          reinterpret_cast<const char*>(&ms), sizeof(ms));
  }

  // setsockopt/getsockopt on Winsock take a `const char*` for the value
  // pointer, whereas POSIX takes `const void*`. Most call sites pass
  // `&opt` (an int*/struct*), which implicitly converts to void* on POSIX
  // but NOT to char* on MSVC. Provide reinterpret helpers the call sites
  // can use when the compiler complains.
  template <class T>
  inline int bambu_setsockopt(SOCKET s, int level, int optname, const T* val, int len) {
      return ::setsockopt(s, level, optname, reinterpret_cast<const char*>(val), len);
  }
  template <class T>
  inline int bambu_getsockopt(SOCKET s, int level, int optname, T* val, int* len) {
      return ::getsockopt(s, level, optname, reinterpret_cast<char*>(val), len);
  }

  // ---- One-time WSAStartup. Idempotent across TUs. -------------------
  struct WinsockInit {
      WinsockInit() { WSADATA d; ::WSAStartup(MAKEWORD(2,2), &d); }
      ~WinsockInit() { ::WSACleanup(); }
  };
  inline void ensure_winsock_init() {
      static WinsockInit s_init;
      (void)s_init;
  }

#else  // ---- POSIX ------------------------------------------------------
  #include <arpa/inet.h>
  #include <fcntl.h>
  #include <netdb.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <sys/ioctl.h>
  #include <sys/select.h>
  #include <sys/socket.h>
  #include <sys/types.h>
  #include <unistd.h>
  #include <cerrno>

  using bridge_socklen_t = socklen_t;

  inline int bambu_close_socket(int fd) { return ::close(fd); }
  inline int bambu_last_socket_error() { return errno; }

  inline int bambu_set_nonblocking(int fd, bool nonblocking) {
      int flags = ::fcntl(fd, F_GETFL, 0);
      if (flags < 0) return -1;
      if (nonblocking) flags |= O_NONBLOCK; else flags &= ~O_NONBLOCK;
      return ::fcntl(fd, F_SETFL, flags);
  }
  inline int bambu_bytes_available(int fd, unsigned long* out) {
      int n = 0;
      int rc = ::ioctl(fd, FIONREAD, &n);
      if (out) *out = static_cast<unsigned long>(n < 0 ? 0 : n);
      return rc;
  }

  inline int bambu_set_recv_timeout_ms(int fd, unsigned ms) {
      struct timeval tv{ static_cast<long>(ms / 1000),
                         static_cast<long>((ms % 1000) * 1000) };
      return ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }
  inline int bambu_set_send_timeout_ms(int fd, unsigned ms) {
      struct timeval tv{ static_cast<long>(ms / 1000),
                         static_cast<long>((ms % 1000) * 1000) };
      return ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  }
  template <class T>
  inline int bambu_setsockopt(int fd, int level, int optname, const T* val, int len) {
      return ::setsockopt(fd, level, optname, val, static_cast<socklen_t>(len));
  }
  template <class T>
  inline int bambu_getsockopt(int fd, int level, int optname, T* val, int* len) {
      socklen_t sl = static_cast<socklen_t>(*len);
      int rc = ::getsockopt(fd, level, optname, val, &sl);
      *len = static_cast<int>(sl);
      return rc;
  }
  inline void ensure_winsock_init() {}
#endif
