# Bambu Bridge — Windows-port audit + sequencing plan

Read-only audit of `src/bambu_bridge/` (the server side of the slicer-to-printer
bridge) for Windows portability. Companion to the earlier client-side audit,
which declared the bridge server "Linux-only by design" and flagged it as the
much larger piece of work.

> **Scope**: every file under `src/bambu_bridge/` (network servers, headless
> orchestrator, CLI, routers, plugin/source dlopen shims, harness, CMake).
> The OpenSSL TLS layer (`tls/CertFactory`) is already cross-platform and is
> not re-audited here; same for `third_party/nlohmann/json.hpp`. Tests
> harness under `tests/bridge/harness/` is included.

> **Out of repo** at audit time: `src/bambu_bridge/harness/` does not exist —
> the harness lives at `tests/bridge/harness/`. The scope list calling out
> "`harness/`" maps to that path.

---

## 1. Executive summary

The bridge-server code is a thin (≈14.5k LoC across the in-scope tree),
mostly-self-contained BSD-sockets implementation that talks SSDP / MQTT /
FTPS / RTSPS / a custom Bambu virtual-tunnel protocol to a small fleet of
LAN printers. It is unambiguously POSIX-flavoured today: **every transport
file under `src/bambu_bridge/server/` has an explicit `#error "… is
Linux-only for now"` guard on `_WIN32`** (six files), so the question is
not "what's accidentally non-portable" — it's "what's the cost of removing
those guards".

The shape of the port is overwhelmingly a **sockets migration**. There is
no process model (no `fork`/`exec`/`pipe`/`waitpid`), no Linux-specific
kernel API (no `epoll`/`signalfd`/`inotify`/`/proc`), no AF_UNIX, no raw
pthread (everything uses `std::thread`/`std::mutex`/`std::atomic`), and
no realtime POSIX timers. The dlopen shims (`BambuSourceHandle`,
`BambuNetworkingPluginHandle`) already have working `_WIN32` branches.
The only non-socket POSIX surface that needs work is: one self-pipe in
`SignalHandler` (≈30 LoC), one tempfile spool helper in `UploadSpool`
(≈30 LoC), `ifaddrs`/`getifaddrs` interface enumeration in `BridgeApp`
(≈25 LoC), `chmod`/`fchmod`/`O_CLOEXEC` in `CertFactory` (out of strict
scope; the cross-platform branches are already present), and `clock_gettime`
in one place in `VirtualTunnelServer` (trivially replaced with
`std::chrono::steady_clock`).

**Verdict: port-feasibly, not a rewrite.** The biggest single chunk of work
is the `select(2)` / `socket(2)` / `setsockopt(2)` / `recvfrom(2)` /
`sendto(2)` / IGMP-membership / `MSG_DONTWAIT|MSG_PEEK` / `ioctl(FIONREAD)`
plumbing across six server files (≈3700 LoC of which maybe 300–500 are
literal socket-API callsites). The realistic path is **not** "wholesale
boost::asio rewrite" — it is a thin `bridge::net` compatibility shim
(Winsock vs Berkeley) plus a `bridge::sys` shim for the four non-socket
POSIX bits, then strip the `#error` guards. A boost::asio rewrite is
*possible* but unwarranted given the existing select-loop design works
and the project has no other consumer of asio.

**Total realistic effort: medium (8–14 engineer-days)**, dominated by the
Winsock shim + per-file testing on Windows. A full asio rewrite would be
high (25–40+ days) and is not recommended.

---

## 2. Findings by category

Conventions:
- **Effort buckets**: trivial (<1h), small (1–4h), medium (4–16h), large (>16h)
- Line numbers reflect the tree at audit time (2026-05-18, branch
  `bambu-virtual-shared`).
- "callsite" = a single function call; one file usually has many.

### 2.1 Raw socket APIs not abstracted via boost::asio

Six files speak BSD sockets directly. They run a classic
`select(2) + accept(2)` blocking-listener-thread-per-server model with a
small `sleep(100ms)` poll interval so `stop()` can flip an atomic and
the listener thread can observe it. There is no asio in the tree
despite what the `CMakeLists.txt` comment claims (lines 138–146 of
`CMakeLists.txt` reference "boost::asio" aspirationally; no `#include
<boost/asio.hpp>` anywhere under `src/bambu_bridge/`).

