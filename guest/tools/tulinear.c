// Prints Turnip's layout (row pitch, size) for a linear, dmabuf-exportable 2D image, to compare
// with the stock Adreno driver's (tools/android/vkimgtrailer.c). Usage: tulinear W H [vkformat]
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    uint32_t w = argc > 2 ? atoi(argv[1]) : 1812, h = argc > 2 ? atoi(argv[2]) : 2176;
    VkFormat fmt = argc > 3 ? atoi(argv[3]) : VK_FORMAT_B8G8R8A8_UNORM;
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai};
    VkInstance inst; if (vkCreateInstance(&ici, NULL, &inst)) return 1;
    VkPhysicalDevice pd; uint32_t n = 1; vkEnumeratePhysicalDevices(inst, &n, &pd);
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
    VkDevice dev; if (vkCreateDevice(pd, &dci, NULL, &dev)) return 1;
    VkExternalMemoryImageCreateInfo emi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkImageCreateInfo ic = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &emi, .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = 1, .tiling = VK_IMAGE_TILING_LINEAR, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
    VkImage img; VkResult r = vkCreateImage(dev, &ic, NULL, &img);
    if (r) { printf("vkCreateImage -> %d\n", r); return 1; }
    VkImageSubresource sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0}; VkSubresourceLayout lay;
    vkGetImageSubresourceLayout(dev, img, &sub, &lay);
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
    printf("turnip %ux%u fmt %d: rowPitch %lu size %lu offset %lu; mem req %lu align %lu\n", w, h, fmt,
           (unsigned long)lay.rowPitch, (unsigned long)lay.size, (unsigned long)lay.offset, (unsigned long)mr.size, (unsigned long)mr.alignment);
    return 0;
}
