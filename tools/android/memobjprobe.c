// Zero-copy option 1 probe (host side): can the stock Adreno GLES driver sample a dmabuf it did
// not allocate through GL_EXT_memory_object_fd (opaque-fd import)? Allocates a linear RGBA buffer
// from /dev/dma_heap/system (where Turnip-KGSL's exported buffers come from, exp_name "system"),
// fills it on the CPU, imports it, draws it into an FBO and reads it back. Then rewrites the
// buffer on the CPU and checks that GL sees the new content without a re-import (true sharing,
// not a copy at import time). Run with adb shell (build: tools/android/build.sh).
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef GL_HANDLE_TYPE_OPAQUE_FD_EXT
#define GL_HANDLE_TYPE_OPAQUE_FD_EXT 0x9586
#define GL_TEXTURE_TILING_EXT 0x9580
#define GL_LINEAR_TILING_EXT 0x9585
#define GL_OPTIMAL_TILING_EXT 0x9584
#define GL_DEDICATED_MEMORY_OBJECT_EXT 0x9581
#endif
typedef void (*PFNCREATEMEM)(GLsizei, GLuint *);
typedef void (*PFNIMPORTFD)(GLuint, GLuint64, GLenum, GLint);
typedef void (*PFNTEXSTORAGEMEM2D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64);
typedef void (*PFNMEMPARAMIV)(GLuint, GLenum, const GLint *);

#define W 256
#define H 256

static void fill(uint32_t *p, uint32_t salt) {
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) p[y * W + x] = 0xff000000u | ((x ^ salt) & 0xff) | (((y + salt) & 0xff) << 8) | ((salt * 37 & 0xff) << 16);
}

static int check(const uint32_t *want, const char *what) {
    uint32_t *got = malloc(W * H * 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, got);
    int bad = 0, first = -1;
    for (int i = 0; i < W * H; i++)
        if (got[i] != want[i]) { if (first < 0) first = i; bad++; }
    printf("%s: %d/%d pixels differ", what, bad, W * H);
    if (first >= 0) printf(" (first at %d: got %08x want %08x)", first, got[first], want[first]);
    printf("\n");
    free(got);
    return bad;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    // Options: dedicated, optimal / notiling (default linear), size multiplier xN.
    int dedicated = 0, tiling = 1, mult = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "dedicated")) dedicated = 1;
        else if (!strcmp(argv[i], "optimal")) tiling = 2;
        else if (!strcmp(argv[i], "notiling")) tiling = 0;
        else if (argv[i][0] == 'x') mult = atoi(argv[i] + 1);
    }
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(dpy, NULL, NULL);
    EGLint cfga[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    EGLConfig cfg; EGLint n;
    eglChooseConfig(dpy, cfga, &cfg, 1, &n);
    EGLint ctxa[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxa);
    EGLint pba[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    EGLSurface pb = eglCreatePbufferSurface(dpy, cfg, pba);
    if (!eglMakeCurrent(dpy, pb, pb, ctx)) { printf("make_current failed\n"); return 1; }
    printf("GL_RENDERER %s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

    PFNCREATEMEM CreateMemoryObjectsEXT = (PFNCREATEMEM)eglGetProcAddress("glCreateMemoryObjectsEXT");
    PFNIMPORTFD ImportMemoryFdEXT = (PFNIMPORTFD)eglGetProcAddress("glImportMemoryFdEXT");
    PFNTEXSTORAGEMEM2D TexStorageMem2DEXT = (PFNTEXSTORAGEMEM2D)eglGetProcAddress("glTexStorageMem2DEXT");
    PFNMEMPARAMIV MemoryObjectParameterivEXT = (PFNMEMPARAMIV)eglGetProcAddress("glMemoryObjectParameterivEXT");
    if (!CreateMemoryObjectsEXT || !ImportMemoryFdEXT || !TexStorageMem2DEXT) { printf("entry points missing\n"); return 1; }

    int heap = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
    if (heap < 0) { perror("open dma_heap/system"); return 1; }
    struct dma_heap_allocation_data alloc = {.len = (unsigned)mult * W * H * 4, .fd_flags = O_RDWR | O_CLOEXEC};
    if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0) { perror("DMA_HEAP_IOCTL_ALLOC"); return 1; }
    int buf = alloc.fd;
    uint32_t *map = mmap(NULL, W * H * 4, PROT_READ | PROT_WRITE, MAP_SHARED, buf, 0);
    uint32_t *want = malloc(W * H * 4);
    fill(want, 1); memcpy(map, want, W * H * 4);

    GLuint mem; CreateMemoryObjectsEXT(1, &mem);
    if (dedicated && MemoryObjectParameterivEXT) { GLint one = 1; MemoryObjectParameterivEXT(mem, GL_DEDICATED_MEMORY_OBJECT_EXT, &one); }
    // The import takes ownership of the fd: give it a dup so we keep ours for the mapping.
    ImportMemoryFdEXT(mem, (GLuint64)mult * W * H * 4, GL_HANDLE_TYPE_OPAQUE_FD_EXT, dup(buf));
    printf("import: glGetError 0x%x\n", glGetError());
    GLuint tex; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
    if (tiling) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_TILING_EXT, tiling == 1 ? GL_LINEAR_TILING_EXT : GL_OPTIMAL_TILING_EXT);
    printf("tiling param: glGetError 0x%x\n", glGetError());
    TexStorageMem2DEXT(GL_TEXTURE_2D, 1, GL_RGBA8, W, H, mem, 0);
    GLenum e = glGetError();
    printf("tex storage: glGetError 0x%x\n", e);
    if (e) { printf("RESULT: FAIL (no texture)\n"); return 2; }

    // Read the texture through an FBO (glReadPixels reads the memory as the GPU sees it).
    GLuint fbo; glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    printf("fbo status 0x%x\n", glCheckFramebufferStatus(GL_FRAMEBUFFER));
    int bad = check(want, "initial content");

    fill(want, 77); memcpy(map, want, W * H * 4);
    glFinish();
    bad += check(want, "after CPU rewrite (no re-import)");

    // And the other way: GL writes, the CPU reads the dmabuf.
    glClearColor(1, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT); glFinish();
    printf("after GL clear to red, dmabuf[0]=%08x (want ff0000ff)\n", map[0]);
    printf(bad ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    return bad != 0;
}