The migration cost is dominated by:
1. `int fd` → `SOCKET` typedef + `INVALID_SOCKET`/`SOCKET_ERROR` checks.
2. `::close(fd)` → `::closesocket(fd)` on Win32.
3. `MSG_NOSIGNAL` (Linux-only) → `signal(SIGPIPE, SIG_IGN)` is irrelevant on
   Windows; just `#ifdef` it away.
4. `SO_REUSEPORT` — Linux-only, already guarded by `#ifdef SO_REUSEPORT`
   in three places (good).
5. `MSG_DONTWAIT` (Linux-only) — must switch to per-socket non-blocking
   mode via `ioctlsocket(FIONBIO)` on Windows.
6. `ssize_t` → `int` on Windows.
7. `socklen_t` is provided by Winsock since XP — OK as-is.
8. `inet_pton`/`inet_ntop` exist on Windows since Vista. OK with `ws2tcpip.h`.
9. `select()` exists; `fd_set` is implementation-detail-different (Windows
   `fd_set` is an array of `SOCKET`, not a bitmask) but the
   `FD_ZERO`/`FD_SET` macros abstract that.
10. `ioctl(fd, FIONREAD, &n)` → `ioctlsocket(fd, FIONREAD, &n)`.
11. WSAStartup/WSACleanup must happen once per process; a static initialiser
    in a `bridge::net::winsock_bootstrap` translation unit is the cheapest fix.
12. `WSAGetLastError()` instead of `errno` for socket-API errors.
13. IGMP membership (`IP_ADD_MEMBERSHIP` + `ip_mreq`) exists on Windows;
    same struct.
14. Broadcast (`SO_BROADCAST`), keepalive, `TCP_NODELAY`, `SO_RCVTIMEO`,
    `SO_KEEPALIVE`, `SO_REUSEADDR` — all available; `SO_RCVTIMEO` takes
    `DWORD` milliseconds on Windows vs `timeval` on POSIX (needs a tiny
    wrapper).

| file:line | description | proposed fix | effort |
|---|---|---|---|
| `server/SsdpListener.cpp:45,48,50,52,59,63,90,103,109,129` | UDP listener: socket/setsockopt(REUSEADDR,REUSEPORT,BROADCAST)/inet_pton/bind/shutdown/close/select/recvfrom/inet_ntop. Single fd lifetime, simple. | Replace with `bridge::net` shim + `ioctlsocket(FIONBIO)` for the receive-loop's non-blocking flag (currently relies on `select` timeout). | small |
| `server/SsdpResponder.cpp:143,147,150,153,161,166,173,177,181,190,193,266,269,271,318,328,346,385,413,417` | Two UDP sockets (recv on :1900, send for replies/multicast). IGMP `IP_ADD_MEMBERSHIP`, `IP_MULTICAST_TTL`, `IP_MULTICAST_LOOP`. `<net/if.h>` is included but only for `IF_NAMESIZE`-style constants — verify if it's actually used; if not, drop the include on Windows. | Shim. `inet_addr` is deprecated on Windows but works; keep or replace with `inet_pton`. | small |
| `server/MqttBroker.cpp:144,147,155,159,161,165,167,173,174,320,349,406-413,418,431,461,462,466,472,496,509,594-603` | TCP listener + per-session worker thread. Calls `socket/setsockopt(REUSEADDR,KEEPALIVE,TCP_NODELAY,RCVTIMEO)/bind/listen/accept/getsockname/select/recv(MSG_PEEK)/recv(MSG_DONTWAIT)/send(MSG_NOSIGNAL)/shutdown/close`. `MSG_NOSIGNAL` and `MSG_DONTWAIT` are the two Linux-isms; the latter has a Windows path via `ioctlsocket`. SO_RCVTIMEO encoding differs (timeval vs DWORD). | Shim + `MSG_NOSIGNAL`-erasure (it's only suppressing SIGPIPE, which doesn't exist on Windows). | medium |
| `server/FtpsServer.cpp:168,171,179,183,186,191,288,475,479,484,485,488,505,526,536,539,587,591,592,596,601,625,630` | TCP control listener + active-mode data connections + passive-mode data listener (PASV). Same surface as MqttBroker plus `accept`+`SO_RCVTIMEO` per-connection. Also includes `<signal.h>` to install SIGPIPE→SIG_IGN at startup (irrelevant on Windows). | Shim + `#ifndef _WIN32` around the SIGPIPE handler. | medium |
| `server/RtspServer.cpp:159,162,170,174,177,182,259,261,704,706,856-871,887,893,914,924,927` | TCP listener + per-session thread for RTSPS control + RTP-over-TCP interleaved frames. Same surface. `signal()` SIGPIPE→IGN at startup. | Shim. | medium |
| `server/VirtualTunnelServer.cpp:112,115,117,124,127,130,139,170,171,353,355,465,495,497,537` | TCP listener + per-client thread. Also calls `ioctl(FIONREAD)` to peek pending bytes and `recv(MSG_DONTWAIT|MSG_PEEK)` to detect TLS-ClientHello vs PROXY-protocol prefix. `getsockopt(SO_ERROR)` post-accept. | Shim + `ioctlsocket(FIONBIO)` to switch fd to non-blocking before the peek-recv, then back. | medium |
| `headless/BridgeApp.cpp:35-38,49-72` | `detect_primary_lan_ip()` walks `getifaddrs()` + filters out docker/br-/virbr/veth interfaces by name and picks first non-loopback IPv4. | Use `GetAdaptersAddresses()` on Windows (iphlpapi.lib) inside the same function; same filter logic still applies on names like "vEthernet". Add `iphlpapi` to Windows link list. | small |

