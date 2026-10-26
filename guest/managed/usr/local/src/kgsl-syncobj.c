// kgsl-syncobj: LD_PRELOAD that emulates DRM sync objects (drm_syncobj) on the fake render node
// (/dev/dri/renderD0 = /dev/kgsl-3d0), so KWin and Chromium can use explicit sync
// (wp_linux_drm_syncobj_v1) for the GPU buffers clients hand the compositor.
//
// Why: Chromium draws a frame in its GPU process but commits it from the browser process. Without
// a fence travelling with the buffer, KWin can show a buffer the GPU has not finished (garbage and
// green flashes after Android overlays make Chromium re-create its buffers). Desktop Linux carries
// that fence implicitly on the dmabuf or explicitly in a DRM syncobj; KGSL is not a DRM driver and
// this kernel (5.15) has neither, but KGSL does make real sync_file fences, which is enough.
//
// How:
// - A syncobj has a binary payload (no fence / signalled / a sync_file fd), process-local, and on
//   first timeline use a timeline: one shared page in a memfd holding the highest available
//   ("materialized") and signalled points plus a futex word. The memfd is the syncobj fd that
//   HANDLE_TO_FD exports and the Wayland protocol passes between processes; FD_TO_HANDLE maps it.
// - Attaching a real sync_file to a point (TRANSFER from a binary syncobj) marks the point
//   available; a watcher thread in that process poll()s the fence and marks it signalled. Every
//   change bumps the futex word and wakes waiters in all processes.
// - TIMELINE_WAIT futex-waits on the word; EVENTFD registrations are served by a notifier thread
//   per timeline that writes the eventfd once its point is reached.
// - Exporting a point as a sync_file waits until the point is signalled, then returns a dup of a
//   signalled KGSL fence (a fence event on a retired timestamp of this process's own context).
//   Fences imported in this process are kept and exported as they are.
//
// KWin (>= 6.6.19) and Chromium (>= 6.11) only use syncobjs on new enough kernels, as they rely
// on DRM_IOCTL_SYNCOBJ_EVENTFD, which this provides. So uname() reports a raised version to the
// programs listed in /etc/localdesktop/explicit-sync, only while the real kernel is older than the
// configured floor, and logs it once per process. The same file can turn everything off.
//
// LOCALDESKTOP_SYNCOBJ_LOG=1 logs every emulated call to stderr.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/futex.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

// ---- DRM uapi (sizes vary between kernel versions; fields are read only within the given size)

#define DRM_TYPE 'd'
#define NR_GET_CAP 0x0C
#define NR_SYNCOBJ_CREATE 0xBF
#define NR_SYNCOBJ_DESTROY 0xC0
#define NR_SYNCOBJ_HANDLE_TO_FD 0xC1
#define NR_SYNCOBJ_FD_TO_HANDLE 0xC2
#define NR_SYNCOBJ_WAIT 0xC3
#define NR_SYNCOBJ_RESET 0xC4
#define NR_SYNCOBJ_SIGNAL 0xC5
#define NR_SYNCOBJ_TIMELINE_WAIT 0xCA
#define NR_SYNCOBJ_QUERY 0xCB
#define NR_SYNCOBJ_TRANSFER 0xCC
#define NR_SYNCOBJ_TIMELINE_SIGNAL 0xCD
#define NR_SYNCOBJ_EVENTFD 0xCF

#define DRM_CAP_SYNCOBJ 0x13
#define DRM_CAP_SYNCOBJ_TIMELINE 0x14
#define DRM_SYNCOBJ_CREATE_SIGNALED (1 << 0)
#define DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE (1 << 0)
#define DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_TIMELINE (1 << 1)
#define DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE (1 << 0)
#define DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_TIMELINE (1 << 1)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL (1 << 0)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT (1 << 1)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE (1 << 2)
#define DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED (1 << 0)

struct drm_get_cap {
  uint64_t capability;
  uint64_t value;
};
struct drm_syncobj_create {
  uint32_t handle;
  uint32_t flags;
};
struct drm_syncobj_destroy {
  uint32_t handle;
  uint32_t pad;
};
struct drm_syncobj_handle {
  uint32_t handle;
  uint32_t flags;
  int32_t fd;
  uint32_t pad;
  uint64_t point;  // newer kernels only
};
struct drm_syncobj_transfer {
  uint32_t src_handle;
  uint32_t dst_handle;
  uint64_t src_point;
  uint64_t dst_point;
  uint32_t flags;
  uint32_t pad;
};
struct drm_syncobj_wait {
  uint64_t handles;
  int64_t timeout_nsec;  // absolute CLOCK_MONOTONIC
  uint32_t count_handles;
  uint32_t flags;
  uint32_t first_signaled;
  uint32_t pad;
};
struct drm_syncobj_timeline_wait {
  uint64_t handles;
  uint64_t points;
  int64_t timeout_nsec;  // absolute CLOCK_MONOTONIC
  uint32_t count_handles;
  uint32_t flags;
  uint32_t first_signaled;
  uint32_t pad;
};
struct drm_syncobj_eventfd {
  uint32_t handle;
  uint32_t flags;
  uint64_t point;
  int32_t fd;
  uint32_t pad;
};
struct drm_syncobj_array {
  uint64_t handles;
  uint32_t count_handles;
  uint32_t pad;
};
struct drm_syncobj_timeline_array {
  uint64_t handles;
  uint64_t points;
  uint32_t count_handles;
  uint32_t flags;
};

