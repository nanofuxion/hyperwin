// vkreplay: Stand-alone off-screen Vulkan replay tool for .vkcap files captured by libvulkan_vkcap.so.
//
// Runs directly from `adb shell` with HyperWin completely closed, loading any Vulkan wrapper
// (e.g. /data/local/tmp/libvulkan_malivk.so) or /system/lib64/libvulkan.so directly.
//
// Usage:
//   HWGPU_VK_DRIVER=/system/lib64/libvulkan.so \
//     /data/local/tmp/vkreplay <wrapper_or_driver.so> <capture.vkcap> [out_prefix]
//
// Writes <out_prefix>_rt<N>.ppm for each color render target drawn during the captured submits
// and prints pixel statistics + total replay time.

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include "vkcap_format.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using namespace vkcap;

struct Reader {
    const uint8_t* p = nullptr;
    const uint8_t* end = nullptr;
    bool ok = true;
    void take(void* dst, size_t n) {
        if (size_t(end - p) < n) { ok = false; memset(dst, 0, n); p = end; return; }
        memcpy(dst, p, n);
        p += n;
    }
    uint32_t u32() { uint32_t v = 0; take(&v, 4); return v; }
    int32_t i32() { int32_t v = 0; take(&v, 4); return v; }
    uint64_t u64() { uint64_t v = 0; take(&v, 8); return v; }
    float f32() { float v = 0; take(&v, 4); return v; }
    std::string str() {
        uint32_t n = u32();
        if (!ok || size_t(end - p) < n) { ok = false; return {}; }
        std::string s(reinterpret_cast<const char*>(p), n);
        p += n;
        return s;
    }
    template <class T> T pod() { T v{}; take(&v, sizeof(T)); return v; }
};

static uint32_t find_mem_type(const VkPhysicalDeviceMemoryProperties& mp, uint32_t type_bits,
                              VkMemoryPropertyFlags want, bool host_visible) {
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    }
    if (host_visible) {
        VkMemoryPropertyFlags fb = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & fb) == fb) return i;
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) return i;
    }
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (type_bits & (1u << i)) return i;
    return 0;
}

static float half_to_float(uint16_t h) {
    uint32_t s = (uint32_t(h & 0x8000u)) << 16;
    uint32_t e = (h >> 10) & 0x1fu;
    uint32_t m = h & 0x3ffu;
    uint32_t f = 0;
    if (e == 0) {
        if (m == 0) f = s;
        else {
            while (!(m & 0x400u)) { m <<= 1; e--; }
            e++; m &= ~0x400u;
            f = s | ((e + 112u) << 23) | (m << 13);
        }
    } else if (e == 31) {
        f = s | 0x7f800000u | (m << 13);
    } else {
        f = s | ((e + 112u) << 23) | (m << 13);
    }
    float out; memcpy(&out, &f, 4); return out;
}

static bool is_depth_fmt(VkFormat f) {
    switch (f) {
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_S8_UINT: case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return true;
    default: return false;
    }
}

struct ImgInfo {
    VkImage img = VK_NULL_HANDLE;
    VkImageCreateInfo ci{};
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool drawn = false;
};

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <wrapper_or_driver.so> <capture.vkcap> [out_prefix]\n", argv[0]);
        return 2;
    }
    const char* lib_path = argv[1];
    const char* cap_path = argv[2];
    std::string out_prefix = argc >= 4 ? argv[3] : "/data/local/tmp/vkreplay_out";

    auto t0 = std::chrono::steady_clock::now();

    void* lib = dlopen(lib_path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "dlopen(%s) failed: %s\n", lib_path, dlerror());
        return 1;
    }
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
    if (!gipa) gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vk_icdGetInstanceProcAddr"));
    if (!gipa) {
        fprintf(stderr, "vkGetInstanceProcAddr / vk_icdGetInstanceProcAddr not found in %s\n", lib_path);
        return 1;
    }

    FILE* fp = fopen(cap_path, "rb");
    if (!fp) {
        fprintf(stderr, "Cannot open %s\n", cap_path);
        return 1;
    }
    FileHeader fh{};
    if (fread(&fh, sizeof(fh), 1, fp) != 1 || fh.magic != kMagic || fh.version != kVersion) {
        fprintf(stderr, "Invalid vkcap file: %s\n", cap_path);
        fclose(fp);
        return 1;
    }
    printf("vkreplay: loading '%s' (app='%s') on '%s'\n", cap_path, fh.app_name, lib_path);

    // Load global Vulkan entry points
    auto pfnCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = fh.app_name;
    app.pEngineName = "DXVK";
    app.apiVersion = VK_MAKE_VERSION(1, 1, 0);
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    VkResult vr = pfnCreateInstance(&ici, nullptr, &inst);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "vkCreateInstance failed: %d\n", int(vr));
        return 1;
    }

#define I_FN(n) auto pfn_##n = reinterpret_cast<PFN_##n>(gipa(inst, #n))
    I_FN(vkEnumeratePhysicalDevices);
    I_FN(vkGetPhysicalDeviceProperties);
    I_FN(vkGetPhysicalDeviceMemoryProperties);
    I_FN(vkGetPhysicalDeviceQueueFamilyProperties);
    I_FN(vkEnumerateDeviceExtensionProperties);
    I_FN(vkGetPhysicalDeviceFeatures);
    I_FN(vkCreateDevice);
    I_FN(vkGetDeviceProcAddr);
#undef I_FN

    uint32_t pd_count = 0;
    pfn_vkEnumeratePhysicalDevices(inst, &pd_count, nullptr);
    if (!pd_count) { fprintf(stderr, "No Vulkan physical devices!\n"); return 1; }
    std::vector<VkPhysicalDevice> pds(pd_count);
    pfn_vkEnumeratePhysicalDevices(inst, &pd_count, pds.data());
    VkPhysicalDevice pd = pds[0];
    VkPhysicalDeviceProperties pdp{};
    pfn_vkGetPhysicalDeviceProperties(pd, &pdp);
    VkPhysicalDeviceMemoryProperties mp{};
    pfn_vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    printf("vkreplay: GPU = %s (api %u.%u.%u)\n", pdp.deviceName,
           VK_VERSION_MAJOR(pdp.apiVersion), VK_VERSION_MINOR(pdp.apiVersion), VK_VERSION_PATCH(pdp.apiVersion));

    uint32_t qfc = 0;
    pfn_vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfc, nullptr);
    std::vector<VkQueueFamilyProperties> qfp(qfc);
    pfn_vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfc, qfp.data());
    uint32_t qfam = 0;
    for (uint32_t i = 0; i < qfc; i++) {
        if (qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfam = i; break; }
    }

    // Available extensions & features
    uint32_t avail_ec = 0;
    pfn_vkEnumerateDeviceExtensionProperties(pd, nullptr, &avail_ec, nullptr);
    std::vector<VkExtensionProperties> avail_exts(avail_ec);
    pfn_vkEnumerateDeviceExtensionProperties(pd, nullptr, &avail_ec, avail_exts.data());
    std::set<std::string> avail_ext_set;
    for (const auto& e : avail_exts) avail_ext_set.insert(e.extensionName);
    VkPhysicalDeviceFeatures avail_feats{};
    pfn_vkGetPhysicalDeviceFeatures(pd, &avail_feats);

    // Read first record: REC_DEVICE_INFO
    RecHeader rh{};
    std::vector<uint8_t> payload;
    auto next_rec = [&]() -> bool {
        if (fread(&rh, sizeof(rh), 1, fp) != 1) return false;
        payload.resize(rh.size);
        if (rh.size && fread(payload.data(), 1, rh.size, fp) != rh.size) return false;
        return true;
    };

    std::vector<std::string> req_exts;
    VkPhysicalDeviceFeatures req_feats{};
    if (!next_rec() || rh.tag != REC_DEVICE_INFO) {
        fprintf(stderr, "Expected REC_DEVICE_INFO first!\n");
        return 1;
    }
    {
        Reader r{payload.data(), payload.data() + payload.size()};
        uint32_t ec = r.u32();
        for (uint32_t i = 0; i < ec; i++) {
            std::string e = r.str();
            if (avail_ext_set.count(e)) req_exts.push_back(e);
            else printf("vkreplay: note: skipping unavailable ext '%s'\n", e.c_str());
        }
        req_feats = r.pod<VkPhysicalDeviceFeatures>();
        // Mask requested features against available features
        auto* dst = reinterpret_cast<VkBool32*>(&req_feats);
        const auto* src = reinterpret_cast<const VkBool32*>(&avail_feats);
        for (size_t i = 0; i < sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32); i++)
            dst[i] = (dst[i] && src[i]) ? VK_TRUE : VK_FALSE;
    }

    std::vector<const char*> ext_ptrs;
    for (const auto& e : req_exts) ext_ptrs.push_back(e.c_str());
    float qprio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &qprio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(ext_ptrs.size());
    dci.ppEnabledExtensionNames = ext_ptrs.data();
    dci.pEnabledFeatures = &req_feats;

    VkDevice dev = VK_NULL_HANDLE;
    vr = pfn_vkCreateDevice(pd, &dci, nullptr, &dev);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "vkCreateDevice failed: %d\n", int(vr));
        return 1;
    }

