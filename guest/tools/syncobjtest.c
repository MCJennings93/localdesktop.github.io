// syncobjtest: exercises kgsl-syncobj.so (DRM syncobj emulation) the way KWin and Chromium use it,
// across two processes sharing a timeline. Run inside the session (ld-run) with
//   LD_PRELOAD="$LD_PRELOAD /usr/local/lib/kgsl-syncobj.so" ./syncobjtest
// A copy named kwin_wayland also checks the uname() override.
// Build: gcc -O2 -Wall -o syncobjtest syncobjtest.c -ldrm -lEGL -lGLESv2 (-I/usr/include/libdrm)
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>

static int failures;
#define CHECK(cond, ...)                         \
  do {                                           \
    int ok_ = (cond);                            \
    printf("  %s ", ok_ ? "PASS" : "FAIL");      \
    printf(__VA_ARGS__);                         \
    printf("\n");                                \
    if (!ok_) failures++;                        \
  } while (0)

static int ready(int fd, int ms) {
  struct pollfd p = {.fd = fd, .events = POLLIN};
  return poll(&p, 1, ms) == 1;
}

static double ms_since(struct timespec *t0) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (t.tv_sec - t0->tv_sec) * 1e3 + (t.tv_nsec - t0->tv_nsec) / 1e6;
}

// A real GPU fence: about 200 full-screen clears through Zink, then an EGL native fence.
static int gpu_fence(void) {
  static EGLDisplay dpy = EGL_NO_DISPLAY;
  if (dpy == EGL_NO_DISPLAY) {
    PFNEGLGETPLATFORMDISPLAYEXTPROC get =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    dpy = get(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    eglInitialize(dpy, NULL, NULL);
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext context = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attributes);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
    GLuint fbo, rb;
    glGenFramebuffers(1, &fbo);
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGB565, 2048, 2048);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
  }
  for (int i = 0; i < 400; i++) {
    glClearColor(i / 400.0f, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
  }
  PFNEGLCREATESYNCKHRPROC create = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
  PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup_fence =
      (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress("eglDupNativeFenceFDANDROID");
  EGLSyncKHR sync = create(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
  glFlush();
  return dup_fence(dpy, sync);
}

static void send_fd(int sock, int fd) {
  char byte = 0;
  struct iovec iov = {.iov_base = &byte, .iov_len = 1};
  char control[CMSG_SPACE(sizeof(int))] = {0};
  struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control,
                       .msg_controllen = sizeof control};
  struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
  c->cmsg_level = SOL_SOCKET;
  c->cmsg_type = SCM_RIGHTS;
  c->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(c), &fd, sizeof fd);
  sendmsg(sock, &msg, 0);
}

static int recv_fd(int sock) {
  char byte;
  struct iovec iov = {.iov_base = &byte, .iov_len = 1};
  char control[CMSG_SPACE(sizeof(int))] = {0};
  struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control,
                       .msg_controllen = sizeof control};
  if (recvmsg(sock, &msg, 0) <= 0) return -1;
  int fd;
  memcpy(&fd, CMSG_DATA(CMSG_FIRSTHDR(&msg)), sizeof fd);
  return fd;
}