// ---- KGSL uapi, for signalled fences

#define KGSL_IOC_TYPE 0x09
struct kgsl_drawctxt_create {
  unsigned int flags;
  unsigned int drawctxt_id;
};
struct kgsl_timestamp_event {
  int type;
  unsigned int timestamp;
  unsigned int context_id;
  void *priv;
  size_t len;
};
#define IOCTL_KGSL_DRAWCTXT_CREATE _IOWR(KGSL_IOC_TYPE, 0x13, struct kgsl_drawctxt_create)
#define IOCTL_KGSL_TIMESTAMP_EVENT _IOWR(KGSL_IOC_TYPE, 0x33, struct kgsl_timestamp_event)
#define KGSL_TIMESTAMP_EVENT_FENCE 2
#define KGSL_CONTEXT_NO_GMEM_ALLOC 0x00000002
#define KGSL_CONTEXT_PREAMBLE 0x00000010
#define KGSL_CONTEXT_TYPE_GL (1 << 20)

// ---- configuration

#define CONFIG_FILE "/etc/localdesktop/explicit-sync"
static struct {
  int loaded;
  int enabled;
  int floor_major, floor_minor;
  char programs[512];
  int log;
} config;
static pthread_once_t config_once = PTHREAD_ONCE_INIT;

static void trim(char *s) {
  char *end = s + strlen(s);
  while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r')) *--end = 0;
  char *start = s;
  while (*start == ' ' || *start == '\t') start++;
  memmove(s, start, strlen(start) + 1);
}

static void load_config(void) {
  config.enabled = 1;
  config.floor_major = 6;
  config.floor_minor = 12;
  snprintf(config.programs, sizeof config.programs,
           "kwin_wayland brave chrome chromium electron steamwebhelper");
  FILE *file = fopen(CONFIG_FILE, "re");
  if (file) {
    char line[600];
    while (fgets(line, sizeof line, file)) {
      char *hash = strchr(line, '#');
      if (hash) *hash = 0;
      char *equals = strchr(line, '=');
      if (!equals) continue;
      *equals = 0;
      char *key = line, *value = equals + 1;
      trim(key);
      trim(value);
      if (!strcmp(key, "enabled")) {
        config.enabled = !strcmp(value, "yes") || !strcmp(value, "true") || !strcmp(value, "1");
      } else if (!strcmp(key, "min_kernel")) {
        sscanf(value, "%d.%d", &config.floor_major, &config.floor_minor);
      } else if (!strcmp(key, "programs")) {
        snprintf(config.programs, sizeof config.programs, "%s", value);
      }
    }
    fclose(file);
  }
  const char *log = getenv("LOCALDESKTOP_SYNCOBJ_LOG");
  config.log = log && *log && strcmp(log, "0");
  config.loaded = 1;
}

static int enabled(void) {
  pthread_once(&config_once, load_config);
  return config.enabled;
}

static void trace(const char *format, ...) {
  if (!config.log) return;
  char line[256];
  va_list ap;
  va_start(ap, format);
  int n = vsnprintf(line, sizeof line, format, ap);
  va_end(ap);
  if (n > 0) {
    fprintf(stderr, "kgsl-syncobj[%d]: %s\n", (int)getpid(), line);
  }
}

// ---- uname

static const char *program_name(void) {
  static char name[256];
  if (!name[0]) {
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    exe[n > 0 ? n : 0] = 0;
    const char *slash = strrchr(exe, '/');
    snprintf(name, sizeof name, "%.255s", slash ? slash + 1 : exe);
  }
  return name;
}

static int listed_program(void) {
  const char *name = program_name();
  size_t length = strlen(name);
  for (const char *p = config.programs; *p;) {
    while (*p == ' ' || *p == '\t') p++;
    const char *end = p;
    while (*end && *end != ' ' && *end != '\t') end++;
    if ((size_t)(end - p) == length && length && !strncmp(p, name, length)) return 1;
    p = end;
  }
  return 0;
}

int uname(struct utsname *buf) {
  static int (*real_uname)(struct utsname *);
  if (!real_uname) real_uname = (int (*)(struct utsname *))dlsym(RTLD_NEXT, "uname");
  int ret = real_uname(buf);
  if (ret || !enabled() || !listed_program()) return ret;
  int major = 0, minor = 0;
  sscanf(buf->release, "%d.%d", &major, &minor);
  if (major > config.floor_major || (major == config.floor_major && minor >= config.floor_minor)) {
    return ret;  // the real kernel is new enough: report it as it is
  }
  char real_release[sizeof buf->release];
  memcpy(real_release, buf->release, sizeof real_release);
  // "<floor>.0-ld-<real>", cut to fit; parsers read the leading numbers.
  snprintf(buf->release, sizeof buf->release, "%d.%d.0-ld-%.40s", config.floor_major,
           config.floor_minor, real_release);
  static atomic_int logged;
  if (!atomic_exchange(&logged, 1)) {
    fprintf(stderr, "kgsl-syncobj: reporting kernel %d.%d to %s for explicit sync (real %s)\n",
            config.floor_major, config.floor_minor, program_name(), real_release);
  }
  return ret;
}

