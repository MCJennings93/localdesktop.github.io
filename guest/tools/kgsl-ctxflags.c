// kgsl-ctxflags: diagnostic LD_PRELOAD (not shipped) that ORs KGSL_CTX_FLAGS (hex) into the flags of
// every IOCTL_KGSL_DRAWCTXT_CREATE, e.g. a preemption style (KGSL_CONTEXT_PREEMPT_STYLE_*) or a
// context type, to test how KGSL's handling of Turnip's contexts affects rendering. Logs each
// context it changes to stderr.
// Build: gcc -O2 -Wall -fPIC -shared -I/root/src/mesa-26.2.3/src/freedreno/vulkan
//   -o kgsl-ctxflags.so kgsl-ctxflags.c -ldl (one line)
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>
#define __user
#include "msm_kgsl.h"

int ioctl(int fd, unsigned long request, ...) {
  static int (*real)(int, unsigned long, ...);
  if (!real) real = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
  va_list ap;
  va_start(ap, request);
  void *arg = va_arg(ap, void *);
  va_end(ap);
  if (request == IOCTL_KGSL_DRAWCTXT_CREATE && arg) {
    const char *extra = getenv("KGSL_CTX_FLAGS");
    struct kgsl_drawctxt_create *req = arg;
    if (extra) {
      unsigned int before = req->flags;
      req->flags |= (unsigned int)strtoul(extra, NULL, 16);
      int ret = real(fd, request, arg);
      fprintf(stderr, "kgsl-ctxflags: pid %d context %u: flags %#x -> %#x (ret %d)\n", getpid(),
              req->drawctxt_id, before, req->flags, ret);
      return ret;
    }
  }
  return real(fd, request, arg);
}