**Category total LoC affected: ≈300–500 actual socket callsites across
≈3700 LoC of server code. Total effort: medium-high (≈5–7 engineer-days)
once the shim is in place.**

### 2.2 POSIX-only headers (`<unistd.h>`, `<sys/*.h>`, `<arpa/*.h>`, `<netinet/*.h>`, `<netdb.h>`)

These are downstream of category 2.1 — once a shim moves socket calls
behind a header, the includes follow. Listed for completeness; each is
a 1-line `#ifdef _WIN32` swap to `winsock2.h`/`ws2tcpip.h`/`windows.h`.

| file:line | headers | proposed fix | effort |
|---|---|---|---|
| `server/MqttBroker.cpp:24-31` | arpa/inet, fcntl, netinet/in,tcp, sys/select,socket,types, unistd | shim header | trivial |
| `server/SsdpListener.cpp:11-15` | arpa/inet, netinet/in, sys/select,socket, unistd | shim header | trivial |
| `server/SsdpResponder.cpp:18-25` | arpa/inet, fcntl, net/if, netinet/in, sys/select,socket,types, unistd | shim header | trivial |
| `server/RtspServer.cpp:66-74` | arpa/inet, fcntl, netinet/in,tcp, signal, sys/select,socket,types, unistd | shim header | trivial |
| `server/FtpsServer.cpp:67-75` | arpa/inet, fcntl, netinet/in,tcp, signal, sys/select,socket,types, unistd | shim header | trivial |
| `server/VirtualTunnelServer.cpp:8-15` | arpa/inet, netdb, netinet/in, sys/select,socket,types,ioctl, unistd | shim header | trivial |
| `server/SsdpResponder.hpp:69` | netinet/in (uses sockaddr_in in a member?) | Verify; either inline-include from shim or `#ifdef`. | trivial |
| `headless/BridgeApp.cpp:35-38` | arpa/inet, ifaddrs, net/if, sys/socket | replaced by adapter shim in 2.1 | trivial |
| `headless/SignalHandler.cpp:20-22` | fcntl, signal, unistd | rewritten under category 2.3 | (folded) |
| `router/UploadSpool.cpp:7-10` | fcntl, sys/stat, sys/types, unistd | rewritten under category 2.5/2.8 | (folded) |
| `router/LanUploadSink.cpp:18` | unistd | for `::unlink(2)` — replace with `std::filesystem::remove`. | trivial |
| `router/CloudUploadSink.cpp:24` | unistd | for `::unlink(2)` — replace with `std::filesystem::remove`. | trivial |

**Category total LoC: ≈40 include lines. Total effort: trivial in
aggregate (≤1 engineer-day) once the shim header exists.**

### 2.3 Signal handling

Two distinct designs in-tree, both of which need Windows replacements:

| file:line | description | proposed fix | effort |
|---|---|---|---|
| `headless/SignalHandler.cpp:42-128` | Self-pipe trick. Installs `sigaction(SIGINT|SIGTERM)` with a handler that writes one byte to a pipe; worker thread blocks on `read()` and dispatches the user callback in normal context. Uses `pipe()`, `fcntl(F_GETFL/F_SETFL O_NONBLOCK)`, `sigaction`, `sigemptyset`. | Windows path: use `SetConsoleCtrlHandler(CTRL_C_EVENT|CTRL_CLOSE_EVENT|CTRL_BREAK_EVENT)` + a `condition_variable` (no signal-safety concern on Windows because handler runs in a separate thread already). Alternative: `std::signal(SIGINT, ...)` — Windows CRT supports SIGINT (CTRL+C) and SIGTERM but not SIGCHLD/SIGPIPE/SIGHUP. The self-pipe is unnecessary on Windows since handler-thread context is already safe. | small |
| `server/FtpsServer.cpp:96-101` | `sigaction(SIGPIPE, SIG_IGN)` to prevent SIGPIPE from killing the process on a broken TLS write. | `#ifndef _WIN32`-guard the whole block. On Windows, broken-pipe surfaces as `WSAECONNRESET` from `send()` instead. | trivial |
| `server/RtspServer.cpp:95-97` | Same: `sigaction(SIGPIPE, SIG_IGN)` at startup. | Same: `#ifndef _WIN32`-guard. | trivial |
| `cli/bridge_cli.cpp:339-340,426-427,547-548,681-682,802-803,913-914,1210-1211` | Seven subcommands each install `std::signal(SIGINT,...)` + `std::signal(SIGTERM,...)`. `SIGINT` works on Windows; `SIGTERM` is technically defined by the Windows CRT but never raised by the OS — fine to leave as a dead branch. | Leave as-is. Optionally also wire `SetConsoleCtrlHandler` for clean teardown on console-close. | trivial |

**Category total LoC: ≈140 (mostly SignalHandler.cpp). Total effort:
small (≈4–6 hours).**

### 2.4 Process / fork

**No findings.** No `fork`, no `exec*`, no `waitpid`, no `kill()`, no
`popen`, no `system()`. The only `pipe()` call is the self-pipe in
SignalHandler (folded into category 2.3). No process-model port needed.

### 2.5 Filesystem permissions

| file:line | description | proposed fix | effort |
|---|---|---|---|
| `router/UploadSpool.cpp:25,27,32,35,36,42` | `mkstemp` + `fchmod(fd, 0600)` + `write/close/unlink`. POSIX-only mkstemp; Windows has `_mktemp_s`+`_open` or `tmpfile_s` but neither lets you set mode atomically. | Replace whole helper with `std::filesystem::temp_directory_path()` + a UUID-suffixed filename + `std::ofstream` write. Drop the mode-0600 hardening on Windows (ACL-based; rely on per-user TEMP). | small |
| `router/UploadSpool.cpp:18-19` | `getenv("TMPDIR")` with fallback to `/tmp`. | Replace with `std::filesystem::temp_directory_path()` (looks at `TMPDIR` on POSIX, `TMP`/`TEMP`/`USERPROFILE` on Windows). Or detect Windows and probe `TMP`/`TEMP` explicitly. | trivial |
| `router/LanUploadSink.cpp:102` | `::unlink(tmp_path.c_str())` | `std::filesystem::remove(tmp_path)` | trivial |
| `router/CloudUploadSink.cpp:105` | `::unlink(tmp_path.c_str())` | `std::filesystem::remove(tmp_path)` | trivial |
| `tls/CertFactory.cpp:108-128,162-182,385,395` | `getenv("XDG_CONFIG_HOME")` / `getenv("HOME")` + `chmod(0700)` + `open(O_CREAT\|O_WRONLY\|O_TRUNC\|O_CLOEXEC, 0600)` + `fchmod(0600)`. Out of strict scope (the file is `tls/`, not `bambu_bridge/`-rooted), but already has Windows branches at the top of the file (`fcntl.h` guard at lines 50,53). | Already partially handled — confirm cross-platform path resolves to `%APPDATA%\BambuStudio\bridge-tls\` on Windows. | (out of scope) |

**Category total LoC: ≈30 (in scope; cert factory excluded).
Total effort: small (≈2 hours).**

### 2.6 Time APIs

| file:line | description | proposed fix | effort |
|---|---|---|---|
| `server/VirtualTunnelServer.cpp:151` | `timespec ts{}; clock_gettime(CLOCK_MONOTONIC, &ts);` for a single timestamp computation. | `auto now = std::chrono::steady_clock::now();` — already idiomatic in the rest of the tree. | trivial |

No `gettimeofday`, no `localtime_r`, no `nanosleep`. Everything else
uses `std::chrono::steady_clock` and `std::this_thread::sleep_for`
(spot-checked).

**Category total LoC: 1 line. Total effort: trivial.**

### 2.7 Threads — raw pthread bypass

**No findings.** Zero `pthread_*` calls under `src/bambu_bridge/`.
Everything is `std::thread`/`std::mutex`/`std::condition_variable`/
`std::atomic`. The only pthread mention in the broader scope is
`tests/bridge/harness/ShimRecorder.cpp:30` (`pthread_getname_np` for a
best-effort thread-name label), which is already `#if defined(__linux__)`-
guarded and falls through cleanly to `std::thread::id` formatting on
other platforms.