// ---- helpers

static int (*real_ioctl)(int, unsigned long, ...);

static int is_kgsl(int fd) {
  static dev_t kgsl_dev;
  static int state;  // 0 unknown, 1 present, -1 absent
  struct stat st;
  if (!state) {
    state = stat("/dev/kgsl-3d0", &st) == 0 ? 1 : -1;
    if (state > 0) kgsl_dev = st.st_rdev;
  }
  return state > 0 && fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) && st.st_rdev == kgsl_dev;
}

static int64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int fence_ready(int fd, int timeout_ms) {
  struct pollfd p = {.fd = fd, .events = POLLIN};
  int ret;
  do {
    ret = poll(&p, 1, timeout_ms);
  } while (ret < 0 && errno == EINTR);
  // An error on the fence fd counts as signalled rather than hanging the caller.
  return ret == 1;
}

static long futex(atomic_uint *word, int op, unsigned int value, const struct timespec *timeout) {
  return syscall(SYS_futex, word, op, value, timeout, NULL, 0);
}

// ---- shared timelines

#define TIMELINE_MAGIC 0x4c44534fu  // "LDSO"
struct shared_timeline {
  uint32_t magic;
  uint32_t layout;
  _Atomic uint64_t available;  // highest point with a fence attached (or signalled)
  _Atomic uint64_t signalled;  // highest signalled point
  atomic_uint seq;             // futex word, bumped on every change
};

struct eventfd_wait {
  struct eventfd_wait *next;
  uint64_t point;
  int available;  // wait for availability only
  int fd;
};

struct timeline {
  atomic_int refs;
  int memfd;
  struct shared_timeline *shared;
  pthread_mutex_t lock;  // guards waits and notifier
  struct eventfd_wait *waits;
  int notifier_running;
  int closing;
};

static void timeline_unref(struct timeline *t) {
  if (atomic_fetch_sub(&t->refs, 1) != 1) return;
  munmap(t->shared, sizeof *t->shared);
  close(t->memfd);
  free(t);
}

static struct timeline *timeline_map(int memfd) {
  struct shared_timeline *shared =
      mmap(NULL, sizeof *shared, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
  if (shared == MAP_FAILED) return NULL;
  struct timeline *t = calloc(1, sizeof *t);
  t->refs = 1;
  t->memfd = memfd;
  t->shared = shared;
  pthread_mutex_init(&t->lock, NULL);
  return t;
}

static struct timeline *timeline_new(void) {
  int memfd = memfd_create("ld-syncobj", MFD_CLOEXEC);
  if (memfd < 0) return NULL;
  if (ftruncate(memfd, 4096)) {
    close(memfd);
    return NULL;
  }
  struct timeline *t = timeline_map(memfd);
  if (!t) {
    close(memfd);
    return NULL;
  }
  t->shared->magic = TIMELINE_MAGIC;
  t->shared->layout = 1;
  return t;
}

static void timeline_wake(struct timeline *t) {
  atomic_fetch_add(&t->shared->seq, 1);
  futex(&t->shared->seq, FUTEX_WAKE, INT_MAX, NULL);
}

static void atomic_max(_Atomic uint64_t *value, uint64_t candidate) {
  uint64_t current = atomic_load(value);
  while (candidate > current && !atomic_compare_exchange_weak(value, &current, candidate)) {
  }
}

static void timeline_raise(struct timeline *t, uint64_t point, int signalled) {
  atomic_max(&t->shared->available, point);
  if (signalled) atomic_max(&t->shared->signalled, point);
  timeline_wake(t);
}

static int timeline_reached(struct timeline *t, uint64_t point, int available) {
  return (available ? atomic_load(&t->shared->available) : atomic_load(&t->shared->signalled)) >= point;
}

// Waits until the point is reached or the absolute deadline passes; 0 if reached.
static int timeline_wait(struct timeline *t, uint64_t point, int available, int64_t deadline) {
  for (;;) {
    unsigned int seq = atomic_load(&t->shared->seq);
    if (timeline_reached(t, point, available)) return 0;
    int64_t left = deadline - now_ns();
    if (left <= 0) return -ETIME;
    struct timespec timeout = {.tv_sec = left / 1000000000LL, .tv_nsec = left % 1000000000LL};
    futex(&t->shared->seq, FUTEX_WAIT, seq, &timeout);
  }
}

// Writes the eventfds of a timeline's registrations once their points are reached.
static void *notifier_main(void *arg) {
  struct timeline *t = arg;
  pthread_mutex_lock(&t->lock);
  while (!t->closing && t->waits) {
    unsigned int seq = atomic_load(&t->shared->seq);
    for (struct eventfd_wait **w = &t->waits; *w;) {
      if (timeline_reached(t, (*w)->point, (*w)->available)) {
        struct eventfd_wait *done = *w;
        *w = done->next;
        uint64_t one = 1;
        (void)!write(done->fd, &one, sizeof one);
        close(done->fd);
        free(done);
      } else {
        w = &(*w)->next;
      }
    }
    if (!t->waits) break;
    pthread_mutex_unlock(&t->lock);
    struct timespec timeout = {.tv_sec = 1};
    futex(&t->shared->seq, FUTEX_WAIT, seq, &timeout);
    pthread_mutex_lock(&t->lock);
  }
  for (struct eventfd_wait *w = t->waits; w;) {
    struct eventfd_wait *next = w->next;
    close(w->fd);
    free(w);
    w = next;
  }
  t->waits = NULL;
  t->notifier_running = 0;
  pthread_mutex_unlock(&t->lock);
  timeline_unref(t);
  return NULL;
}

static int timeline_add_eventfd(struct timeline *t, uint64_t point, int available, int fd) {
  if (timeline_reached(t, point, available)) {
    uint64_t one = 1;
    (void)!write(fd, &one, sizeof one);
    return 0;
  }
  struct eventfd_wait *w = calloc(1, sizeof *w);
  w->point = point;
  w->available = available;
  w->fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
  if (w->fd < 0) {
    free(w);
    return -errno;
  }
  pthread_mutex_lock(&t->lock);
  w->next = t->waits;
  t->waits = w;
  if (!t->notifier_running) {
    t->notifier_running = 1;
    atomic_fetch_add(&t->refs, 1);
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attr, notifier_main, t)) {
      t->notifier_running = 0;
      atomic_fetch_sub(&t->refs, 1);
    }
    pthread_attr_destroy(&attr);
  }
  pthread_mutex_unlock(&t->lock);
  timeline_wake(t);  // let a running notifier pick the new registration up
  return 0;
}

