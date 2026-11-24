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
#include <sys/system_properties.h>
#include <atomic>
#include <cstdio>
#include <string.h>
#include <android/log.h>

#include <algorithm>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
    // Not Vortek's: DXVK refuses a device without geometry shaders, which PanVK on Valhall does not
    // report. Games that use them still fail; the many that do not, run. (Mali's own driver has them.)
    f->geometryShader = VK_TRUE;
}

// --- instance-level hooks ---

// Zink (OpenGL in Windows) gets the driver as it is: it turns on whatever it is told exists, and
// the driver then refuses the device. Everything else gets Vortek's changes.
bool stretches(VkPhysicalDevice pd) { return engine_of(pd).id != kEngineZink; }

VKAPI_ATTR void VKAPI_CALL get_features(VkPhysicalDevice pd, VkPhysicalDeviceFeatures* f) {
    NEXT_I(wrap::instance_of(pd), vkGetPhysicalDeviceFeatures)(pd, f);
    if (stretches(pd)) fake_features(f);
}

VKAPI_ATTR void VKAPI_CALL get_features2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2* f) {
    NEXT_I(wrap::instance_of(pd), vkGetPhysicalDeviceFeatures2)(pd, f);
    if (!stretches(pd)) return;
    fake_features(&f->features);
    // DXVK 2 and later refuse a device without robustBufferAccess2, which Mali lacks while it has
    // the rest of VK_EXT_robustness2. Without it, out-of-range reads still stay inside the buffer
    // (robustBufferAccess) but need not return zero. GameNative's Mali wrapper reports it the same way.
    if (auto* r2 = static_cast<VkPhysicalDeviceRobustness2FeaturesEXT*>(const_cast<void*>(
            find_struct(f->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT))))
        r2->robustBufferAccess2 = r2->robustImageAccess2 = VK_TRUE;
    // Direct3D 12 needs instance step rate 0, which Mali lacks; pipelines get the largest divisor
    // instead (zero_divisor), so every instance reads the first element as long as there are fewer.
    if (auto* dv = static_cast<VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR*>(const_cast<void*>(
            find_struct(f->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR))))
        dv->vertexAttributeInstanceRateZeroDivisor |= dv->vertexAttributeInstanceRateDivisor;
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
    auto real = NEXT_I(wrap::instance_of(pd), vkGetPhysicalDeviceProperties2);
    real(pd, p);
    if (auto* ext = static_cast<VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT*>(const_cast<void*>(
            find_struct(p->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT)));
        ext && !ext->maxVertexAttribDivisor) {
        VkPhysicalDeviceVertexAttributeDivisorPropertiesKHR khr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 again{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &khr};
        real(pd, &again);
        ext->maxVertexAttribDivisor = khr.maxVertexAttribDivisor;
    }
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
    if (stretches(pd) && isFormatScaled(format)) fp->bufferFeatures |= VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
}

VKAPI_ATTR void VKAPI_CALL get_format_properties2(VkPhysicalDevice pd, VkFormat format, VkFormatProperties2* fp) {
    NEXT_I(wrap::instance_of(pd), vkGetPhysicalDeviceFormatProperties2)(pd, format, fp);
    if (!stretches(pd) || !isFormatScaled(format)) return;
    fp->formatProperties.bufferFeatures |= VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
    if (auto* fp3 = static_cast<VkFormatProperties3*>(
            const_cast<void*>(find_struct(fp->pNext, VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3))))
        fp3->bufferFeatures |= VK_FORMAT_FEATURE_2_VERTEX_BUFFER_BIT;
}

bool driver_has(VkPhysicalDevice pd, const char* ext) {
    auto real = NEXT_I(wrap::instance_of(pd), vkEnumerateDeviceExtensionProperties);
    uint32_t n = 0;
    real(pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> all(n);
    real(pd, nullptr, &n, all.data());
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(all[i].extensionName, ext)) return true;
    return false;
}

// vkd3d-proton asks for VK_EXT_vertex_attribute_divisor, which Mali only has under its Vulkan 1.4
// name, VK_KHR_vertex_attribute_divisor. Same features and pipeline structs; only the properties
// struct differs, and get_properties2 fills that one.
constexpr const char* kDivisorExt = "VK_EXT_vertex_attribute_divisor";
constexpr const char* kDivisorKhr = "VK_KHR_vertex_attribute_divisor";

