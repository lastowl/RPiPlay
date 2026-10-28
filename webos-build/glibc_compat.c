/*
 * glibc compatibility shims for running on glibc 2.19
 * Bridges symbols that exist in glibc 2.34 headers but not in glibc 2.19 runtime
 */
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/syscall.h>

/* --- stat/fstat/lstat wrappers ---
 * glibc 2.34 exposes fstat/stat/lstat as real libc symbols (GLIBC_2.33).
 * glibc 2.19 only has __fxstat/__xstat/__lxstat (the old versioned backend).
 * _STAT_VER for ARM is 3.
 */
#define _STAT_VER_ARM 3

extern int __fxstat(int ver, int fd, void *buf);
extern int __xstat(int ver, const char *path, void *buf);
extern int __lxstat(int ver, const char *path, void *buf);

int fstat(int fd, void *buf) {
    return __fxstat(_STAT_VER_ARM, fd, buf);
}

int stat(const char *path, void *buf) {
    return __xstat(_STAT_VER_ARM, path, buf);
}

int lstat(const char *path, void *buf) {
    return __lxstat(_STAT_VER_ARM, path, buf);
}

/* --- atexit wrapper ---
 * atexit lives in libc_nonshared.a, not exported from libc.so.
 * __cxa_atexit IS exported at GLIBC_2.4.
 */
extern int __cxa_atexit(void (*func)(void *), void *arg, void *dso_handle);
extern void *__dso_handle __attribute__((weak));

int atexit(void (*func)(void)) {
    return __cxa_atexit((void (*)(void *))func, 0, &__dso_handle);
}

/* --- pthread_atfork ---
 * Not exported from libpthread.so on glibc 2.19 (it's in libpthread_nonshared.a).
 * OpenSSL registers fork handlers but the TV never forks the process.
 */
int pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void)) {
    (void)prepare; (void)parent; (void)child;
    return 0;
}

/* --- __libc_single_threaded ---
 * Added in glibc 2.32. libstdc++.a references it for exception handling.
 * 0 = assume multi-threaded (safe default).
 */
char __libc_single_threaded = 0;

/* --- getentropy ---
 * Added in glibc 2.25. OpenSSL uses it for random seeding.
 * TV kernel 3.10 lacks SYS_getrandom, so fall back to /dev/urandom.
 */
int getentropy(void *buffer, size_t length) {
    if (length > 256) {
        errno = EIO;
        return -1;
    }

#ifdef SYS_getrandom
    long ret = syscall(SYS_getrandom, buffer, length, 0);
    if (ret >= 0 && (size_t)ret == length)
        return 0;
#endif

    int fd = open("/dev/urandom", 0);
    if (fd < 0) {
        errno = EIO;
        return -1;
    }

    size_t total = 0;
    while (total < length) {
        ssize_t n = read(fd, (char *)buffer + total, length - total);
        if (n <= 0) {
            close(fd);
            errno = EIO;
            return -1;
        }
        total += n;
    }
    close(fd);
    return 0;
}

/* --- explicit_bzero --- added in glibc 2.25 */
void explicit_bzero(void *s, size_t n) {
    volatile unsigned char *p = s;
    while (n--)
        *p++ = 0;
}