// ---- fence watcher: marks timeline points signalled when their sync_file signals

struct fence_watch {
  struct fence_watch *next;
  int fence;
  struct timeline *timeline;
  uint64_t point;
};

static pthread_mutex_t watch_lock = PTHREAD_MUTEX_INITIALIZER;
static struct fence_watch *watches;
static int watcher_running;
static int watcher_pipe[2] = {-1, -1};

static void *watcher_main(void *arg) {
  (void)arg;
  struct pollfd fds[256];
  struct fence_watch *order[256];
  for (;;) {
    pthread_mutex_lock(&watch_lock);
    int n = 0;
    fds[n++] = (struct pollfd){.fd = watcher_pipe[0], .events = POLLIN};
    for (struct fence_watch *w = watches; w && n < 256; w = w->next) {
      order[n] = w;
      fds[n++] = (struct pollfd){.fd = w->fence, .events = POLLIN};
    }
    pthread_mutex_unlock(&watch_lock);
    int ret = poll(fds, n, -1);
    if (ret < 0 && errno != EINTR) continue;
    if (fds[0].revents & POLLIN) {
      char drain[64];
      (void)!read(watcher_pipe[0], drain, sizeof drain);
    }
    pthread_mutex_lock(&watch_lock);
    for (int i = 1; i < n; i++) {
      if (!fds[i].revents) continue;
      struct fence_watch *done = order[i];
      for (struct fence_watch **w = &watches; *w; w = &(*w)->next) {
        if (*w == done) {
          *w = done->next;
          break;
        }
      }
      timeline_raise(done->timeline, done->point, 1);
      trace("fence for point %llu signalled", (unsigned long long)done->point);
      close(done->fence);
      timeline_unref(done->timeline);
      free(done);
    }
    pthread_mutex_unlock(&watch_lock);
  }
  return NULL;
}

static void watch_fence(int fence, struct timeline *t, uint64_t point) {
  struct fence_watch *w = calloc(1, sizeof *w);
  w->fence = fence;
  w->timeline = t;
  w->point = point;
  atomic_fetch_add(&t->refs, 1);
  pthread_mutex_lock(&watch_lock);
  if (!watcher_running) {
    if (watcher_pipe[0] < 0 && pipe2(watcher_pipe, O_CLOEXEC | O_NONBLOCK)) {
      pthread_mutex_unlock(&watch_lock);
      fence_ready(fence, -1);  // no watcher: fall back to waiting here
      timeline_raise(t, point, 1);
      close(fence);
      timeline_unref(t);
      free(w);
      return;
    }
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    watcher_running = !pthread_create(&thread, &attr, watcher_main, NULL);
    pthread_attr_destroy(&attr);
  }
  w->next = watches;
  watches = w;
  pthread_mutex_unlock(&watch_lock);
  (void)!write(watcher_pipe[1], "w", 1);
}

// ---- signalled fences

static pthread_mutex_t signalled_lock = PTHREAD_MUTEX_INITIALIZER;
static int signalled_fence = -1;

