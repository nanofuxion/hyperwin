// libvulkan_vortek.so: the Mali workarounds of Vortek's renderer (Winlator's vortekrenderer,
// brunodev85/winlator-app, LGPL-2.1), under gfxstream instead of behind Vortek's own transport.
//
// From Vortek's vulkan_helper.c and request_handler.c, redone as driver hooks:
//   - features DXVK wants but Mali lacks are reported, then dropped when the device is created
//   - memory heaps are reported at most 2/3 of the largest, as Vortek does by default
//   - USCALED/SSCALED vertex formats are reported usable; pipelines read them as integers and
//     the vertex shader converts (Vortek's convertFormatScaled)
//   - extensions are hidden per engine and driver, as Vortek's setupExposedDeviceExtensions does
//   - vkd3d gets Vortek's subgroup and texel alignment answers
// Vortek's SPIR-V passes run unchanged: its shader_inspector.c is built into this library
// (clip distance, image bound checks on Mali with DXVK, point size export and in/out sizes for
// DXVK after 2.3.1). Vertex and fragment modules are kept as code until a pipeline needs them,
// because the scaled-format fix depends on the pipeline's vertex input.
//
// BC textures: Vortek decodes BC1 to BC5 on the CPU. bcn_layer, loaded under this wrapper,
// decodes BC1 to BC7 on the GPU instead (see bcn.cpp for why copy2 is turned into copy).
#include <string.h>

#include <algorithm>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "wrap.h"

// Vortek's shader inspector (vortek_shim/ stands in for the headers it expects).
extern "C" {
struct VkContext {
    VkDriverId driverID;
    int engineID;
    uint32_t engineVersion;
};
struct ShaderModule {
    VkShaderModule module;
    uint32_t* code;
    size_t codeSize;
};
struct ShaderInspector {
    bool checkClipDistance, convertFormatScaled, removeImageBoundCheck, removePointSizeExport,
        checkInOutVariablesSize;
};
struct VulkanWrapper {
    PFN_vkCreateShaderModule vkCreateShaderModule;
};
ShaderInspector* ShaderInspector_create(VkContext* context, VkPhysicalDevice pd, VkPhysicalDeviceFeatures* supported);
VkResult ShaderInspector_createModule(ShaderInspector* si, VkDevice device, const uint32_t* code, size_t codeSize,
                                      ShaderModule** out);
VkResult ShaderInspector_inspectShaderStages(ShaderInspector* si, VkDevice device, VkPipelineShaderStageCreateInfo* stages,
                                             uint32_t stageCount, const VkPipelineVertexInputStateCreateInfo* vertexInput);
bool isFormatScaled(VkFormat format);

VKAPI_ATTR VkResult VKAPI_CALL forward_create_shader_module(VkDevice dev, const VkShaderModuleCreateInfo* info,
                                                           const VkAllocationCallbacks* alloc, VkShaderModule* out) {
    return NEXT_D(dev, vkCreateShaderModule)(dev, info, alloc, out);
}
VulkanWrapper vulkanWrapper = {forward_create_shader_module};
}