### 2.8 Hardcoded Unix paths

| file:line | description | proposed fix | effort |
|---|---|---|---|
| `cli/bridge_cli.cpp:777,1104` | `std::filesystem::path("/tmp/bridge_uploads") / dev_id` — the default `--cache-dir` for two subcommands. | Replace with `std::filesystem::temp_directory_path() / "bridge_uploads" / dev_id`. The `std::filesystem::path` type already handles separator translation. | trivial |
| `router/UploadSpool.cpp:19` | `"/tmp"` fallback if `TMPDIR` unset. | Folded into category 2.5: use `std::filesystem::temp_directory_path()`. | (folded) |
| `BambuNetworkingPluginHandle.cpp:199,204` | Hardcoded `/usr/local/lib/libbambu_networking.dylib` and `/usr/lib/x86_64-linux-gnu/libbambu_networking.so` in the default-candidate probe list. | Already `#ifdef`-fenced (the Windows branch at line 196 adds `bambu_networking.dll`). No change. | (none) |
| `BambuSourceHandle.cpp:78,83,84` | Same pattern for libBambuSource. Already Windows-branched. | No change. | (none) |
| `cli/bridge_cli.cpp:55,68,131,132,164,165` etc. | "--bind", "--inventory-poll-seconds" etc. are CLI strings, not paths. False positive. | n/a | n/a |

**Category total LoC: ≈6 path literals in 2 files (excluding already-
guarded dlopen paths). Total effort: trivial.**

### 2.9 `getenv("HOME")` and POSIX env-var conventions