VKAPI_ATTR VkResult VKAPI_CALL enumerate_device_extensions(VkPhysicalDevice pd, const char* layer, uint32_t* count,
                                                           VkExtensionProperties* props) {
    auto real = NEXT_I(wrap::instance_of(pd), vkEnumerateDeviceExtensionProperties);
    if (layer || !stretches(pd)) return real(pd, layer, count, props);
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
    auto has = [&](const char* name) {
        return std::any_of(all.begin(), all.end(), [&](const VkExtensionProperties& e) { return !strcmp(e.extensionName, name); });
    };
    if (stretches(pd) && has(kDivisorKhr) && !has(kDivisorExt)) {
        VkExtensionProperties e{};
        strcpy(e.extensionName, kDivisorExt);
        e.specVersion = 3;
        all.push_back(e);
    }
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
    if (!si) return NEXT_D(dev, vkCreateShaderModule)(dev, info, alloc, out);  // a device Vortek leaves alone
    ShaderModule* m = nullptr;
    VkResult r;
    if (info->pNext) {
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
    if (!inspector_of(dev)) return NEXT_D(dev, vkDestroyShaderModule)(dev, handle, alloc);
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
    VkPipelineVertexInputDivisorStateCreateInfoKHR divisor{};
    std::vector<VkVertexInputBindingDivisorDescriptionKHR> divisors;
};

// What stands in for a divisor of 0 on this device: 0 when the driver takes 0 itself.
uint32_t zero_divisor(VkDevice dev) {
    static std::unordered_map<VkDevice, uint32_t> known;
    {
        std::lock_guard<std::mutex> l(g_lock);
        if (auto it = known.find(dev); it != known.end()) return it->second;
    }
    VkPhysicalDevice pd = wrap::physical_of(dev);
    VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f};
    NEXT_I(wrap::instance_of(pd), vkGetPhysicalDeviceFeatures2)(pd, &f2);
    VkPhysicalDeviceVertexAttributeDivisorPropertiesKHR p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p};
    NEXT_I(wrap::instance_of(pd), vkGetPhysicalDeviceProperties2)(pd, &p2);
    uint32_t v = f.vertexAttributeInstanceRateZeroDivisor ? 0 : std::max(p.maxVertexAttribDivisor, 1u);
    std::lock_guard<std::mutex> l(g_lock);
    known[dev] = v;
    return v;
}

// Mali has no clip distances (shaderClipDistance 0), but Vortek reports them so DXVK starts, and Mali's
// shader compiler crashes (null read in spir2lir::evaluate_address) on any access to gl_ClipDistance.
// DXVK's fixed-function Direct3D 9 vertex shader writes it in a loop. Every access chain into the
// ClipDistance member of an output block is pointed at a private array instead, so the built-in is
// declared but never touched. User clip planes then do not clip, as before on this GPU.
static bool drop_clip_distance(std::vector<uint32_t>& w) {
    if (w.size() < 5) return false;
    struct Ins { size_t at; uint32_t wc, op; };
    std::vector<Ins> ins;
    for (size_t i = 5; i < w.size();) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc || i + wc > w.size()) return false;
        ins.push_back({i, wc, op});
        i += wc;
    }
    uint32_t S = 0, m = 0;
    bool found = false;
    for (auto& x : ins)
        if (x.op == 72 && x.wc >= 5 && w[x.at + 3] == 11 /*BuiltIn*/ && w[x.at + 4] == 3 /*ClipDistance*/) S = w[x.at + 1], m = w[x.at + 2], found = true;
    if (!found) return false;
    auto def = [&](uint32_t id, uint32_t op, size_t idw) -> const Ins* {   // the instruction of that opcode whose result is id
        for (auto& x : ins)
            if (x.op == op && x.wc > idw && w[x.at + idw] == id) return &x;
        return nullptr;
    };
    const Ins* st = def(S, 30, 1);                         // OpTypeStruct
    if (!st || st->wc < 3 + m) return false;
    uint32_t A = w[st->at + 2 + m];
    const Ins* arr = def(A, 28, 1);                        // OpTypeArray
    if (!arr) return false;
    uint32_t F = w[arr->at + 2];
    auto const_value = [&](uint32_t id, uint32_t* v) {
        const Ins* c = def(id, 43, 2);                     // OpConstant
        if (!c || c->wc < 4) return false;
        *v = w[c->at + 3];
        return true;
    };
    std::vector<uint32_t> vars;                            // output variables of that block type
    for (auto& x : ins)
        if (x.op == 59 && w[x.at + 3] == 3) {
            const Ins* p = def(w[x.at + 1], 32, 1);
            if (p && w[p->at + 2] == 3 && w[p->at + 3] == S) vars.push_back(w[x.at + 2]);
        }
    auto is_clip_chain = [&](const Ins& x) {
        if ((x.op != 65 && x.op != 66) || x.wc < 5) return false;
        uint32_t v;
        bool ours = false;
        for (uint32_t id : vars) ours |= id == w[x.at + 3];
        return ours && const_value(w[x.at + 4], &v) && v == m;
    };
    bool any = false;
    for (auto& x : ins) any |= is_clip_chain(x);
    if (!any) return false;
    uint32_t bound = w[3];
    std::vector<uint32_t> decl;
    auto pointer = [&](uint32_t sc, uint32_t t) {
        for (auto& x : ins)
            if (x.op == 32 && w[x.at + 2] == sc && w[x.at + 3] == t) return w[x.at + 1];
        uint32_t p = bound++;
        decl.insert(decl.end(), {(4u << 16) | 32, p, sc, t});
        return p;
    };
    uint32_t pPA = pointer(6 /*Private*/, A), pPF = pointer(6, F);
    uint32_t nul = bound++, var = bound++;
    decl.insert(decl.end(), {(3u << 16) | 46, A, nul});            // OpConstantNull
    decl.insert(decl.end(), {(5u << 16) | 59, pPA, var, 6, nul});  // OpVariable Private, zeroed
    std::vector<uint32_t> out(w.begin(), w.begin() + 5);
    bool declared = false;
    for (auto& x : ins) {
        const uint32_t* p = &w[x.at];
        if (x.op == 15) {                                  // OpEntryPoint: the new variable joins the interface
            out.push_back(((x.wc + 1) << 16) | 15);
            out.insert(out.end(), p + 1, p + x.wc);
            out.push_back(var);
            continue;
        }
        if (x.op == 54 && !declared) {                     // before the first function
            out.insert(out.end(), decl.begin(), decl.end());
            declared = true;
        }
        if (is_clip_chain(x)) {
            if (x.wc == 5) {
                out.insert(out.end(), {(4u << 16) | 65, pPA, p[2], var});
            } else {
                out.push_back(((x.wc - 1) << 16) | x.op);
                out.insert(out.end(), {pPF, p[2], var});
                out.insert(out.end(), p + 5, p + x.wc);
            }
            continue;
        }
        out.insert(out.end(), p, p + x.wc);
    }
    out[3] = bound;
    w.swap(out);
    return true;
}