#define D_FN(n) auto pfn_##n = reinterpret_cast<PFN_##n>(pfn_vkGetDeviceProcAddr(dev, #n))
    D_FN(vkGetDeviceQueue);
    D_FN(vkDeviceWaitIdle);
    D_FN(vkAllocateMemory);
    D_FN(vkFreeMemory);
    D_FN(vkMapMemory);
    D_FN(vkUnmapMemory);
    D_FN(vkCreateBuffer);
    D_FN(vkDestroyBuffer);
    D_FN(vkGetBufferMemoryRequirements);
    D_FN(vkBindBufferMemory);
    D_FN(vkCreateImage);
    D_FN(vkGetImageMemoryRequirements);
    D_FN(vkBindImageMemory);
    D_FN(vkCreateImageView);
    D_FN(vkCreateBufferView);
    D_FN(vkCreateSampler);
    D_FN(vkCreateShaderModule);
    D_FN(vkCreateDescriptorSetLayout);
    D_FN(vkCreatePipelineLayout);
    D_FN(vkCreateRenderPass);
    D_FN(vkCreateFramebuffer);
    D_FN(vkCreateGraphicsPipelines);
    D_FN(vkCreateComputePipelines);
    D_FN(vkCreateDescriptorPool);
    D_FN(vkAllocateDescriptorSets);
    D_FN(vkUpdateDescriptorSets);
    D_FN(vkCreateQueryPool);
    D_FN(vkCreateCommandPool);
    D_FN(vkAllocateCommandBuffers);
    D_FN(vkBeginCommandBuffer);
    D_FN(vkEndCommandBuffer);
    D_FN(vkResetCommandBuffer);
    D_FN(vkQueueSubmit);
    D_FN(vkQueueWaitIdle);
    D_FN(vkCmdBeginRenderPass);
    D_FN(vkCmdEndRenderPass);
    D_FN(vkCmdNextSubpass);
    D_FN(vkCmdBindPipeline);
    D_FN(vkCmdBindDescriptorSets);
    D_FN(vkCmdBindVertexBuffers);
    D_FN(vkCmdBindIndexBuffer);
    D_FN(vkCmdPushConstants);
    D_FN(vkCmdSetViewport);
    D_FN(vkCmdSetScissor);
    D_FN(vkCmdSetBlendConstants);
    D_FN(vkCmdSetDepthBias);
    D_FN(vkCmdSetDepthBounds);
    D_FN(vkCmdSetStencilReference);
    D_FN(vkCmdSetStencilCompareMask);
    D_FN(vkCmdSetStencilWriteMask);
    D_FN(vkCmdDraw);
    D_FN(vkCmdDrawIndexed);
    D_FN(vkCmdDispatch);
    D_FN(vkCmdClearAttachments);
    D_FN(vkCmdClearColorImage);
    D_FN(vkCmdClearDepthStencilImage);
    D_FN(vkCmdCopyBuffer);
    D_FN(vkCmdCopyBufferToImage);
    D_FN(vkCmdCopyImage);
    D_FN(vkCmdCopyImageToBuffer);
    D_FN(vkCmdBlitImage);
    D_FN(vkCmdResolveImage);
    D_FN(vkCmdUpdateBuffer);
    D_FN(vkCmdFillBuffer);
    D_FN(vkCmdPipelineBarrier);
    D_FN(vkCmdResetQueryPool);
    D_FN(vkCmdBeginQuery);
    D_FN(vkCmdEndQuery);