| file:line | description | proposed fix | effort |
|---|---|---|---|
| `tls/CertFactory.cpp:108,111` | `XDG_CONFIG_HOME` then `HOME` fallback. Out of strict scope. | Add `USERPROFILE`/`APPDATA` Windows branch. | (out of scope) |
| `BambuSourceHandle.cpp:69,79-87` | `home_dir()` returns `getenv("HOME")`. Used only in the macOS/Linux library-probe candidates (Windows branch above doesn't reach it). | No change — Windows branch doesn't use HOME. | (none) |
| `BambuNetworkingPluginHandle.cpp:186,206` | Same pattern. | No change. | (none) |
| `cli/bridge_cli.cpp:186`, `headless/BridgeAppCliArgs.cpp:98` | `getenv("BAMBU_BRIDGE_PLUGIN_PATH")` — application-specific env var, equally valid on both platforms. | No change. | (none) |
| `server/SsdpResponder.cpp:403`, `server/SsdpListener.cpp:140-141` | `getenv("BAMBU_BRIDGE_VERBOSE")` — same. | No change. | (none) |
| `router/UploadSpool.cpp:18` | `getenv("TMPDIR")` — see 2.5. | (folded) | (folded) |

**Category total LoC affected: 1 line in UploadSpool. Total effort:
trivial.**

### 2.10 CMakeLists — Windows link list & build wiring

The current `CMakeLists.txt` has a forward-looking `if (WIN32)` block at
lines 144–146 that adds `ws2_32` and `crypt32`. The actual port will need:

| concern | proposed fix | effort |
|---|---|---|
| `ws2_32` (Winsock 2) | already in `if (WIN32)` block at L144. | (done) |
| `crypt32` (OpenSSL needs it on Win for cert store) | already in `if (WIN32)` block. | (done) |
| `iphlpapi` (for `GetAdaptersAddresses` in the BridgeApp LAN-IP detector — replaces `getifaddrs`) | add to the `if (WIN32)` block. | trivial |
| `mswsock` (if any `AcceptEx`-style fast paths are introduced; not needed today since we use plain `accept`) | not currently needed. | (none) |
| `bcrypt` / `secur32` (OpenSSL on Win sometimes pulls these transitively; verify at link time) | add if `OpenSSL::SSL` doesn't pull them. | trivial |
| Define `WIN32_LEAN_AND_MEAN` + `NOMINMAX` before `<windows.h>` is included anywhere | add `target_compile_definitions(bambu_bridge PUBLIC -DWIN32_LEAN_AND_MEAN -DNOMINMAX)` inside the `if (WIN32)` block. | trivial |
| Include-order issue: `<winsock2.h>` MUST be included before `<windows.h>` — easy to violate via transitive includes. | Put `#include <winsock2.h>`/`<ws2tcpip.h>` at the top of the planned `bridge::net` shim header and have every server `.cpp` include that shim *first*. | small (discipline) |
| MSVC vs mingw warning fixes (e.g. `ssize_t` not standard on MSVC) | Use a typedef in the shim: `using bridge_ssize_t = std::make_signed_t<std::size_t>;`. | trivial |
| WSAStartup / WSACleanup wrapper TU | New file `server/WinsockInit.cpp` linked into `bambu_bridge` under `if (WIN32)`. Static initialiser pattern (like `ensure_openssl_init` in MqttBroker.cpp). | small |

**Category total LoC: ≈10 in CMakeLists + ≈80 in a new shim TU. Total
effort: small (≈3 hours including verifying the include-order discipline).**

---

## 3. Recommended sequencing

Phased, each phase independently buildable + testable on both Linux and
Windows. Run Wine-on-Linux + the existing `tests/bridge/` suite as the
smoke-test gate between phases.

### Phase A — Shim infrastructure (1–2 days)
1. Add `src/bambu_bridge/server/Sockets.hpp` — single shim header that
   conditionally includes either `<winsock2.h>+<ws2tcpip.h>+<iphlpapi.h>`
   on Win32 or the existing POSIX headers, exposes `using socket_t = …;`
   plus `bridge_close_socket()`, `bridge_set_nonblocking()`,
   `bridge_last_socket_error()`.
2. Add `src/bambu_bridge/server/WinsockInit.cpp` (Win-only) with a static-
   init WSAStartup/WSACleanup pair.
3. CMakeLists: add `iphlpapi` to the WIN32 link list, define
   `WIN32_LEAN_AND_MEAN`+`NOMINMAX`, conditionally include the new TU.
4. Strip the six `#error "Linux-only"` guards.
5. Verify Linux build is unchanged (everything still compiles to the
   POSIX branches).

### Phase B — Server transports (5–7 days, the bulk of the work)
6. Migrate one server at a time, smallest first. Suggested order:
   - `SsdpListener` (155 LoC, simplest UDP listener) — proof-of-concept.
   - `SsdpResponder` (425 LoC, UDP + IGMP membership).
   - `MqttBroker` (849 LoC, TCP + TLS + select-loop).
   - `VirtualTunnelServer` (553 LoC, TCP + FIONREAD peek).
   - `FtpsServer` (947 LoC, TCP + PASV data channel).
   - `RtspServer` (934 LoC, TCP + interleaved RTP).
7. For each server: replace `int fd` with `socket_t`, `::close` with
   `bridge_close_socket`, `MSG_NOSIGNAL`/`MSG_DONTWAIT` with shim
   equivalents, `ioctl(FIONREAD)` with `ioctlsocket(FIONREAD)`,
   `SO_RCVTIMEO` with the platform-correct argument type.
8. Run `tests/bridge/` after each file under Wine + Linux.

### Phase C — Non-socket POSIX (1–2 days)
9. Rewrite `headless/SignalHandler.cpp` to dispatch via
   `SetConsoleCtrlHandler` on Windows; keep self-pipe on POSIX.
10. Rewrite `router/UploadSpool.cpp` to use `std::filesystem` +
    `std::ofstream`. Drop the 0600 mode on Windows (rely on ACL).
11. Replace `::unlink` in `router/LanUploadSink.cpp` and
    `router/CloudUploadSink.cpp` with `std::filesystem::remove`.
12. Replace `clock_gettime` in `VirtualTunnelServer.cpp` with
    `std::chrono::steady_clock`.
13. Replace `getifaddrs` in `headless/BridgeApp.cpp` with a small
    `#ifdef _WIN32` branch calling `GetAdaptersAddresses`.

### Phase D — CLI + integration polish (1–2 days)
14. `cli/bridge_cli.cpp`: change two `/tmp/bridge_uploads` defaults to
    `std::filesystem::temp_directory_path() / "bridge_uploads"`.
15. `#ifndef _WIN32`-guard the two `sigaction(SIGPIPE, SIG_IGN)` blocks
    in `FtpsServer` and `RtspServer`.
16. Full E2E test suite (`tests/bridge/`) on Linux to confirm no
    regression. Then cross-compile + Wine-run on Linux; then native
    mingw-w64 build on a Windows machine.

### Phase E — CI + documentation (0.5 day)
17. Add a Windows build to the existing CMake matrix.
18. Document the shim in `docs/bambu_bridge_developer_guide.md`.

**Total: 8.5–13.5 engineer-days.**

---

## 4. Drop-everything-and-rewrite trigger

**No.** No file in `src/bambu_bridge/` is so deeply POSIX-coupled that
rewriting from scratch beats porting. The closest candidate is
`headless/SignalHandler.cpp` (130 LoC of self-pipe machinery that is
entirely irrelevant on Windows), but the file is already so small and
single-purpose that adding a parallel Win32 implementation in the same
header is faster than rewriting against a new abstraction. The two
implementations would share only the `SignalHandler` ctor/dtor surface,
which is already minimal.

There is also no candidate for "delete and replace with a third-party
library" — the project explicitly avoids dragging in Mongoose, Paho-MQTT,
libcurl-multi, or asio for the same reasons it avoids them elsewhere
(static-link friendliness, minimal transitive deps, OpenSSL-only TLS,
control over the framing layer for shim-recording test reproducibility).

---

## 5. Out-of-scope items

These are **explicitly not portable** and should not be ported as part
of the bridge-server Windows effort:

1. `tests/bridge/harness/ShimRecorder.cpp:30` — `pthread_getname_np` and
   `syscall(SYS_gettid)` are Linux-only and only used for human-readable
   thread labels in the shim-trace file. **Already guarded by
   `#if defined(__linux__)`** — fall-through path uses `std::thread::id`
   stream-format. No work needed.
2. `tls/CertFactory.cpp` filesystem permissions (`chmod 0700` /
   `fchmod 0600`) — Windows ACL semantics are different and the
   per-user `%APPDATA%` directory is already access-controlled by the
   OS. Drop the mode hardening on Windows; rely on per-user APPDATA.
3. The `getifaddrs`-based docker/podman/libvirt interface filter at
   `headless/BridgeApp.cpp:62-66` checks for `docker0`/`br-*`/`virbr*`/
   `veth*` interface names. The equivalent Windows virtual interfaces
   are named `vEthernet (…)` (Hyper-V) and `Local Area Connection* N`
   (loopback variants). The filter logic should be ported with a new
   Windows-side prefix list; **do NOT try to make the filter generic** —
   it's a heuristic, not a contract.
4. There is no inotify, no `/proc`, no `epoll`, no `AF_UNIX`, no
   `signalfd`/`timerfd`/`eventfd`, no D-Bus, no systemd. So none of
   those have an "out-of-scope" entry — they simply don't exist in the
   tree.

---

## 6. Total effort estimate

| Phase | Description | Effort |
|---|---|---|
| A | Shim infrastructure (Sockets.hpp + WinsockInit + CMake) | 1–2 days |
| B | Server transports (6 files × select-loop migration) | 5–7 days |
| C | Non-socket POSIX (SignalHandler + UploadSpool + ifaddrs + clock_gettime + unlinks) | 1–2 days |
| D | CLI + integration polish (paths + SIGPIPE guards + E2E) | 1–2 days |
| E | CI + docs | 0.5 day |
| **Total** | | **8.5–13.5 engineer-days (≈2 weeks of focused work)** |

**Bucket: medium.** This is not a rewrite; it is roughly 8–14 person-days
of methodical migration with a clear phase structure and reliable
checkpoints (each phase keeps Linux green). Highest single risk is the
`MSG_DONTWAIT|MSG_PEEK` pattern in `VirtualTunnelServer.cpp` and
`MqttBroker.cpp`, which needs an `ioctlsocket(FIONBIO)`-flip dance on
Windows that has subtler error semantics than the POSIX flag-based path
— budget 1 extra day for that specifically. Second-highest risk is the
include-order discipline around `<winsock2.h>` vs `<windows.h>`, which
is a one-time setup cost but easy to regress; centralising it in the
`Sockets.hpp` shim is the mitigation.

A boost::asio rewrite of the same code would be approximately 3–4×
larger (≈25–40 days) and is not recommended given:
- No other consumer of asio in the tree.
- The existing select-loop design is well-tested under the
  shim-recorder harness — preserving line-by-line equivalence with
  the Linux behaviour is much easier in a shim than in a rewrite.
- TLS is OpenSSL native, not asio's `ssl::stream`, so the rewrite
  would carry no transport-side simplification benefit either.

---

## Appendix A — Per-file inventory (quick reference)

| file | LoC | category surface | per-file effort |
|---|---:|---|---|
| `BambuNetworkingPluginHandle.cpp` | 247 | already cross-platform (LoadLibrary + dlopen branches at lines 215/224/234/242). | (none) |
| `BambuNetworkingPluginHandle.hpp` | 326 | header-only ABI mirror. | (none) |
| `BambuSourceHandle.cpp` | 333 | already cross-platform (LoadLibrary + dlopen branches at lines 95/104/114/122). | (none) |
| `BambuSourceHandle.hpp` | 175 | header-only ABI mirror. | (none) |
| `BridgeService.cpp` / .hpp | ~150 | no socket / no POSIX-specific surface. | (none) |
| `CloudInventory.cpp` / .hpp | ~200 | std::chrono + std::thread + nlohmann::json — clean. | (none) |
| `cli/bridge_cli.cpp` | 1263 | 7×`std::signal(SIGINT/SIGTERM)` (works on Win), 2× `/tmp/bridge_uploads` literals. | trivial |
| `headless/BridgeApp.cpp` | 954 | `getifaddrs` block (lines 35-72) + the rest is BridgeApp orchestration calling into servers (cross-platform). | small |
| `headless/BridgeApp.hpp` | 511 | header. | (none) |
| `headless/BridgeAppCliArgs.cpp/.hpp` | 272 | env-vars only. | (none) |
| `headless/SignalHandler.cpp/.hpp` | 192 | self-pipe + sigaction; needs Win branch. | small |
| `router/*.cpp/*.hpp` (15 pairs) | ~2200 | mostly std::thread + std::chrono + STL containers. Exceptions: `UploadSpool.cpp` (mkstemp+fchmod, fold to filesystem); `LanUploadSink.cpp` + `CloudUploadSink.cpp` (one `::unlink` each). | small (combined) |
| `server/SsdpListener.cpp/.hpp` | 247 | UDP socket. | small |
| `server/SsdpResponder.cpp/.hpp` | 592 | UDP socket + IGMP. | small |
| `server/MqttBroker.cpp/.hpp` | 1008 | TCP + TLS + select-loop. | medium |
| `server/MqttFraming.cpp/.hpp` | 662 | pure byte-level MQTT framing — cross-platform. | (none) |
| `server/FtpsServer.cpp/.hpp` | 1101 | TCP + TLS + PASV. | medium |
| `server/RtspServer.cpp/.hpp` | 1074 | TCP + TLS + RTP-over-TCP. | medium |
| `server/VirtualTunnelServer.cpp/.hpp` | 742 | TCP + FIONREAD peek + TLS-fronted PROXY. | medium |
| `server/ICameraSource.hpp` / `IUplink.hpp` / `IUploadSink.hpp` | ~230 | interface headers, no platform surface. | (none) |
| `tests/bridge/harness/ShimRecorder.cpp` | ~120 | one Linux-only thread-name call, already `#if defined(__linux__)`-guarded. | (none) |
| `CMakeLists.txt` | 171 | needs iphlpapi + new shim TU. | trivial |
