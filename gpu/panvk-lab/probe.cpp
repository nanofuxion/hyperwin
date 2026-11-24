// What DXVK 3.1.1 needs from a Vulkan driver, checked on the real GPU.
//   probe /path/to/libvulkan_panfrost.so
// Loads the driver the way gfxstream does, lists what it reports, then creates a device with every
// feature DXVK refuses to start without (gpu/work/dxvk/src/dxvk/dxvk_device_info.cpp, ENABLE_* true).
// Exit code is the number of missing required things, so a script can compare drivers.
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include <vulkan/vulkan.h>

#define LOADI(n) auto n = (PFN_##n)gipa(inst, #n)

struct Feat { const char* what; VkBool32* p; };

int main(int argc, char** argv) {
    if (argc < 2) { puts("usage: probe driver.so"); return 99; }
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { printf("FAIL cannot load %s: %s\n", argv[1], dlerror()); return 98; }
    auto gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
    if (!gipa) { puts("FAIL no vk_icdGetInstanceProcAddr"); return 98; }
    VkInstance inst = nullptr;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "probe", 1, "probe", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app};
    auto create = (PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance");
    VkResult r = create(&ici, nullptr, &inst);
    if (r) { printf("FAIL vkCreateInstance %d\n", r); return 97; }
    LOADI(vkEnumeratePhysicalDevices);
    LOADI(vkGetPhysicalDeviceProperties2);
    LOADI(vkGetPhysicalDeviceFeatures2);
    LOADI(vkGetPhysicalDeviceMemoryProperties);
    LOADI(vkEnumerateDeviceExtensionProperties);
    LOADI(vkCreateDevice);
    uint32_t n = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (!n) { puts("FAIL no physical device"); return 96; }

    VkPhysicalDeviceVulkan11Properties p11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
    VkPhysicalDeviceVulkan12Properties p12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES, &p11};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p12};
    vkGetPhysicalDeviceProperties2(pd, &props);
    auto& L = props.properties.limits;
    printf("device  %s\napi     %u.%u.%u  driver 0x%x  id %d\ndriver  %s | %s\n", props.properties.deviceName,
           VK_VERSION_MAJOR(props.properties.apiVersion), VK_VERSION_MINOR(props.properties.apiVersion),
           VK_VERSION_PATCH(props.properties.apiVersion), props.properties.driverVersion, p12.driverID,
           p12.driverName, p12.driverInfo);
    printf("limits  subgroup %u  maxImage2D %u  maxBound %u  uboRange %u  ssboRange %u  pushConst %u  maxMem %u\n",
           p11.subgroupSize, L.maxImageDimension2D, L.maxBoundDescriptorSets, L.maxUniformBufferRange,
           L.maxStorageBufferRange, L.maxPushConstantsSize, L.maxMemoryAllocationCount);
    printf("points  pointSizeRange %g..%g  granularity %g  largePoints in features below\n", L.pointSizeRange[0], L.pointSizeRange[1], L.pointSizeGranularity);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryHeapCount; i++)
        printf("heap %u  %llu MB%s\n", i, (unsigned long long)(mp.memoryHeaps[i].size >> 20),
               mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? "  device-local" : "");

    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &ne, nullptr);
    std::vector<VkExtensionProperties> ext(ne);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &ne, ext.data());
    auto has = [&](const char* name) { for (auto& e : ext) if (!strcmp(e.extensionName, name)) return true; return false; };
    printf("%u device extensions\n", ne);
    int missing = 0;
    // Required by DXVK 3.1.1 (the *_true rows of ENABLE_EXT) plus the ones its features sit behind.
    const char* need[] = {"VK_KHR_swapchain", "VK_KHR_load_store_op_none", "VK_EXT_depth_clip_enable",
                          "VK_EXT_robustness2", "VK_KHR_maintenance5", "VK_KHR_maintenance6"};
    // DXVK works without these but loses features or speed; vkd3d-proton insists on some of them.
    const char* want[] = {"VK_EXT_transform_feedback", "VK_EXT_vertex_attribute_divisor",
                          "VK_KHR_vertex_attribute_divisor", "VK_EXT_custom_border_color",
                          "VK_EXT_extended_dynamic_state3", "VK_EXT_graphics_pipeline_library",
                          "VK_EXT_conservative_rasterization", "VK_EXT_memory_budget", "VK_KHR_present_wait",
                          "VK_EXT_shader_stencil_export", "VK_EXT_fragment_shader_interlock",
                          "VK_EXT_descriptor_buffer", "VK_EXT_device_fault", "VK_EXT_pipeline_robustness",
                          "VK_KHR_pipeline_binary", "VK_KHR_push_descriptor", "VK_EXT_swapchain_maintenance1"};
    for (auto* x : need) if (!has(x)) { printf("MISSING ext (required)  %s\n", x); missing++; }
    for (auto* x : want) if (!has(x)) printf("missing ext (optional)  %s\n", x);

    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceDepthClipEnableFeaturesEXT fdc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT};
    VkPhysicalDeviceRobustness2FeaturesEXT frb{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    VkPhysicalDeviceMaintenance5FeaturesKHR fm5{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
    VkPhysicalDeviceMaintenance6FeaturesKHR fm6{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    // Chain only what the driver lists, so a missing extension never sees its struct.
    void* head = nullptr;
    auto push = [&](void* s) { ((VkBaseOutStructure*)s)->pNext = (VkBaseOutStructure*)head; head = s; };
    push(&f11); push(&f12); push(&f13);
    if (has("VK_EXT_depth_clip_enable")) push(&fdc);
    if (has("VK_EXT_robustness2")) push(&frb);
    if (has("VK_KHR_maintenance5")) push(&fm5);
    if (has("VK_KHR_maintenance6")) push(&fm6);
    f2.pNext = head;
    vkGetPhysicalDeviceFeatures2(pd, &f2);

    VkPhysicalDeviceFeatures& c = f2.features;
#define C(x) {"core." #x, &c.x}
#define V1(x) {"vk11." #x, &f11.x}
#define V2(x) {"vk12." #x, &f12.x}
#define V3(x) {"vk13." #x, &f13.x}
    Feat req[] = {
        C(depthBiasClamp), C(depthClamp), C(dualSrcBlend), C(fillModeNonSolid), C(fragmentStoresAndAtomics),
        C(fullDrawIndexUint32), C(geometryShader), C(imageCubeArray), C(independentBlend), C(multiDrawIndirect),
        C(multiViewport), C(occlusionQueryPrecise), C(robustBufferAccess), C(sampleRateShading),
        C(samplerAnisotropy), C(shaderClipDistance), C(shaderCullDistance), C(shaderImageGatherExtended),
        C(shaderInt16), C(shaderInt64), C(shaderSampledImageArrayDynamicIndexing), C(textureCompressionBC),
        V1(shaderDrawParameters), V1(storageBuffer16BitAccess),
        V2(bufferDeviceAddress), V2(descriptorIndexing), V2(storageBuffer8BitAccess),
        V2(descriptorBindingSampledImageUpdateAfterBind), V2(descriptorBindingUpdateUnusedWhilePending),
        V2(descriptorBindingPartiallyBound), V2(hostQueryReset), V2(runtimeDescriptorArray),
        V2(samplerMirrorClampToEdge), V2(scalarBlockLayout), V2(shaderInt8), V2(timelineSemaphore),
        V2(uniformBufferStandardLayout), V2(vulkanMemoryModel),
        V3(inlineUniformBlock), V3(computeFullSubgroups), V3(dynamicRendering), V3(maintenance4),
        V3(shaderDemoteToHelperInvocation), V3(shaderZeroInitializeWorkgroupMemory), V3(subgroupSizeControl),
        V3(synchronization2),
        {"depthClipEnable", &fdc.depthClipEnable}, {"robustBufferAccess2", &frb.robustBufferAccess2},
        {"nullDescriptor", &frb.nullDescriptor}, {"maintenance5", &fm5.maintenance5},
        {"maintenance6", &fm6.maintenance6},
    };
    for (auto& f : req) {
        if (!*f.p) { printf("MISSING feature (required)  %s\n", f.what); missing++; }
    }
    // Tessellation and the rest: DXVK can run without, D3D11 feature level 11_0 cannot.
    if (!c.tessellationShader) puts("missing feature (optional)  core.tessellationShader (feature level 11_0 needs it)");
    if (!c.shaderFloat64) puts("missing feature (optional)  core.shaderFloat64");
    if (!c.depthBounds) puts("missing feature (optional)  core.depthBounds");

    // Create the device exactly like DXVK: only what the driver supports, extensions it lists.
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, 0, 1, &prio};
    std::vector<const char*> en;
    for (auto* x : need) if (has(x)) en.push_back(x);
    for (auto* x : want) if (has(x)) en.push_back(x);
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, 0, 1, &qci, 0, nullptr,
                           (uint32_t)en.size(), en.data(), nullptr};
    VkDevice dev = nullptr;
    r = vkCreateDevice(pd, &dci, nullptr, &dev);
    printf("vkCreateDevice with %zu extensions and every supported feature: %s (%d)\n", en.size(),
           r ? "FAIL" : "ok", r);
    if (r) missing++;
    printf("%s: %d required things missing\n", missing ? "FAIL" : "PASS", missing);
    return missing;
}