#undef D_FN

    VkQueue queue = VK_NULL_HANDLE;
    pfn_vkGetDeviceQueue(dev, qfam, 0, &queue);

    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = qfam;
    VkCommandPool cpool = VK_NULL_HANDLE;
    pfn_vkCreateCommandPool(dev, &cpci, nullptr, &cpool);
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = cpool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    pfn_vkAllocateCommandBuffers(dev, &cbai, &cmd);

    // Big general-purpose descriptor pool for replay
    VkDescriptorPoolSize dp_sizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLER, 8192},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16384},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16384},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4096},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 4096},
        {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 4096},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16384},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8192},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 8192},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 4096},
        {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 2048},
    };
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 32768;
    dpci.poolSizeCount = uint32_t(sizeof(dp_sizes) / sizeof(dp_sizes[0]));
    dpci.pPoolSizes = dp_sizes;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    pfn_vkCreateDescriptorPool(dev, &dpci, nullptr, &dpool);

    // Reusable staging buffer (32 MiB) for uploads & readback
    const VkDeviceSize kStageSize = 32ull << 20;
    VkBuffer stage_buf = VK_NULL_HANDLE;
    VkDeviceMemory stage_mem = VK_NULL_HANDLE;
    void* stage_ptr = nullptr;
    {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = kStageSize;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        pfn_vkCreateBuffer(dev, &bci, nullptr, &stage_buf);
        VkMemoryRequirements mr{};
        pfn_vkGetBufferMemoryRequirements(dev, stage_buf, &mr);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = find_mem_type(mp, mr.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
        pfn_vkAllocateMemory(dev, &mai, nullptr, &stage_mem);
        pfn_vkBindBufferMemory(dev, stage_buf, stage_mem, 0);
        pfn_vkMapMemory(dev, stage_mem, 0, kStageSize, 0, &stage_ptr);
    }

    struct ReplayMem { VkDeviceMemory mem = VK_NULL_HANDLE; uint64_t size = 0; void* mapped = nullptr; };
    std::unordered_map<uint64_t, ReplayMem> mems;
    std::unordered_map<uint64_t, VkBuffer> bufs;
    std::unordered_map<uint64_t, ImgInfo> imgs;
    std::unordered_map<uint64_t, VkImageView> img_views;
    std::unordered_map<uint64_t, uint64_t> view_to_img;
    std::unordered_map<uint64_t, VkBufferView> buf_views;
    std::unordered_map<uint64_t, VkSampler> samplers;
    std::unordered_map<uint64_t, VkShaderModule> shaders;
    std::unordered_map<uint64_t, VkDescriptorSetLayout> dsls;
    std::unordered_map<uint64_t, VkPipelineLayout> playouts;
    std::unordered_map<uint64_t, VkRenderPass> rps;
    std::unordered_map<uint64_t, VkFramebuffer> fbs;
    std::unordered_map<uint64_t, std::vector<uint64_t>> fb_imgs;
    std::unordered_map<uint64_t, VkPipeline> pipes;
    std::unordered_map<uint64_t, VkDescriptorSet> dsets;
    std::unordered_map<uint64_t, VkQueryPool> qpools;

    auto run_oneshot = [&](auto&& fn) {
        pfn_vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        pfn_vkBeginCommandBuffer(cmd, &bi);
        fn(cmd);
        pfn_vkEndCommandBuffer(cmd);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        pfn_vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        pfn_vkQueueWaitIdle(queue);
    };

    VkShaderModule magenta_fs = VK_NULL_HANDLE;
    if (getenv("VKREPLAY_MAGENTA_FS")) {
        static const uint32_t kMagentaSpv[] = {
            0x07230203,0x00010300,0x000d000b,0x0000000d,
            0x00000000,0x00020011,0x00000001,0x0006000b,
            0x00000001,0x4c534c47,0x6474732e,0x3035342e,
            0x00000000,0x0003000e,0x00000000,0x00000001,
            0x0006000f,0x00000004,0x00000004,0x6e69616d,
            0x00000000,0x00000009,0x00030010,0x00000004,
            0x00000007,0x00030003,0x00000002,0x000001c2,
            0x000a0004,0x475f4c47,0x4c474f4f,0x70635f45,
            0x74735f70,0x5f656c79,0x656e696c,0x7269645f,
            0x69746365,0x00006576,0x00080004,0x475f4c47,
            0x4c474f4f,0x6e695f45,0x64756c63,0x69645f65,
            0x74636572,0x00657669,0x00040005,0x00000004,
            0x6e69616d,0x00000000,0x00030005,0x00000009,
            0x0000306f,0x00040047,0x00000009,0x0000001e,
            0x00000000,0x00020013,0x00000002,0x00030021,
            0x00000003,0x00000002,0x00030016,0x00000006,
            0x00000020,0x00040017,0x00000007,0x00000006,
            0x00000004,0x00040020,0x00000008,0x00000003,
            0x00000007,0x0004003b,0x00000008,0x00000009,
            0x00000003,0x0004002b,0x00000006,0x0000000a,
            0x3f800000,0x0004002b,0x00000006,0x0000000b,
            0x00000000,0x0007002c,0x00000007,0x0000000c,
            0x0000000a,0x0000000b,0x0000000a,0x0000000a,
            0x00050036,0x00000002,0x00000004,0x00000000,
            0x00000003,0x000200f8,0x00000005,0x0003003e,
            0x00000009,0x0000000c,0x000100fd,0x00010038
        };
        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smci.codeSize = sizeof(kMagentaSpv);
        smci.pCode = kMagentaSpv;
        pfn_vkCreateShaderModule(dev, &smci, nullptr, &magenta_fs);
    }

    uint32_t n_submits = 0, n_draws = 0, n_draws_skipped = 0, n_rps = 0, n_pipe_fail = 0;
    uint32_t max_submit = getenv("VKREPLAY_MAX_SUBMIT") ? uint32_t(atoi(getenv("VKREPLAY_MAX_SUBMIT"))) : 0u;
    uint32_t max_rp = getenv("VKREPLAY_MAX_RP") ? uint32_t(atoi(getenv("VKREPLAY_MAX_RP"))) : 0u;
    bool in_submit = false;
    bool in_rp = false;
    bool gfx_pipe_bound = false;
    bool comp_pipe_bound = false;

    while (next_rec()) {
        if (rh.tag == REC_EOF) break;
        Reader r{payload.data(), payload.data() + payload.size()};
        switch (rh.tag) {
        case REC_ALLOC_MEM: {
            uint64_t id = r.u64(), sz = r.u64();
            uint32_t flags = r.u32(), hv = r.u32();
            VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            mai.allocationSize = sz;
            mai.memoryTypeIndex = find_mem_type(mp, 0xffffffffu, flags, hv != 0);
            ReplayMem rm; rm.size = sz;
            if (pfn_vkAllocateMemory(dev, &mai, nullptr, &rm.mem) == VK_SUCCESS) {
                if (hv) pfn_vkMapMemory(dev, rm.mem, 0, sz, 0, &rm.mapped);
                mems[id] = rm;
            }
            break;
        }
        case REC_MEM_DATA: {
            uint64_t id = r.u64(), off = r.u64(), sz = r.u64();
            auto it = mems.find(id);
            if (it != mems.end() && it->second.mapped && off + sz <= it->second.size) {
                r.take(static_cast<uint8_t*>(it->second.mapped) + off, size_t(sz));
            }
            break;
        }
        case REC_CREATE_BUFFER: {
            uint64_t id = r.u64();
            auto ci = r.pod<VkBufferCreateInfo>();
            uint64_t mid = r.u64(), moff = r.u64();
            ci.pNext = nullptr;
            ci.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            VkBuffer b = VK_NULL_HANDLE;
            if (pfn_vkCreateBuffer(dev, &ci, nullptr, &b) == VK_SUCCESS) {
                if (mid && mems.count(mid)) {
                    pfn_vkBindBufferMemory(dev, b, mems[mid].mem, moff);
                } else {
                    VkMemoryRequirements mr{};
                    pfn_vkGetBufferMemoryRequirements(dev, b, &mr);
                    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                    mai.allocationSize = mr.size;
                    mai.memoryTypeIndex = find_mem_type(mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false);
                    VkDeviceMemory m = VK_NULL_HANDLE;
                    if (pfn_vkAllocateMemory(dev, &mai, nullptr, &m) == VK_SUCCESS) pfn_vkBindBufferMemory(dev, b, m, 0);
                }
                bufs[id] = b;
            }
            break;
        }
        case REC_CREATE_IMAGE: {
            uint64_t id = r.u64();
            auto ci = r.pod<VkImageCreateInfo>();
            uint64_t mid = r.u64(), moff = r.u64();
            VkImageLayout lay = VkImageLayout(r.u32());
            ci.pNext = nullptr;
            ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            ci.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            VkImage im = VK_NULL_HANDLE;
            if (pfn_vkCreateImage(dev, &ci, nullptr, &im) == VK_SUCCESS) {
                VkMemoryRequirements mr{};
                pfn_vkGetImageMemoryRequirements(dev, im, &mr);
                // Always allocate dedicated compatible memory for images (avoids alignment mismatch on replay!)
                VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                mai.allocationSize = mr.size;
                mai.memoryTypeIndex = find_mem_type(mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false);
                VkDeviceMemory m = VK_NULL_HANDLE;
                if (pfn_vkAllocateMemory(dev, &mai, nullptr, &m) == VK_SUCCESS)
                    pfn_vkBindImageMemory(dev, im, m, 0);
                imgs[id] = {im, ci, VK_IMAGE_LAYOUT_UNDEFINED, false};
                if (lay != VK_IMAGE_LAYOUT_UNDEFINED && lay != VK_IMAGE_LAYOUT_PREINITIALIZED) {
                    VkImageAspectFlags asp = is_depth_fmt(ci.format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
                    if (ci.format == VK_FORMAT_D24_UNORM_S8_UINT || ci.format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                        ci.format == VK_FORMAT_D16_UNORM_S8_UINT)
                        asp |= VK_IMAGE_ASPECT_STENCIL_BIT;
                    run_oneshot([&](VkCommandBuffer cb) {
                        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                        b.newLayout = lay;
                        b.image = im;
                        b.subresourceRange = {asp, 0, ci.mipLevels, 0, ci.arrayLayers};
                        pfn_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                 0, 0, nullptr, 0, nullptr, 1, &b);
                    });
                    imgs[id].layout = lay;
                }
            }
            (void)mid; (void)moff;
            break;
        }
        case REC_BUFFER_UPLOAD: {
            uint64_t bid = r.u64(), dst_off = r.u64(), sz = r.u64();
            auto it = bufs.find(bid);
            if (it != bufs.end() && sz > 0 && sz <= kStageSize) {
                r.take(stage_ptr, size_t(sz));
                run_oneshot([&](VkCommandBuffer cb) {
                    VkBufferCopy reg{0, dst_off, sz};
                    pfn_vkCmdCopyBuffer(cb, stage_buf, it->second, 1, &reg);
                });
            }
            break;
        }
        case REC_IMAGE_UPLOAD: {
            uint64_t iid = r.u64();
            auto reg = r.pod<VkBufferImageCopy>();
            VkImageLayout flay = VkImageLayout(r.u32());
            uint64_t sz = r.u64();
            auto it = imgs.find(iid);
            if (it != imgs.end() && sz > 0 && sz <= kStageSize) {
                r.take(stage_ptr, size_t(sz));
                VkImage im = it->second.img;
                VkImageLayout old_lay = it->second.layout;
                VkImageLayout target_lay = (old_lay != VK_IMAGE_LAYOUT_UNDEFINED) ? old_lay : flay;
                if (target_lay == VK_IMAGE_LAYOUT_UNDEFINED) target_lay = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                run_oneshot([&](VkCommandBuffer cb) {
                    VkImageMemoryBarrier b1{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                    b1.oldLayout = old_lay;
                    b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    b1.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    b1.image = im;
                    b1.subresourceRange = {reg.imageSubresource.aspectMask, reg.imageSubresource.mipLevel, 1,
                                           reg.imageSubresource.baseArrayLayer, reg.imageSubresource.layerCount};
                    pfn_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                             0, 0, nullptr, 0, nullptr, 1, &b1);
                    pfn_vkCmdCopyBufferToImage(cb, stage_buf, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);
                    VkImageMemoryBarrier b2{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                    b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    b2.newLayout = target_lay;
                    b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT;
                    b2.image = im;
                    b2.subresourceRange = b1.subresourceRange;
                    pfn_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                             0, 0, nullptr, 0, nullptr, 1, &b2);
                });
                it->second.layout = target_lay;
            }
            break;
        }
        case REC_CREATE_IMAGE_VIEW: {
            uint64_t id = r.u64(), iid = r.u64();
            auto ci = r.pod<VkImageViewCreateInfo>();
            if (imgs.count(iid)) {
                ci.pNext = nullptr; ci.image = imgs[iid].img;
                VkImageView v = VK_NULL_HANDLE;
                if (pfn_vkCreateImageView(dev, &ci, nullptr, &v) == VK_SUCCESS) {
                    img_views[id] = v;
                    view_to_img[id] = iid;
                }
            }
            break;
        }
        case REC_CREATE_BUFFER_VIEW: {
            uint64_t id = r.u64(), bid = r.u64();
            auto ci = r.pod<VkBufferViewCreateInfo>();
            if (bufs.count(bid)) {
                ci.pNext = nullptr; ci.buffer = bufs[bid];
                VkBufferView v = VK_NULL_HANDLE;
                if (pfn_vkCreateBufferView(dev, &ci, nullptr, &v) == VK_SUCCESS) buf_views[id] = v;
            }
            break;
        }
        case REC_CREATE_SAMPLER: {
            uint64_t id = r.u64();
            auto ci = r.pod<VkSamplerCreateInfo>();
            ci.pNext = nullptr;
            VkSampler s = VK_NULL_HANDLE;
            if (pfn_vkCreateSampler(dev, &ci, nullptr, &s) == VK_SUCCESS) samplers[id] = s;
            break;
        }
        case REC_CREATE_SHADER: {
            uint64_t id = r.u64();
            uint32_t sz = r.u32();
            std::vector<uint32_t> code((sz + 3) / 4);
            r.take(code.data(), sz);
            VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            ci.codeSize = sz; ci.pCode = code.data();
            VkShaderModule m = VK_NULL_HANDLE;
            if (pfn_vkCreateShaderModule(dev, &ci, nullptr, &m) == VK_SUCCESS) shaders[id] = m;
            break;
        }
        case REC_CREATE_DSL: {
            uint64_t id = r.u64();
            uint32_t bc = r.u32();
            std::vector<VkDescriptorSetLayoutBinding> binds(bc);
            std::vector<std::vector<VkSampler>> imm(bc);
            for (uint32_t i = 0; i < bc; i++) {
                binds[i] = r.pod<VkDescriptorSetLayoutBinding>();
                uint32_t ns = r.u32();
                if (ns) {
                    imm[i].resize(ns);
                    for (uint32_t j = 0; j < ns; j++) imm[i][j] = samplers[r.u64()];
                    binds[i].pImmutableSamplers = imm[i].data();
                } else {
                    binds[i].pImmutableSamplers = nullptr;
                }
            }
            VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            ci.bindingCount = bc; ci.pBindings = binds.data();
            VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
            if (pfn_vkCreateDescriptorSetLayout(dev, &ci, nullptr, &dsl) == VK_SUCCESS) dsls[id] = dsl;
            break;
        }
        case REC_CREATE_PLAYOUT: {
            uint64_t id = r.u64();
            uint32_t sc = r.u32();
            std::vector<VkDescriptorSetLayout> sl(sc);
            for (uint32_t i = 0; i < sc; i++) sl[i] = dsls[r.u64()];
            uint32_t pc = r.u32();
            std::vector<VkPushConstantRange> pcs(pc);
            for (uint32_t i = 0; i < pc; i++) pcs[i] = r.pod<VkPushConstantRange>();
            VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            ci.setLayoutCount = sc; ci.pSetLayouts = sl.data();
            ci.pushConstantRangeCount = pc; ci.pPushConstantRanges = pcs.data();
            VkPipelineLayout pl = VK_NULL_HANDLE;
            if (pfn_vkCreatePipelineLayout(dev, &ci, nullptr, &pl) == VK_SUCCESS) playouts[id] = pl;
            break;
        }
        case REC_CREATE_RENDERPASS: {
            uint64_t id = r.u64();
            uint32_t ac = r.u32();
            std::vector<VkAttachmentDescription> atts(ac);
            for (uint32_t i = 0; i < ac; i++) atts[i] = r.pod<VkAttachmentDescription>();
            uint32_t sc = r.u32();
            std::vector<VkSubpassDescription> subs(sc);
            std::vector<std::vector<VkAttachmentReference>> inr(sc), colr(sc), resr(sc);
            std::vector<VkAttachmentReference> dsr(sc);
            std::vector<std::vector<uint32_t>> pres(sc);
            for (uint32_t i = 0; i < sc; i++) {
                subs[i].flags = r.u32();
                subs[i].pipelineBindPoint = VkPipelineBindPoint(r.u32());
                uint32_t ni = r.u32(); inr[i].resize(ni); for (uint32_t j = 0; j < ni; j++) inr[i][j] = r.pod<VkAttachmentReference>();
                uint32_t nc = r.u32(); colr[i].resize(nc); for (uint32_t j = 0; j < nc; j++) colr[i][j] = r.pod<VkAttachmentReference>();
                uint32_t nr = r.u32(); resr[i].resize(nr); for (uint32_t j = 0; j < nr; j++) resr[i][j] = r.pod<VkAttachmentReference>();
                uint32_t hd = r.u32(); if (hd) dsr[i] = r.pod<VkAttachmentReference>();
                uint32_t np = r.u32(); pres[i].resize(np); for (uint32_t j = 0; j < np; j++) pres[i][j] = r.u32();
                subs[i].inputAttachmentCount = ni; subs[i].pInputAttachments = ni ? inr[i].data() : nullptr;
                subs[i].colorAttachmentCount = nc; subs[i].pColorAttachments = nc ? colr[i].data() : nullptr;
                subs[i].pResolveAttachments = nr ? resr[i].data() : nullptr;
                subs[i].pDepthStencilAttachment = hd ? &dsr[i] : nullptr;
                subs[i].preserveAttachmentCount = np; subs[i].pPreserveAttachments = np ? pres[i].data() : nullptr;
            }
            uint32_t dc = r.u32();
            std::vector<VkSubpassDependency> deps(dc);
            for (uint32_t i = 0; i < dc; i++) deps[i] = r.pod<VkSubpassDependency>();
            VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
            ci.attachmentCount = ac; ci.pAttachments = atts.data();
            ci.subpassCount = sc; ci.pSubpasses = subs.data();
            ci.dependencyCount = dc; ci.pDependencies = deps.data();
            VkRenderPass rp = VK_NULL_HANDLE;
            if (pfn_vkCreateRenderPass(dev, &ci, nullptr, &rp) == VK_SUCCESS) rps[id] = rp;
            break;
        }
        case REC_CREATE_FRAMEBUFFER: {
            uint64_t id = r.u64(), rpid = r.u64();
            uint32_t w = r.u32(), h = r.u32(), layers = r.u32(), vc = r.u32();
            std::vector<VkImageView> vs(vc);
            std::vector<uint64_t> iids;
            for (uint32_t i = 0; i < vc; i++) {
                uint64_t vid = r.u64();
                vs[i] = img_views[vid];
                if (view_to_img.count(vid)) iids.push_back(view_to_img[vid]);
            }
            VkFramebufferCreateInfo ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            ci.renderPass = rps[rpid]; ci.attachmentCount = vc; ci.pAttachments = vs.data();
            ci.width = w; ci.height = h; ci.layers = layers;
            VkFramebuffer fb = VK_NULL_HANDLE;
            if (ci.renderPass && pfn_vkCreateFramebuffer(dev, &ci, nullptr, &fb) == VK_SUCCESS) {
                fbs[id] = fb;
                fb_imgs[id] = std::move(iids);
            }
            break;
        }
        case REC_CREATE_GPIPE: {
            uint64_t id = r.u64();
            VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            ci.flags = r.u32();
            uint32_t sc = r.u32();
            std::vector<VkPipelineShaderStageCreateInfo> stages(sc);
            std::vector<std::string> names(sc);
            std::vector<VkSpecializationInfo> specs(sc);
            std::vector<std::vector<VkSpecializationMapEntry>> smaps(sc);
            std::vector<std::vector<uint8_t>> sdata(sc);
            bool mods_ok = true;
            for (uint32_t i = 0; i < sc; i++) {
                stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                stages[i].flags = r.u32();
                stages[i].stage = VkShaderStageFlagBits(r.u32());
                stages[i].module = shaders[r.u64()];
                if (!stages[i].module) mods_ok = false;
                names[i] = r.str();
                stages[i].pName = names[i].c_str();
                if (r.u32()) {
                    uint32_t mc = r.u32();
                    smaps[i].resize(mc);
                    for (uint32_t j = 0; j < mc; j++) smaps[i][j] = r.pod<VkSpecializationMapEntry>();
                    uint32_t ds = r.u32();
                    sdata[i].resize(ds);
                    if (ds) r.take(sdata[i].data(), ds);
                    specs[i] = {mc, smaps[i].data(), ds, sdata[i].data()};
                    stages[i].pSpecializationInfo = &specs[i];
                }
                if (magenta_fs && stages[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
                    stages[i].module = magenta_fs;
                    stages[i].pName = "main";
                    stages[i].pSpecializationInfo = nullptr;
                }
            }
            ci.stageCount = sc; ci.pStages = stages.data();
            VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            VkPipelineVertexInputDivisorStateCreateInfoEXT vidiv{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT};
            std::vector<VkVertexInputBindingDescription> vbinds;
            std::vector<VkVertexInputAttributeDescription> vattrs;
            std::vector<VkVertexInputBindingDivisorDescriptionEXT> vdivs;
            if (r.u32()) {
                uint32_t bc = r.u32(); vbinds.resize(bc); for (uint32_t j = 0; j < bc; j++) vbinds[j] = r.pod<VkVertexInputBindingDescription>();
                uint32_t ac = r.u32(); vattrs.resize(ac); for (uint32_t j = 0; j < ac; j++) vattrs[j] = r.pod<VkVertexInputAttributeDescription>();
                uint32_t dc = r.u32(); vdivs.resize(dc); for (uint32_t j = 0; j < dc; j++) vdivs[j] = r.pod<VkVertexInputBindingDivisorDescriptionEXT>();
                vi.vertexBindingDescriptionCount = bc; vi.pVertexBindingDescriptions = vbinds.data();
                vi.vertexAttributeDescriptionCount = ac; vi.pVertexAttributeDescriptions = vattrs.data();
                if (dc) {
                    vidiv.vertexBindingDivisorCount = dc; vidiv.pVertexBindingDivisors = vdivs.data();
                    vi.pNext = &vidiv;
                }
                ci.pVertexInputState = &vi;
            }
            VkPipelineInputAssemblyStateCreateInfo ia{};
            if (r.u32()) { ia = r.pod<VkPipelineInputAssemblyStateCreateInfo>(); ia.pNext = nullptr; ci.pInputAssemblyState = &ia; }
            VkPipelineTessellationStateCreateInfo ts{};
            if (r.u32()) { ts = r.pod<VkPipelineTessellationStateCreateInfo>(); ts.pNext = nullptr; ci.pTessellationState = &ts; }
            VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            std::vector<VkViewport> vps; std::vector<VkRect2D> scs;
            if (r.u32()) {
                vp.viewportCount = r.u32();
                uint32_t nvp = r.u32(); vps.resize(nvp); for (uint32_t j = 0; j < nvp; j++) vps[j] = r.pod<VkViewport>();
                vp.pViewports = nvp ? vps.data() : nullptr;
                vp.scissorCount = r.u32();
                uint32_t nsc = r.u32(); scs.resize(nsc); for (uint32_t j = 0; j < nsc; j++) scs[j] = r.pod<VkRect2D>();
                vp.pScissors = nsc ? scs.data() : nullptr;
                ci.pViewportState = &vp;
            }
            VkPipelineRasterizationStateCreateInfo rs{};
            if (r.u32()) {
                rs = r.pod<VkPipelineRasterizationStateCreateInfo>(); rs.pNext = nullptr;
                if (getenv("VKREPLAY_NO_DS_CULL") || getenv("VKREPLAY_NO_CULL")) rs.cullMode = VK_CULL_MODE_NONE;
                ci.pRasterizationState = &rs;
            }
            VkPipelineMultisampleStateCreateInfo ms{};
            uint32_t sm_val = 0xffffffffu;
            if (r.u32()) {
                ms = r.pod<VkPipelineMultisampleStateCreateInfo>();
                ms.pNext = nullptr;
                uint32_t has_sm = r.u32(); sm_val = r.u32();
                ms.pSampleMask = has_sm ? &sm_val : nullptr;
                ci.pMultisampleState = &ms;
            }
            VkPipelineDepthStencilStateCreateInfo ds{};
            if (r.u32()) {
                ds = r.pod<VkPipelineDepthStencilStateCreateInfo>(); ds.pNext = nullptr;
                if (getenv("VKREPLAY_NO_DS_CULL") || getenv("VKREPLAY_NO_DEPTH")) ds.depthTestEnable = VK_FALSE;
                if (getenv("VKREPLAY_NO_DS_CULL") || getenv("VKREPLAY_NO_STENCIL")) ds.stencilTestEnable = VK_FALSE;
                ci.pDepthStencilState = &ds;
            }
            VkPipelineColorBlendStateCreateInfo cb{};
            std::vector<VkPipelineColorBlendAttachmentState> cba;
            if (r.u32()) {
                cb = r.pod<VkPipelineColorBlendStateCreateInfo>();
                cb.pNext = nullptr;
                uint32_t ac = r.u32(); cba.resize(ac); for (uint32_t j = 0; j < ac; j++) cba[j] = r.pod<VkPipelineColorBlendAttachmentState>();
                cb.attachmentCount = ac; cb.pAttachments = cba.data();
                ci.pColorBlendState = &cb;
            }
            uint32_t ndyn = r.u32();
            std::vector<VkDynamicState> dyns(ndyn);
            for (uint32_t j = 0; j < ndyn; j++) dyns[j] = VkDynamicState(r.u32());
            VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            if (ndyn) { dyn.dynamicStateCount = ndyn; dyn.pDynamicStates = dyns.data(); ci.pDynamicState = &dyn; }
            ci.layout = playouts[r.u64()];
            ci.renderPass = rps[r.u64()];
            ci.subpass = r.u32();
            VkPipeline pipe = VK_NULL_HANDLE;
            if (mods_ok && ci.layout && ci.renderPass) {
                VkResult pr = pfn_vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &pipe);
                if (pr == VK_SUCCESS && pipe) pipes[id] = pipe;
                else n_pipe_fail++;
            }
            break;
        }
        case REC_CREATE_CPIPE: {
            uint64_t id = r.u64();
            VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            ci.flags = r.u32();
            ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            ci.stage.flags = r.u32();
            ci.stage.stage = VkShaderStageFlagBits(r.u32());
            ci.stage.module = shaders[r.u64()];
            std::string nm = r.str();
            ci.stage.pName = nm.c_str();
            VkSpecializationInfo spec{};
            std::vector<VkSpecializationMapEntry> smap;
            std::vector<uint8_t> sdata;
            if (r.u32()) {
                uint32_t mc = r.u32(); smap.resize(mc); for (uint32_t j = 0; j < mc; j++) smap[j] = r.pod<VkSpecializationMapEntry>();
                uint32_t ds = r.u32(); sdata.resize(ds); if (ds) r.take(sdata.data(), ds);
                spec = {mc, smap.data(), ds, sdata.data()};
                ci.stage.pSpecializationInfo = &spec;
            }
            ci.layout = playouts[r.u64()];
            VkPipeline pipe = VK_NULL_HANDLE;
            if (ci.stage.module && ci.layout &&
                pfn_vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &pipe) == VK_SUCCESS)
                pipes[id] = pipe;
            break;
        }
        case REC_ALLOC_DSET: {
            uint64_t id = r.u64(), dsl_id = r.u64();
            if (dsls.count(dsl_id)) {
                VkDescriptorSetLayout l = dsls[dsl_id];
                VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                ai.descriptorPool = dpool; ai.descriptorSetCount = 1; ai.pSetLayouts = &l;
                VkDescriptorSet ds = VK_NULL_HANDLE;
                if (pfn_vkAllocateDescriptorSets(dev, &ai, &ds) == VK_SUCCESS) dsets[id] = ds;
            }
            break;
        }
        case REC_UPDATE_DSET: {
            uint64_t id = r.u64();
            uint32_t wc = r.u32();
            VkDescriptorSet ds = dsets[id];
            std::vector<VkWriteDescriptorSet> writes;
            std::vector<std::vector<VkDescriptorImageInfo>> img_infos(wc);
            std::vector<std::vector<VkDescriptorBufferInfo>> buf_infos(wc);
            std::vector<std::vector<VkBufferView>> tex_infos(wc);
            for (uint32_t i = 0; i < wc; i++) {
                uint32_t bind = r.u32(), elem = r.u32(), type = r.u32();
                uint32_t ni = r.u32();
                img_infos[i].resize(ni);
                bool ok = (ds != VK_NULL_HANDLE);
                for (uint32_t j = 0; j < ni; j++) {
                    uint64_t sid = r.u64(), vid = r.u64(); uint32_t lay = r.u32();
                    img_infos[i][j] = {sid ? samplers[sid] : VK_NULL_HANDLE, vid ? img_views[vid] : VK_NULL_HANDLE, VkImageLayout(lay)};
                    if ((type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                         type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) && !img_infos[i][j].imageView) ok = false;
                }
                uint32_t nb = r.u32();
                buf_infos[i].resize(nb);
                for (uint32_t j = 0; j < nb; j++) {
                    uint64_t bid = r.u64(), off = r.u64(), rng = r.u64();
                    buf_infos[i][j] = {bid ? bufs[bid] : VK_NULL_HANDLE, off, rng};
                    if (!buf_infos[i][j].buffer) ok = false;
                }
                uint32_t nt = r.u32();
                tex_infos[i].resize(nt);
                for (uint32_t j = 0; j < nt; j++) {
                    uint64_t tid = r.u64();
                    tex_infos[i][j] = tid ? buf_views[tid] : VK_NULL_HANDLE;
                    if (!tex_infos[i][j]) ok = false;
                }
                if (!ok) continue;
                VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w.dstSet = ds; w.dstBinding = bind; w.dstArrayElement = elem;
                w.descriptorType = VkDescriptorType(type);
                if (ni) { w.descriptorCount = ni; w.pImageInfo = img_infos[i].data(); }
                else if (nb) { w.descriptorCount = nb; w.pBufferInfo = buf_infos[i].data(); }
                else if (nt) { w.descriptorCount = nt; w.pTexelBufferView = tex_infos[i].data(); }
                else continue;
                writes.push_back(w);
            }
            if (!writes.empty()) pfn_vkUpdateDescriptorSets(dev, uint32_t(writes.size()), writes.data(), 0, nullptr);
            break;
        }
        case REC_CREATE_QPOOL: {
            uint64_t id = r.u64();
            auto ci = r.pod<VkQueryPoolCreateInfo>();
            ci.pNext = nullptr;
            VkQueryPool qp = VK_NULL_HANDLE;
            if (pfn_vkCreateQueryPool(dev, &ci, nullptr, &qp) == VK_SUCCESS) qpools[id] = qp;
            break;
        }
        case REC_SUBMIT_BEGIN: {
            pfn_vkResetCommandBuffer(cmd, 0);
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            pfn_vkBeginCommandBuffer(cmd, &bi);
            in_submit = true;
            in_rp = false;
            gfx_pipe_bound = false;
            comp_pipe_bound = false;
            n_submits++;
            break;
        }
        case REC_SUBMIT_END: {
            if (in_rp) { pfn_vkCmdEndRenderPass(cmd); in_rp = false; }
            if (in_submit) {
                pfn_vkEndCommandBuffer(cmd);
                VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                si.commandBufferCount = 1;
                si.pCommandBuffers = &cmd;
                pfn_vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
                pfn_vkQueueWaitIdle(queue);
                in_submit = false;
                if (max_submit && n_submits >= max_submit) goto done_replay;
            }
            break;
        }
        case REC_CMD_BEGIN_RP: {
            uint64_t rpid = r.u64(), fbid = r.u64();
            auto area = r.pod<VkRect2D>();
            uint32_t cc = r.u32();
            std::vector<VkClearValue> clears(cc);
            for (uint32_t i = 0; i < cc; i++) clears[i] = r.pod<VkClearValue>();
            auto contents = VkSubpassContents(r.u32());
            if (in_submit && rps.count(rpid) && fbs.count(fbid)) {
                VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
                bi.renderPass = rps[rpid]; bi.framebuffer = fbs[fbid];
                bi.renderArea = area; bi.clearValueCount = cc; bi.pClearValues = clears.data();
                pfn_vkCmdBeginRenderPass(cmd, &bi, contents);
                in_rp = true;
                n_rps++;
                for (uint64_t iid : fb_imgs[fbid]) {
                    if (imgs.count(iid) && !is_depth_fmt(imgs[iid].ci.format)) imgs[iid].drawn = true;
                }
            }
            break;
        }
        case REC_CMD_END_RP:
            if (in_submit && in_rp) {
                pfn_vkCmdEndRenderPass(cmd);
                in_rp = false;
                if (max_rp && n_rps >= max_rp) {
                    pfn_vkEndCommandBuffer(cmd);
                    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                    si.commandBufferCount = 1;
                    si.pCommandBuffers = &cmd;
                    pfn_vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
                    pfn_vkQueueWaitIdle(queue);
                    in_submit = false;
                    goto done_replay;
                }
            }
            break;
        case REC_CMD_NEXT_SUBPASS:
            if (in_submit && in_rp) pfn_vkCmdNextSubpass(cmd, VkSubpassContents(r.u32()));
            break;
        case REC_CMD_BIND_PIPE: {
            auto bp = VkPipelineBindPoint(r.u32()); uint64_t pid = r.u64();
            if (in_submit && pipes.count(pid) && pipes[pid]) {
                pfn_vkCmdBindPipeline(cmd, bp, pipes[pid]);
                if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS) gfx_pipe_bound = true;
                else if (bp == VK_PIPELINE_BIND_POINT_COMPUTE) comp_pipe_bound = true;
            } else {
                if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS) gfx_pipe_bound = false;
                else if (bp == VK_PIPELINE_BIND_POINT_COMPUTE) comp_pipe_bound = false;
            }
            break;
        }
        case REC_CMD_BIND_DSETS: {
            auto bp = VkPipelineBindPoint(r.u32()); uint64_t pl = r.u64();
            uint32_t fs = r.u32(), sc = r.u32();
            std::vector<VkDescriptorSet> ss(sc);
            bool ok = playouts.count(pl) > 0;
            for (uint32_t i = 0; i < sc; i++) { ss[i] = dsets[r.u64()]; if (!ss[i]) ok = false; }
            uint32_t dc = r.u32();
            std::vector<uint32_t> doff(dc);
            for (uint32_t i = 0; i < dc; i++) doff[i] = r.u32();
            if (in_submit && ok && sc) pfn_vkCmdBindDescriptorSets(cmd, bp, playouts[pl], fs, sc, ss.data(), dc, dc ? doff.data() : nullptr);
            break;
        }
        case REC_CMD_BIND_VB: {
            uint32_t fb = r.u32(), cnt = r.u32();
            std::vector<VkBuffer> bs(cnt); std::vector<VkDeviceSize> offs(cnt);
            bool ok = true;
            for (uint32_t i = 0; i < cnt; i++) { bs[i] = bufs[r.u64()]; offs[i] = r.u64(); if (!bs[i]) ok = false; }
            if (in_submit && ok && cnt) pfn_vkCmdBindVertexBuffers(cmd, fb, cnt, bs.data(), offs.data());
            break;
        }
        case REC_CMD_BIND_IB: {
            uint64_t bid = r.u64(), off = r.u64(); auto it = VkIndexType(r.u32());
            if (in_submit && bufs.count(bid)) pfn_vkCmdBindIndexBuffer(cmd, bufs[bid], off, it);
            break;
        }
        case REC_CMD_PUSH_CONST: {
            uint64_t pl = r.u64(); uint32_t st = r.u32(), off = r.u32(), sz = r.u32();
            std::vector<uint8_t> d(sz); r.take(d.data(), sz);
            if (in_submit && playouts.count(pl)) pfn_vkCmdPushConstants(cmd, playouts[pl], st, off, sz, d.data());
            break;
        }
        case REC_CMD_SET_VIEWPORT: {
            uint32_t f = r.u32(), c = r.u32();
            std::vector<VkViewport> v(c); for (uint32_t i = 0; i < c; i++) v[i] = r.pod<VkViewport>();
            if (in_submit && c) pfn_vkCmdSetViewport(cmd, f, c, v.data());
            break;
        }
        case REC_CMD_SET_SCISSOR: {
            uint32_t f = r.u32(), c = r.u32();
            std::vector<VkRect2D> v(c); for (uint32_t i = 0; i < c; i++) v[i] = r.pod<VkRect2D>();
            if (in_submit && c) pfn_vkCmdSetScissor(cmd, f, c, v.data());
            break;
        }
        case REC_CMD_SET_BLEND_CONST: {
            float c[4]; r.take(c, 16);
            if (in_submit) pfn_vkCmdSetBlendConstants(cmd, c);
            break;
        }
        case REC_CMD_SET_DEPTH_BIAS: {
            float a = r.f32(), b = r.f32(), c = r.f32();
            if (in_submit) pfn_vkCmdSetDepthBias(cmd, a, b, c);
            break;
        }
        case REC_CMD_SET_DEPTH_BOUNDS: {
            float a = r.f32(), b = r.f32();
            if (in_submit) pfn_vkCmdSetDepthBounds(cmd, a, b);
            break;
        }
        case REC_CMD_SET_STENCIL_REF: { uint32_t f = r.u32(), v = r.u32(); if (in_submit) pfn_vkCmdSetStencilReference(cmd, f, v); break; }
        case REC_CMD_SET_STENCIL_CMP: { uint32_t f = r.u32(), v = r.u32(); if (in_submit) pfn_vkCmdSetStencilCompareMask(cmd, f, v); break; }
        case REC_CMD_SET_STENCIL_WRITE: { uint32_t f = r.u32(), v = r.u32(); if (in_submit) pfn_vkCmdSetStencilWriteMask(cmd, f, v); break; }
        case REC_CMD_DRAW: {
            uint32_t a = r.u32(), b = r.u32(), c = r.u32(), d = r.u32();
            if (in_submit && in_rp && gfx_pipe_bound) { pfn_vkCmdDraw(cmd, a, b, c, d); n_draws++; }
            else n_draws_skipped++;
            break;
        }
        case REC_CMD_DRAW_INDEXED: {
            uint32_t a = r.u32(), b = r.u32(), c = r.u32(); int32_t d = r.i32(); uint32_t e = r.u32();
            if (in_submit && in_rp && gfx_pipe_bound) { pfn_vkCmdDrawIndexed(cmd, a, b, c, d, e); n_draws++; }
            else n_draws_skipped++;
            break;
        }
        case REC_CMD_DISPATCH: {
            uint32_t x = r.u32(), y = r.u32(), z = r.u32();
            if (in_submit && !in_rp && comp_pipe_bound) pfn_vkCmdDispatch(cmd, x, y, z);
            break;
        }
        case REC_CMD_CLEAR_ATT: {
            uint32_t ac = r.u32(); std::vector<VkClearAttachment> a(ac); for (uint32_t i = 0; i < ac; i++) a[i] = r.pod<VkClearAttachment>();
            uint32_t rc = r.u32(); std::vector<VkClearRect> rects(rc); for (uint32_t i = 0; i < rc; i++) rects[i] = r.pod<VkClearRect>();
            if (in_submit && in_rp && ac && rc) pfn_vkCmdClearAttachments(cmd, ac, a.data(), rc, rects.data());
            break;
        }
        case REC_CMD_CLEAR_COLOR: {
            uint64_t iid = r.u64(); auto lay = VkImageLayout(r.u32()); auto col = r.pod<VkClearColorValue>();
            uint32_t rc = r.u32(); std::vector<VkImageSubresourceRange> rng(rc); for (uint32_t i = 0; i < rc; i++) rng[i] = r.pod<VkImageSubresourceRange>();
            if (in_submit && !in_rp && imgs.count(iid) && rc) pfn_vkCmdClearColorImage(cmd, imgs[iid].img, lay, &col, rc, rng.data());
            break;
        }
        case REC_CMD_CLEAR_DS: {
            uint64_t iid = r.u64(); auto lay = VkImageLayout(r.u32()); auto ds = r.pod<VkClearDepthStencilValue>();
            uint32_t rc = r.u32(); std::vector<VkImageSubresourceRange> rng(rc); for (uint32_t i = 0; i < rc; i++) rng[i] = r.pod<VkImageSubresourceRange>();
            if (in_submit && !in_rp && imgs.count(iid) && rc) pfn_vkCmdClearDepthStencilImage(cmd, imgs[iid].img, lay, &ds, rc, rng.data());
            break;
        }
        case REC_CMD_COPY_BUF: {
            uint64_t s = r.u64(), d = r.u64(); uint32_t rc = r.u32();
            std::vector<VkBufferCopy> regs(rc); for (uint32_t i = 0; i < rc; i++) regs[i] = r.pod<VkBufferCopy>();
            if (in_submit && !in_rp && bufs.count(s) && bufs.count(d) && rc) pfn_vkCmdCopyBuffer(cmd, bufs[s], bufs[d], rc, regs.data());
            break;
        }
        case REC_CMD_COPY_BUF_TO_IMG: {
            uint64_t s = r.u64(), d = r.u64(); auto lay = VkImageLayout(r.u32()); uint32_t rc = r.u32();
            std::vector<VkBufferImageCopy> regs(rc); for (uint32_t i = 0; i < rc; i++) regs[i] = r.pod<VkBufferImageCopy>();
            if (in_submit && !in_rp && bufs.count(s) && imgs.count(d) && rc) pfn_vkCmdCopyBufferToImage(cmd, bufs[s], imgs[d].img, lay, rc, regs.data());
            break;
        }
        case REC_CMD_COPY_IMG: {
            uint64_t s = r.u64(); auto sl = VkImageLayout(r.u32()); uint64_t d = r.u64(); auto dl = VkImageLayout(r.u32());
            uint32_t rc = r.u32(); std::vector<VkImageCopy> regs(rc); for (uint32_t i = 0; i < rc; i++) regs[i] = r.pod<VkImageCopy>();
            if (in_submit && !in_rp && imgs.count(s) && imgs.count(d) && rc) {
                pfn_vkCmdCopyImage(cmd, imgs[s].img, sl, imgs[d].img, dl, rc, regs.data());
                if (!is_depth_fmt(imgs[d].ci.format)) imgs[d].drawn = true;
            }
            break;
        }
        case REC_CMD_BLIT_IMG: {
            uint64_t s = r.u64(); auto sl = VkImageLayout(r.u32()); uint64_t d = r.u64(); auto dl = VkImageLayout(r.u32());
            auto flt = VkFilter(r.u32()); uint32_t rc = r.u32();
            std::vector<VkImageBlit> regs(rc); for (uint32_t i = 0; i < rc; i++) regs[i] = r.pod<VkImageBlit>();
            if (in_submit && !in_rp && imgs.count(s) && imgs.count(d) && rc) {
                pfn_vkCmdBlitImage(cmd, imgs[s].img, sl, imgs[d].img, dl, rc, regs.data(), flt);
                if (!is_depth_fmt(imgs[d].ci.format)) imgs[d].drawn = true;
            }
            break;
        }
        case REC_CMD_RESOLVE_IMG: {
            uint64_t s = r.u64(); auto sl = VkImageLayout(r.u32()); uint64_t d = r.u64(); auto dl = VkImageLayout(r.u32());
            uint32_t rc = r.u32(); std::vector<VkImageResolve> regs(rc); for (uint32_t i = 0; i < rc; i++) regs[i] = r.pod<VkImageResolve>();
            if (in_submit && !in_rp && imgs.count(s) && imgs.count(d) && rc) {
                pfn_vkCmdResolveImage(cmd, imgs[s].img, sl, imgs[d].img, dl, rc, regs.data());
                imgs[d].drawn = true;
            }
            break;
        }
        case REC_CMD_UPDATE_BUF: {
            uint64_t d = r.u64(), off = r.u64(), sz = r.u64();
            std::vector<uint8_t> data(static_cast<size_t>(sz)); r.take(data.data(), static_cast<size_t>(sz));
            if (in_submit && !in_rp && bufs.count(d)) pfn_vkCmdUpdateBuffer(cmd, bufs[d], off, sz, data.data());
            break;
        }
        case REC_CMD_FILL_BUF: {
            uint64_t d = r.u64(), off = r.u64(), sz = r.u64(); uint32_t val = r.u32();
            if (in_submit && !in_rp && bufs.count(d)) pfn_vkCmdFillBuffer(cmd, bufs[d], off, sz, val);
            break;
        }
        case REC_CMD_BARRIER: {
            uint32_t ss = r.u32(), ds = r.u32(), df = r.u32();
            uint32_t mc = r.u32(); std::vector<VkMemoryBarrier> mb(mc); for (uint32_t i = 0; i < mc; i++) mb[i] = r.pod<VkMemoryBarrier>();
            uint32_t bc = r.u32(); std::vector<VkBufferMemoryBarrier> bb;
            for (uint32_t i = 0; i < bc; i++) {
                uint64_t bid = r.u64(); auto b = r.pod<VkBufferMemoryBarrier>();
                if (bufs.count(bid)) { b.buffer = bufs[bid]; bb.push_back(b); }
            }
            uint32_t ic = r.u32(); std::vector<VkImageMemoryBarrier> ib;
            for (uint32_t i = 0; i < ic; i++) {
                uint64_t iid = r.u64(); auto b = r.pod<VkImageMemoryBarrier>();
                if (imgs.count(iid)) {
                    b.image = imgs[iid].img;
                    b.oldLayout = imgs[iid].layout;
                    imgs[iid].layout = b.newLayout;
                    ib.push_back(b);
                }
            }
            if (in_submit) pfn_vkCmdPipelineBarrier(cmd, ss, ds, df, mc, mc ? mb.data() : nullptr,
                                                    uint32_t(bb.size()), bb.empty() ? nullptr : bb.data(),
                                                    uint32_t(ib.size()), ib.empty() ? nullptr : ib.data());
            break;
        }
        case REC_CMD_QUERY_RESET: { uint64_t q = r.u64(); uint32_t f = r.u32(), c = r.u32(); if (in_submit && !in_rp && qpools.count(q)) pfn_vkCmdResetQueryPool(cmd, qpools[q], f, c); break; }
        case REC_CMD_QUERY_BEGIN: { uint64_t q = r.u64(); uint32_t qi = r.u32(), fl = r.u32(); if (in_submit && qpools.count(q)) pfn_vkCmdBeginQuery(cmd, qpools[q], qi, fl); break; }
        case REC_CMD_QUERY_END: { uint64_t q = r.u64(); uint32_t qi = r.u32(); if (in_submit && qpools.count(q)) pfn_vkCmdEndQuery(cmd, qpools[q], qi); break; }
        default: break;
        }
    }
