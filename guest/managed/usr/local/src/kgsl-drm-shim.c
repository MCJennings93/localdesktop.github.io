/* KGSL-as-DRM shim for Local Desktop. drm.c is from DroidDeck (GPL-3.0),
 * tools/linuxfs/preload/drm.c at 8ebba02; the ioctl wrapper below replaces the hook
 * DroidDeck routes through its libblsession. /dev/kgsl-3d0 is bound at /dev/dri/renderD0,
 * so Mesa and KWin open it as a render node; KGSL has no GEM, so PRIME handles are
 * emulated as tokens for dma-buf fds. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <unistd.h>

#define MAX_HANDLES 4096

struct gem_close {
  uint32_t handle;
  uint32_t pad;
};
#define GEM_CLOSE _IOW('d', 0x09, struct gem_close)

struct drm_version {
  int version_major;
  int version_minor;
  int version_patchlevel;
  size_t name_len;
  char *name;
  size_t date_len;
  char *date;
  size_t desc_len;
  char *desc;
};
#define DRM_VERSION _IOWR('d', 0x00, struct drm_version)

struct handle_entry {
  uint32_t handle;
  int dma_fd;
  int drm_fd;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct handle_entry handle_entries[MAX_HANDLES];
static uint32_t next_handle = 1;
static dev_t kgsl_dev;
static int kgsl_state;

static int is_kgsl(int fd) {
  struct stat st;
  pthread_mutex_lock(&lock);
  if (kgsl_state == 0) {
    kgsl_state = stat("/dev/kgsl-3d0", &st) == 0 ? 1 : -1;
    kgsl_dev = st.st_rdev;
  }
  pthread_mutex_unlock(&lock);
  return kgsl_state > 0 && fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) && st.st_rdev == kgsl_dev;
}

static void *real(const char *name) {
  void *fn = dlsym(RTLD_NEXT, name);
  if (!fn) {
    errno = ENOSYS;
  }
  return fn;
}

static int same_file_description(int a, int b) {
  if (a == b) {
    return 1;
  }
#ifdef SYS_kcmp
  int saved_errno = errno;
  long ret = syscall(SYS_kcmp, getpid(), getpid(), 0  , a, b);
  errno = saved_errno;
  return ret == 0;
#else
  return 0;
#endif
}

static int find_handle_locked(int fd, uint32_t handle) {
  for (int i = 0; i < MAX_HANDLES; i++) {
    if (handle_entries[i].handle == handle &&
        same_file_description(handle_entries[i].drm_fd, fd)) {
      return i;
    }
  }
  return -1;
}

static uint32_t alloc_handle_locked(void) {
  for (int tries = 0; tries <= MAX_HANDLES; tries++) {
    uint32_t candidate = next_handle++;
    if (next_handle == 0) {
      next_handle = 1;
    }
    if (candidate == 0) {
      continue;
    }
    int used = 0;
    for (int i = 0; i < MAX_HANDLES; i++) {
      if (handle_entries[i].handle == candidate) {
        used = 1;
        break;
      }
    }
    if (!used) {
      return candidate;
    }
  }
  return 0;
}

int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle) {
  if (!is_kgsl(fd)) {
    int (*fn)(int, int, uint32_t *) = (int (*)(int, int, uint32_t *)) real("drmPrimeFDToHandle");
    return fn ? fn(fd, prime_fd, handle) : -ENOSYS;
  }
  int dup_fd = fcntl(prime_fd, F_DUPFD_CLOEXEC, 0);
  if (dup_fd < 0) {
    return -errno;
  }
  int ret = -ENOMEM;
  pthread_mutex_lock(&lock);
  for (int i = 0; i < MAX_HANDLES; i++) {
    if (handle_entries[i].handle == 0) {
      uint32_t token = alloc_handle_locked();
      if (!token) {
        break;
      }
      handle_entries[i].handle = token;
      handle_entries[i].dma_fd = dup_fd;
      handle_entries[i].drm_fd = fd;
      *handle = token;
      ret = 0;
      break;
    }
  }
  pthread_mutex_unlock(&lock);
  if (ret) {
    close(dup_fd);
  }
  return ret;
}

int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd) {
  if (!is_kgsl(fd)) {
    int (*fn)(int, uint32_t, uint32_t, int *) = (int (*)(int, uint32_t, uint32_t, int *)) real("drmPrimeHandleToFD");
    return fn ? fn(fd, handle, flags, prime_fd) : -ENOSYS;
  }
  int ret = -EINVAL;
  pthread_mutex_lock(&lock);
  int index = find_handle_locked(fd, handle);
  if (index >= 0) {
    *prime_fd = fcntl(handle_entries[index].dma_fd, F_DUPFD_CLOEXEC, 0);
    ret = *prime_fd < 0 ? -errno : 0;
  }
  pthread_mutex_unlock(&lock);
  return ret;
}

static int release_handle(int fd, uint32_t handle) {
  int found = 0;
  pthread_mutex_lock(&lock);
  int index = find_handle_locked(fd, handle);
  if (index >= 0) {
    close(handle_entries[index].dma_fd);
    handle_entries[index] = (struct handle_entry){0};
    found = 1;
  }
  pthread_mutex_unlock(&lock);
  return found;
}

int drmCloseBufferHandle(int fd, uint32_t handle) {
  if (!is_kgsl(fd)) {
    int (*fn)(int, uint32_t) = (int (*)(int, uint32_t)) real("drmCloseBufferHandle");
    return fn ? fn(fd, handle) : -ENOSYS;
  }
  return release_handle(fd, handle) ? 0 : -EINVAL;
}

__attribute__((visibility("hidden"))) int bl_drm_gem_close(int fd, unsigned long request, void *arg, int *rc) {
  if (request != GEM_CLOSE || !arg || !is_kgsl(fd)) {
    return 0;
  }
  if (release_handle(fd, ((struct gem_close *) arg)->handle)) {
    *rc = 0;
  } else {
    errno = EINVAL;
    *rc = -1;
  }
  return 1;
}

/* KGSL rejects DRM_IOCTL_VERSION, and Chromium's Wayland GPU process aborts on a render
 * node that won't report one (drm_render_node_handle.cc: "Can't get version for device").
 * libdrm asks twice, first for the string lengths, then with buffers; answer both. */
