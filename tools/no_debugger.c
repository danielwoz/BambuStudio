// Anti-anti-debug LD_PRELOAD shim for libbambu_networking.so.
//
// The closed-source plugin refuses to run when it detects a tracer.
// Common checks it likely uses:
//   1. ptrace(PTRACE_TRACEME, 0, 0, 0) — fails with EPERM if traced.
//   2. /proc/self/status — TracerPid != 0 if traced.
//   3. Scanning /proc/<pid>/comm or cmdline for "gdb" / "lldb".
//
// We intercept just enough libc to lie about all three. Compile as
//
//   gcc -O2 -fPIC -shared -ldl -o no_debugger.so no_debugger.c
//
// then:
//
//   LD_PRELOAD=$PWD/no_debugger.so gdb --args ./bambu-studio …

#define _GNU_SOURCE
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// --- ptrace: PTRACE_TRACEME → success ---------------------------------
// We intercept the symbol the plugin's libc calls. If the plugin's own
// debug check is `ptrace(PTRACE_TRACEME, 0, 0, 0) == -1`, returning 0
// here tells it "you're not being traced".
long ptrace(enum __ptrace_request req, ...) {
    if (req == PTRACE_TRACEME) return 0;
    errno = EPERM;
    return -1;
}

// --- /proc/self/status spoofing ---------------------------------------
// The plugin may open /proc/self/status (or /proc/<pid>/status for
// itself) and look for "TracerPid:\t<n>". We detect those opens and
// hand back a file that has "TracerPid:\t0" forced regardless of the
// real value.

typedef int  (*orig_open_t)(const char*, int, ...);
typedef int  (*orig_openat_t)(int, const char*, int, ...);
typedef FILE* (*orig_fopen_t)(const char*, const char*);

static int     looks_like_self_status(const char* path) {
    if (!path) return 0;
    // /proc/self/status or /proc/<mypid>/status
    if (strcmp(path, "/proc/self/status") == 0) return 1;
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "/proc/%d/status", getpid());
    if (strcmp(path, prefix) == 0) return 1;
    // /proc/<tid>/status — same process, different thread. We can't
    // cheaply check that's our pid, but any /proc/<digits>/status
    // request from inside our process is almost certainly self-inspection.
    if (strncmp(path, "/proc/", 6) != 0) return 0;
    const char* p = path + 6;
    while (isdigit((unsigned char)*p)) p++;
    return strcmp(p, "/status") == 0 || strcmp(p, "/task") == 0
        || strncmp(p, "/task/", 6) == 0;
}

static int spoofed_status_fd(void) {
    static const char content[] =
        "Name:\tbambu-studio\n"
        "Umask:\t0022\n"
        "State:\tR (running)\n"
        "Tgid:\t1\n"
        "Ngid:\t0\n"
        "Pid:\t1\n"
        "PPid:\t1\n"
        "TracerPid:\t0\n"
        "Uid:\t1000\t1000\t1000\t1000\n"
        "Gid:\t1000\t1000\t1000\t1000\n";
    char path[] = "/tmp/nodbgstatusXXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    unlink(path);
    if (write(fd, content, sizeof(content) - 1) < 0) {
        close(fd);
        return -1;
    }
    lseek(fd, 0, SEEK_SET);
    return fd;
}

int open(const char* path, int flags, ...) {
    static orig_open_t real = NULL;
    if (!real) real = (orig_open_t)dlsym(RTLD_NEXT, "open");

    if (looks_like_self_status(path)) {
        int fd = spoofed_status_fd();
        if (fd >= 0) return fd;
    }

    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap; va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    return real(path, flags, mode);
}

int openat(int dirfd, const char* path, int flags, ...) {
    static orig_openat_t real = NULL;
    if (!real) real = (orig_openat_t)dlsym(RTLD_NEXT, "openat");

    if (dirfd == AT_FDCWD && looks_like_self_status(path)) {
        int fd = spoofed_status_fd();
        if (fd >= 0) return fd;
    }

    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap; va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    return real(dirfd, path, flags, mode);
}

FILE* fopen(const char* path, const char* mode) {
    static orig_fopen_t real = NULL;
    if (!real) real = (orig_fopen_t)dlsym(RTLD_NEXT, "fopen");

    if (looks_like_self_status(path)) {
        int fd = spoofed_status_fd();
        if (fd >= 0) return fdopen(fd, "r");
    }
    return real(path, mode);
}

// --- /proc/<pid>/{comm,cmdline,exe} for OTHER processes ---------------
// If the plugin scans /proc to find a "gdb" process, we filter the
// output of those reads so it never finds one. The cheap-and-cheerful
// version: intercept opendir("/proc") and skip directory entries whose
// /proc/<entry>/comm matches "gdb*". This is enough for the common
// scan-for-debugger pattern.
//
// NOT implementing this layer yet — start with ptrace + TracerPid and
// only escalate if the plugin's check still triggers. Most plugins
// stop at the cheap checks; adding more here just risks breaking the
// slicer's own /proc reads.