// A sync_file that is already signalled: a KGSL fence event on retired timestamp 0 of a context
// of our own, made once per process and duplicated for every use.
static int new_signalled_fence(int kgsl) {
  pthread_mutex_lock(&signalled_lock);
  if (signalled_fence < 0) {
    struct kgsl_drawctxt_create create = {
        .flags = KGSL_CONTEXT_PREAMBLE | KGSL_CONTEXT_NO_GMEM_ALLOC | KGSL_CONTEXT_TYPE_GL};
    if (real_ioctl(kgsl, IOCTL_KGSL_DRAWCTXT_CREATE, &create) == 0) {
      int fd = -1;
      struct kgsl_timestamp_event event = {.type = KGSL_TIMESTAMP_EVENT_FENCE,
                                           .timestamp = 0,
                                           .context_id = create.drawctxt_id,
                                           .priv = &fd,
                                           .len = sizeof fd};
      if (real_ioctl(kgsl, IOCTL_KGSL_TIMESTAMP_EVENT, &event) == 0) {
        signalled_fence = fd;
      }
    }
    if (signalled_fence < 0) {
      fprintf(stderr, "kgsl-syncobj: cannot make a signalled fence (%s)\n", strerror(errno));
    }
  }
  int fd = signalled_fence >= 0 ? fcntl(signalled_fence, F_DUPFD_CLOEXEC, 0) : -1;
  pthread_mutex_unlock(&signalled_lock);
  return fd;
}

// ---- syncobjs

#define PAYLOAD_NONE (-1)
#define PAYLOAD_SIGNALLED (-2)
#define MAX_SYNCOBJS 4096

