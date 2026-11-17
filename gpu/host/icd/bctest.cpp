// Uploads one BC1 block through a wrapper driver and reads the texels back.
//   bctest /path/to/libvulkan_bcn.so
// The block is solid red. On a GPU without BC support this only passes when the wrapper decodes
// it. Both upload calls are tried: vkCmdCopyBufferToImage and vkCmdCopyBufferToImage2 (DXVK's).
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#include <vulkan/vulkan.h>

#define CHECK(x)                                                         \
    do {                                                                 \
        VkResult r_ = (x);                                               \
        if (r_ != VK_SUCCESS) {                                          \
            printf("FAIL %s: %d\n", #x, r_);                             \
            return 1;                                                    \
        }                                                                \
    } while (0)

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
static VkInstance inst;
static VkDevice dev;
#define I(fn) auto fn = reinterpret_cast<PFN_##fn>(gipa(inst, #fn))
#define D(fn) auto fn = reinterpret_cast<PFN_##fn>(gdpa(dev, #fn))

static uint32_t mem_type(const VkPhysicalDeviceMemoryProperties& mp, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return 0;
}

int main(int argc, char** argv) {
    void* lib = dlopen(argc > 1 ? argv[1] : "libvulkan_bcn.so", RTLD_NOW | RTLD_LOCAL);
    gipa = lib ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vk_icdGetInstanceProcAddr")) : nullptr;
    if (!gipa) {
        printf("FAIL cannot load %s: %s\n", argc > 1 ? argv[1] : "", dlerror());
        return 1;
    }
    I(vkCreateInstance);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "bctest", 1, "DXVK", 0, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app};
    CHECK(vkCreateInstance(&ici, nullptr, &inst));
    I(vkEnumeratePhysicalDevices);
    I(vkGetPhysicalDeviceFeatures);
    I(vkGetPhysicalDeviceProperties);
    I(vkGetPhysicalDeviceMemoryProperties);
    I(vkCreateDevice);
    gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(inst, "vkGetDeviceProcAddr"));
    uint32_t n = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    VkPhysicalDeviceFeatures f{};
    vkGetPhysicalDeviceFeatures(pd, &f);
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(pd, &props);
    printf("%s, textureCompressionBC %u\n", props.deviceName, f.textureCompressionBC);
    if (!f.textureCompressionBC) {
        printf("FAIL no BC support reported\n");
        return 1;
    }
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);

    float prio = 1;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, 0, 1, &prio};
    VkPhysicalDeviceFeatures want{};
    want.textureCompressionBC = VK_TRUE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, nullptr, 0, 1, &qci, 0, nullptr, 0, nullptr, &want};
    CHECK(vkCreateDevice(pd, &dci, nullptr, &dev));
    D(vkGetDeviceQueue2);
    D(vkCreateImage);
    D(vkGetImageMemoryRequirements);
    D(vkAllocateMemory);
    D(vkBindImageMemory);
    D(vkCreateBuffer);
    D(vkGetBufferMemoryRequirements);
    D(vkBindBufferMemory);
    D(vkMapMemory);
    D(vkCreateCommandPool);
    D(vkAllocateCommandBuffers);
    D(vkBeginCommandBuffer);
    D(vkEndCommandBuffer);
    D(vkCmdPipelineBarrier);
    D(vkCmdCopyBufferToImage);
    D(vkCmdCopyBufferToImage2);
    D(vkCmdCopyImageToBuffer);
    D(vkQueueSubmit);
    D(vkQueueWaitIdle);
    D(vkCreateFence);
    VkDeviceQueueInfo2 qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2, nullptr, 0, 0, 0};
    VkQueue q;
    vkGetDeviceQueue2(dev, &qi, &q);  // as gfxstream does

    auto buffer = [&](VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer* b, void** p) -> VkResult {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, size, usage, VK_SHARING_MODE_EXCLUSIVE};
        VkResult r = vkCreateBuffer(dev, &bci, nullptr, b);
        if (r) return r;
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev, *b, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size,
                                 mem_type(mp, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        VkDeviceMemory m;
        if ((r = vkAllocateMemory(dev, &mai, nullptr, &m)) || (r = vkBindBufferMemory(dev, *b, m, 0))) return r;
        return vkMapMemory(dev, m, 0, size, 0, p);
    };

    // One BC1 block: color0 = color1 = pure red (RGB565 0xF800), every index 0.
    const uint8_t block[8] = {0x00, 0xF8, 0x00, 0xF8, 0, 0, 0, 0};
    VkBuffer src, dst;
    void *srcp, *dstp;
    CHECK(buffer(sizeof(block), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &src, &srcp));
    CHECK(buffer(64, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &dst, &dstp));
    memcpy(srcp, block, sizeof(block));

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0, 0};
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(dev, &pci, nullptr, &pool));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    CHECK(vkCreateFence(dev, &fci, nullptr, &fence));

    int failed = 0;
    for (int use2 = 0; use2 < 2; use2++) {
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, nullptr, 0, VK_IMAGE_TYPE_2D,
                             VK_FORMAT_BC1_RGBA_UNORM_BLOCK, {4, 4, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                             VK_IMAGE_TILING_OPTIMAL,
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             VK_SHARING_MODE_EXCLUSIVE, 0, nullptr, VK_IMAGE_LAYOUT_UNDEFINED};
        VkImage img;
        CHECK(vkCreateImage(dev, &ii, nullptr, &img));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(dev, img, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size,
                                 mem_type(mp, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        VkDeviceMemory im;
        CHECK(vkAllocateMemory(dev, &mai, nullptr, &im));
        CHECK(vkBindImageMemory(dev, img, im, 0));

        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool,
                                        VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        VkCommandBuffer cmd;
        CHECK(vkAllocateCommandBuffers(dev, &cai, &cmd));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(cmd, &bi));
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                               VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
        VkBufferImageCopy region{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {4, 4, 1}};
        if (use2) {
            VkBufferImageCopy2 r2{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2, nullptr, 0, 0, 0, region.imageSubresource,
                                  region.imageOffset, region.imageExtent};
            VkCopyBufferToImageInfo2 ci{VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2, nullptr, src, img,
                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r2};
            vkCmdCopyBufferToImage2(cmd, &ci);
        } else {
            vkCmdCopyBufferToImage(cmd, src, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
        vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &region);
        CHECK(vkEndCommandBuffer(cmd));
        memset(dstp, 0x55, 64);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &cmd};
        CHECK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
        CHECK(vkQueueWaitIdle(q));
        const uint8_t* t = static_cast<const uint8_t*>(dstp);
        bool ok = true;
        for (int i = 0; i < 16; i++) ok &= t[i * 4] == 255 && t[i * 4 + 1] == 0 && t[i * 4 + 2] == 0 && t[i * 4 + 3] == 255;
        printf("%s: texel 0 = %u %u %u %u, %s\n", use2 ? "vkCmdCopyBufferToImage2" : "vkCmdCopyBufferToImage", t[0], t[1],
               t[2], t[3], ok ? "red" : "WRONG");
        failed |= !ok;
    }
    printf(failed ? "FAIL\n" : "PASS\n");
    return failed;
}