namespace {

constexpr int kEngineUnknown = 0, kEngineDXVK = 1, kEngineVKD3D = 2, kEngineZink = 3;
constexpr uint32_t engine_version(uint32_t a, uint32_t b, uint32_t c) { return a << 22 | b << 12 | c; }

struct Engine {
    int id = kEngineUnknown;
    uint32_t version = 0;
};

std::mutex g_lock;
std::unordered_map<VkInstance, Engine> g_engine;
std::unordered_map<VkDevice, ShaderInspector*> g_inspector;

Engine engine_of(VkPhysicalDevice pd) {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_engine.find(wrap::instance_of(pd));
    return it == g_engine.end() ? Engine() : it->second;
}

VkDriverId driver_of(VkPhysicalDevice pd) {
    VkPhysicalDeviceDriverProperties driver{};
    driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    VkPhysicalDeviceProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props.pNext = &driver;
    NEXT_I(wrap::instance_of(pd), vkGetPhysicalDeviceProperties2)(pd, &props);
    return driver.driverID;
}

bool contains_ci(const char* hay, const char* needle) {
    std::string h = hay, n = needle;
    std::transform(h.begin(), h.end(), h.begin(), ::tolower);
    return h.find(n) != std::string::npos;
}

const void* find_struct(const void* chain, VkStructureType type) {
    for (auto* s = static_cast<const VkBaseInStructure*>(chain); s; s = s->pNext)
        if (s->sType == type) return s;
    return nullptr;
}

// Extensions this wrapper hides. The first four would let shaders reach the driver without
// passing the inspector. The rest follow Vortek's setupExposedDeviceExtensions.
std::set<std::string> hidden_extensions(VkPhysicalDevice pd) {
    std::set<std::string> hide = {"VK_EXT_graphics_pipeline_library", "VK_KHR_pipeline_library",
                                  "VK_EXT_shader_module_identifier", "VK_EXT_shader_object"};
    Engine e = engine_of(pd);
    VkDriverId d = driver_of(pd);
    if (e.id == kEngineDXVK) {
        hide.insert({"VK_EXT_hdr_metadata", "VK_EXT_swapchain_maintenance1"});
        if ((d == VK_DRIVER_ID_ARM_PROPRIETARY || d == VK_DRIVER_ID_SAMSUNG_PROPRIETARY) &&
            e.version < engine_version(2, 0, 0))
            hide.insert({"VK_EXT_extended_dynamic_state", "VK_EXT_extended_dynamic_state2",
                         "VK_EXT_extended_dynamic_state3"});
        else if (d == VK_DRIVER_ID_QUALCOMM_PROPRIETARY)
            hide.insert("VK_KHR_shader_float_controls");
    } else if (e.id == kEngineZink) {
        hide.insert({"VK_EXT_extended_dynamic_state", "VK_EXT_extended_dynamic_state2",
                     "VK_EXT_extended_dynamic_state3", "VK_EXT_color_write_enable", "VK_KHR_push_descriptor"});
    }
    return hide;
}

// Vortek's checkDeviceFeatures: reported whether or not the driver has them.
void fake_features(VkPhysicalDeviceFeatures* f) {
    f->depthClamp = f->depthBiasClamp = f->fillModeNonSolid = f->sampleRateShading = VK_TRUE;
    f->samplerAnisotropy = f->shaderClipDistance = f->shaderCullDistance = f->occlusionQueryPrecise = VK_TRUE;
    f->independentBlend = f->multiViewport = f->fullDrawIndexUint32 = f->logicOp = VK_TRUE;
    f->shaderImageGatherExtended = f->variableMultisampleRate = f->dualSrcBlend = f->imageCubeArray = VK_TRUE;
    f->drawIndirectFirstInstance = f->fragmentStoresAndAtomics = f->multiDrawIndirect = VK_TRUE;
    f->tessellationShader = VK_TRUE;
}

// --- instance-level hooks ---

VKAPI_ATTR void VKAPI_CALL get_features(VkPhysicalDevice pd, VkPhysicalDeviceFeatures* f) {
    NEXT_I(wrap::instance_of(pd), vkGetPhysicalDeviceFeatures)(pd, f);
    fake_features(f);
}

VKAPI_ATTR void VKAPI_CALL get_features2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2* f) {
    NEXT_I(wrap::instance_of(pd), vkGetPhysicalDeviceFeatures2)(pd, f);
    fake_features(&f->features);
}

void clamp_heaps(VkPhysicalDeviceMemoryProperties* mp, void* chain) {
    VkDeviceSize largest = 0;
    for (uint32_t i = 0; i < mp->memoryHeapCount; i++) largest = std::max(largest, mp->memoryHeaps[i].size);
    const VkDeviceSize cap = largest * 2 / 3;
    for (uint32_t i = 0; i < mp->memoryHeapCount; i++) mp->memoryHeaps[i].size = std::min(mp->memoryHeaps[i].size, cap);
    auto* budget = static_cast<VkPhysicalDeviceMemoryBudgetPropertiesEXT*>(const_cast<void*>(
        find_struct(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT)));
    if (budget)
        for (uint32_t i = 0; i < mp->memoryHeapCount; i++) budget->heapBudget[i] = std::min(budget->heapBudget[i], cap);
}