// DXVK 3.1+ (dxbc_spv) loads vertex input components via OpAccessChain + OpLoad on scalar pointers,
// whereas Vortek's shader_inspector.c (convertFormatScaled) only rewrites direct OpLoad on the input
// vector variable. For any input variable whose pipeline vertex format is USCALED/SSCALED, turn
//   %p = OpAccessChain %ptr_float %var %const_k ; %x = OpLoad %float %p
// into
//   %v = OpLoad %vNfloat %var ; %x = OpCompositeExtract %float %v k
// so shader_inspector.c converts the vector load cleanly to integer + Convert[SU]ToF.
static bool flatten_scaled_inputs(std::vector<uint32_t>& w, const VkPipelineVertexInputStateCreateInfo* vi) {
    if (!vi || w.size() < 5) return false;
    std::unordered_set<uint32_t> scaled_locs;
    for (uint32_t i = 0; i < vi->vertexAttributeDescriptionCount; i++) {
        if (isFormatScaled(vi->pVertexAttributeDescriptions[i].format))
            scaled_locs.insert(vi->pVertexAttributeDescriptions[i].location);
    }
    if (scaled_locs.empty()) return false;
    struct Ins { size_t at; uint32_t wc, op; };
    std::vector<Ins> ins;
    for (size_t i = 5; i < w.size();) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc || i + wc > w.size()) return false;
        ins.push_back({i, wc, op});
        i += wc;
    }
    auto def = [&](uint32_t id, uint32_t op, size_t idw) -> const Ins* {
        for (auto& x : ins)
            if (x.op == op && x.wc > idw && w[x.at + idw] == id) return &x;
        return nullptr;
    };
    auto const_u32 = [&](uint32_t id, uint32_t* v) {
        const Ins* c = def(id, 43 /*OpConstant*/, 2);
        if (!c || c->wc < 4) return false;
        *v = w[c->at + 3];
        return true;
    };
    // Find Input variables decorated with a scaled Location, and their vector value type.
    std::unordered_map<uint32_t, uint32_t> var_vec_type;
    for (auto& x : ins) {
        if (x.op == 71 /*OpDecorate*/ && x.wc >= 4 && w[x.at + 2] == 30 /*Location*/ && scaled_locs.count(w[x.at + 3])) {
            uint32_t var = w[x.at + 1];
            const Ins* vd = def(var, 59 /*OpVariable*/, 2);
            if (!vd || vd->wc < 4 || w[vd->at + 3] != 1 /*Input*/) continue;
            const Ins* ptr = def(w[vd->at + 1], 32 /*OpTypePointer*/, 1);
            if (!ptr || ptr->wc < 4 || w[ptr->at + 2] != 1 /*Input*/) continue;
            const Ins* vec = def(w[ptr->at + 3], 23 /*OpTypeVector*/, 1);
            if (!vec) continue;
            var_vec_type[var] = w[ptr->at + 3];
        }
    }
    if (var_vec_type.empty()) return false;
    struct Chain { uint32_t var, vec_type, idx; };
    std::unordered_map<uint32_t, Chain> chains;
    for (auto& x : ins) {
        if ((x.op == 65 /*OpAccessChain*/ || x.op == 66 /*OpInBoundsAccessChain*/) && x.wc == 5) {
            auto it = var_vec_type.find(w[x.at + 3]);
            uint32_t idx = 0;
            if (it != var_vec_type.end() && const_u32(w[x.at + 4], &idx))
                chains[w[x.at + 2]] = {it->first, it->second, idx};
        }
    }
    if (chains.empty()) return false;
    // Only rewrite chains whose sole uses inside functions are plain OpLoad.
    std::unordered_set<uint32_t> bad;
    bool in_fn = false;
    for (auto& x : ins) {
        if (x.op == 54 /*OpFunction*/) in_fn = true;
        if (!in_fn) continue;
        if (x.op == 65 || x.op == 66) continue;
        if (x.op == 61 /*OpLoad*/ && x.wc == 4 && chains.count(w[x.at + 3])) continue;
        for (uint32_t k = 1; k < x.wc; k++)
            if (chains.count(w[x.at + k])) bad.insert(w[x.at + k]);
    }
    for (uint32_t id : bad) chains.erase(id);
    if (chains.empty()) return false;
    uint32_t bound = w[3];
    std::vector<uint32_t> out(w.begin(), w.begin() + 5);
    for (auto& x : ins) {
        const uint32_t* p = &w[x.at];
        if ((x.op == 65 || x.op == 66) && x.wc == 5 && chains.count(p[2])) continue;
        if (x.op == 61 && x.wc == 4) {
            auto it = chains.find(p[3]);
            if (it != chains.end()) {
                uint32_t tmp = bound++;
                out.insert(out.end(), {(4u << 16) | 61, it->second.vec_type, tmp, it->second.var});
                out.insert(out.end(), {(5u << 16) | 81 /*OpCompositeExtract*/, p[1], p[2], tmp, it->second.idx});
                continue;
            }
        }
        out.insert(out.end(), p, p + x.wc);
    }
    out[3] = bound;
    w.swap(out);
    return true;
}