// The compositor side (KWin's calls): import the timeline, wait for acquire points by eventfd,
// export a point as a sync_file, check materialisation.
static int compositor(int sock) {
  int drm = open("/dev/dri/renderD0", O_RDWR | O_CLOEXEC);
  int timeline_fd = recv_fd(sock);
  uint32_t timeline = 0;
  CHECK(drmSyncobjFDToHandle(drm, timeline_fd, &timeline) == 0, "compositor: import timeline fd");

  int ev = eventfd(0, EFD_CLOEXEC);
  CHECK(drmSyncobjEventfd(drm, timeline, 1, ev, 0) == 0, "compositor: eventfd for point 1");
  char go = 1;
  write(sock, &go, 1);
  CHECK(ready(ev, 2000), "compositor: point 1 (TIMELINE_SIGNAL in the other process) woke the eventfd");

  // Point 2 gets a real GPU fence in the other process.
  int ev2 = eventfd(0, EFD_CLOEXEC);
  drmSyncobjEventfd(drm, timeline, 2, ev2, 0);
  write(sock, &go, 1);
  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  int woke = ready(ev2, 3000);
  CHECK(woke, "compositor: point 2 (GPU fence) woke the eventfd after %.1f ms", ms_since(&t0));

  uint64_t point = 2;
  CHECK(drmSyncobjTimelineWait(drm, &timeline, &point, 1, 0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE, NULL) == 0,
        "compositor: point 2 materialised");
  point = 3;
  int ret = drmSyncobjTimelineWait(drm, &timeline, &point, 1, 0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE, NULL);
  CHECK(ret != 0, "compositor: point 3 not materialised yet (%s)", ret ? strerror(-ret) : "returned 0");

  // exportSyncFile(2): transfer point -> temp binary -> sync_file.
  uint32_t temp;
  int sync_file = -1;
  drmSyncobjCreate(drm, 0, &temp);
  CHECK(drmSyncobjTransfer(drm, temp, 0, timeline, 2, 0) == 0, "compositor: transfer point 2 to binary");
  CHECK(drmSyncobjExportSyncFile(drm, temp, &sync_file) == 0 && ready(sync_file, 0),
        "compositor: exported point 2 as a signalled sync_file");
  drmSyncobjDestroy(drm, temp);

  // Blocking wait with a timeout for point 3, signalled by the other side after 100 ms.
  write(sock, &go, 1);
  clock_gettime(CLOCK_MONOTONIC, &t0);
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  int64_t deadline = now.tv_sec * 1000000000LL + now.tv_nsec + 2000000000LL;
  ret = drmSyncobjTimelineWait(drm, &timeline, &point, 1, deadline, 0, NULL);
  double waited = ms_since(&t0);
  CHECK(ret == 0 && waited > 50, "compositor: blocking wait for point 3 returned after %.1f ms", waited);

  // Release: signal point 4 for the producer.
  uint64_t release = 4;
  CHECK(drmSyncobjTimelineSignal(drm, &timeline, &release, 1) == 0, "compositor: signal release point 4");
  drmSyncobjDestroy(drm, timeline);
  return failures;
}

static int producer(int sock) {
  int drm = open("/dev/dri/renderD0", O_RDWR | O_CLOEXEC);
  uint64_t cap = 0;
  CHECK(drmGetCap(drm, DRM_CAP_SYNCOBJ_TIMELINE, &cap) == 0 && cap == 1,
        "producer: DRM_CAP_SYNCOBJ_TIMELINE = %llu", (unsigned long long)cap);
  uint32_t timeline;
  int timeline_fd = -1;
  CHECK(drmSyncobjCreate(drm, 0, &timeline) == 0, "producer: create timeline");
  CHECK(drmSyncobjHandleToFD(drm, timeline, &timeline_fd) == 0, "producer: export timeline fd");
  int ev = eventfd(0, EFD_CLOEXEC);
  drmSyncobjEventfd(drm, timeline, 4, ev, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE);
  send_fd(sock, timeline_fd);

  char go;
  read(sock, &go, 1);
  uint64_t point = 1;
  CHECK(drmSyncobjTimelineSignal(drm, &timeline, &point, 1) == 0, "producer: signal point 1");

  read(sock, &go, 1);
  int fence = gpu_fence();
  printf("  info producer: GPU fence %d, signalled at import time: %s\n", fence,
         ready(fence, 0) ? "yes" : "no (good: the wait below is real)");
  uint32_t temp;
  drmSyncobjCreate(drm, 0, &temp);
  CHECK(drmSyncobjImportSyncFile(drm, temp, fence) == 0, "producer: import GPU fence");
  CHECK(drmSyncobjTransfer(drm, timeline, 2, temp, 0, 0) == 0, "producer: transfer fence to point 2");
  drmSyncobjDestroy(drm, temp);
  close(fence);

  read(sock, &go, 1);
  usleep(100000);
  point = 3;
  drmSyncobjTimelineSignal(drm, &timeline, &point, 1);

  CHECK(ready(ev, 3000), "producer: release point 4 (WAIT_AVAILABLE eventfd) fired");
  drmSyncobjDestroy(drm, timeline);
  return failures;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  struct utsname name;
  uname(&name);
  printf("== uname release as seen by %s: %s\n", program_invocation_short_name, name.release);
  int pair[2];
  socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair);
  pid_t child = fork();
  if (child == 0) {
    printf("== compositor process %d\n", (int)getpid());
    _exit(compositor(pair[1]));
  }
  printf("== producer process %d\n", (int)getpid());
  int failed = producer(pair[0]);
  int status = 0;
  waitpid(child, &status, 0);
  failed += WIFEXITED(status) ? WEXITSTATUS(status) : 100;
  printf("== %s (%d failures)\n", failed ? "FAILED" : "ALL PASSED", failed);
  return failed != 0;
}
