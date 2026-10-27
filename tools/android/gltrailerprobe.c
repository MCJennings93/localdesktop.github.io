// Zero-copy option 1, GL side: imports a /dev/dma_heap/system dmabuf (laid out like a Turnip
// linear image: 1812x2176 BGRA, row pitch 7424) into the stock Adreno GLES driver through
// GL_EXT_memory_object_fd, after writing the Adreno metadata trailer into its last 8 KiB
// (format reverse-engineered from the driver's own exports, tools/android/vkimgtrailer.c):
//   mode B (buffer): trailer for a plain buffer; glBufferStorageMemEXT + a PBO upload
//                    (glTexSubImage2D from the buffer: a GPU-side copy). Fails on driver 0676.77
//                    (glBufferStorageMemEXT: GL_INVALID_OPERATION).
//   mode A (image):  trailer for a linear image with Turnip's pitch; glTexStorageMem2DEXT.
//                    Passes; this is what the app does (GlesRenderer::import_dmabuf_shared).
// Checks the pixels through an FBO readback, then rewrites the dmabuf on the CPU and checks
// again (true sharing). Usage: gltrailerprobe A|B [iterations for timing]
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define GL_HANDLE_TYPE_OPAQUE_FD_EXT 0x9586
#define GL_TEXTURE_TILING_EXT 0x9580
#define GL_LINEAR_TILING_EXT 0x9585
#define GL_DEDICATED_MEMORY_OBJECT_EXT 0x9581
#define GL_BGRA_EXT 0x80E1
typedef void (*PFNCREATEMEM)(GLsizei, GLuint *);
typedef void (*PFNIMPORTFD)(GLuint, GLuint64, GLenum, GLint);
typedef void (*PFNTEXSTORAGEMEM2D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64);
typedef void (*PFNBUFSTORAGEMEM)(GLenum, GLsizeiptr, GLuint, GLuint64);
typedef void (*PFNMEMPARAMIV)(GLuint, GLenum, const GLint *);

#define W 1812
#define H 2176
#define PITCH 7424
#define TRAILER 8192
#define META 5300 /* the metadata is the last META bytes of the dmabuf */

static void put(uint8_t *t, int off, uint32_t v) { memcpy(t + off, &v, 4); }

// Offsets are from the start of the last 8 KiB (as dumped by vkimgtrailer).
static void write_trailer(uint8_t *t, int image, uint32_t data_size) {
    memset(t, 0, TRAILER);
    put(t, 2892, 1);
    put(t, 2896, getpid());
    put(t, 2900, data_size + META);
    put(t, 2908, 0x40);
    put(t, 2912, image ? 0x57 : 0x3e); /* BGRA8 / "buffer" */
    put(t, 2916, 1); put(t, 2920, 1); put(t, 2928, 1); put(t, 2932, 1);
    put(t, 2940, data_size); put(t, 2948, 1); put(t, 2960, data_size);
    if (image) {
        put(t, 2968, PITCH); put(t, 2972, data_size); put(t, 2980, 4);
        put(t, 2984, W); put(t, 2988, H); put(t, 2992, PITCH / 4); put(t, 2996, H);
    } else {
        put(t, 2968, data_size); put(t, 2972, data_size); put(t, 2980, 1);
        put(t, 2984, data_size); put(t, 2988, 1); put(t, 2992, data_size); put(t, 2996, 1);
    }
    put(t, 8152, 0x5143); put(t, 8156, 6); put(t, 8160, 0x0060c510); put(t, 8164, 0xc5100014); put(t, 8168, 4);
    if (image) { put(t, 8172, 1); put(t, 8176, 1); put(t, 8184, META); put(t, 8188, 0xde5cda7a); }
}

static void fill(uint8_t *p, uint32_t salt) {
    for (int y = 0; y < H; y++) {
        uint32_t *row = (uint32_t *)(p + (size_t)y * PITCH);
        for (int x = 0; x < W; x++) row[x] = 0xff000000u | ((x + salt) & 0xff) << 16 | (y & 0xff) << 8 | ((x ^ y ^ salt) & 0xff);
        for (int x = W; x < PITCH / 4; x++) row[x] = 0xdeadbeef; /* padding must never show */
    }
}

