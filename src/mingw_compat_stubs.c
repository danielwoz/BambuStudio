/* MinGW runtime-symbol shims for MSVC link.
 *
 * The Windows build links a handful of MinGW-built static archives that
 * ship from the Strawberry/MSYS2 toolchain (libiconv.a in particular).
 * When that archive was compiled with -fstack-protector / _FORTIFY_SOURCE
 * it references a few MinGW runtime helpers that the MSVC CRT does not
 * provide:
 *
 *     ___chkstk_ms      __stack_chk_fail   __stack_chk_guard
 *     __mingw_snprintf  __strcpy_chk       __chk_fail
 *
 * None of these names collide with the MSVC CRT (which uses
 * __security_cookie / __security_check_cookie instead), so we provide
 * minimal, correct implementations here so the MSVC linker resolves them.
 *
 * This file is compiled only on MSVC (see src/CMakeLists.txt).
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)

/* Stack-protector canary value. GCC reads this global; we never trip the
 * check (it only fails on actual stack-buffer overruns). */
void *__stack_chk_guard = (void *)0xDEADBEEFCAFEBABEULL;

/* Called by GCC-built code when the stack canary is clobbered. */
void __stack_chk_fail(void)
{
    abort();
}

/* Generic _FORTIFY_SOURCE failure handler. */
void __chk_fail(void)
{
    abort();
}

/* Fortified strcpy: copy at most dstlen bytes, abort on overflow — same
 * contract GCC's __strcpy_chk expects. */
char *__strcpy_chk(char *dst, const char *src, size_t dstlen)
{
    size_t n = strlen(src) + 1;
    if (n > dstlen)
        __chk_fail();
    memcpy(dst, src, n);
    return dst;
}

/* MinGW's printf-family entry used by some fortified builds. */
int __mingw_snprintf(char *s, size_t n, const char *fmt, ...)
{
    int r;
    va_list ap;
    va_start(ap, fmt);
    r = vsnprintf(s, n, fmt, ap);
    va_end(ap);
    return r;
}

/* GCC's stack-probe helper. The "_ms" variant only PROBES the stack
 * (touching guard pages for frames larger than one page) and must
 * preserve every register, including RAX which carries the frame size on
 * entry; the caller adjusts RSP itself afterwards. An empty leaf function
 * compiles to a bare `ret`, which preserves all registers and simply
 * skips the page-probe. That is safe for the modest stack frames in the
 * libiconv conversion routines that reference it. */
void ___chkstk_ms(void)
{
}

#endif /* _MSC_VER */
