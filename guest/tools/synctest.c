// synctest: a fullscreen Wayland GLES client for checking GPU-buffer hand-off sync. Every frame
// is one solid colour, cycling red, green, blue, drawn by a deliberately slow fragment shader so
// the GPU is still busy when the frame is committed. A compositor that reads the buffer too early
// shows a mix of colours (a partly drawn frame) or the previous frame's colour (a stale one);
// Local Desktop's frame-stats probe counts both.
// Usage: synctest [seconds, default 30] [shader iterations, default 400] [passes per frame, default 1]
//                 [ramp steps, default 0]
// With ramp steps N > 1, frame k is grey level (k mod N)/(N-1) instead: frames shown in order only
// ever step forward through the ramp (skipping some), so a recording shows any out-of-order frame.
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include "xdg-shell-client-protocol.h"

static struct wl_compositor *compositor;
static struct xdg_wm_base *wm_base;
static int width = 0, height = 0, configured = 0, closed = 0;

static void ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
  (void)data;
  xdg_wm_base_pong(base, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = {ping};

static void global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
                   uint32_t version) {
  (void)data;
  (void)version;
  if (!strcmp(interface, "wl_compositor")) {
    compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
  } else if (!strcmp(interface, "xdg_wm_base")) {
    wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
    xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
  }
}
static void global_remove(void *data, struct wl_registry *registry, uint32_t name) {
  (void)data;
  (void)registry;
  (void)name;
}
static const struct wl_registry_listener registry_listener = {global, global_remove};

static void surface_configure(void *data, struct xdg_surface *surface, uint32_t serial) {
  (void)data;
  xdg_surface_ack_configure(surface, serial);
  configured = 1;
}
static const struct xdg_surface_listener surface_listener = {surface_configure};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t w, int32_t h,
                               struct wl_array *states) {
  (void)data;
  (void)toplevel;
  (void)states;
  if (w > 0 && h > 0) {
    width = w;
    height = h;
  }
}
static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
  (void)data;
  (void)toplevel;
  closed = 1;
}
static const struct xdg_toplevel_listener toplevel_listener = {toplevel_configure, toplevel_close};

static const char *vertex_source =
    "attribute vec2 pos; void main() { gl_Position = vec4(pos, 0.0, 1.0); }";
// Burns GPU time but always ends at exactly `color`: x stays in [0,1), so step(1.5, x) is 0,
// which the shader compiler can't prove (multiplying by 0.0 let it drop the loop).
static const char *fragment_source =
    "precision highp float; uniform vec3 color; uniform int iterations;\n"
    "void main() { float x = gl_FragCoord.x * 0.001;\n"
    "  for (int i = 0; i < 4096; i++) { if (i >= iterations) break; x = fract(sin(x * 12.9898 + 78.233) * 43758.5453); }\n"
    "  gl_FragColor = vec4(color + vec3(step(1.5, x)), 1.0); }";

static GLuint compile(GLenum type, const char *source) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, NULL);
  glCompileShader(shader);
  GLint ok;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[1024];
    glGetShaderInfoLog(shader, sizeof log, NULL, log);
    fprintf(stderr, "shader: %s\n", log);
    exit(1);
  }
  return shader;
}

int main(int argc, char **argv) {
  int seconds = argc > 1 ? atoi(argv[1]) : 30;
  int iterations = argc > 2 ? atoi(argv[2]) : 400;
  int passes = argc > 3 ? atoi(argv[3]) : 1;
  int ramp = argc > 4 ? atoi(argv[4]) : 0;
  struct wl_display *display = wl_display_connect(NULL);
  if (!display) {
    fprintf(stderr, "no Wayland display\n");
    return 1;
  }
  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &registry_listener, NULL);
  wl_display_roundtrip(display);
  struct wl_surface *surface = wl_compositor_create_surface(compositor);
  struct xdg_surface *xdg = xdg_wm_base_get_xdg_surface(wm_base, surface);
  xdg_surface_add_listener(xdg, &surface_listener, NULL);
  struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg);
  xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
  xdg_toplevel_set_title(toplevel, "synctest");
  xdg_toplevel_set_fullscreen(toplevel, NULL);
  wl_surface_commit(surface);
  while (!configured || !width) wl_display_dispatch(display);

  EGLDisplay egl = eglGetDisplay((EGLNativeDisplayType)display);
  eglInitialize(egl, NULL, NULL);
  eglBindAPI(EGL_OPENGL_ES_API);
  EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                         EGL_BLUE_SIZE, 8, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
  EGLConfig config;
  EGLint count;
  eglChooseConfig(egl, attributes, &config, 1, &count);
  EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLContext context = eglCreateContext(egl, config, EGL_NO_CONTEXT, context_attributes);
  struct wl_egl_window *window = wl_egl_window_create(surface, width, height);
  EGLSurface egl_surface = eglCreateWindowSurface(egl, config, (EGLNativeWindowType)window, NULL);
  eglMakeCurrent(egl, egl_surface, egl_surface, context);
  printf("synctest: %dx%d, %d iterations x %d passes, renderer %s\n", width, height, iterations, passes,
         (const char *)glGetString(GL_RENDERER));

  GLuint program = glCreateProgram();
  glAttachShader(program, compile(GL_VERTEX_SHADER, vertex_source));
  glAttachShader(program, compile(GL_FRAGMENT_SHADER, fragment_source));
  glBindAttribLocation(program, 0, "pos");
  glLinkProgram(program);
  glUseProgram(program);
  GLint color_location = glGetUniformLocation(program, "color");
  glUniform1i(glGetUniformLocation(program, "iterations"), iterations);
  static const GLfloat quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
  glEnableVertexAttribArray(0);
  static const GLfloat colors[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

  struct timespec start, now;
  clock_gettime(CLOCK_MONOTONIC, &start);
  long frames = 0;
  do {
    wl_display_dispatch_pending(display);
    int attached_width, attached_height;
    wl_egl_window_get_attached_size(window, &attached_width, &attached_height);
    if (attached_width != width || attached_height != height) {
      wl_egl_window_resize(window, width, height, 0, 0);
    }
    glViewport(0, 0, width, height);
    if (ramp > 1) {
      GLfloat grey = (GLfloat)(frames % ramp) / (ramp - 1);
      glUniform3f(color_location, grey, grey, grey);
    } else {
      glUniform3fv(color_location, 1, colors[frames % 3]);
    }
    for (int pass = 0; pass < passes; pass++) glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    eglSwapBuffers(egl, egl_surface);
    frames++;
    clock_gettime(CLOCK_MONOTONIC, &now);
  } while (!closed && now.tv_sec - start.tv_sec < seconds);
  printf("synctest: %ld frames in %d s (%.1f fps)\n", frames, seconds, frames / (double)seconds);
  return 0;
}
