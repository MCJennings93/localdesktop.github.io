// zink-kopper-shim: LD_PRELOAD for GPU OpenGL in Local Desktop (no DRM device, Turnip on KGSL).
// Mesa EGL only uses Zink's kopper presenter (Vulkan WSI swapchain) on its software path, which
// LIBGL_ALWAYS_SOFTWARE selects; but Zink then also insists on a CPU Vulkan device. This hides
// that variable from libgallium (Zink) only, so EGL takes kopper and Zink picks the Adreno.
// The other route (GALLIUM_DRIVER=zink) presents through EGL's wl_shm readback and is black.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <string.h>
#include <stdlib.h>
static char *(*real_getenv)(const char *);
char *getenv(const char *name) {
    if (!real_getenv) real_getenv = dlsym(RTLD_NEXT, "getenv");
    if (name && !strcmp(name, "LIBGL_ALWAYS_SOFTWARE")) {
        Dl_info info;
        if (dladdr(__builtin_return_address(0), &info) && info.dli_fname && strstr(info.dli_fname, "libgallium"))
            return NULL;
    }
    return real_getenv(name);
}
