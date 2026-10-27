// gpuload: keeps the GPU busy without a window, to test whether other GPU work (as when Android
// draws an overlay on the same Adreno) changes how a client renders. Surfaceless EGL (Zink on
// Turnip in the session), a heavy fragment shader drawn into an offscreen framebuffer in a loop.
// Usage: gpuload <seconds> [loop count per pixel, default 200] [busy ms per 10 ms, default 10]
// The last argument makes it bursty: e.g. 3 keeps the GPU busy ~3 ms in every 10 ms.
// Build: gcc -O2 -Wall -o gpuload gpuload.c -lEGL -lGLESv2
#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}

static GLuint shader(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512];
    glGetShaderInfoLog(s, sizeof log, NULL, log);
    fprintf(stderr, "gpuload: shader: %s\n", log);
    exit(1);
  }
  return s;
}

int main(int argc, char **argv) {
  double seconds = argc > 1 ? atof(argv[1]) : 30;
  int loops = argc > 2 ? atoi(argv[2]) : 200;
  double busy = (argc > 3 ? atof(argv[3]) : 10) / 1000;
  EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
  if (!eglInitialize(dpy, NULL, NULL)) {
    fprintf(stderr, "gpuload: eglInitialize failed\n");
    return 1;
  }
  eglBindAPI(EGL_OPENGL_ES_API);
  const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attribs);
  if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
    fprintf(stderr, "gpuload: no context\n");
    return 1;
  }
  printf("gpuload: %s, %d loops, busy %.0f ms per 10 ms\n", glGetString(GL_RENDERER), loops,
         busy * 1000);
  fflush(stdout);

  GLuint tex, fbo;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1024, 1024, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  glViewport(0, 0, 1024, 1024);

  const char *vs = "attribute vec2 p; void main() { gl_Position = vec4(p, 0.0, 1.0); }";
  char fs[512];
  snprintf(fs, sizeof fs,
           "precision highp float; uniform float t;"
           "void main() { vec2 v = gl_FragCoord.xy / 1024.0; float a = t;"
           "for (int i = 0; i < %d; i++) { a = fract(sin(dot(v, vec2(12.9898, 78.233)) + a) * 43758.5453); v += a; }"
           "gl_FragColor = vec4(a); }",
           loops);
  GLuint prog = glCreateProgram();
  glAttachShader(prog, shader(GL_VERTEX_SHADER, vs));
  glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs));
  glBindAttribLocation(prog, 0, "p");
  glLinkProgram(prog);
  glUseProgram(prog);
  GLint t_loc = glGetUniformLocation(prog, "t");
  const GLfloat quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
  glEnableVertexAttribArray(0);

  double start = now(), end = start + seconds;
  long draws = 0;
  while (now() < end) {
    double period = now();
    while (now() - period < busy) {
      glUniform1f(t_loc, (float)draws);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
      glFinish();
      draws++;
    }
    double rest = 0.010 - (now() - period);
    if (rest > 0) usleep((useconds_t)(rest * 1e6));
  }
  printf("gpuload: %ld draws in %.1f s (%.1f ms each)\n", draws, now() - start,
         (now() - start) * 1000 / (draws ? draws : 1));
  return 0;
}