VKAPI_ATTR void VKAPI_CALL get_memory_properties(VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties* mp) {
    NEXT_I(wrap::instance_of(pd), vkGetPhysicalDeviceMemoryProperties)(pd, mp);
    clamp_heaps(mp, nullptr);
}

VKAPI_ATTR void VKAPI_CALL get_memory_properties2(VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties2* mp) {
    NEXT_I(wrap::instance_of(pd), vkGetPhysicalDeviceMemoryProperties2)(pd, mp);
    clamp_heaps(&mp->memoryProperties, mp->pNext);
}

VKAPI_ATTR void VKAPI_CALL get_properties2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2* p) {
    NEXT_I(wrap::instance_of(pd), vkGetPhysicalDeviceProperties2)(pd, p);
    if (engine_of(pd).id != kEngineVKD3D) return;
    if (auto* v11 = static_cast<VkPhysicalDeviceVulkan11Properties*>(const_cast<void*>(
            find_struct(p->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES))))
        v11->subgroupSupportedOperations &= ~VK_SUBGROUP_FEATURE_BALLOT_BIT;
    if (auto* v13 = static_cast<VkPhysicalDeviceVulkan13Properties*>(const_cast<void*>(
            find_struct(p->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES)))) {
        v13->storageTexelBufferOffsetSingleTexelAlignment = VK_TRUE;
        v13->uniformTexelBufferOffsetSingleTexelAlignment = VK_TRUE;
    }
}

VKAPI_ATTR void VKAPI_CALL get_format_properties(VkPhysicalDevice pd, VkFormat format, VkFormatProperties* fp) {
    NEXT_I(wrap::instance_of(pd), vkGetPhysicalDeviceFormatProperties)(pd, format, fp);
    if (isFormatScaled(format)) fp->bufferFeatures |= VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
}

VKAPI_ATTR void VKAPI_CALL get_format_properties2(VkPhysicalDevice pd, VkFormat format, VkFormatProperties2* fp) {
    NEXT_I(wrap::instance_of(pd), vkGetPhysicalDeviceFormatProperties2)(pd, format, fp);
    if (!isFormatScaled(format)) return;
    fp->formatProperties.bufferFeatures |= VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
    if (auto* fp3 = static_cast<VkFormatProperties3*>(
            const_cast<void*>(find_struct(fp->pNext, VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3))))
        fp3->bufferFeatures |= VK_FORMAT_FEATURE_2_VERTEX_BUFFER_BIT;
}

VKAPI_ATTR VkResult VKAPI_CALL enumerate_device_extensions(VkPhysicalDevice pd, const char* layer, uint32_t* count,
                                                           VkExtensionProperties* props) {
    auto real = NEXT_I(wrap::instance_of(pd), vkEnumerateDeviceExtensionProperties);
    if (layer) return real(pd, layer, count, props);
    uint32_t n = 0;
    VkResult r = real(pd, nullptr, &n, nullptr);
    if (r != VK_SUCCESS) return r;
    std::vector<VkExtensionProperties> all(n);
    r = real(pd, nullptr, &n, all.data());
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) return r;
    all.resize(n);
    auto hide = hidden_extensions(pd);
    all.erase(std::remove_if(all.begin(), all.end(), [&](const VkExtensionProperties& e) { return hide.count(e.extensionName); }),
              all.end());
    if (!props) {
        *count = uint32_t(all.size());
        return VK_SUCCESS;
    }
    uint32_t give = std::min<uint32_t>(*count, uint32_t(all.size()));
    std::copy(all.begin(), all.begin() + give, props);
    *count = give;
    return give < all.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

// --- device-level hooks: shader modules and pipelines ---

ShaderInspector* inspector_of(VkDevice dev) {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_inspector.find(dev);
    return it == g_inspector.end() ? nullptr : it->second;
}

