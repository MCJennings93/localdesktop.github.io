// dmabuf-trace: diagnostic LD_PRELOAD (not shipped) that shows how a program allocates GPU buffers
// and how its CPU writes to them are bracketed, to chase Chromium's green video frames (decoded
// frames copied into dmabufs by the CPU, then sampled by the GPU). Logs to stderr, tagged with the
// pid and Chromium's process --type:
//   gbm   every gbm_bo_create* / gbm_bo_import (format, size, modifiers, usage)
//   mmap  every CPU mapping of a dmabuf (size, protection, exporter from /proc/self/fdinfo)
//   sync  every 2 s per process: DMA_BUF_IOCTL_SYNC counts by START/END and READ/WRITE, and
//         mappings of dmabufs that never saw a SYNC
// Build: gcc -O2 -Wall -fPIC -shared -o dmabuf-trace.so dmabuf-trace.c -ldl -lpthread
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
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
#include <time.h>
#include <unistd.h>

struct gbm_device;
struct gbm_bo;

static char tag[64];

static const char *process_tag(void) {
  if (!tag[0]) {
    char cmdline[4096] = {0};
    int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    ssize_t n = fd >= 0 ? read(fd, cmdline, sizeof cmdline - 1) : 0;
    if (fd >= 0) close(fd);
    const char *type = "main";
    for (ssize_t i = 0; i < n; i += strlen(cmdline + i) + 1) {
      if (!strncmp(cmdline + i, "--type=", 7)) type = cmdline + i + 7;
    }
    snprintf(tag, sizeof tag, "%d:%.40s", (int)getpid(), type);
  }
  return tag;
}

static void logf_(const char *format, ...) {
  char line[512];
  va_list ap;
  va_start(ap, format);
  vsnprintf(line, sizeof line, format, ap);
  va_end(ap);
  fprintf(stderr, "dmabuf-trace[%s] %s\n", process_tag(), line);
}

static int is_dmabuf(int fd, char *exporter, size_t size) {
  char path[64], target[128];
  snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
  ssize_t n = readlink(path, target, sizeof target - 1);
  if (n <= 0) return 0;
  target[n] = 0;
  if (!strstr(target, "dmabuf")) return 0;
  if (exporter) {
    snprintf(exporter, size, "?");
    snprintf(path, sizeof path, "/proc/self/fdinfo/%d", fd);
    FILE *info = fopen(path, "re");
    if (info) {
      char line[128];
      while (fgets(line, sizeof line, info)) {
        if (!strncmp(line, "exp_name:", 9)) {
          char *v = line + 9;
          while (*v == ' ' || *v == '\t') v++;
          v[strcspn(v, "\n")] = 0;
          snprintf(exporter, size, "%s", v);
        }
      }
      fclose(info);
    }
  }
  return 1;
}

static const char *fourcc(uint32_t f, char out[5]) {
  for (int i = 0; i < 4; i++) out[i] = (char)((f >> (8 * i)) & 0xff);
  out[4] = 0;
  return out;
}

// ---- gbm


struct gbm_bo *gbm_bo_create(struct gbm_device *gbm, uint32_t w, uint32_t h, uint32_t format,
                             uint32_t flags) {
  static struct gbm_bo *(*real)(struct gbm_device *, uint32_t, uint32_t, uint32_t, uint32_t);
  if (!real) real = dlsym(RTLD_NEXT, "gbm_bo_create");
  char f[5];
  logf_("gbm create %ux%u %s usage=0x%x", w, h, fourcc(format, f), flags);
  return real(gbm, w, h, format, flags);
}

struct gbm_bo *gbm_bo_create_with_modifiers2(struct gbm_device *gbm, uint32_t w, uint32_t h,
                                             uint32_t format, const uint64_t *modifiers,
                                             unsigned int count, uint32_t flags) {
  static struct gbm_bo *(*real)(struct gbm_device *, uint32_t, uint32_t, uint32_t,
                                const uint64_t *, unsigned int, uint32_t);
  if (!real) real = dlsym(RTLD_NEXT, "gbm_bo_create_with_modifiers2");
  char f[5];
  logf_("gbm create_with_modifiers2 %ux%u %s usage=0x%x modifiers=%u first=0x%llx", w, h,
        fourcc(format, f), flags, count, count ? (unsigned long long)modifiers[0] : 0ULL);
  return real(gbm, w, h, format, modifiers, count, flags);
}

