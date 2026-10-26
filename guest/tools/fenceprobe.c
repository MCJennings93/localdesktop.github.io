// fenceprobe: checks the GPU-fence facilities explicit sync (wp_linux_drm_syncobj emulation) needs.
//  1. KGSL: can a sync_file be made for an already-retired timestamp, i.e. an already signalled
//     fence, without the kernel's sw_sync debug interface? (IOCTL_KGSL_TIMESTAMP_EVENT, FENCE)
//  2. sync_file basics on this kernel: poll() readiness, SYNC_IOC_MERGE, SYNC_IOC_FILE_INFO.
//  3. EGL (Zink on Turnip): EGL_ANDROID_native_fence_sync, the source of Chromium's GPU fences;
//     does eglDupNativeFenceFDANDROID give a real sync_file that signals when the GPU is done?
// Run inside the session (ld-run), where the app's SELinux domain may use the GPU.
// Build: gcc -O2 -Wall -o fenceprobe fenceprobe.c -lEGL -lGLESv2
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/sync_file.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define KGSL_IOC_TYPE 0x09
struct kgsl_drawctxt_create {
  unsigned int flags;
  unsigned int drawctxt_id;
};
struct kgsl_drawctxt_destroy {
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
#define IOCTL_KGSL_DRAWCTXT_DESTROY _IOW(KGSL_IOC_TYPE, 0x14, struct kgsl_drawctxt_destroy)
#define IOCTL_KGSL_TIMESTAMP_EVENT _IOWR(KGSL_IOC_TYPE, 0x33, struct kgsl_timestamp_event)
#define KGSL_TIMESTAMP_EVENT_FENCE 2
#define KGSL_CONTEXT_NO_GMEM_ALLOC 0x00000002
#define KGSL_CONTEXT_PREAMBLE 0x00000010
#define KGSL_CONTEXT_TYPE_GL (1 << 20)

static const char *describe_fd(int fd) {
  static char target[256];
  char path[64];
  snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
  ssize_t n = readlink(path, target, sizeof target - 1);
  target[n > 0 ? n : 0] = 0;
  return target;
}

static int ready(int fd, int timeout_ms) {
  struct pollfd p = {.fd = fd, .events = POLLIN};
  return poll(&p, 1, timeout_ms) == 1 && (p.revents & POLLIN);
}

static void file_info(const char *label, int fd) {
  struct sync_file_info info = {0};
  int ret = ioctl(fd, SYNC_IOC_FILE_INFO, &info);
  printf("  %s: SYNC_IOC_FILE_INFO %s, status=%d, fences=%u, name='%s'\n", label,
         ret ? strerror(errno) : "ok", info.status, info.num_fences, info.name);
}

static int kgsl_fence(int kgsl, unsigned int context, unsigned int timestamp) {
  int fd = -1;
  struct kgsl_timestamp_event event = {.type = KGSL_TIMESTAMP_EVENT_FENCE,
                                       .timestamp = timestamp,
                                       .context_id = context,
                                       .priv = &fd,
                                       .len = sizeof fd};
  if (ioctl(kgsl, IOCTL_KGSL_TIMESTAMP_EVENT, &event)) {
    printf("  IOCTL_KGSL_TIMESTAMP_EVENT(ts %u): %s\n", timestamp, strerror(errno));
    return -1;
  }
  return fd;
}

static void probe_kgsl(void) {
  printf("== 1. KGSL signalled fence\n");
  int kgsl = open("/dev/kgsl-3d0", O_RDWR | O_CLOEXEC);
  if (kgsl < 0) {
    printf("  open /dev/kgsl-3d0: %s\n", strerror(errno));
    return;
  }
  struct kgsl_drawctxt_create create = {
      .flags = KGSL_CONTEXT_PREAMBLE | KGSL_CONTEXT_NO_GMEM_ALLOC | KGSL_CONTEXT_TYPE_GL};
  if (ioctl(kgsl, IOCTL_KGSL_DRAWCTXT_CREATE, &create)) {
    printf("  IOCTL_KGSL_DRAWCTXT_CREATE: %s\n", strerror(errno));
    return;
  }
  printf("  context %u created\n", create.drawctxt_id);
  int a = kgsl_fence(kgsl, create.drawctxt_id, 0);
  if (a >= 0) {
    printf("  fence for timestamp 0: fd %d -> %s, ready now: %s\n", a, describe_fd(a),
           ready(a, 0) ? "YES (signalled)" : "no");
    file_info("timestamp-0 fence", a);
    int b = dup(a);
    struct sync_merge_data merge = {.name = "probe merge", .fd2 = b, .fence = -1};
    int ret = ioctl(a, SYNC_IOC_MERGE, &merge);
    printf("  SYNC_IOC_MERGE: %s, merged fd ready: %s\n", ret ? strerror(errno) : "ok",
           ret ? "-" : (ready(merge.fence, 0) ? "yes" : "no"));
    close(b);
    if (!ret) close(merge.fence);
    close(a);
  }
  int c = kgsl_fence(kgsl, create.drawctxt_id, 1000);
  if (c >= 0) {
    printf("  fence for future timestamp 1000: ready within 50 ms: %s (expected: no)\n",
           ready(c, 50) ? "yes" : "no");
    close(c);
  }
  struct kgsl_drawctxt_destroy destroy = {.drawctxt_id = create.drawctxt_id};
  ioctl(kgsl, IOCTL_KGSL_DRAWCTXT_DESTROY, &destroy);
  close(kgsl);
}

static void probe_egl(void) {
  printf("== 3. EGL native fences (Zink)\n");
  PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
  EGLDisplay dpy = get_platform_display
                       ? get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL)
                       : EGL_NO_DISPLAY;
  if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) {
    printf("  surfaceless EGL display unavailable (0x%x)\n", eglGetError());
    return;
  }
  const char *extensions = eglQueryString(dpy, EGL_EXTENSIONS);
  int native = strstr(extensions, "EGL_ANDROID_native_fence_sync") != NULL;
  printf("  EGL_ANDROID_native_fence_sync: %s\n", native ? "YES" : "no");
  printf("  EGL_KHR_fence_sync: %s, EGL_KHR_wait_sync: %s\n",
         strstr(extensions, "EGL_KHR_fence_sync") ? "yes" : "no",
         strstr(extensions, "EGL_KHR_wait_sync") ? "yes" : "no");
  eglBindAPI(EGL_OPENGL_ES_API);
  EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLContext context = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, context_attributes);
  if (context == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
    printf("  no surfaceless context (0x%x)\n", eglGetError());
    return;
  }
  printf("  renderer: %s\n", (const char *)glGetString(GL_RENDERER));
  if (!native) return;
  PFNEGLCREATESYNCKHRPROC create_sync = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
  PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup_fence =
      (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress("eglDupNativeFenceFDANDROID");
  // Some GPU work: clear a renderbuffer-backed framebuffer many times.
  GLuint fbo, rb;
  glGenFramebuffers(1, &fbo);
  glGenRenderbuffers(1, &rb);
  glBindRenderbuffer(GL_RENDERBUFFER, rb);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_RGB565, 2048, 2048);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
  for (int i = 0; i < 200; i++) {
    glClearColor(i / 200.0f, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
  }
  EGLSyncKHR sync = create_sync(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
  glFlush();
  int fd = sync != EGL_NO_SYNC_KHR ? dup_fence(dpy, sync) : -1;
  printf("  eglDupNativeFenceFDANDROID: fd %d -> %s\n", fd, fd >= 0 ? describe_fd(fd) : "-");
  if (fd < 0) return;
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  int was_ready = ready(fd, 0);
  int done = ready(fd, 2000);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  printf("  ready immediately: %s; signalled within 2 s: %s (after %.2f ms)\n",
         was_ready ? "yes" : "no", done ? "YES" : "no",
         (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
  file_info("EGL fence", fd);
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  probe_kgsl();
  printf("== 2. (sync_file basics are reported with the fences above)\n");
  probe_egl();
  return 0;
}