// Our VkShaderModule handles are ShaderModule*: vertex and fragment code waiting for a pipeline,
// or a real module for every other stage.
VKAPI_ATTR VkResult VKAPI_CALL create_shader_module(VkDevice dev, const VkShaderModuleCreateInfo* info,
                                                    const VkAllocationCallbacks* alloc, VkShaderModule* out) {
    ShaderInspector* si = inspector_of(dev);
    ShaderModule* m = nullptr;
    VkResult r;
    if (!si || info->pNext) {
        // Built as asked, but still behind a ShaderModule so every handle is the same kind.
        m = static_cast<ShaderModule*>(calloc(1, sizeof(ShaderModule)));
        r = NEXT_D(dev, vkCreateShaderModule)(dev, info, alloc, &m->module);
    } else {
        // Vortek edits the code in place; work on a copy.
        std::vector<uint32_t> code(info->pCode, info->pCode + info->codeSize / 4);
        r = ShaderInspector_createModule(si, dev, code.data(), info->codeSize, &m);
    }
    if (r != VK_SUCCESS) {
        if (m) free(m);
        return r;
    }
    *out = reinterpret_cast<VkShaderModule>(m);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL destroy_shader_module(VkDevice dev, VkShaderModule handle, const VkAllocationCallbacks* alloc) {
    if (!handle) return;
    auto* m = reinterpret_cast<ShaderModule*>(handle);
    if (m->module) NEXT_D(dev, vkDestroyShaderModule)(dev, m->module, alloc);
    free(m->code);
    free(m);
}

// Turns one pipeline's stages into stages the driver can take. Vertex and fragment code is
// patched for this pipeline and built into modules that are destroyed once it exists.
struct StageFix {
    std::vector<VkPipelineShaderStageCreateInfo> stages;
    std::vector<ShaderModule> temp;             // per-pipeline copies Vortek's inspector edits
    std::vector<VkShaderModule> built;          // modules made for this pipeline only
    std::vector<VkVertexInputAttributeDescription> attrs;
    VkPipelineVertexInputStateCreateInfo vertex_input{};
};

VkResult fix_stages(VkDevice dev, ShaderInspector* si, const VkPipelineShaderStageCreateInfo* in, uint32_t n,
                    const VkPipelineVertexInputStateCreateInfo* vi, StageFix* fix) {
    fix->stages.assign(in, in + n);
    fix->temp.assign(n, ShaderModule{});
    if (vi) {
        fix->vertex_input = *vi;
        fix->attrs.assign(vi->pVertexAttributeDescriptions, vi->pVertexAttributeDescriptions + vi->vertexAttributeDescriptionCount);
        fix->vertex_input.pVertexAttributeDescriptions = fix->attrs.data();
    }
    std::vector<VkPipelineShaderStageCreateInfo> inspected;
    std::vector<uint32_t> index;
    std::vector<bool> deferred;  // the inspector builds this stage's module for this pipeline
    for (uint32_t i = 0; i < n; i++) {
        VkPipelineShaderStageCreateInfo& s = fix->stages[i];
        ShaderModule& t = fix->temp[i];
        const uint32_t* code = nullptr;
        size_t size = 0;
        if (s.module) {
            auto* m = reinterpret_cast<ShaderModule*>(s.module);
            t.module = m->module;  // set for stages other than vertex and fragment
            code = m->code, size = m->codeSize;
        } else if (auto* mi = static_cast<const VkShaderModuleCreateInfo*>(s.pNext);
                   mi && mi->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
            code = mi->pCode, size = mi->codeSize;  // code given inline (maintenance5)
            s.pNext = mi->pNext;
        } else {
            continue;  // nothing to look at; passed on as written
        }
        if (!t.module) {
            t.code = static_cast<uint32_t*>(malloc(size));
            memcpy(t.code, code, size);
            t.codeSize = size;
        }
        s.module = reinterpret_cast<VkShaderModule>(&t);
        inspected.push_back(s);
        index.push_back(i);
        deferred.push_back(!t.module);
    }
    VkResult r = ShaderInspector_inspectShaderStages(si, dev, inspected.data(), uint32_t(inspected.size()),
                                                    vi ? &fix->vertex_input : nullptr);
    for (size_t k = 0; k < index.size(); k++) {
        ShaderModule& t = fix->temp[index[k]];
        fix->stages[index[k]].module = inspected[k].module;
        if (deferred[k] && t.module) fix->built.push_back(t.module);
        free(t.code);  // the inspector frees and clears it on success
        t.code = nullptr;
    }
    return r;
}

void release(VkDevice dev, std::vector<StageFix>& fixes) {
    for (auto& f : fixes)
        for (VkShaderModule m : f.built) NEXT_D(dev, vkDestroyShaderModule)(dev, m, nullptr);
}

VKAPI_ATTR VkResult VKAPI_CALL create_graphics_pipelines(VkDevice dev, VkPipelineCache cache, uint32_t n,
                                                         const VkGraphicsPipelineCreateInfo* infos,
                                                         const VkAllocationCallbacks* alloc, VkPipeline* out) {
    ShaderInspector* si = inspector_of(dev);
    if (!si) return NEXT_D(dev, vkCreateGraphicsPipelines)(dev, cache, n, infos, alloc, out);
    std::vector<VkGraphicsPipelineCreateInfo> copy(infos, infos + n);
    std::vector<StageFix> fixes(n);
    VkResult r = VK_SUCCESS;
    for (uint32_t i = 0; i < n && r == VK_SUCCESS; i++) {
        r = fix_stages(dev, si, infos[i].pStages, infos[i].stageCount, infos[i].pVertexInputState, &fixes[i]);
        copy[i].pStages = fixes[i].stages.data();
        if (infos[i].pVertexInputState) copy[i].pVertexInputState = &fixes[i].vertex_input;
    }
    if (r == VK_SUCCESS) r = NEXT_D(dev, vkCreateGraphicsPipelines)(dev, cache, n, copy.data(), alloc, out);
    release(dev, fixes);
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL create_compute_pipelines(VkDevice dev, VkPipelineCache cache, uint32_t n,
                                                        const VkComputePipelineCreateInfo* infos,
                                                        const VkAllocationCallbacks* alloc, VkPipeline* out) {
    ShaderInspector* si = inspector_of(dev);
    if (!si) return NEXT_D(dev, vkCreateComputePipelines)(dev, cache, n, infos, alloc, out);
    std::vector<VkComputePipelineCreateInfo> copy(infos, infos + n);
    std::vector<StageFix> fixes(n);
    VkResult r = VK_SUCCESS;
    for (uint32_t i = 0; i < n && r == VK_SUCCESS; i++) {
        r = fix_stages(dev, si, &infos[i].stage, 1, nullptr, &fixes[i]);
        copy[i].stage = fixes[i].stages[0];
    }
    if (r == VK_SUCCESS) r = NEXT_D(dev, vkCreateComputePipelines)(dev, cache, n, copy.data(), alloc, out);
    release(dev, fixes);
    return r;
}

// gfxstream and DXVK both upload with copy2; bcn_layer only watches the plain call.
VKAPI_ATTR void VKAPI_CALL copy_buffer_to_image2(VkCommandBuffer cmd, const VkCopyBufferToImageInfo2* info) {
    VkDevice dev = wrap::device_of(cmd);
    bool plain = !info->pNext;
    for (uint32_t i = 0; plain && i < info->regionCount; i++) plain = !info->pRegions[i].pNext;
    if (!plain) return NEXT_D(dev, vkCmdCopyBufferToImage2)(cmd, info);
    std::vector<VkBufferImageCopy> regions(info->regionCount);
    for (uint32_t i = 0; i < info->regionCount; i++) {
        const VkBufferImageCopy2& r = info->pRegions[i];
        regions[i] = {r.bufferOffset, r.bufferRowLength, r.bufferImageHeight, r.imageSubresource, r.imageOffset, r.imageExtent};
    }
    NEXT_D(dev, vkCmdCopyBufferToImage)(cmd, info->srcBuffer, info->dstImage, info->dstImageLayout, info->regionCount,
                                        regions.data());
}

__attribute__((constructor)) void load_bcn_layer() { wrap::default_bcn_layer(reinterpret_cast<void*>(&load_bcn_layer)); }

}  // namespace

namespace wrap {

void on_instance(VkInstance inst, const VkInstanceCreateInfo* info) {
    Engine e;
    if (const VkApplicationInfo* app = info->pApplicationInfo) {
        if (app->pEngineName) {
            if (contains_ci(app->pEngineName, "dxvk")) e.id = kEngineDXVK;
            else if (contains_ci(app->pEngineName, "vkd3d")) e.id = kEngineVKD3D;
            else if (contains_ci(app->pEngineName, "zink")) e.id = kEngineZink;
        }
        e.version = app->engineVersion;
    }
    std::lock_guard<std::mutex> l(g_lock);
    g_engine[inst] = e;
}

// Vortek's disableUnsupportedDeviceFeatures: drop what fake_features reported but the driver lacks.
void patch_device_create(VkPhysicalDevice pd, VkDeviceCreateInfo* info) {
    VkPhysicalDeviceFeatures have{};
    NEXT_I(instance_of(pd), vkGetPhysicalDeviceFeatures)(pd, &have);
    static thread_local VkPhysicalDeviceFeatures enabled;
    VkPhysicalDeviceFeatures* want = nullptr;
    if (info->pEnabledFeatures) {
        enabled = *info->pEnabledFeatures;
        info->pEnabledFeatures = want = &enabled;
    } else if (auto* f2 = static_cast<VkPhysicalDeviceFeatures2*>(
                   const_cast<void*>(find_struct(info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)))) {
        want = &f2->features;  // gfxstream's own copy of the guest's request
    }
    if (!want) return;
    auto* w = reinterpret_cast<VkBool32*>(want);
    auto* h = reinterpret_cast<const VkBool32*>(&have);
    for (size_t i = 0; i < sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32); i++)
        if (w[i] && !h[i]) w[i] = VK_FALSE;
}

void on_device(VkPhysicalDevice pd, VkDevice dev, const VkDeviceCreateInfo*) {
    Engine e = engine_of(pd);
    VkContext ctx{driver_of(pd), e.id, e.version};
    VkPhysicalDeviceFeatures have{};
    NEXT_I(instance_of(pd), vkGetPhysicalDeviceFeatures)(pd, &have);
    ShaderInspector* si = ShaderInspector_create(&ctx, pd, &have);
    std::lock_guard<std::mutex> l(g_lock);
    g_inspector[dev] = si;
}

void on_device_destroy(VkDevice dev) {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_inspector.find(dev);
    if (it == g_inspector.end()) return;
    free(it->second);
    g_inspector.erase(it);
}

#define HOOK(name, fn) {name, reinterpret_cast<PFN_vkVoidFunction>(fn)}
const Hook* hooks() {
    static const Hook table[] = {
        HOOK("vkGetPhysicalDeviceFeatures", get_features),
        HOOK("vkGetPhysicalDeviceFeatures2", get_features2),
        HOOK("vkGetPhysicalDeviceFeatures2KHR", get_features2),
        HOOK("vkGetPhysicalDeviceMemoryProperties", get_memory_properties),
        HOOK("vkGetPhysicalDeviceMemoryProperties2", get_memory_properties2),
        HOOK("vkGetPhysicalDeviceMemoryProperties2KHR", get_memory_properties2),
        HOOK("vkGetPhysicalDeviceProperties2", get_properties2),
        HOOK("vkGetPhysicalDeviceProperties2KHR", get_properties2),
        HOOK("vkGetPhysicalDeviceFormatProperties", get_format_properties),
        HOOK("vkGetPhysicalDeviceFormatProperties2", get_format_properties2),
        HOOK("vkGetPhysicalDeviceFormatProperties2KHR", get_format_properties2),
        HOOK("vkEnumerateDeviceExtensionProperties", enumerate_device_extensions),
        HOOK("vkCreateShaderModule", create_shader_module),
        HOOK("vkDestroyShaderModule", destroy_shader_module),
        HOOK("vkCreateGraphicsPipelines", create_graphics_pipelines),
        HOOK("vkCreateComputePipelines", create_compute_pipelines),
        HOOK("vkCmdCopyBufferToImage2", copy_buffer_to_image2),
        HOOK("vkCmdCopyBufferToImage2KHR", copy_buffer_to_image2),
        {nullptr, nullptr},
    };
    return table;
}

}  // namespace wrap