// Reads the texture back (RGBA order) and compares with the BGRA source.
static int check(GLuint fbo, const uint8_t *src, const char *what) {
    static uint32_t got[W * H];
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, got);
    int bad = 0, first = -1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint32_t s = ((const uint32_t *)(src + (size_t)y * PITCH))[x];
            uint32_t want = (s & 0xff00ff00u) | (s & 0xff) << 16 | (s >> 16 & 0xff); /* BGRA -> RGBA */
            if (got[y * W + x] != want) { if (first < 0) first = y * W + x; bad++; }
        }
    printf("%s: %d/%d pixels differ", what, bad, W * H);
    if (first >= 0) printf(" (first (%d,%d))", first % W, first / W);
    printf("\n");
    return bad;
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int image = argc > 1 && argv[1][0] == 'A';
    int iters = argc > 2 ? atoi(argv[2]) : 0;
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY); eglInitialize(dpy, NULL, NULL);
    EGLint cfga[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    EGLConfig cfg; EGLint n; eglChooseConfig(dpy, cfga, &cfg, 1, &n);
    EGLint ctxa[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxa);
    EGLint pba[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    EGLSurface pb = eglCreatePbufferSurface(dpy, cfg, pba);
    eglMakeCurrent(dpy, pb, pb, ctx);
    PFNCREATEMEM CreateMemoryObjectsEXT = (PFNCREATEMEM)eglGetProcAddress("glCreateMemoryObjectsEXT");
    PFNIMPORTFD ImportMemoryFdEXT = (PFNIMPORTFD)eglGetProcAddress("glImportMemoryFdEXT");
    PFNTEXSTORAGEMEM2D TexStorageMem2DEXT = (PFNTEXSTORAGEMEM2D)eglGetProcAddress("glTexStorageMem2DEXT");
    PFNBUFSTORAGEMEM BufferStorageMemEXT = (PFNBUFSTORAGEMEM)eglGetProcAddress("glBufferStorageMemEXT");
    PFNMEMPARAMIV MemoryObjectParameterivEXT = (PFNMEMPARAMIV)eglGetProcAddress("glMemoryObjectParameterivEXT");

    uint32_t data = PITCH * H;
    // EXTRA=<bytes>: slack between the image and the trailer (the trailer stays at the end).
    uint32_t extra = getenv("EXTRA") ? atoi(getenv("EXTRA")) : 0;
    int heap = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
    struct dma_heap_allocation_data a = {.len = data + extra + TRAILER, .fd_flags = O_RDWR | O_CLOEXEC};
    if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &a)) { perror("alloc"); return 1; }
    uint8_t *map = mmap(NULL, data + extra + TRAILER, PROT_READ | PROT_WRITE, MAP_SHARED, a.fd, 0);
    fill(map, 1);
    write_trailer(map + data + extra, image, data + extra);

    GLuint mem; CreateMemoryObjectsEXT(1, &mem);
    GLint one = 1; MemoryObjectParameterivEXT(mem, GL_DEDICATED_MEMORY_OBJECT_EXT, &one);
    ImportMemoryFdEXT(mem, data + extra + TRAILER, GL_HANDLE_TYPE_OPAQUE_FD_EXT, dup(a.fd));
    printf("import: 0x%x\n", glGetError());

    GLuint tex; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
    GLuint pbo = 0;
    if (image) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_TILING_EXT, GL_LINEAR_TILING_EXT);
        TexStorageMem2DEXT(GL_TEXTURE_2D, 1, GL_RGBA8, W, H, mem, 0);
        GLenum e = glGetError();
        printf("tex storage mem: 0x%x\n", e);
        if (e) { printf("RESULT: FAIL (no texture)\n"); return 2; }
        // DELETE_MEM=1: drop the memory object at once; the texture must keep the pages.
        if (getenv("DELETE_MEM")) {
            void (*DeleteMemoryObjectsEXT)(GLsizei, const GLuint *) = (void (*)(GLsizei, const GLuint *))eglGetProcAddress("glDeleteMemoryObjectsEXT");
            DeleteMemoryObjectsEXT(1, &mem);
            printf("deleted the memory object: 0x%x\n", glGetError());
        }
    } else {
        glGenBuffers(1, &pbo); glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
        BufferStorageMemEXT(GL_PIXEL_UNPACK_BUFFER, data, mem, 0);
        printf("buffer storage mem: 0x%x\n", glGetError());
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, W, H);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, PITCH / 4);
    }
    GLuint fbo; glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    // GL_BGRA_EXT upload from the PBO: the GPU swizzles while copying.
#define UPLOAD() do { if (!image) { glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_BGRA_EXT, GL_UNSIGNED_BYTE, 0); } } while (0)
    UPLOAD(); printf("upload: 0x%x\n", glGetError());
    if (glGetError()) {}
    int bad = check(fbo, map, "initial");
    fill(map, 99); UPLOAD(); glFinish();
    bad += check(fbo, map, "after CPU rewrite");
    if (iters) {
        // Timing: one frame's worth of work per iteration (B: the GPU copy; A: nothing to do,
        // so draw-equivalent readback of one pixel to force sampling).
        double t0 = now();
        for (int i = 0; i < iters; i++) { UPLOAD(); uint32_t px; glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &px); }
        printf("%s: %.2f ms per frame (upload + sync)\n", image ? "A" : "B", (now() - t0) * 1000 / iters);
    }
    printf(bad ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    return bad != 0;
}
