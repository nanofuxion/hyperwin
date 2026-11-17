// Draws one triangle whose positions come from an R16G16B16A16_SSCALED vertex buffer, through a
// wrapper driver, and checks the pixels. Mali has no SSCALED vertex formats; libvulkan_vortek.so
// reports them anyway and rewrites the pipeline (integer input, converted in the vertex shader).
//   scaledtest /path/to/libvulkan_vortek.so
// Covers the wrapper's deferred shader modules and Vortek's convertFormatScaled pass.
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#include <initializer_list>

#include <vulkan/vulkan.h>

static const uint32_t kVert[] = {
#include "scaled.vert.inc"
};
static const uint32_t kFrag[] = {
#include "scaled.frag.inc"
};

#define CHECK(x)                                     \
    do {                                             \
        VkResult r_ = (x);                           \
        if (r_ != VK_SUCCESS) {                      \
            printf("FAIL %s: %d\n", #x, r_);         \
            return 1;                                \
        }                                            \
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
    void* lib = dlopen(argc > 1 ? argv[1] : "libvulkan_vortek.so", RTLD_NOW | RTLD_LOCAL);
    gipa = lib ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vk_icdGetInstanceProcAddr")) : nullptr;
    if (!gipa) {
        printf("FAIL cannot load: %s\n", dlerror());
        return 1;
    }
    const VkFormat kFormat = VK_FORMAT_R16G16B16A16_SSCALED;
    I(vkCreateInstance);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "scaledtest", 1, "DXVK", VK_MAKE_VERSION(2, 4, 0),
                          VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app};
    CHECK(vkCreateInstance(&ici, nullptr, &inst));
    I(vkEnumeratePhysicalDevices);
    I(vkGetPhysicalDeviceFormatProperties);
    I(vkGetPhysicalDeviceMemoryProperties);
    I(vkCreateDevice);
    gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(inst, "vkGetDeviceProcAddr"));
    uint32_t n = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    for (VkFormat f : {VK_FORMAT_R8G8B8A8_USCALED, VK_FORMAT_R8G8B8A8_SSCALED, VK_FORMAT_R16G16B16A16_USCALED,
                       VK_FORMAT_R16G16B16A16_SSCALED, VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R8G8_SSCALED}) {
        VkFormatProperties p{};
        vkGetPhysicalDeviceFormatProperties(pd, f, &p);
        printf("  format %d vertex buffer: %s\n", f, (p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) ? "yes" : "no");
    }
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(pd, kFormat, &fp);
    bool usable = fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
    printf("R16G16B16A16_SSCALED vertex buffers: %s\n", usable ? "reported" : "not reported");
    if (!usable) {
        printf("FAIL\n");
        return 1;
    }
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    float prio = 1;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, 0, 1, &prio};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, nullptr, 0, 1, &qci};
    CHECK(vkCreateDevice(pd, &dci, nullptr, &dev));
    D(vkGetDeviceQueue); D(vkCreateImage); D(vkGetImageMemoryRequirements); D(vkAllocateMemory);
    D(vkBindImageMemory); D(vkCreateImageView); D(vkCreateBuffer); D(vkGetBufferMemoryRequirements);
    D(vkBindBufferMemory); D(vkMapMemory); D(vkCreateRenderPass); D(vkCreateFramebuffer);
    D(vkCreateShaderModule); D(vkDestroyShaderModule); D(vkCreatePipelineLayout); D(vkCreateGraphicsPipelines);
    D(vkCreateCommandPool); D(vkAllocateCommandBuffers); D(vkBeginCommandBuffer); D(vkEndCommandBuffer);
    D(vkCmdBeginRenderPass); D(vkCmdEndRenderPass); D(vkCmdBindPipeline); D(vkCmdBindVertexBuffers);
    D(vkCmdDraw); D(vkCmdCopyImageToBuffer); D(vkCmdPipelineBarrier); D(vkQueueSubmit); D(vkQueueWaitIdle);
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);

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

    // A triangle over the whole target: (-1,-1) (3,-1) (-1,3), as signed 16-bit integers.
    const int16_t verts[] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    VkBuffer vb, out;
    void *vbp, *outp;
    CHECK(buffer(sizeof(verts), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vb, &vbp));
    CHECK(buffer(16 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &out, &outp));
    memcpy(vbp, verts, sizeof(verts));

    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, nullptr, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
                         {4, 4, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VkImage img;
    CHECK(vkCreateImage(dev, &ii, nullptr, &img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, img, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size,
                             mem_type(mp, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory im;
    CHECK(vkAllocateMemory(dev, &mai, nullptr, &im));
    CHECK(vkBindImageMemory(dev, img, im, 0));
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0, img, VK_IMAGE_VIEW_TYPE_2D,
                              VK_FORMAT_R8G8B8A8_UNORM, {}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view;
    CHECK(vkCreateImageView(dev, &vci, nullptr, &view));

    VkAttachmentDescription att{0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, nullptr, 1, &ref};
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, nullptr, 0, 1, &att, 1, &sub};
    VkRenderPass rp;
    CHECK(vkCreateRenderPass(dev, &rpi, nullptr, &rp));
    VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, nullptr, 0, rp, 1, &view, 4, 4, 1};
    VkFramebuffer fb;
    CHECK(vkCreateFramebuffer(dev, &fbi, nullptr, &fb));

    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, sizeof(kVert), kVert};
    VkShaderModule vs, fs;
    CHECK(vkCreateShaderModule(dev, &smi, nullptr, &vs));
    smi.codeSize = sizeof(kFrag);
    smi.pCode = kFrag;
    CHECK(vkCreateShaderModule(dev, &smi, nullptr, &fs));
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(dev, &pli, nullptr, &layout));
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main"}};
    VkVertexInputBindingDescription bind{0, 8, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attr{0, 0, kFormat, 0};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, nullptr, 0, 1, &bind, 1, &attr};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0,
                                              VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport vp{0, 0, 4, 4, 0, 1};
    VkRect2D sc{{0, 0}, {4, 4}};
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, nullptr, 0, 1, &vp, 1, &sc};
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpi.stageCount = 2;
    gpi.pStages = stages;
    gpi.pVertexInputState = &vi;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vps;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState = &ms;
    gpi.pColorBlendState = &cb;
    gpi.layout = layout;
    gpi.renderPass = rp;
    VkPipeline pipe;
    CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipe));
    vkDestroyShaderModule(dev, vs, nullptr);
    vkDestroyShaderModule(dev, fs, nullptr);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0, 0};
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(dev, &pci, nullptr, &pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(dev, &cai, &cmd));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(cmd, &bi));
    VkClearValue clear{};
    clear.color.float32[0] = 1;  // red where the triangle did not land
    clear.color.float32[3] = 1;
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, rp, fb, {{0, 0}, {4, 4}}, 1, &clear};
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &zero);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &b);
    VkBufferImageCopy region{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {4, 4, 1}};
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out, 1, &region);
    CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &cmd};
    CHECK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(q));

    const uint8_t* px = static_cast<const uint8_t*>(outp);
    int green = 0;
    for (int i = 0; i < 16; i++) green += px[i * 4] == 0 && px[i * 4 + 1] == 255 && px[i * 4 + 2] == 0;
    printf("pixels green: %d of 16 (pixel 0 = %u %u %u %u)\n", green, px[0], px[1], px[2], px[3]);
    printf(green == 16 ? "PASS\n" : "FAIL\n");
    return green == 16 ? 0 : 1;
}
