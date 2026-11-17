// Shared core of the phone-side wrapper drivers. See wrap.h.
#include "wrap.h"

#include <vulkan/vk_layer.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "hwgpu-icd", __VA_ARGS__)
#else
#define LOG(...) (fprintf(stderr, "hwgpu-icd: " __VA_ARGS__), fputc('\n', stderr))
#endif

#define EXPORT extern "C" __attribute__((visibility("default")))

namespace wrap {

// Module callbacks with nothing to do by default.
__attribute__((weak)) void on_instance(VkInstance, const VkInstanceCreateInfo*) {}
__attribute__((weak)) void on_device(VkPhysicalDevice, VkDevice, const VkDeviceCreateInfo*) {}
__attribute__((weak)) void on_device_destroy(VkDevice) {}
__attribute__((weak)) void patch_device_create(VkPhysicalDevice, VkDeviceCreateInfo*) {}

// Set by a module's constructor: its own layer, used unless HWGPU_VK_LAYERS names others. Kept in
// the module, not the environment, so wrappers stacked in one process each keep their own.
// Plain arrays: module constructors may run before this file's C++ initializers.
char g_default_layers[1024], g_default_gipa[64], g_default_gdpa[64];

void default_bcn_layer(void* self) {
    Dl_info info{};
    if (!dladdr(self, &info) || !info.dli_fname) return;
    std::string dir = info.dli_fname;
    dir = dir.substr(0, dir.rfind('/') + 1);
    snprintf(g_default_layers, sizeof(g_default_layers), "%slibbcn_layer.so", dir.c_str());
    // bcn_layer's own names for its entry points.
    snprintf(g_default_gipa, sizeof(g_default_gipa), "BCnLayer_GetInstanceProcAddr");
    snprintf(g_default_gdpa, sizeof(g_default_gdpa), "BCnLayer_GetDeviceProcAddr");
}

namespace {

PFN_vkGetInstanceProcAddr g_gipa;  // the driver
std::mutex g_lock;

// Layers between the wrapper and the driver, top first (HWGPU_VK_LAYERS). The core links them the
// way a loader does, so stock layers such as bcn_layer run unchanged.
struct Layer {
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkGetDeviceProcAddr gdpa;
};
std::vector<Layer> g_layers;
std::unordered_map<VkPhysicalDevice, VkInstance> g_pd_inst;
std::unordered_map<VkDevice, VkPhysicalDevice> g_dev_pd;
std::unordered_map<VkDevice, PFN_vkGetDeviceProcAddr> g_gdpa;
std::unordered_map<VkQueue, VkDevice> g_queue_dev;
std::unordered_map<VkCommandBuffer, VkDevice> g_cmd_dev;
// next_*() results by handle and call-site name pointer: names come from NEXT_* string literals.
std::map<std::pair<const void*, const char*>, PFN_vkVoidFunction> g_next;

// Android HAL drivers (vulkan.*.so, many custom builds) have no ICD entry points, only this.
struct HwModule {  // hw_module_t
    uint32_t tag;
    uint16_t module_api_version, hal_api_version;
    const char *id, *name, *author;
    struct Methods {
        int (*open)(const HwModule*, const char*, void**);
    }* methods;
    void* dso;
    uint64_t reserved[32 - 7];
};
struct HwVulkanDevice {  // hwvulkan_device_t
    struct {
        uint32_t tag, version;
        HwModule* module;
        uint64_t reserved[12];
        int (*close)(void*);
    } common;
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
    PFN_vkCreateInstance CreateInstance;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
};

bool load_layer(const std::string& entry);

bool load() {
    static std::once_flag once;
    std::call_once(once, [] {
        // A stack of wrappers (HWGPU_VK_CHAIN, top first, ':' between): each opens the one after
        // it, and the last one opens HWGPU_VK_DRIVER, or the phone's libvulkan.so.
        std::string next, me;
        Dl_info self{};
        if (dladdr(reinterpret_cast<void*>(&load), &self) && self.dli_fname) {
            me = self.dli_fname;
            me = me.substr(me.rfind('/') + 1);
        }
        if (const char* chain = getenv("HWGPU_VK_CHAIN")) {
            std::string all = chain;
            std::vector<std::string> links;
            for (size_t at = 0; at <= all.size();) {
                size_t end = all.find(':', at);
                if (end == std::string::npos) end = all.size();
                links.push_back(all.substr(at, end - at));
                at = end + 1;
            }
            for (size_t i = 0; i + 1 < links.size(); i++)
                if (links[i] == me) next = links[i + 1];
        }
        const char* path = !next.empty() ? next.c_str() : getenv("HWGPU_VK_DRIVER");
        void* lib = dlopen(path && *path ? path : "libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (!lib) {
            LOG("cannot open %s: %s", path ? path : "libvulkan.so", dlerror());
            return;
        }
        // The system loader, then an ICD, then an Android HAL module.
        g_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
        if (!g_gipa) {
            g_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vk_icdGetInstanceProcAddr"));
            using Negotiate = VkResult (*)(uint32_t*);
            if (auto neg = reinterpret_cast<Negotiate>(dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion"))) {
                uint32_t v = 5;
                neg(&v);
            }
        }
        if (!g_gipa) {
            auto* hmi = static_cast<const HwModule*>(dlsym(lib, "HMI"));
            HwVulkanDevice* dev = nullptr;
            if (hmi && hmi->methods && hmi->methods->open(hmi, "vk0", reinterpret_cast<void**>(&dev)) == 0 && dev)
                g_gipa = dev->GetInstanceProcAddr;
        }
        LOG("driver %s: %s", path && *path ? path : "libvulkan.so", g_gipa ? "loaded" : "no entry point");
        const char* env_layers = getenv("HWGPU_VK_LAYERS");
        std::string layers = env_layers ? env_layers : g_default_layers;
        // bcn_layer is one library per process, so it runs once: in the BC textures wrapper when the
        // stack has one, else in the first wrapper that wants it.
        const char* stack = getenv("HWGPU_VK_CHAIN");
        if (!env_layers && stack && strstr(stack, "libvulkan_bcn.so") && me != "libvulkan_bcn.so") layers.clear();
        if (!layers.empty()) {
            std::string all = layers;
            for (size_t at = 0; at <= all.size();) {
                size_t end = all.find(':', at);
                if (end == std::string::npos) end = all.size();
                if (end > at) load_layer(all.substr(at, end - at));
                at = end + 1;
            }
        }
    });
    return g_gipa != nullptr;
}

// The string value after "key": in a layer manifest, or "" when absent.
std::string json_value(const std::string& text, const char* key, size_t from = 0) {
    size_t k = text.find(std::string("\"") + key + "\"", from);
    if (k == std::string::npos) return "";
    size_t a = text.find('"', text.find(':', k) + 1);
    size_t b = a == std::string::npos ? a : text.find('"', a + 1);
    return b == std::string::npos ? "" : text.substr(a + 1, b - a - 1);
}

// One entry of HWGPU_VK_LAYERS: a layer manifest (.json) or the layer library itself.
bool load_layer(const std::string& entry) {
    // A bare library uses the standard names unless HWGPU_VK_LAYER_GIPA/GDPA name others.
    const char* g_env = getenv("HWGPU_VK_LAYER_GIPA");
    const char* d_env = getenv("HWGPU_VK_LAYER_GDPA");
    const bool own = !getenv("HWGPU_VK_LAYERS");  // the module's default layer, with its own names
    std::string lib = entry,
                gipa_name = own && *g_default_gipa ? g_default_gipa : g_env ? g_env : "vkGetInstanceProcAddr",
                gdpa_name = own && *g_default_gdpa ? g_default_gdpa : d_env ? d_env : "vkGetDeviceProcAddr";
    if (entry.size() > 5 && entry.compare(entry.size() - 5, 5, ".json") == 0) {
        std::string text;
        if (FILE* f = fopen(entry.c_str(), "r")) {
            char buf[4096];
            for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
            fclose(f);
        }
        lib = json_value(text, "library_path");
        if (lib.empty()) return false;
        if (lib[0] != '/' && entry.find('/') != std::string::npos) lib = entry.substr(0, entry.rfind('/') + 1) + lib;
        size_t fns = text.find("\"functions\"");
        if (fns != std::string::npos) {
            std::string g = json_value(text, "vkGetInstanceProcAddr", fns), d = json_value(text, "vkGetDeviceProcAddr", fns);
            if (!g.empty()) gipa_name = g;
            if (!d.empty()) gdpa_name = d;
        }
    }
    void* h = dlopen(lib.c_str(), RTLD_NOW | RTLD_LOCAL);
    Layer l{h ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(h, gipa_name.c_str())) : nullptr,
            h ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(dlsym(h, gdpa_name.c_str())) : nullptr};
    if (!l.gipa || !l.gdpa) {
        LOG("layer %s: %s", entry.c_str(), h ? "no entry points" : dlerror());
        return false;
    }
    g_layers.push_back(l);
    LOG("layer %s loaded", lib.c_str());
    return true;
}

// The top of the chain: the first layer, or the driver.
PFN_vkGetInstanceProcAddr top_gipa() { return g_layers.empty() ? g_gipa : g_layers[0].gipa; }

// What layer i calls next.
PFN_vkGetInstanceProcAddr below_gipa(size_t i) { return i + 1 < g_layers.size() ? g_layers[i + 1].gipa : g_gipa; }

const Hook* find_hook(const Hook* table, const char* name) {
    for (const Hook* h = table; h && h->name; h++)
        if (!strcmp(h->name, name)) return h;
    return nullptr;
}

// A layer's own dispatchable objects take their parent's dispatch key, as a loader would set it.
VKAPI_ATTR VkResult VKAPI_CALL set_instance_loader_data(VkInstance inst, void* object) {
    *static_cast<void**>(object) = *reinterpret_cast<void**>(inst);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL set_device_loader_data(VkDevice dev, void* object) {
    *static_cast<void**>(object) = *reinterpret_cast<void**>(dev);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL create_instance(const VkInstanceCreateInfo* info, const VkAllocationCallbacks* alloc,
                                               VkInstance* out) {
    VkInstanceCreateInfo chained = *info;
    std::vector<VkLayerInstanceLink> links(g_layers.size());
    VkLayerInstanceCreateInfo link_info{VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO, info->pNext, VK_LAYER_LINK_INFO, {}};
    VkLayerInstanceCreateInfo data_info{VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO, &link_info, VK_LOADER_DATA_CALLBACK, {}};
    if (!g_layers.empty()) {
        for (size_t i = 0; i < links.size(); i++) {
            links[i].pNext = i + 1 < links.size() ? &links[i + 1] : nullptr;
            links[i].pfnNextGetInstanceProcAddr = below_gipa(i);
            links[i].pfnNextGetPhysicalDeviceProcAddr = nullptr;
        }
        link_info.u.pLayerInfo = links.data();
        data_info.u.pfnSetInstanceLoaderData = set_instance_loader_data;
        chained.pNext = &data_info;
    }
    auto real = reinterpret_cast<PFN_vkCreateInstance>(top_gipa()(nullptr, "vkCreateInstance"));
    VkResult r = real(&chained, alloc, out);
    if (r == VK_SUCCESS) on_instance(*out, info);
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL enumerate_physical_devices(VkInstance inst, uint32_t* count, VkPhysicalDevice* pds) {
    VkResult r = NEXT_I(inst, vkEnumeratePhysicalDevices)(inst, count, pds);
    if (pds && (r == VK_SUCCESS || r == VK_INCOMPLETE)) {
        std::lock_guard<std::mutex> l(g_lock);
        for (uint32_t i = 0; i < *count; i++) g_pd_inst[pds[i]] = inst;
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL enumerate_physical_device_groups(VkInstance inst, uint32_t* count,
                                                                VkPhysicalDeviceGroupProperties* groups) {
    VkResult r = NEXT_I(inst, vkEnumeratePhysicalDeviceGroups)(inst, count, groups);
    if (groups && (r == VK_SUCCESS || r == VK_INCOMPLETE)) {
        std::lock_guard<std::mutex> l(g_lock);
        for (uint32_t i = 0; i < *count; i++)
            for (uint32_t j = 0; j < groups[i].physicalDeviceCount; j++) g_pd_inst[groups[i].physicalDevices[j]] = inst;
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL create_device(VkPhysicalDevice pd, const VkDeviceCreateInfo* info,
                                             const VkAllocationCallbacks* alloc, VkDevice* out) {
    VkInstance inst = instance_of(pd);
    VkDeviceCreateInfo chained = *info;
    patch_device_create(pd, &chained);
    std::vector<VkLayerDeviceLink> links(g_layers.size());
    VkLayerDeviceCreateInfo link_info{VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, chained.pNext, VK_LAYER_LINK_INFO, {}};
    VkLayerDeviceCreateInfo data_info{VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, &link_info, VK_LOADER_DATA_CALLBACK, {}};
    auto driver_gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(g_gipa(inst, "vkGetDeviceProcAddr"));
    if (!g_layers.empty()) {
        for (size_t i = 0; i < links.size(); i++) {
            links[i].pNext = i + 1 < links.size() ? &links[i + 1] : nullptr;
            links[i].pfnNextGetInstanceProcAddr = below_gipa(i);
            links[i].pfnNextGetDeviceProcAddr = i + 1 < links.size() ? g_layers[i + 1].gdpa : driver_gdpa;
        }
        link_info.u.pLayerInfo = links.data();
        data_info.u.pfnSetDeviceLoaderData = set_device_loader_data;
        chained.pNext = &data_info;
    }
    VkResult r = NEXT_I(inst, vkCreateDevice)(pd, &chained, alloc, out);
    if (r != VK_SUCCESS) {
        // What was asked for, to find the feature or extension the driver turned down.
        std::string asked;
        for (auto* s = static_cast<const VkBaseInStructure*>(info->pNext); s; s = s->pNext) asked += " " + std::to_string(s->sType);
        for (uint32_t i = 0; i < chained.enabledExtensionCount; i++) asked += std::string(" ") + chained.ppEnabledExtensionNames[i];
        LOG("vkCreateDevice failed (%d); structs and extensions:%s", r, asked.c_str());
    }
    if (r != VK_SUCCESS) return r;
    // Device calls start at the top layer. Asking a layer's instance proc addr for
    // vkGetDeviceProcAddr would skip it.
    auto gdpa = g_layers.empty() ? driver_gdpa : g_layers[0].gdpa;
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_dev_pd[*out] = pd;
        g_gdpa[*out] = gdpa;
    }
    on_device(pd, *out, info);
    return r;
}

VKAPI_ATTR void VKAPI_CALL destroy_device(VkDevice dev, const VkAllocationCallbacks* alloc) {
    on_device_destroy(dev);
    auto real = NEXT_D(dev, vkDestroyDevice);
    {
        std::lock_guard<std::mutex> l(g_lock);
        for (auto it = g_queue_dev.begin(); it != g_queue_dev.end();) it = it->second == dev ? g_queue_dev.erase(it) : ++it;
        for (auto it = g_cmd_dev.begin(); it != g_cmd_dev.end();) it = it->second == dev ? g_cmd_dev.erase(it) : ++it;
        for (auto it = g_next.begin(); it != g_next.end();) it = it->first.first == dev ? g_next.erase(it) : ++it;
        g_dev_pd.erase(dev);
        g_gdpa.erase(dev);
    }
    real(dev, alloc);
}

VKAPI_ATTR void VKAPI_CALL get_device_queue(VkDevice dev, uint32_t family, uint32_t index, VkQueue* q) {
    NEXT_D(dev, vkGetDeviceQueue)(dev, family, index, q);
    std::lock_guard<std::mutex> l(g_lock);
    if (*q) g_queue_dev[*q] = dev;
}

VKAPI_ATTR void VKAPI_CALL get_device_queue2(VkDevice dev, const VkDeviceQueueInfo2* info, VkQueue* q) {
    // The plain call gets the same queue. Layers that only track vkGetDeviceQueue (bcn_layer) see it.
    if (!info->flags && !info->pNext)
        NEXT_D(dev, vkGetDeviceQueue)(dev, info->queueFamilyIndex, info->queueIndex, q);
    else
        NEXT_D(dev, vkGetDeviceQueue2)(dev, info, q);
    std::lock_guard<std::mutex> l(g_lock);
    if (*q) g_queue_dev[*q] = dev;
}

VKAPI_ATTR VkResult VKAPI_CALL allocate_command_buffers(VkDevice dev, const VkCommandBufferAllocateInfo* info,
                                                        VkCommandBuffer* cmds) {
    VkResult r = NEXT_D(dev, vkAllocateCommandBuffers)(dev, info, cmds);
    if (r == VK_SUCCESS) {
        std::lock_guard<std::mutex> l(g_lock);
        for (uint32_t i = 0; i < info->commandBufferCount; i++) g_cmd_dev[cmds[i]] = dev;
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL free_command_buffers(VkDevice dev, VkCommandPool pool, uint32_t n, const VkCommandBuffer* cmds) {
    NEXT_D(dev, vkFreeCommandBuffers)(dev, pool, n, cmds);
    std::lock_guard<std::mutex> l(g_lock);
    for (uint32_t i = 0; i < n; i++) g_cmd_dev.erase(cmds[i]);
}

PFN_vkVoidFunction get_device_proc_addr(VkDevice dev, const char* name);
PFN_vkVoidFunction get_instance_proc_addr(VkInstance inst, const char* name);

// The core's own hooks. They keep the handle maps; a module never replaces these names.
const Hook kCore[] = {
    {"vkGetInstanceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(get_instance_proc_addr)},
    {"vkGetDeviceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(get_device_proc_addr)},
    {"vkCreateInstance", reinterpret_cast<PFN_vkVoidFunction>(create_instance)},
    {"vkEnumeratePhysicalDevices", reinterpret_cast<PFN_vkVoidFunction>(enumerate_physical_devices)},
    {"vkEnumeratePhysicalDeviceGroups", reinterpret_cast<PFN_vkVoidFunction>(enumerate_physical_device_groups)},
    {"vkEnumeratePhysicalDeviceGroupsKHR", reinterpret_cast<PFN_vkVoidFunction>(enumerate_physical_device_groups)},
    {"vkCreateDevice", reinterpret_cast<PFN_vkVoidFunction>(create_device)},
    {"vkDestroyDevice", reinterpret_cast<PFN_vkVoidFunction>(destroy_device)},
    {"vkGetDeviceQueue", reinterpret_cast<PFN_vkVoidFunction>(get_device_queue)},
    {"vkGetDeviceQueue2", reinterpret_cast<PFN_vkVoidFunction>(get_device_queue2)},
    {"vkAllocateCommandBuffers", reinterpret_cast<PFN_vkVoidFunction>(allocate_command_buffers)},
    {"vkFreeCommandBuffers", reinterpret_cast<PFN_vkVoidFunction>(free_command_buffers)},
    {nullptr, nullptr},
};

// A replacement is only offered where the driver has the call, so feature probing stays honest.
PFN_vkVoidFunction wrapped(PFN_vkVoidFunction real, const char* name) {
    if (!real) return nullptr;
    if (const Hook* h = find_hook(kCore, name)) return h->fn;
    if (const Hook* h = find_hook(hooks(), name)) return h->fn;
    return real;
}

PFN_vkVoidFunction any_handle(const char* name);

PFN_vkVoidFunction get_instance_proc_addr(VkInstance inst, const char* name) {
    if (!load() || !name) return nullptr;
    // gfxstream also looks up instance and device calls with no instance, as a loader exports them,
    // and calls them for every instance and device it has. A driver has nothing to give for those,
    // so they get a stub that finds the right chain from the call's first handle.
    if (!inst) {
        PFN_vkVoidFunction real = g_gipa(nullptr, name);
        if (!real && strncmp(name, "vkEnumerateInstance", 19) && strcmp(name, "vkCreateInstance")) return any_handle(name);
    }
    return wrapped((inst ? top_gipa() : g_gipa)(inst, name), name);
}

PFN_vkVoidFunction get_device_proc_addr(VkDevice dev, const char* name) {
    if (!name) return nullptr;
    PFN_vkGetDeviceProcAddr gdpa;
    {
        std::lock_guard<std::mutex> l(g_lock);
        auto it = g_gdpa.find(dev);
        if (it == g_gdpa.end()) return nullptr;
        gdpa = it->second;
    }
    return wrapped(gdpa(dev, name), name);
}

template <typename K, typename V>
V lookup(const std::unordered_map<K, V>& m, K k) {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = m.find(k);
    return it == m.end() ? V() : it->second;
}

// The stubs any_handle hands out: stub i saves the call's arguments, asks resolve_stub for the
// function name i maps to on the first argument's chain, puts the arguments back and jumps there.
constexpr int kStubs = 512;
const char* g_stub_name[kStubs];
int g_stubs_used;

PFN_vkVoidFunction resolve_handle(void* handle, const char* name) {
    VkDevice dev = VK_NULL_HANDLE;
    VkInstance inst = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> l(g_lock);
        if (g_gdpa.count(static_cast<VkDevice>(handle))) dev = static_cast<VkDevice>(handle);
        else if (auto q = g_queue_dev.find(static_cast<VkQueue>(handle)); q != g_queue_dev.end()) dev = q->second;
        else if (auto c = g_cmd_dev.find(static_cast<VkCommandBuffer>(handle)); c != g_cmd_dev.end()) dev = c->second;
        else if (auto pd = g_pd_inst.find(static_cast<VkPhysicalDevice>(handle)); pd != g_pd_inst.end()) inst = pd->second;
        else inst = static_cast<VkInstance>(handle);
    }
    return dev ? get_device_proc_addr(dev, name) : get_instance_proc_addr(inst, name);
}

extern "C" __attribute__((used, visibility("hidden"))) PFN_vkVoidFunction hwgpu_resolve_stub(void* handle, int i) {
    PFN_vkVoidFunction fn = handle ? resolve_handle(handle, g_stub_name[i]) : nullptr;
    if (!fn) __android_log_print(ANDROID_LOG_ERROR, "hwgpu", "wrapper: no %s for handle %p", g_stub_name[i], handle);
    return fn;
}

extern "C" void hwgpu_stub0();
#if defined(__aarch64__)
// 8 bytes per stub: mov w16, #i; b common. common keeps x0-x7 and q0-q7, the argument registers.
__asm__(R"(
    .text
    .p2align 3
    .globl hwgpu_stub0
    .hidden hwgpu_stub0
hwgpu_stub0:
    .set i, 0
    .rept 512
    mov w16, #i
    b hwgpu_stub_common
    .set i, i + 1
    .endr
hwgpu_stub_common:
    stp x29, x30, [sp, #-208]!
    mov x29, sp
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    stp q0, q1, [sp, #80]
    stp q2, q3, [sp, #112]
    stp q4, q5, [sp, #144]
    stp q6, q7, [sp, #176]
    str x8, [sp, #200]
    mov w1, w16
    bl hwgpu_resolve_stub
    mov x17, x0
    ldp x0, x1, [sp, #16]
    ldp x2, x3, [sp, #32]
    ldp x4, x5, [sp, #48]
    ldp x6, x7, [sp, #64]
    ldp q0, q1, [sp, #80]
    ldp q2, q3, [sp, #112]
    ldp q4, q5, [sp, #144]
    ldp q6, q7, [sp, #176]
    ldr x8, [sp, #200]
    ldp x29, x30, [sp], #208
    cbz x17, 1f
    br x17
1:  ret
)");
#endif

PFN_vkVoidFunction any_handle(const char* name) {
#if defined(__aarch64__)
    std::lock_guard<std::mutex> l(g_lock);
    for (int i = 0; i < g_stubs_used; i++)
        if (!strcmp(g_stub_name[i], name)) return reinterpret_cast<PFN_vkVoidFunction>(reinterpret_cast<char*>(&hwgpu_stub0) + 8 * i);
    if (g_stubs_used == kStubs) return nullptr;
    g_stub_name[g_stubs_used] = strdup(name);
    return reinterpret_cast<PFN_vkVoidFunction>(reinterpret_cast<char*>(&hwgpu_stub0) + 8 * g_stubs_used++);
#else
    (void)name;
    return nullptr;
#endif
}

}  // namespace

PFN_vkVoidFunction next_i(VkInstance inst, const char* name) {
    {
        std::lock_guard<std::mutex> l(g_lock);
        auto it = g_next.find({inst, name});
        if (it != g_next.end()) return it->second;
    }
    PFN_vkVoidFunction fn = (inst ? top_gipa() : g_gipa)(inst, name);
    std::lock_guard<std::mutex> l(g_lock);
    g_next[{inst, name}] = fn;
    return fn;
}

PFN_vkVoidFunction next_d(VkDevice dev, const char* name) {
    PFN_vkGetDeviceProcAddr gdpa;
    {
        std::lock_guard<std::mutex> l(g_lock);
        auto it = g_next.find({dev, name});
        if (it != g_next.end()) return it->second;
        auto g = g_gdpa.find(dev);
        if (g == g_gdpa.end()) return nullptr;
        gdpa = g->second;
    }
    PFN_vkVoidFunction fn = gdpa(dev, name);
    std::lock_guard<std::mutex> l(g_lock);
    g_next[{dev, name}] = fn;
    return fn;
}

VkDevice device_of(VkQueue q) { return lookup(g_queue_dev, q); }
VkDevice device_of(VkCommandBuffer c) { return lookup(g_cmd_dev, c); }
VkInstance instance_of(VkPhysicalDevice pd) { return lookup(g_pd_inst, pd); }
VkPhysicalDevice physical_of(VkDevice d) { return lookup(g_dev_pd, d); }

}  // namespace wrap

// What gfxstream (or any loader) looks up when it opens this library as its Vulkan driver.
EXPORT PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance inst, const char* name) {
    return wrap::get_instance_proc_addr(inst, name);
}

EXPORT VkResult vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* version) {
    if (*version > 5) *version = 5;
    return VK_SUCCESS;
}
