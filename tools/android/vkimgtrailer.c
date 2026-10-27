// Dumps the in-band metadata trailer that the stock Adreno Vulkan driver appends to exported
// (opaque-fd) memory for a linear 2D image, and the image's row pitch. Usage: vkimgtrailer W H [vkformat]
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    uint32_t w = argc > 2 ? atoi(argv[1]) : 1812, h = argc > 2 ? atoi(argv[2]) : 2176;
    VkFormat fmt = argc > 3 ? atoi(argv[3]) : VK_FORMAT_B8G8R8A8_UNORM;
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai};
    VkInstance inst; vkCreateInstance(&ici, NULL, &inst);
    VkPhysicalDevice pd; uint32_t n = 1; vkEnumeratePhysicalDevices(inst, &n, &pd);
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
    const char *exts[] = {"VK_KHR_external_memory_fd"};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = exts};
    VkDevice dev; vkCreateDevice(pd, &dci, NULL, &dev);
    VkExternalMemoryImageCreateInfo emi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    VkImageCreateInfo ic = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &emi, .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = 1, .tiling = VK_IMAGE_TILING_LINEAR, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VkImage img; VkResult r = vkCreateImage(dev, &ic, NULL, &img);
    if (r) { printf("vkCreateImage -> %d\n", r); return 1; }
    VkImageSubresource sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0}; VkSubresourceLayout lay;
    vkGetImageSubresourceLayout(dev, img, &sub, &lay);
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
    printf("image %ux%u fmt %d: rowPitch %lu (=%lu px) size %lu offset %lu; mem req %lu align %lu\n", w, h, fmt, (unsigned long)lay.rowPitch, (unsigned long)lay.rowPitch / 4, (unsigned long)lay.size, (unsigned long)lay.offset, (unsigned long)mr.size, (unsigned long)mr.alignment);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t t = 0; while (!(mr.memoryTypeBits & (1u << t))) t++;
    VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = img};
    VkExportMemoryAllocateInfo ex = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &ded, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ex, mr.size, t};
    VkDeviceMemory mem; if ((r = vkAllocateMemory(dev, &mai, NULL, &mem))) { printf("alloc -> %d\n", r); return 1; }
    PFN_vkGetMemoryFdKHR getFd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdKHR");
    VkMemoryGetFdInfoKHR gfi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = mem, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    int fd; if ((r = getFd(dev, &gfi, &fd))) { printf("getFd -> %d\n", r); return 1; }
    off_t sz = lseek(fd, 0, SEEK_END);
    unsigned char *m = mmap(NULL, sz, PROT_READ, MAP_SHARED, fd, 0);
    // The metadata sits at the end of the dmabuf (the last 8 KiB hold it for buffers).
    long data = (long)sz - 8192;
    printf("dmabuf size %ld = data(page-aligned req) %ld + %ld\n", (long)sz, data, (long)sz - data);
    printf("trailer nonzero words (offset from %ld):", data);
    for (long o = data; o < sz; o += 4) { unsigned v = *(unsigned *)(m + o); if (v) printf(" [%ld]=%08x", o - data, v); }
    printf("\n");
    if (getenv("DUMP")) { FILE *f = fopen(getenv("DUMP"), "wb"); fwrite(m + data, 1, sz - data, f); fclose(f); }
    return 0;
}
