// memfd-reopen-shim: LD_PRELOAD that lets programs reopen their own memfds read-only through
// /proc/self/fd/N, which Android's SELinux policy forbids for app processes (the memfd's tmpfs
// type has no "open" permission for the app domain; the kernel answers EACCES without an audit
// log).
//
// Firefox depends on it: its main process checks whether it can reopen a memfd read-only and, if
// not, falls back to POSIX shared memory without seals, while its content processes skip that
// check, keep expecting sealed memfds, and reject the parent's memory ("Shared memory
// PlatformHandle is not safe to map") and crash. Its own source notes the reopen "is always the
// case on Android" and, on FreeBSD, makes the read-only handle with a plain dup.
//
// Here, when opening /proc/self/fd/N (or /proc/<own pid>/fd/N, /dev/fd/N) read-only fails with EACCES and N
// is a memfd, the result is a duplicate of N instead. A duplicate keeps N's read-write mode, so it is
// only handed out while the memory is, or can still be made, write-protected by a seal: Firefox
// reopens unsealed memfds and seals frozen ones with F_SEAL_FUTURE_WRITE (Linux >= 5.1) before
// sharing them, so no fd can write to them. Chromium instead adds F_SEAL_SEAL without a write seal
// and relies on the read-only fd's mode alone; it checks that mode and crashes on a writable fd, so
// it keeps the EACCES and uses its own fallback. Every other open is passed through untouched.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// The fd number when `path` names one of this process's own fds in /proc, else -1.
static int own_proc_fd(const char *path) {
  const char *rest;
  if (!strncmp(path, "/proc/self/fd/", 14)) {
    rest = path + 14;
  } else if (!strncmp(path, "/dev/fd/", 8)) {
    rest = path + 8;
  } else if (!strncmp(path, "/proc/", 6)) {
    char *end;
    long pid = strtol(path + 6, &end, 10);
    if (end == path + 6 || pid != getpid() || strncmp(end, "/fd/", 4)) return -1;
    rest = end + 4;
  } else {
    return -1;
  }
  char *end;
  long fd = strtol(rest, &end, 10);
  return (end != rest && !*end && fd >= 0 && fd <= 1 << 20) ? (int)fd : -1;
}

static int is_memfd(int fd) {
  char link[32], target[64];
  snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
  ssize_t n = readlink(link, target, sizeof target - 1);
  if (n <= 0) return 0;
  target[n] = 0;
  return !strncmp(target, "/memfd:", 7);
}

// Whether no fd can write to the memfd, now or once its owner adds a write seal. F_SEAL_SEAL
// without a write seal means it stays writable through any read-write fd for good.
static int can_be_write_protected(int fd) {
  int seals = fcntl(fd, F_GET_SEALS);
  if (seals < 0) return 0;
  return (seals & (F_SEAL_WRITE | F_SEAL_FUTURE_WRITE)) || !(seals & F_SEAL_SEAL);
}

// Called after the real open failed: the replacement result, or -1 to keep the failure.
static int reopen_fallback(const char *path, int flags, int saved_errno) {
  if (saved_errno != EACCES || (flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC))) {
    return -1;
  }
  int fd = own_proc_fd(path);
  if (fd < 0 || !is_memfd(fd) || !can_be_write_protected(fd)) return -1;
  return fcntl(fd, (flags & O_CLOEXEC) ? F_DUPFD_CLOEXEC : F_DUPFD, 0);
}

#define OPEN_WRAPPER(name, ...)                                                   \
  int name(const char *path, int flags, ...) {                                    \
    static int (*real)(const char *, int, ...);                                   \
    if (!real) real = (int (*)(const char *, int, ...))dlsym(RTLD_NEXT, #name);   \
    mode_t mode = 0;                                                              \
    if (flags & (O_CREAT | __O_TMPFILE)) {                                        \
      va_list ap;                                                                 \
      va_start(ap, flags);                                                        \
      mode = va_arg(ap, mode_t);                                                  \
      va_end(ap);                                                                 \
    }                                                                             \
    int ret = real(path, flags, mode);                                            \
    if (ret < 0) {                                                                 \
      int saved = errno;                                                          \
      int dup = reopen_fallback(path, flags, saved);                              \
      if (dup >= 0) return dup;                                                   \
      errno = saved;                                                              \
    }                                                                             \
    return ret;                                                                   \
  }

OPEN_WRAPPER(open)
OPEN_WRAPPER(open64)

#define OPENAT_WRAPPER(name)                                                        \
  int name(int dirfd, const char *path, int flags, ...) {                           \
    static int (*real)(int, const char *, int, ...);                                \
    if (!real) real = (int (*)(int, const char *, int, ...))dlsym(RTLD_NEXT, #name);\
    mode_t mode = 0;                                                                \
    if (flags & (O_CREAT | __O_TMPFILE)) {                                          \
      va_list ap;                                                                   \
      va_start(ap, flags);                                                          \
      mode = va_arg(ap, mode_t);                                                    \
      va_end(ap);                                                                   \
    }                                                                               \
    int ret = real(dirfd, path, flags, mode);                                       \
    if (ret < 0 && path[0] == '/') {                                                \
      int saved = errno;                                                            \
      int dup = reopen_fallback(path, flags, saved);                                \
      if (dup >= 0) return dup;                                                     \
      errno = saved;                                                                \
    }                                                                               \
    return ret;                                                                     \
  }

OPENAT_WRAPPER(openat)
OPENAT_WRAPPER(openat64)

// _FORTIFY_SOURCE builds call these for open() without a mode.
int __open_2(const char *path, int flags) { return open(path, flags); }
int __open64_2(const char *path, int flags) { return open64(path, flags); }
int __openat_2(int dirfd, const char *path, int flags) { return openat(dirfd, path, flags); }
int __openat64_2(int dirfd, const char *path, int flags) { return openat64(dirfd, path, flags); }
