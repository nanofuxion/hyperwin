// Shared core of the phone-side wrapper drivers (libvulkan_vortek.so, ...).
//
// gfxstream loads one of these instead of the phone's Vulkan (ANDROID_EMU_VK_LOADER_PATH). The
// wrapper loads the real driver itself: HWGPU_VK_DRIVER names a driver .so (Turnip and other
// custom drivers), otherwise the system loader, libvulkan.so. Handles pass through untouched; the
// wrapper only replaces the calls its fixes need and forwards everything else.
//
// A fix module fills in wrap_hooks(): name -> replacement. Replacements call the real driver
// through next_i() / next_d().
#ifndef HWGPU_WRAP_H
#define HWGPU_WRAP_H

#include <vulkan/vulkan.h>

#include <mutex>
#include <unordered_map>

namespace wrap {

struct Hook {
    const char* name;
    PFN_vkVoidFunction fn;
};

// Supplied by the fix module. Ends with {nullptr, nullptr}.
const Hook* hooks();

// The real driver's entry points.
PFN_vkVoidFunction next_i(VkInstance instance, const char* name);
PFN_vkVoidFunction next_d(VkDevice device, const char* name);

// Which device a queue or command buffer belongs to, and which instance a physical device does.
VkDevice device_of(VkQueue queue);
VkDevice device_of(VkCommandBuffer cmd);
VkInstance instance_of(VkPhysicalDevice pd);
VkPhysicalDevice physical_of(VkDevice device);

// Typed lookup: NEXT_I(instance, vkGetPhysicalDeviceFeatures)(pd, &f)
#define NEXT_I(inst, fn) reinterpret_cast<PFN_##fn>(wrap::next_i((inst), #fn))
#define NEXT_D(dev, fn) reinterpret_cast<PFN_##fn>(wrap::next_d((dev), #fn))

// Called by the core after the real create calls succeed, so a module can set up its state.
// Modules that need nothing leave these alone (weak defaults in wrap.cpp).
void on_instance(VkInstance instance, const VkInstanceCreateInfo* info);
void on_device(VkPhysicalDevice pd, VkDevice device, const VkDeviceCreateInfo* info);
void on_device_destroy(VkDevice device);
// Called with the core's own copy of a device create info before the driver sees it. A module may
// repoint its arrays or edit the structs its pNext chain points at.
void patch_device_create(VkPhysicalDevice pd, VkDeviceCreateInfo* info);

// Loads bcn_layer (GPU BC texture decoding) from beside the library that holds `self`, unless
// HWGPU_VK_LAYERS is set. For a module's load-time constructor.
void default_bcn_layer(void* self);

}  // namespace wrap

#endif
