// NOT USED (kept for reference, not shipped): tried 2026-10-23..26 as sync step B and removed.
// Measured unnecessary: guest/tools/synctest.c showed 0 partly drawn frames with or without it,
// since KGSL runs GPU work in submission order and same-process clients submit before committing.
// Its libwayland hooks also crashed programs that load libwayland in a local scope or carry their
// own copy (kactivitymanagerd → black screen, Brave). If a commit-time wait is ever needed, do it
// without hooking libwayland (e.g. a Vulkan present layer). Build in the guest like the shipped
// shims: gcc -O2 -Wall -fPIC -shared -o kgsl-commit-wait.so kgsl-commit-wait.c -ldl -lpthread
//
// kgsl-commit-wait: LD_PRELOAD that makes a Wayland client's GPU frames complete before they are
// handed over. A client attaches a GPU buffer (dmabuf) and commits its wl_surface right after
// queueing the GPU work that draws it; desktop Linux then relies on the kernel's implicit fences
// on the dmabuf, or on explicit sync, to keep the compositor from reading early. Neither exists
// here: KGSL attaches no fences to its buffers and this kernel (5.15) lacks
// DMA_BUF_IOCTL_IMPORT_SYNC_FILE, so the compositor could show a half-drawn or stale frame (seen
// as garbage and green flashes). This preload records the last timestamp each KGSL GPU context of
// the process submitted (IOCTL_KGSL_GPU_COMMAND / GPU_AUX_COMMAND) and, before a wl_surface
// commit, waits until those have retired (IOCTL_KGSL_DEVICE_WAITTIMESTAMP_CTXTID).
//
// It covers clients that render and commit in the same process: Mesa (Zink, Vulkan WSI), Qt, KWin
// presenting to the app. Chromium renders in its GPU process but commits from the browser process,
// so it needs explicit sync (wp_linux_drm_syncobj) instead.
//
// LOCALDESKTOP_COMMIT_WAIT=0 turns it off for a process (for A/B tests); =log logs each wait.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <wayland-client-core.h>
#include <wayland-util.h>

#define KGSL_IOC_TYPE 0x09

struct kgsl_gpu_command {
  uint64_t flags;
  uint64_t cmdlist;
  unsigned int cmdsize;
  unsigned int numcmds;
  uint64_t objlist;
  unsigned int objsize;
  unsigned int numobjs;
  uint64_t synclist;
  unsigned int syncsize;
  unsigned int numsyncs;
  unsigned int context_id;
  unsigned int timestamp;
};

struct kgsl_gpu_aux_command {
  uint64_t flags;
  uint64_t cmdlist;
  uint32_t cmdsize;
  uint32_t numcmds;
  uint64_t synclist;
  uint32_t syncsize;
  uint32_t numsyncs;
  uint32_t context_id;
  uint32_t timestamp;
};

struct kgsl_device_waittimestamp_ctxtid {
  unsigned int context_id;
  unsigned int timestamp;
  unsigned int timeout;
};

struct kgsl_drawctxt_destroy {
  unsigned int drawctxt_id;
};

#define IOCTL_KGSL_DEVICE_WAITTIMESTAMP_CTXTID \
  _IOW(KGSL_IOC_TYPE, 0x7, struct kgsl_device_waittimestamp_ctxtid)
#define IOCTL_KGSL_DRAWCTXT_DESTROY _IOW(KGSL_IOC_TYPE, 0x14, struct kgsl_drawctxt_destroy)
#define IOCTL_KGSL_GPU_COMMAND _IOWR(KGSL_IOC_TYPE, 0x4A, struct kgsl_gpu_command)
#define IOCTL_KGSL_GPU_AUX_COMMAND _IOWR(KGSL_IOC_TYPE, 0x57, struct kgsl_gpu_aux_command)

// The longest a commit waits for the GPU. A frame should take a few ms; the cap only keeps a
// wedged context (or GPU work waiting on the compositor) from hanging the client for good.
#define WAIT_TIMEOUT_MS 500
#define WL_SURFACE_COMMIT 6
#define MAX_CONTEXTS 64
#ifndef WL_CLOSURE_MAX_ARGS
#define WL_CLOSURE_MAX_ARGS 20  // libwayland's limit (wayland-private.h)
#endif

