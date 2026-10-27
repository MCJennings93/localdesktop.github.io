// Zero-copy option 1 probe, Vulkan side: (a) what kind of fd the stock Adreno Vulkan driver
// exports as VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD (a dmabuf? which heap?), and (b) whether
// it imports a /dev/dma_heap/system dmabuf as an opaque fd and really shares its pages (CPU fills
// the dmabuf, the GPU copies it into a host-visible buffer, the CPU checks).
#include <vulkan/vulkan.h>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define SIZE (1 << 20)
#define CHK(x) do { VkResult r_ = (x); if (r_) { printf("%s -> %d\n", #x, r_); return 1; } } while (0)

static VkDevice dev; static VkPhysicalDevice pd;
static int memtype(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return -1;
}
static void describe(int fd, const char *what) {
    char p[64], l[256] = ""; snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(p, l, sizeof l - 1); if (n > 0) l[n] = 0;
    printf("%s: fd %d -> %s\n", what, fd, l);
    snprintf(p, sizeof p, "/proc/self/fdinfo/%d", fd);
    FILE *f = fopen(p, "r"); char line[256];
    while (f && fgets(line, sizeof line, f)) if (strstr(line, "exp_name") || strstr(line, "size")) printf("   %s", line);
    if (f) fclose(f);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai};
    VkInstance inst; CHK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t n = 1; vkEnumeratePhysicalDevices(inst, &n, &pd);
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
    const char *exts[] = {"VK_KHR_external_memory_fd"};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = exts};
    CHK(vkCreateDevice(pd, &dci, NULL, &dev));
    PFN_vkGetMemoryFdKHR getFd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdKHR");
    PFN_vkGetMemoryFdPropertiesKHR fdProps = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdPropertiesKHR");

    VkPhysicalDeviceExternalBufferInfo ebi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    VkExternalBufferProperties ebp = {VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    vkGetPhysicalDeviceExternalBufferProperties(pd, &ebi, &ebp);
    printf("opaque fd buffer features 0x%x (1=dedicated 2=export 4=import)\n", ebp.externalMemoryProperties.externalMemoryFeatures);
    ebi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    vkGetPhysicalDeviceExternalBufferProperties(pd, &ebi, &ebp);
    printf("dma_buf buffer features 0x%x\n", ebp.externalMemoryProperties.externalMemoryFeatures);

    // (a) export
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = SIZE, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkExternalMemoryBufferCreateInfo embci = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    bci.pNext = &embci;
    VkBuffer eb; CHK(vkCreateBuffer(dev, &bci, NULL, &eb));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, eb, &mr);
    // The driver reports opaque fds as dedicated-only (feature bit 1).
    VkMemoryDedicatedAllocateInfo eded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .buffer = eb};
    VkExportMemoryAllocateInfo emai = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &eded, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &emai, mr.size, memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
    VkDeviceMemory em; CHK(vkAllocateMemory(dev, &mai, NULL, &em));
    VkMemoryGetFdInfoKHR gfi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = em, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    int efd = -1; CHK(getFd(dev, &gfi, &efd));
    describe(efd, "exported opaque fd");
    off_t esz = lseek(efd, 0, SEEK_END);
    unsigned char *emap = mmap(NULL, esz, PROT_READ, MAP_SHARED, efd, 0);
    if (emap == MAP_FAILED) perror("mmap exported");
    else {
        printf("exported trailer (bytes %d..%ld), nonzero words:", SIZE, (long)esz);
        int shown = 0;
        for (long o = SIZE; o < esz && shown < 24; o += 4) { unsigned w = *(unsigned *)(emap + o); if (w) { printf(" [%ld]=%08x", o - SIZE, w); shown++; } }
        printf("\n");
    }
    // Control: re-import the driver's own fd.
    {
        VkBuffer cbuf; vkCreateBuffer(dev, &bci, NULL, &cbuf);
        VkMemoryRequirements cmr; vkGetBufferMemoryRequirements(dev, cbuf, &cmr);
        VkMemoryDedicatedAllocateInfo cd = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .buffer = cbuf};
        VkImportMemoryFdInfoKHR ci = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, &cd, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, dup(efd)};
        VkMemoryAllocateInfo cai2 = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ci, cmr.size, memtype(cmr.memoryTypeBits, 0)};
        VkDeviceMemory cm; printf("control: re-import own exported fd -> %d (req size %lu)\n", vkAllocateMemory(dev, &cai2, NULL, &cm), (unsigned long)cmr.size);
    }

    // (b) import a dma-heap dmabuf as opaque
    int heap = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
    struct dma_heap_allocation_data a = {.len = getenv("PAD") ? SIZE + 8192 : SIZE, .fd_flags = O_RDWR | O_CLOEXEC};
    if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &a)) { perror("alloc"); return 1; }
    describe(a.fd, "dma-heap fd");
    unsigned *cpu = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, a.fd, 0);
    for (int i = 0; i < SIZE / 4; i++) cpu[i] = 0x9e3779b9u * i;
    if (getenv("PAD") && getenv("TRAILER") && emap != MAP_FAILED) { unsigned char *t = mmap(NULL, SIZE + 8192, PROT_READ | PROT_WRITE, MAP_SHARED, a.fd, 0); memcpy(t + SIZE, emap + SIZE, 8192); printf("copied the trailer\n"); }
    VkMemoryFdPropertiesKHR fp = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    if (fdProps) printf("vkGetMemoryFdPropertiesKHR(opaque) -> %d bits 0x%x\n", fdProps(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, a.fd, &fp), fp.memoryTypeBits);
    VkBuffer ib; CHK(vkCreateBuffer(dev, &bci, NULL, &ib));
    vkGetBufferMemoryRequirements(dev, ib, &mr);
    VkMemoryDedicatedAllocateInfo ided = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .buffer = ib};
    VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, &ided, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, .fd = dup(a.fd)};
    VkMemoryAllocateInfo imai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, mr.size, memtype(mr.memoryTypeBits, 0)};
    VkDeviceMemory im; VkResult r = vkAllocateMemory(dev, &imai, NULL, &im);
    printf("import dma-heap fd as opaque -> %d\n", r);
    if (r) { printf("RESULT: import rejected\n"); return 1; }
    CHK(vkBindBufferMemory(dev, ib, im, 0));

    // GPU copy imported -> host-visible destination, CPU compares.
    VkBufferCreateInfo dci2 = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = SIZE, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer db; CHK(vkCreateBuffer(dev, &dci2, NULL, &db));
    vkGetBufferMemoryRequirements(dev, db, &mr);
    VkMemoryAllocateInfo dmai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory dm; CHK(vkAllocateMemory(dev, &dmai, NULL, &dm)); CHK(vkBindBufferMemory(dev, db, dm, 0));
    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkCommandPool pool; CHK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .commandBufferCount = 1};
    VkCommandBuffer cb; CHK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cb, &cbi);
    VkBufferCopy reg = {0, 0, SIZE}; vkCmdCopyBuffer(cb, ib, db, 1, &reg);
    vkEndCommandBuffer(cb);
    VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    CHK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE)); CHK(vkQueueWaitIdle(q));
    unsigned *out; CHK(vkMapMemory(dev, dm, 0, SIZE, 0, (void **)&out));
    int bad = 0; for (int i = 0; i < SIZE / 4; i++) bad += out[i] != cpu[i];
    printf("GPU read of imported dmabuf: %d/%d words differ (out[1]=%08x want %08x)\n", bad, SIZE / 4, out[1], cpu[1]);
    printf(bad ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    return bad != 0;
}
