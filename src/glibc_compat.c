#ifdef SB_A30
/*
 * glibc_compat.c — compatibility shim for glibc 2.23 (Miyoo A30 / SpruceOS).
 *
 * The cross-compiler on Debian Bullseye (glibc 2.31) emits calls to symbols
 * that do not exist in glibc 2.23:
 *   - fcntl64@GLIBC_2.28  (emitted by compiler for fcntl calls)
 *   - getentropy@GLIBC_2.25 (pulled in by libcurl's SSL entropy path)
 *
 * Providing local definitions here resolves these symbols statically so
 * the dynamic linker never needs to find them in libc.so.6.
 */

#include <stdarg.h>
#include <stddef.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>

/* Bind to the old fcntl@GLIBC_2.4 which exists in glibc 2.23. */
extern int __fcntl_v4(int fd, int cmd, ...)
    __asm__("fcntl@GLIBC_2.4");

/* Provide fcntl64 locally — forward to fcntl@GLIBC_2.4. */
int fcntl64(int fd, int cmd, ...)
{
    va_list args;
    va_start(args, cmd);
    long arg = va_arg(args, long);
    va_end(args);
    return __fcntl_v4(fd, cmd, arg);
}

/*
 * getentropy — added in glibc 2.25, absent from A30's glibc 2.23.
 * libcurl references it (weakly) for SSL entropy.  Provide a strong
 * definition here so the dynamic linker never searches libc.so.6 for it.
 *
 * Uses getrandom(2) (kernel 3.17+) with a /dev/urandom fallback.
 */
int getentropy(void *buf, size_t len)
{
    if (len > 256) { errno = EIO; return -1; }
#ifdef SYS_getrandom
    long ret = syscall(SYS_getrandom, buf, (long)len, 0);
    if (ret == (long)len) return 0;
#endif
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    size_t done = 0;
    while (done < len) {
        ssize_t r = read(fd, (char *)buf + done, len - done);
        if (r <= 0) { close(fd); errno = EIO; return -1; }
        done += (size_t)r;
    }
    close(fd);
    return 0;
}

#endif /* SB_A30 */
