// vkfencetest: does a Vulkan fence (or vkQueueWaitIdle) only signal once the GPU work is done?
// Each iteration clears a destination buffer from the CPU, has the GPU copy a patterned source
// buffer into it, waits, and checks every word. xctxtest showed OpenGL (Zink) seeing uploads
// unfinished after glFinish while another process used the GPU; this takes Zink out and tests
// Turnip on KGSL directly. Run gpuload alongside to add GPU work from another process.
// Usage: vkfencetest <fence|idle> [iterations, default 300] [MiB, default 32]
// Build: gcc -O2 -Wall -I/root/src/mesa-26.2.3/include -o vkfencetest vkfencetest.c -lvulkan
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define CHECK(x)                                                     \
  do {                                                               \
    VkResult r_ = (x);                                               \
    if (r_ != VK_SUCCESS) {                                          \
      fprintf(stderr, "vkfencetest: %s failed: %d\n", #x, (int)r_); \
      exit(1);                                                       \
    }                                                                \
  } while (0)

static VkDevice dev;
static VkPhysicalDeviceMemoryProperties mem_props;

// A host-visible buffer, preferably coherent; *coherent says whether it is.
static void *host_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buf,
                         VkDeviceMemory *mem, int *coherent) {
  VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                           .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
  CHECK(vkCreateBuffer(dev, &bi, NULL, buf));
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(dev, *buf, &req);
  int type = -1;
  for (int pass = 0; pass < 2 && type < 0; pass++) {
    VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 (pass == 0 ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : 0);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++)
      if ((req.memoryTypeBits & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & want) == want) {
        type = (int)i;
        break;
      }
  }
  if (type < 0) {
    fprintf(stderr, "vkfencetest: no host-visible memory\n");
    exit(1);
  }
  *coherent = !!(mem_props.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                             .allocationSize = req.size, .memoryTypeIndex = (uint32_t)type};
  CHECK(vkAllocateMemory(dev, &ai, NULL, mem));
  CHECK(vkBindBufferMemory(dev, *buf, *mem, 0));
  void *p;
  CHECK(vkMapMemory(dev, *mem, 0, VK_WHOLE_SIZE, 0, &p));
  return p;
}

int main(int argc, char **argv) {
  const char *mode = argc > 1 ? argv[1] : "fence";
  int iterations = argc > 2 ? atoi(argv[2]) : 300;
  VkDeviceSize size = (VkDeviceSize)(argc > 3 ? atoi(argv[3]) : 32) << 20;
  VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
  VkInstanceCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
  VkInstance inst;
  CHECK(vkCreateInstance(&ii, NULL, &inst));
  uint32_t n = 1;
  VkPhysicalDevice pd;
  vkEnumeratePhysicalDevices(inst, &n, &pd);
  if (!n) return fprintf(stderr, "vkfencetest: no device\n"), 1;
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(pd, &props);
  vkGetPhysicalDeviceMemoryProperties(pd, &mem_props);
  float prio = 1;
  VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio};
  VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                           .pQueueCreateInfos = &qi};
  CHECK(vkCreateDevice(pd, &di, NULL, &dev));
  VkQueue queue;
  vkGetDeviceQueue(dev, 0, 0, &queue);

  VkBuffer src, dst;
  VkDeviceMemory src_mem, dst_mem;
  int src_coherent, dst_coherent;
  uint32_t *s = host_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &src, &src_mem, &src_coherent);
  uint32_t *d = host_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &dst, &dst_mem, &dst_coherent);
  size_t words = size / 4;
  for (size_t i = 0; i < words; i++) s[i] = (uint32_t)i | 0x80000000u;
  VkMappedMemoryRange src_range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = src_mem, .size = VK_WHOLE_SIZE};
  VkMappedMemoryRange dst_range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = dst_mem, .size = VK_WHOLE_SIZE};
  if (!src_coherent) CHECK(vkFlushMappedMemoryRanges(dev, 1, &src_range));
  printf("vkfencetest: %s, mode %s, %d iterations of %llu MiB, memory %s\n", props.deviceName, mode,
         iterations, (unsigned long long)(size >> 20), dst_coherent ? "coherent" : "cached (invalidated)");

  VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
  VkCommandPool pool;
  CHECK(vkCreateCommandPool(dev, &pi, NULL, &pool));
  VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                     .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                     .commandBufferCount = 1};
  VkCommandBuffer cmd;
  CHECK(vkAllocateCommandBuffers(dev, &cai, &cmd));
  VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  CHECK(vkBeginCommandBuffer(cmd, &bi));
  VkBufferCopy region = {.size = size};
  vkCmdCopyBuffer(cmd, src, dst, 1, &region);
  // Make the copy visible to the host, as the spec requires before reading it there.
  VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                             .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                             .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                       &barrier, 0, NULL, 0, NULL);
  CHECK(vkEndCommandBuffer(cmd));
  VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence;
  CHECK(vkCreateFence(dev, &fi, NULL, &fence));

  int bad = 0;
  size_t worst = 0;
  for (int it = 0; it < iterations; it++) {
    memset(d, 0, size);
    if (!dst_coherent) CHECK(vkFlushMappedMemoryRanges(dev, 1, &dst_range));
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
                       .pCommandBuffers = &cmd};
    int use_fence = strcmp(mode, "idle") != 0;
    CHECK(vkQueueSubmit(queue, 1, &si, use_fence ? fence : VK_NULL_HANDLE));
    if (use_fence) {
      CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
      CHECK(vkResetFences(dev, 1, &fence));
    } else {
      CHECK(vkQueueWaitIdle(queue));
    }
    if (!dst_coherent) CHECK(vkInvalidateMappedMemoryRanges(dev, 1, &dst_range));
    size_t missing = 0;
    for (size_t i = 0; i < words; i++) missing += d[i] != s[i];
    if (missing) {
      bad++;
      if (missing > worst) worst = missing;
    }
  }
  printf("vkfencetest: %s: %d of %d copies incomplete after the wait (worst: %zu of %zu words)\n",
         mode, bad, iterations, worst, words);
  return 0;
}