static void copy_version_string(const char *value, size_t *len, char *buf) {
  size_t n = strlen(value);
  if (buf && *len) {
    memcpy(buf, value, *len < n ? *len : n);
  }
  *len = n;
}

static int drm_version(int fd, unsigned long request, void *arg, int *rc) {
  if (request != DRM_VERSION || !arg || !is_kgsl(fd)) {
    return 0;
  }
  struct drm_version *v = arg;
  v->version_major = 1;
  v->version_minor = 0;
  v->version_patchlevel = 0;
  copy_version_string("kgsl", &v->name_len, v->name);
  copy_version_string("20260101", &v->date_len, v->date);
  copy_version_string("Qualcomm KGSL (Local Desktop shim)", &v->desc_len, v->desc);
  *rc = 0;
  return 1;
}

/* Turnip-KGSL allocates every shareable buffer (KWin's output buffers among them) from
 * /dev/dma_heap/system. Android's Adreno driver, in the Local Desktop app, can texture from
 * these buffers without copying them, but only when their last 8 KiB hold the metadata block it
 * keeps there in its own exports; the app writes that block (smithay patch,
 * GlesRenderer::import_dmabuf_shared). Give every allocation a page-aligned size plus those
 * 8 KiB, which nothing in the guest uses: buffers are described by offset and stride. */
struct dma_heap_alloc {
  uint64_t len;
  uint32_t fd;
  uint32_t fd_flags;
  uint64_t heap_flags;
};
#define DMA_HEAP_ALLOC _IOWR('H', 0x0, struct dma_heap_alloc)
#define ADRENO_METADATA_AREA 8192

static int is_dma_heap(int fd) {
  static int state;
  static dev_t heap_dev;
  struct stat st;
  if (state == 0) {
    state = stat("/dev/dma_heap/system", &st) == 0 ? 1 : -1;
    heap_dev = st.st_rdev;
  }
  /* All heaps share one major number. */
  return state > 0 && fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) && major(st.st_rdev) == major(heap_dev);
}

static void pad_dma_heap_alloc(int fd, unsigned long request, void *arg) {
  if (request == DMA_HEAP_ALLOC && arg && is_dma_heap(fd)) {
    struct dma_heap_alloc *alloc = arg;
    alloc->len = ((alloc->len + 4095) & ~(uint64_t) 4095) + ADRENO_METADATA_AREA;
  }
}

static void drm_lock_before_fork(void) { pthread_mutex_lock(&lock); }
static void drm_unlock_after_fork(void) { pthread_mutex_unlock(&lock); }
static void drm_reset_after_fork(void) { pthread_mutex_init(&lock, NULL); }

__attribute__((constructor)) static void install_drm_fork_handlers(void) {
  pthread_atfork(drm_lock_before_fork, drm_unlock_after_fork, drm_reset_after_fork);
}

#include <stdarg.h>
int ioctl(int fd, unsigned long request, ...) {
  va_list ap;
  va_start(ap, request);
  void *arg = va_arg(ap, void *);
  va_end(ap);
  int rc;
  if (bl_drm_gem_close(fd, request, arg, &rc) || drm_version(fd, request, arg, &rc)) {
    return rc;
  }
  pad_dma_heap_alloc(fd, request, arg);
  static int (*fn)(int, unsigned long, ...);
  if (!fn) {
    fn = (int (*)(int, unsigned long, ...)) dlsym(RTLD_NEXT, "ioctl");
  }
  return fn(fd, request, arg);
}
