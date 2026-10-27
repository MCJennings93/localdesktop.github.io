// xctxtest: does a texture upload in one GL context reach another context that samples it right
// after? Chromium's GPU process works this way for video: the planes of each frame are uploaded
// into new textures in one context and drawn by the compositor in another, ordered by glFlush or
// a GL fence (sync token). If the driver lets the drawing context run first, it samples a texture
// that is still empty: KGSL memory is zeroed, so a chroma plane reads as zero and the frame turns
// green (src/android/backend/wayland/frame_stats.rs). Surfaceless EGL, two contexts in one share
// group, one thread (as in Chromium's GPU main thread).
// Usage: xctxtest <mode> [iterations, default 2000] [texture size, default 960]
//   mode: flush (glFlush in the uploader, as on a context switch), fence (glFenceSync + glFlush,
//   glWaitSync in the drawer), finish (glFinish in the uploader: the control).
// Prints how many draws saw the texture still empty. Run gpuload alongside to add GPU load.
// Build: gcc -O2 -Wall -o xctxtest xctxtest.c -lEGL -lGLESv2
#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GLuint program(void) {
  const char *vs = "#version 300 es\nin vec2 p; out vec2 uv;"
                   "void main() { uv = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }";
  const char *fs = "#version 300 es\nprecision highp float; in vec2 uv; uniform sampler2D t;"
                   "out vec4 c; void main() { c = texture(t, uv); }";
  GLuint p = glCreateProgram();
  const char *src[2] = {vs, fs};
  GLenum type[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
  for (int i = 0; i < 2; i++) {
    GLuint s = glCreateShader(type[i]);
    glShaderSource(s, 1, &src[i], NULL);
    glCompileShader(s);
    glAttachShader(p, s);
  }
  glBindAttribLocation(p, 0, "p");
  glLinkProgram(p);
  return p;
}

int main(int argc, char **argv) {
  const char *mode = argc > 1 ? argv[1] : "flush";
  int iterations = argc > 2 ? atoi(argv[2]) : 2000;
  int size = argc > 3 ? atoi(argv[3]) : 960;
  EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
  if (!eglInitialize(dpy, NULL, NULL)) return fprintf(stderr, "xctxtest: no display\n"), 1;
  eglBindAPI(EGL_OPENGL_ES_API);
  const EGLint attribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
  EGLContext upload = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attribs);
  EGLContext draw = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, upload, attribs);
  if (upload == EGL_NO_CONTEXT || draw == EGL_NO_CONTEXT) return fprintf(stderr, "xctxtest: no context\n"), 1;

  // The drawer's target and program.
  eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, draw);
  printf("xctxtest: %s, mode %s, %d iterations, %dx%d\n", glGetString(GL_RENDERER), mode,
         iterations, size, size);
  GLuint target, fbo;
  glGenTextures(1, &target);
  glBindTexture(GL_TEXTURE_2D, target);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
  GLuint prog = program();
  const GLfloat quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
  GLuint vbo;
  glGenBuffers(1, &vbo);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);

  // A chroma-like plane: two bytes per pixel, never zero.
  unsigned char *plane = malloc((size_t)size * size * 2);
  memset(plane, 128, (size_t)size * size * 2);
  int empty = 0, partial = 0;
  for (int i = 0; i < iterations; i++) {
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, upload);
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, size, size, 0, GL_RG, GL_UNSIGNED_BYTE, plane);
    GLsync sync = NULL;
    if (!strcmp(mode, "fence")) {
      sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
      glFlush();
    } else if (!strcmp(mode, "finish")) {
      glFinish();
    } else {
      glFlush();
    }

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, draw);
    if (sync) glWaitSync(sync, 0, GL_TIMEOUT_IGNORED);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, 64, 64);
    glUseProgram(prog);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    unsigned char px[64 * 64 * 4];
    glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, px);
    int zeros = 0;
    for (int k = 0; k < 64 * 64; k++) zeros += px[k * 4] == 0;
    if (zeros == 64 * 64) empty++;
    else if (zeros) partial++;
    if (sync) glDeleteSync(sync);
    glDeleteTextures(1, &tex);
  }
  printf("xctxtest: %s: %d of %d draws saw an empty texture, %d a partly filled one\n", mode, empty,
         iterations, partial);
  return 0;
}