struct context {
  int fd;
  unsigned int id;
  unsigned int submitted;  // last timestamp the context submitted
  unsigned int waited;     // last timestamp a commit waited for
  int used;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct context contexts[MAX_CONTEXTS];
static int mode = -1;  // 0 off, 1 on, 2 on and logging

static int (*real_ioctl)(int, unsigned long, ...);

static int get_mode(void) {
  if (mode < 0) {
    const char *value = getenv("LOCALDESKTOP_COMMIT_WAIT");
    mode = !value ? 1 : !strcmp(value, "0") ? 0 : !strcmp(value, "log") ? 2 : 1;
  }
  return mode;
}

static void record_submit(int fd, unsigned int context_id, unsigned int timestamp) {
  pthread_mutex_lock(&lock);
  struct context *slot = NULL;
  for (int i = 0; i < MAX_CONTEXTS; i++) {
    if (contexts[i].used && contexts[i].fd == fd && contexts[i].id == context_id) {
      slot = &contexts[i];
      break;
    }
    if (!slot && !contexts[i].used) slot = &contexts[i];
  }
  if (slot) {
    if (!slot->used || slot->fd != fd || slot->id != context_id) {
      *slot = (struct context){.fd = fd, .id = context_id, .waited = timestamp - 1, .used = 1};
    }
    slot->submitted = timestamp;
  }
  pthread_mutex_unlock(&lock);
}

static void forget_context(int fd, unsigned int context_id) {
  pthread_mutex_lock(&lock);
  for (int i = 0; i < MAX_CONTEXTS; i++) {
    if (contexts[i].used && contexts[i].fd == fd && contexts[i].id == context_id) {
      contexts[i].used = 0;
    }
  }
  pthread_mutex_unlock(&lock);
}

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

// Waits until every context's last submission has retired.
static void wait_for_gpu(void) {
  struct context pending[MAX_CONTEXTS];
  int count = 0;
  pthread_mutex_lock(&lock);
  for (int i = 0; i < MAX_CONTEXTS; i++) {
    if (contexts[i].used && contexts[i].submitted != contexts[i].waited) {
      pending[count++] = contexts[i];
    }
  }
  pthread_mutex_unlock(&lock);
  if (!count) return;

  int saved_errno = errno;
  double start = now_ms();
  for (int i = 0; i < count; i++) {
    struct kgsl_device_waittimestamp_ctxtid wait = {
        .context_id = pending[i].id, .timestamp = pending[i].submitted, .timeout = WAIT_TIMEOUT_MS};
    int ret;
    do {
      ret = real_ioctl(pending[i].fd, IOCTL_KGSL_DEVICE_WAITTIMESTAMP_CTXTID, &wait);
    } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
    if (ret == -1 && errno != ETIMEDOUT) {
      // The context is gone (or the fd was reused): stop tracking it.
      forget_context(pending[i].fd, pending[i].id);
      continue;
    }
    if (ret == -1) {
      fprintf(stderr, "kgsl-commit-wait: context %u timestamp %u not done after %d ms\n",
              pending[i].id, pending[i].submitted, WAIT_TIMEOUT_MS);
    }
    pthread_mutex_lock(&lock);
    for (int j = 0; j < MAX_CONTEXTS; j++) {
      if (contexts[j].used && contexts[j].fd == pending[i].fd && contexts[j].id == pending[i].id) {
        contexts[j].waited = pending[i].submitted;
      }
    }
    pthread_mutex_unlock(&lock);
  }
  if (get_mode() == 2) {
    fprintf(stderr, "kgsl-commit-wait: waited %.2f ms for %d context(s)\n", now_ms() - start, count);
  }
  errno = saved_errno;
}

int ioctl(int fd, unsigned long request, ...) {
  va_list ap;
  va_start(ap, request);
  void *arg = va_arg(ap, void *);
  va_end(ap);
  if (!real_ioctl) {
    real_ioctl = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
  }
  int ret = real_ioctl(fd, request, arg);
  if (ret == 0 && arg) {
    if (request == IOCTL_KGSL_GPU_COMMAND) {
      struct kgsl_gpu_command *command = arg;
      record_submit(fd, command->context_id, command->timestamp);
    } else if (request == IOCTL_KGSL_GPU_AUX_COMMAND) {
      struct kgsl_gpu_aux_command *command = arg;
      record_submit(fd, command->context_id, command->timestamp);
    } else if (request == IOCTL_KGSL_DRAWCTXT_DESTROY) {
      forget_context(fd, ((struct kgsl_drawctxt_destroy *)arg)->drawctxt_id);
    }
  }
  return ret;
}

// The real libwayland function behind a hook. RTLD_NEXT only searches libraries in the global
// scope; programs that get libwayland through a dlopen()ed plugin (Qt's Wayland platform plugin,
// e.g. in kactivitymanagerd) have it in a local scope, where only a handle to the library itself
// finds it. Without this fallback the hook called NULL and crashed those programs.
static void *wayland_function(const char *name) {
  void *fn = dlsym(RTLD_NEXT, name);
  if (!fn) {
    void *library = dlopen("libwayland-client.so.0", RTLD_LAZY | RTLD_NOLOAD);
    if (library) {
      fn = dlsym(library, name);
      dlclose(library);
    }
  }
  if (!fn) {
    fprintf(stderr, "kgsl-commit-wait: %s not found\n", name);
    abort();
  }
  return fn;
}

static void before_request(struct wl_proxy *proxy, uint32_t opcode) {
  // Looked up at run time: this preload also loads into programs without libwayland (which
  // never reach this, as only libwayland's callers call the hooks).
  static const char *(*get_class)(struct wl_proxy *);
  if (opcode != WL_SURFACE_COMMIT || !get_mode()) return;
  if (!get_class) {
    get_class = (const char *(*)(struct wl_proxy *))wayland_function("wl_proxy_get_class");
  }
  if (!strcmp(get_class(proxy), "wl_surface")) {
    wait_for_gpu();
  }
}

// Fills wl_argument values from a request's varargs, as libwayland does internally
// (wl_argument_from_va_list is private), so the request can go on to the array variant.
static void arguments_from_va_list(const char *signature, union wl_argument *args, int count,
                                   va_list ap) {
  int i = 0;
  for (const char *s = signature; *s && i < count; s++) {
    switch (*s) {
      case 'i': args[i++].i = va_arg(ap, int32_t); break;
      case 'u': args[i++].u = va_arg(ap, uint32_t); break;
      case 'f': args[i++].f = va_arg(ap, wl_fixed_t); break;
      case 's': args[i++].s = va_arg(ap, const char *); break;
      case 'o': args[i++].o = va_arg(ap, struct wl_object *); break;
      case 'n': args[i++].o = va_arg(ap, struct wl_object *); break;
      case 'a': args[i++].a = va_arg(ap, struct wl_array *); break;
      case 'h': args[i++].h = va_arg(ap, int32_t); break;
      default: break;  // '?' (nullable) and the version digits
    }
  }
}

static int argument_count(const char *signature) {
  int count = 0;
  for (const char *s = signature; *s; s++) {
    if (strchr("iufsonah", *s)) count++;
  }
  return count;
}

// wl_proxy_get_interface() only exists in newer libwayland; the class name is enough to find
// wl_surface, and the signature comes from the interface libwayland stores first in wl_proxy
// (struct wl_object { const struct wl_interface *interface; ... }).
static const struct wl_interface *proxy_interface(struct wl_proxy *proxy) {
  return *(const struct wl_interface *const *)proxy;
}

struct wl_proxy *wl_proxy_marshal_flags(struct wl_proxy *proxy, uint32_t opcode,
                                        const struct wl_interface *interface, uint32_t version,
                                        uint32_t flags, ...) {
  static struct wl_proxy *(*marshal_array_flags)(struct wl_proxy *, uint32_t,
                                                 const struct wl_interface *, uint32_t, uint32_t,
                                                 union wl_argument *);
  if (!marshal_array_flags) {
    marshal_array_flags = wayland_function("wl_proxy_marshal_array_flags");
  }
  before_request(proxy, opcode);
  const char *signature = proxy_interface(proxy)->methods[opcode].signature;
  union wl_argument args[WL_CLOSURE_MAX_ARGS];
  memset(args, 0, sizeof args);
  va_list ap;
  va_start(ap, flags);
  arguments_from_va_list(signature, args, argument_count(signature), ap);
  va_end(ap);
  return marshal_array_flags(proxy, opcode, interface, version, flags, args);
}

void wl_proxy_marshal(struct wl_proxy *proxy, uint32_t opcode, ...) {
  static void (*marshal_array)(struct wl_proxy *, uint32_t, union wl_argument *);
  if (!marshal_array) {
    marshal_array = wayland_function("wl_proxy_marshal_array");
  }
  before_request(proxy, opcode);
  const char *signature = proxy_interface(proxy)->methods[opcode].signature;
  union wl_argument args[WL_CLOSURE_MAX_ARGS];
  memset(args, 0, sizeof args);
  va_list ap;
  va_start(ap, opcode);
  arguments_from_va_list(signature, args, argument_count(signature), ap);
  va_end(ap);
  marshal_array(proxy, opcode, args);
}

// Requests that already come as arrays need no unpacking.
struct wl_proxy *wl_proxy_marshal_array_flags(struct wl_proxy *proxy, uint32_t opcode,
                                              const struct wl_interface *interface,
                                              uint32_t version, uint32_t flags,
                                              union wl_argument *args) {
  static struct wl_proxy *(*real_fn)(struct wl_proxy *, uint32_t, const struct wl_interface *,
                                     uint32_t, uint32_t, union wl_argument *);
  if (!real_fn) {
    real_fn = wayland_function("wl_proxy_marshal_array_flags");
  }
  before_request(proxy, opcode);
  return real_fn(proxy, opcode, interface, version, flags, args);
}

static void reset_after_fork(void) { pthread_mutex_init(&lock, NULL); }

__attribute__((constructor)) static void install(void) {
  pthread_atfork(NULL, NULL, reset_after_fork);
}