done_replay:
    fclose(fp);
    pfn_vkDeviceWaitIdle(dev);

    if (getenv("VKREPLAY_CHECK_BCN")) {
        int bcn_checked = 0;
        for (auto& kv : imgs) {
            auto& ii = kv.second;
            if (ii.ci.format < 131 || ii.ci.format > 146) continue;
            uint32_t w = ii.ci.extent.width, h = ii.ci.extent.height;
            if (w < 64 || h < 64 || uint64_t(w) * h * 4 > kStageSize) continue;
            run_oneshot([&](VkCommandBuffer cb) {
                VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                b.oldLayout = ii.layout; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                b.image = ii.img; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                pfn_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
                VkBufferImageCopy reg{};
                reg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                reg.imageExtent = {w, h, 1};
                pfn_vkCmdCopyImageToBuffer(cb, ii.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stage_buf, 1, &reg);
            });
            const auto* px = static_cast<const uint8_t*>(stage_ptr);
            uint64_t nb = 0, sum = 0;
            for (uint64_t i = 0; i < uint64_t(w) * h; i++) {
                if (px[i*4] || px[i*4+1] || px[i*4+2]) nb++;
                sum += (px[i*4] + px[i*4+1] + px[i*4+2]) / 3;
            }
            printf("  BCN img=%llu (%ux%u fmt=%d): non_black=%llu/%llu (%.1f%%) avg_luma=%.1f\n",
                   (unsigned long long)kv.first, w, h, int(ii.ci.format),
                   (unsigned long long)nb, (unsigned long long)(uint64_t(w)*h),
                   100.0 * double(nb) / double(uint64_t(w)*h), double(sum) / double(uint64_t(w)*h));
            if (++bcn_checked >= 8) break;
        }
    }

    // List all drawn images and dump every drawn 8-bit/16f color render target to PPM!
    int rt_idx = 0;
    printf("vkreplay: objects: mem=%zu buf=%zu img=%zu sh=%zu pipe=%zu rp=%zu fb=%zu ds=%zu (draws=%u skipped=%u)\n",
           mems.size(), bufs.size(), imgs.size(), shaders.size(), pipes.size(), rps.size(), fbs.size(), dsets.size(),
           n_draws, n_draws_skipped);
    for (auto& kv : imgs) {
        auto& ii = kv.second;
        if (!ii.drawn) continue;
        uint32_t w = ii.ci.extent.width, h = ii.ci.extent.height;
        bool is_bgra = (ii.ci.format == VK_FORMAT_B8G8R8A8_UNORM || ii.ci.format == VK_FORMAT_B8G8R8A8_SRGB);
        bool is_rgba = (ii.ci.format == VK_FORMAT_R8G8B8A8_UNORM || ii.ci.format == VK_FORMAT_R8G8B8A8_SRGB);
        bool is_fp16 = (ii.ci.format == VK_FORMAT_R16G16B16A16_SFLOAT);
        if (ii.ci.samples != VK_SAMPLE_COUNT_1_BIT || w < 64 || h < 64 || (!is_bgra && !is_rgba && !is_fp16)) {
            printf("  drawn img=%llu: %ux%u fmt=%d samples=%u (not dumped)\n",
                   (unsigned long long)kv.first, w, h, int(ii.ci.format), unsigned(ii.ci.samples));
            continue;
        }
        uint64_t bpp = is_fp16 ? 8u : 4u;
        uint64_t bytes = uint64_t(w) * h * bpp;
        if (bytes > kStageSize) continue;
        run_oneshot([&](VkCommandBuffer cb) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.oldLayout = ii.layout;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            b.image = ii.img;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            pfn_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     0, 0, nullptr, 0, nullptr, 1, &b);
            VkBufferImageCopy reg{};
            reg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            reg.imageExtent = {w, h, 1};
            pfn_vkCmdCopyImageToBuffer(cb, ii.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stage_buf, 1, &reg);
        });
        uint64_t non_black = 0, sum_luma = 0;
        std::vector<uint8_t> rgb(size_t(w) * h * 3);
        if (is_fp16) {
            const auto* hp = static_cast<const uint16_t*>(stage_ptr);
            auto to8 = [](float v) -> uint8_t {
                if (!(v > 0.0f)) return 0;
                if (v >= 1.0f) return 255;
                return uint8_t(v * 255.0f + 0.5f);
            };
            for (uint64_t i = 0; i < uint64_t(w) * h; i++) {
                uint8_t r8 = to8(half_to_float(hp[i * 4 + 0]));
                uint8_t g8 = to8(half_to_float(hp[i * 4 + 1]));
                uint8_t b8 = to8(half_to_float(hp[i * 4 + 2]));
                if (r8 || g8 || b8) non_black++;
                sum_luma += (r8 + g8 + b8) / 3;
                rgb[i * 3 + 0] = r8; rgb[i * 3 + 1] = g8; rgb[i * 3 + 2] = b8;
            }
        } else {
            const auto* px = static_cast<const uint8_t*>(stage_ptr);
            for (uint64_t i = 0; i < uint64_t(w) * h; i++) {
                uint8_t r8 = is_bgra ? px[i * 4 + 2] : px[i * 4 + 0];
                uint8_t g8 = px[i * 4 + 1];
                uint8_t b8 = is_bgra ? px[i * 4 + 0] : px[i * 4 + 2];
                if (r8 || g8 || b8) non_black++;
                sum_luma += (r8 + g8 + b8) / 3;
                rgb[i * 3 + 0] = r8; rgb[i * 3 + 1] = g8; rgb[i * 3 + 2] = b8;
            }
        }
        std::string ppm = out_prefix + "_rt" + std::to_string(rt_idx) + ".ppm";
        FILE* out = fopen(ppm.c_str(), "wb");
        if (out) {
            fprintf(out, "P6\n%u %u\n255\n", w, h);
            fwrite(rgb.data(), 1, rgb.size(), out);
            fclose(out);
        }
        printf("  RT%d (img=%llu): %ux%u fmt=%d non_black=%llu/%llu (%.1f%%) avg_luma=%.1f -> %s\n",
               rt_idx, (unsigned long long)kv.first, w, h, int(ii.ci.format),
               (unsigned long long)non_black, (unsigned long long)(uint64_t(w) * h),
               100.0 * double(non_black) / double(uint64_t(w) * h),
               double(sum_luma) / double(uint64_t(w) * h), ppm.c_str());
        rt_idx++;
    }

    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();
    printf("vkreplay: DONE in %.2fs (%u submits, %u renderpasses, %u draws, %u pipeline_fails, %d RTs dumped)\n",
           secs, n_submits, n_rps, n_draws, n_pipe_fail, rt_idx);
    return 0;
}