struct gbm_bo *gbm_bo_create_with_modifiers(struct gbm_device *gbm, uint32_t w, uint32_t h,
                                            uint32_t format, const uint64_t *modifiers,
                                            unsigned int count) {
  static struct gbm_bo *(*real)(struct gbm_device *, uint32_t, uint32_t, uint32_t,
                                const uint64_t *, unsigned int);
  if (!real) real = dlsym(RTLD_NEXT, "gbm_bo_create_with_modifiers");
  char f[5];
  logf_("gbm create_with_modifiers %ux%u %s modifiers=%u first=0x%llx", w, h, fourcc(format, f),
        count, count ? (unsigned long long)modifiers[0] : 0ULL);
  return real(gbm, w, h, format, modifiers, count);
}

struct gbm_bo *gbm_bo_import(struct gbm_device *gbm, uint32_t type, void *buffer, uint32_t usage) {
  static struct gbm_bo *(*real)(struct gbm_device *, uint32_t, void *, uint32_t);
  if (!real) real = dlsym(RTLD_NEXT, "gbm_bo_import");
  logf_("gbm import type=0x%x usage=0x%x", type, usage);
  return real(gbm, type, buffer, usage);
}

// ---- mmap and DMA_BUF_IOCTL_SYNC

#define MAX_TRACKED 256
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
  int fd;
  int synced;
} mapped[MAX_TRACKED];
static atomic_ulong sync_counts[4];  // [start/end][read-only/write]
static atomic_ulong unsynced_maps;
static atomic_long last_report;

static void report(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  long now = ts.tv_sec;
  long last = atomic_load(&last_report);
  if (now - last < 2 || !atomic_compare_exchange_strong(&last_report, &last, now)) return;
  unsigned long counts[4];
  for (int i = 0; i < 4; i++) counts[i] = atomic_exchange(&sync_counts[i], 0);
  if (counts[0] + counts[1] + counts[2] + counts[3] == 0) return;
  logf_("sync start(read=%lu write=%lu) end(read=%lu write=%lu); maps never synced so far=%lu",
        counts[0], counts[1], counts[2], counts[3], atomic_load(&unsynced_maps));
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
  static void *(*real)(void *, size_t, int, int, int, off_t);
  if (!real) real = dlsym(RTLD_NEXT, "mmap");
  void *ret = real(addr, length, prot, flags, fd, offset);
  char exporter[64];
  if (fd >= 0 && ret != MAP_FAILED && is_dmabuf(fd, exporter, sizeof exporter)) {
    logf_("mmap dmabuf fd=%d size=%zu prot=%s%s exporter=%s", fd, length,
          (prot & PROT_READ) ? "r" : "-", (prot & PROT_WRITE) ? "w" : "-", exporter);
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_TRACKED; i++) {
      if (!mapped[i].fd) {
        mapped[i].fd = fd + 1;
        mapped[i].synced = 0;
        break;
      }
    }
    pthread_mutex_unlock(&lock);
    atomic_fetch_add(&unsynced_maps, 1);
  }
  return ret;
}

int ioctl(int fd, unsigned long request, ...) {
  va_list ap;
  va_start(ap, request);
  void *arg = va_arg(ap, void *);
  va_end(ap);
  static int (*real)(int, unsigned long, ...);
  if (!real) real = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
  if (request == DMA_BUF_IOCTL_SYNC && arg) {
    uint64_t flags = ((struct dma_buf_sync *)arg)->flags;
    int end = (flags & DMA_BUF_SYNC_END) != 0;
    int write = (flags & DMA_BUF_SYNC_WRITE) != 0;
    atomic_fetch_add(&sync_counts[end * 2 + write], 1);
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_TRACKED; i++) {
      if (mapped[i].fd == fd + 1 && !mapped[i].synced) {
        mapped[i].synced = 1;
        atomic_fetch_sub(&unsynced_maps, 1);
      }
    }
    pthread_mutex_unlock(&lock);
    report();
  }
  return real(fd, request, arg);
}