// Mali Valhall's LLVM backend aborts ("Cannot select: intrinsic %llvm.bifrost.flush.denormal")
// when DenormFlushToZero 32 is combined with vector int->float conversions (e.g. convertFormatScaled).
static bool drop_denorm_ftz(std::vector<uint32_t>& w) {
    if (w.size() < 5) return false;
    bool any = false;
    for (size_t i = 5; i < w.size();) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc || i + wc > w.size()) return false;
        if ((op == 17 /*OpCapability*/ && wc >= 2 && w[i + 1] == 4465 /*DenormFlushToZero*/) ||
            (op == 16 /*OpExecutionMode*/ && wc >= 3 && w[i + 2] == 4460 /*DenormFlushToZero*/)) {
            any = true;
            break;
        }
        i += wc;
    }
    if (!any) return false;
    std::vector<uint32_t> out(w.begin(), w.begin() + 5);
    for (size_t i = 5; i < w.size();) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if ((op == 17 && wc >= 2 && w[i + 1] == 4465) ||
            (op == 16 && wc >= 3 && w[i + 2] == 4460)) {
            i += wc;
            continue;
        }
        out.insert(out.end(), w.begin() + i, w.begin() + i + wc);
        i += wc;
    }
    w.swap(out);
    return true;
}

VkResult fix_stages(VkDevice dev, ShaderInspector* si, const VkPipelineShaderStageCreateInfo* in, uint32_t n,
                    const VkPipelineVertexInputStateCreateInfo* vi, StageFix* fix) {
    fix->stages.assign(in, in + n);
    fix->temp.assign(n, ShaderModule{});
    if (vi) {
        fix->vertex_input = *vi;
        fix->attrs.assign(vi->pVertexAttributeDescriptions, vi->pVertexAttributeDescriptions + vi->vertexAttributeDescriptionCount);
        fix->vertex_input.pVertexAttributeDescriptions = fix->attrs.data();
        auto* d = static_cast<const VkPipelineVertexInputDivisorStateCreateInfoKHR*>(
            find_struct(vi->pNext, VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_KHR));
        uint32_t zero = d && d == vi->pNext ? zero_divisor(dev) : 0;
        if (zero) {
            fix->divisor = *d;
            fix->divisors.assign(d->pVertexBindingDivisors, d->pVertexBindingDivisors + d->vertexBindingDivisorCount);
            for (auto& b : fix->divisors)
                if (!b.divisor) b.divisor = zero;
            fix->divisor.pVertexBindingDivisors = fix->divisors.data();
            fix->vertex_input.pNext = &fix->divisor;
        }
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
        // Debugging a shader the phone's compiler chokes on: `setprop debug.hwgpu.spvdump 1` writes every
        // stage of every graphics pipeline to $HWGPU_VK_CACHE_DIR/spv-<n>-<stage>.spv before it is built.
        {
            static int on = -1;
            static std::atomic<uint32_t> count{0};
            if (on < 0) {
                char v[92] = "";
                __system_property_get("debug.hwgpu.spvdump", v);
                on = v[0] == '1';
            }
            const char* dir = getenv("HWGPU_VK_CACHE_DIR");
            if (on && dir && code && size) {
                char path[512];
                snprintf(path, sizeof path, "%s/spv-%04u-%x.spv", dir, count++, unsigned(s.stage));
                if (FILE* f = fopen(path, "wb")) {
                    fwrite(code, 1, size, f);
                    fclose(f);
                }
            }
        }
        if (!t.module) {
            std::vector<uint32_t> words(code, code + size / 4);
            if (s.stage != VK_SHADER_STAGE_FRAGMENT_BIT && drop_clip_distance(words)) size = words.size() * 4;
            // adb shell setprop debug.hwgpu.nonewpasses 1: skips the two passes added on 2026-11-22, to tell
            // whether a picture fault comes from them.
            static int no_new = -1;
            if (no_new < 0) {
                char v[PROP_VALUE_MAX] = "";
                __system_property_get("debug.hwgpu.nonewpasses", v);
                no_new = v[0] == '1';
            }
            if (!no_new && s.stage == VK_SHADER_STAGE_VERTEX_BIT && flatten_scaled_inputs(words, vi)) size = words.size() * 4;
            if (!no_new && drop_denorm_ftz(words)) size = words.size() * 4;
            t.code = static_cast<uint32_t*>(malloc(size));
            memcpy(t.code, words.data(), size);
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
    static thread_local std::vector<const char*> names;
    names.assign(info->ppEnabledExtensionNames, info->ppEnabledExtensionNames + info->enabledExtensionCount);
    for (auto& n : names)
        if (!strcmp(n, kDivisorExt) && !driver_has(pd, kDivisorExt)) n = kDivisorKhr;
    // The renamed one may now be there twice.
    for (size_t i = 0; i < names.size(); i++)
        for (size_t j = i + 1; j < names.size();)
            if (!strcmp(names[i], names[j])) names.erase(names.begin() + j); else j++;
    info->enabledExtensionCount = uint32_t(names.size());
    info->ppEnabledExtensionNames = names.data();
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
    // The robustness2 features get_features2 reported, dropped where the driver lacks them.
    if (auto* r2 = static_cast<VkPhysicalDeviceRobustness2FeaturesEXT*>(const_cast<void*>(
            find_struct(info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT)))) {
        VkPhysicalDeviceRobustness2FeaturesEXT real{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &real};
        NEXT_I(instance_of(pd), vkGetPhysicalDeviceFeatures2)(pd, &f2);
        r2->robustBufferAccess2 &= real.robustBufferAccess2;
        r2->robustImageAccess2 &= real.robustImageAccess2;
        r2->nullDescriptor &= real.nullDescriptor;
    }
    if (auto* dv = static_cast<VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR*>(const_cast<void*>(
            find_struct(info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR)))) {
        VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR real{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &real};
        NEXT_I(instance_of(pd), vkGetPhysicalDeviceFeatures2)(pd, &f2);
        dv->vertexAttributeInstanceRateZeroDivisor &= real.vertexAttributeInstanceRateZeroDivisor;
    }
    if (!want) return;
    auto* w = reinterpret_cast<VkBool32*>(want);
    auto* h = reinterpret_cast<const VkBool32*>(&have);
    for (size_t i = 0; i < sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32); i++)
        if (w[i] && !h[i]) w[i] = VK_FALSE;
}

void on_device(VkPhysicalDevice pd, VkDevice dev, const VkDeviceCreateInfo*) {
    if (!stretches(pd)) return;  // no inspector: shaders and pipelines go straight through
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