struct syncobj {
  uint32_t handle;  // 0 = free slot
  int payload;      // PAYLOAD_NONE, PAYLOAD_SIGNALLED or a sync_file fd
  struct timeline *timeline;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct syncobj objects[MAX_SYNCOBJS];
static uint32_t next_handle = 1;

static struct syncobj *find_locked(uint32_t handle) {
  if (!handle) return NULL;
  for (int i = 0; i < MAX_SYNCOBJS; i++) {
    if (objects[i].handle == handle) return &objects[i];
  }
  return NULL;
}

static struct syncobj *add_locked(void) {
  for (int i = 0; i < MAX_SYNCOBJS; i++) {
    if (!objects[i].handle) {
      objects[i] = (struct syncobj){.handle = next_handle++, .payload = PAYLOAD_NONE};
      if (!next_handle) next_handle = 1;
      return &objects[i];
    }
  }
  return NULL;
}

static void set_payload_locked(struct syncobj *s, int payload) {
  if (s->payload >= 0) close(s->payload);
  s->payload = payload;
}

static struct timeline *timeline_of_locked(struct syncobj *s) {
  if (!s->timeline) s->timeline = timeline_new();
  return s->timeline;
}

// Attaches a fence state (PAYLOAD_SIGNALLED or a sync_file fd, consumed) to a timeline point.
static void attach_to_point(struct timeline *t, uint64_t point, int payload) {
  if (payload == PAYLOAD_SIGNALLED || (payload >= 0 && fence_ready(payload, 0))) {
    if (payload >= 0) close(payload);
    timeline_raise(t, point, 1);
    return;
  }
  timeline_raise(t, point, 0);
  watch_fence(payload, t, point);
}

// The payload state of a timeline point, as a binary payload; waits for it to signal (up to
// `deadline`), as a fence for a point signalled in another process can't be passed here.
static int payload_of_point(struct timeline *t, uint64_t point, int64_t deadline) {
  if (timeline_wait(t, point, 0, deadline)) return PAYLOAD_NONE;
  return PAYLOAD_SIGNALLED;
}

static int handle_create(void *arg) {
  struct drm_syncobj_create *c = arg;
  pthread_mutex_lock(&lock);
  struct syncobj *s = add_locked();
  if (s && (c->flags & DRM_SYNCOBJ_CREATE_SIGNALED)) s->payload = PAYLOAD_SIGNALLED;
  if (s) c->handle = s->handle;
  pthread_mutex_unlock(&lock);
  trace("create -> %u", s ? s->handle : 0);
  return s ? 0 : -ENOMEM;
}

static int handle_destroy(void *arg) {
  struct drm_syncobj_destroy *d = arg;
  pthread_mutex_lock(&lock);
  struct syncobj *s = find_locked(d->handle);
  if (s) {
    set_payload_locked(s, PAYLOAD_NONE);
    struct timeline *t = s->timeline;
    *s = (struct syncobj){0};
    pthread_mutex_unlock(&lock);
    if (t) {
      // Each handle maps its own timeline struct, so its notifier can stop with it.
      pthread_mutex_lock(&t->lock);
      t->closing = 1;
      pthread_mutex_unlock(&t->lock);
      timeline_wake(t);
      timeline_unref(t);
    }
    trace("destroy %u", d->handle);
    return 0;
  }
  pthread_mutex_unlock(&lock);
  return -EINVAL;
}

static int handle_to_fd(int kgsl, void *arg, size_t size) {
  struct drm_syncobj_handle *h = arg;
  uint64_t point = size >= sizeof *h ? h->point : 0;
  pthread_mutex_lock(&lock);
  struct syncobj *s = find_locked(h->handle);
  if (!s) {
    pthread_mutex_unlock(&lock);
    return -EINVAL;
  }
  if (!(h->flags & DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE)) {
    struct timeline *t = timeline_of_locked(s);
    h->fd = t ? fcntl(t->memfd, F_DUPFD_CLOEXEC, 0) : -1;
    pthread_mutex_unlock(&lock);
    trace("handle %u -> timeline fd %d", h->handle, h->fd);
    return h->fd >= 0 ? 0 : -ENOMEM;
  }
  int payload;
  if ((h->flags & DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_TIMELINE) && point) {
    struct timeline *t = timeline_of_locked(s);
    if (t) atomic_fetch_add(&t->refs, 1);
    pthread_mutex_unlock(&lock);
    payload = t ? payload_of_point(t, point, now_ns() + 1000000000LL) : PAYLOAD_NONE;
    if (t) timeline_unref(t);
  } else {
    payload = s->payload >= 0 ? fcntl(s->payload, F_DUPFD_CLOEXEC, 0) : s->payload;
    pthread_mutex_unlock(&lock);
  }
  if (payload == PAYLOAD_NONE) return -EINVAL;
  h->fd = payload == PAYLOAD_SIGNALLED ? new_signalled_fence(kgsl) : payload;
  trace("handle %u point %llu -> sync_file %d", h->handle, (unsigned long long)point, h->fd);
  return h->fd >= 0 ? 0 : -ENOMEM;
}

static int is_timeline_fd(int fd) {
  char path[64], target[128];
  snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
  ssize_t n = readlink(path, target, sizeof target - 1);
  if (n <= 0) return 0;
  target[n] = 0;
  if (!strstr(target, "memfd:ld-syncobj")) return 0;
  uint32_t magic = 0;
  return pread(fd, &magic, sizeof magic, 0) == sizeof magic && magic == TIMELINE_MAGIC;
}

static int fd_to_handle(void *arg, size_t size) {
  struct drm_syncobj_handle *h = arg;
  uint64_t point = size >= sizeof *h ? h->point : 0;
  if (h->flags & DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE) {
    int fence = fcntl(h->fd, F_DUPFD_CLOEXEC, 0);
    if (fence < 0) return -errno;
    pthread_mutex_lock(&lock);
    struct syncobj *s = find_locked(h->handle);
    if (!s) {
      pthread_mutex_unlock(&lock);
      close(fence);
      return -EINVAL;
    }
    if ((h->flags & DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_TIMELINE) && point) {
      struct timeline *t = timeline_of_locked(s);
      if (t) atomic_fetch_add(&t->refs, 1);
      pthread_mutex_unlock(&lock);
      if (!t) {
        close(fence);
        return -ENOMEM;
      }
      attach_to_point(t, point, fence);
      timeline_unref(t);
    } else {
      set_payload_locked(s, fence);
      pthread_mutex_unlock(&lock);
    }
    trace("import sync_file into %u point %llu", h->handle, (unsigned long long)point);
    return 0;
  }
  if (!is_timeline_fd(h->fd)) return -EINVAL;
  int memfd = fcntl(h->fd, F_DUPFD_CLOEXEC, 0);
  if (memfd < 0) return -errno;
  struct timeline *t = timeline_map(memfd);
  if (!t) {
    close(memfd);
    return -ENOMEM;
  }
  pthread_mutex_lock(&lock);
  struct syncobj *s = add_locked();
  if (s) {
    s->timeline = t;
    h->handle = s->handle;
  }
  pthread_mutex_unlock(&lock);
  if (!s) {
    timeline_unref(t);
    return -ENOMEM;
  }
  trace("timeline fd %d -> handle %u", h->fd, h->handle);
  return 0;
}

static int transfer(void *arg) {
  struct drm_syncobj_transfer *x = arg;
  pthread_mutex_lock(&lock);
  struct syncobj *src = find_locked(x->src_handle), *dst = find_locked(x->dst_handle);
  if (!src || !dst) {
    pthread_mutex_unlock(&lock);
    return -EINVAL;
  }
  // The source's fence state, as a binary payload owned by us.
  int payload;
  if (x->src_point) {
    struct timeline *t = timeline_of_locked(src);
    if (t) atomic_fetch_add(&t->refs, 1);
    pthread_mutex_unlock(&lock);
    payload = t ? payload_of_point(t, x->src_point, now_ns() + 1000000000LL) : PAYLOAD_NONE;
    if (t) timeline_unref(t);
    pthread_mutex_lock(&lock);
    dst = find_locked(x->dst_handle);
    if (!dst) {
      pthread_mutex_unlock(&lock);
      if (payload >= 0) close(payload);
      return -EINVAL;
    }
  } else {
    payload = src->payload >= 0 ? fcntl(src->payload, F_DUPFD_CLOEXEC, 0) : src->payload;
  }
  if (payload == PAYLOAD_NONE) {
    pthread_mutex_unlock(&lock);
    trace("transfer %u:%llu -> %u:%llu: no fence", x->src_handle,
          (unsigned long long)x->src_point, x->dst_handle, (unsigned long long)x->dst_point);
    return -EINVAL;
  }
  if (x->dst_point) {
    struct timeline *t = timeline_of_locked(dst);
    if (t) atomic_fetch_add(&t->refs, 1);
    pthread_mutex_unlock(&lock);
    if (!t) {
      if (payload >= 0) close(payload);
      return -ENOMEM;
    }
    attach_to_point(t, x->dst_point, payload);
    timeline_unref(t);
  } else {
    set_payload_locked(dst, payload);
    pthread_mutex_unlock(&lock);
  }
  trace("transfer %u:%llu -> %u:%llu", x->src_handle, (unsigned long long)x->src_point,
        x->dst_handle, (unsigned long long)x->dst_point);
  return 0;
}

static int signal_array(void *arg, int reset) {
  struct drm_syncobj_array *a = arg;
  const uint32_t *handles = (const uint32_t *)(uintptr_t)a->handles;
  pthread_mutex_lock(&lock);
  for (uint32_t i = 0; i < a->count_handles; i++) {
    struct syncobj *s = find_locked(handles[i]);
    if (!s) {
      pthread_mutex_unlock(&lock);
      return -EINVAL;
    }
    set_payload_locked(s, reset ? PAYLOAD_NONE : PAYLOAD_SIGNALLED);
  }
  pthread_mutex_unlock(&lock);
  return 0;
}

static int timeline_signal(void *arg) {
  struct drm_syncobj_timeline_array *a = arg;
  const uint32_t *handles = (const uint32_t *)(uintptr_t)a->handles;
  const uint64_t *points = (const uint64_t *)(uintptr_t)a->points;
  for (uint32_t i = 0; i < a->count_handles; i++) {
    pthread_mutex_lock(&lock);
    struct syncobj *s = find_locked(handles[i]);
    if (!s) {
      pthread_mutex_unlock(&lock);
      return -EINVAL;
    }
    uint64_t point = points ? points[i] : 0;
    if (!point) {
      set_payload_locked(s, PAYLOAD_SIGNALLED);
      pthread_mutex_unlock(&lock);
      continue;
    }
    struct timeline *t = timeline_of_locked(s);
    if (t) atomic_fetch_add(&t->refs, 1);
    pthread_mutex_unlock(&lock);
    if (!t) return -ENOMEM;
    timeline_raise(t, point, 1);
    timeline_unref(t);
    trace("signal %u point %llu", handles[i], (unsigned long long)point);
  }
  return 0;
}

static int query(void *arg) {
  struct drm_syncobj_timeline_array *a = arg;
  const uint32_t *handles = (const uint32_t *)(uintptr_t)a->handles;
  uint64_t *points = (uint64_t *)(uintptr_t)a->points;
  pthread_mutex_lock(&lock);
  for (uint32_t i = 0; i < a->count_handles; i++) {
    struct syncobj *s = find_locked(handles[i]);
    if (!s) {
      pthread_mutex_unlock(&lock);
      return -EINVAL;
    }
    struct timeline *t = s->timeline;
    points[i] = !t ? 0
                : (a->flags & DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED)
                    ? atomic_load(&t->shared->available)
                    : atomic_load(&t->shared->signalled);
  }
  pthread_mutex_unlock(&lock);
  return 0;
}

// One wait condition: 1 reached, 0 not yet, <0 error.
static int wait_one(uint32_t handle, uint64_t point, uint32_t flags, int64_t deadline) {
  pthread_mutex_lock(&lock);
  struct syncobj *s = find_locked(handle);
  if (!s) {
    pthread_mutex_unlock(&lock);
    return -EINVAL;
  }
  if (point) {
    struct timeline *t = timeline_of_locked(s);
    if (t) atomic_fetch_add(&t->refs, 1);
    pthread_mutex_unlock(&lock);
    if (!t) return -ENOMEM;
    int available = (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE) != 0;
    int ret = timeline_wait(t, point, available, deadline) == 0;
    timeline_unref(t);
    return ret;
  }
  int payload = s->payload >= 0 ? fcntl(s->payload, F_DUPFD_CLOEXEC, 0) : s->payload;
  pthread_mutex_unlock(&lock);
  if (payload == PAYLOAD_SIGNALLED) return 1;
  if (payload == PAYLOAD_NONE) {
    return (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT) ? 0 : -EINVAL;
  }
  int64_t left = deadline - now_ns();
  int ready = fence_ready(payload, left > 0 ? (int)(left / 1000000) : 0);
  close(payload);
  return ready;
}

static int wait_many(const uint32_t *handles, const uint64_t *points, uint32_t count,
                     uint32_t flags, int64_t timeout, uint32_t *first_signaled) {
  int all = (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) || count == 1;
  for (;;) {
    int pending = 0;
    int64_t slice = now_ns() + 2000000;  // re-check every 2 ms while waiting on several
    int64_t deadline = (all && count == 1) ? timeout : (timeout < slice ? timeout : slice);
    for (uint32_t i = 0; i < count; i++) {
      int ret = wait_one(handles[i], points ? points[i] : 0, flags, all ? deadline : now_ns());
      if (ret < 0) return ret;
      if (ret == 1 && !all) {
        if (first_signaled) *first_signaled = i;
        return 0;
      }
      if (ret == 0) pending++;
    }
    if (!pending && all) return 0;
    if (now_ns() >= timeout) return -ETIME;
    if (!all) {
      struct timespec pause = {.tv_nsec = 1000000};
      nanosleep(&pause, NULL);
    }
  }
}

static int eventfd_register(void *arg) {
  struct drm_syncobj_eventfd *e = arg;
  pthread_mutex_lock(&lock);
  struct syncobj *s = find_locked(e->handle);
  if (!s) {
    pthread_mutex_unlock(&lock);
    return -ENOENT;
  }
  struct timeline *t = timeline_of_locked(s);
  if (t) atomic_fetch_add(&t->refs, 1);
  pthread_mutex_unlock(&lock);
  if (!t) return -ENOMEM;
  // WAIT_AVAILABLE (Chromium's release wait) also fires only once the point is signalled: the
  // caller then exports the point as a sync_file, which here would otherwise block until the
  // fence in the other process signals. Later by at most that fence, never early.
  int ret = timeline_add_eventfd(t, e->point, 0, e->fd);
  timeline_unref(t);
  trace("eventfd on %u point %llu%s", e->handle, (unsigned long long)e->point,
        (e->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE) ? " (available)" : "");
  return ret;
}

// ---- ioctl

static int emulate(int fd, unsigned long request, void *arg, int *handled) {
  *handled = 1;
  size_t size = _IOC_SIZE(request);
  switch (_IOC_NR(request)) {
    case NR_GET_CAP: {
      struct drm_get_cap *cap = arg;
      if (cap->capability != DRM_CAP_SYNCOBJ && cap->capability != DRM_CAP_SYNCOBJ_TIMELINE) break;
      cap->value = 1;
      return 0;
    }
    case NR_SYNCOBJ_CREATE: return handle_create(arg);
    case NR_SYNCOBJ_DESTROY: return handle_destroy(arg);
    case NR_SYNCOBJ_HANDLE_TO_FD: return handle_to_fd(fd, arg, size);
    case NR_SYNCOBJ_FD_TO_HANDLE: return fd_to_handle(arg, size);
    case NR_SYNCOBJ_WAIT: {
      struct drm_syncobj_wait *w = arg;
      return wait_many((const uint32_t *)(uintptr_t)w->handles, NULL, w->count_handles, w->flags,
                       w->timeout_nsec, &w->first_signaled);
    }
    case NR_SYNCOBJ_RESET: return signal_array(arg, 1);
    case NR_SYNCOBJ_SIGNAL: return signal_array(arg, 0);
    case NR_SYNCOBJ_TIMELINE_WAIT: {
      struct drm_syncobj_timeline_wait *w = arg;
      return wait_many((const uint32_t *)(uintptr_t)w->handles,
                       (const uint64_t *)(uintptr_t)w->points, w->count_handles, w->flags,
                       w->timeout_nsec, &w->first_signaled);
    }
    case NR_SYNCOBJ_QUERY: return query(arg);
    case NR_SYNCOBJ_TRANSFER: return transfer(arg);
    case NR_SYNCOBJ_TIMELINE_SIGNAL: return timeline_signal(arg);
    case NR_SYNCOBJ_EVENTFD: return eventfd_register(arg);
  }
  *handled = 0;
  return 0;
}

int ioctl(int fd, unsigned long request, ...) {
  va_list ap;
  va_start(ap, request);
  void *arg = va_arg(ap, void *);
  va_end(ap);
  if (!real_ioctl) real_ioctl = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
  if (_IOC_TYPE(request) == DRM_TYPE && arg && enabled() && is_kgsl(fd)) {
    int handled;
    int ret = emulate(fd, request, arg, &handled);
    if (handled) {
      if (ret < 0) {
        errno = -ret;
        return -1;
      }
      return 0;
    }
  }
  return real_ioctl(fd, request, arg);
}

// After fork only the forking thread exists: helper threads are gone and locks may be held.
static void reset_after_fork(void) {
  pthread_mutex_init(&lock, NULL);
  pthread_mutex_init(&watch_lock, NULL);
  pthread_mutex_init(&signalled_lock, NULL);
  watcher_running = 0;
  watches = NULL;
  for (int i = 0; i < MAX_SYNCOBJS; i++) {
    struct timeline *t = objects[i].timeline;
    if (t) {
      pthread_mutex_init(&t->lock, NULL);
      t->notifier_running = 0;
      t->waits = NULL;
    }
  }
}

__attribute__((constructor)) static void install(void) {
  pthread_atfork(NULL, NULL, reset_after_fork);
}
