/* Placeholder ICD entry points. Windows' vulkan-1.dll loads this the same way it
 * loads any GPU driver. It does not speak virtio-gpu yet, so instance creation
 * reports an incompatible driver and the loader moves on.
 */
#include <stdint.h>
#if defined(HYPERWIN_NO_WINDOWS)
#include "win.h"
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define VK_SUCCESS 0
#define VK_ERROR_INCOMPATIBLE_DRIVER (-9)

typedef void *PFN_vkVoidFunction;

__declspec(dllexport) PFN_vkVoidFunction WINAPI vk_icdGetInstanceProcAddr(void *instance, const char *name);
__declspec(dllexport) PFN_vkVoidFunction WINAPI vk_icdGetPhysicalDeviceProcAddr(void *device, const char *name);

static int name_is(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static int32_t WINAPI create_instance(const void *info, const void *alloc, void **instance)
{
    (void)info; (void)alloc; (void)instance;
    return VK_ERROR_INCOMPATIBLE_DRIVER;
}

static int32_t WINAPI enum_instance_ext(const char *layer, uint32_t *count, void *props)
{
    (void)layer; (void)props;
    if (count) *count = 0;
    return VK_SUCCESS;
}

__declspec(dllexport) int32_t WINAPI vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *version)
{
    if (!version) return -1;
    if (*version > 5) *version = 5;
    return 0;
}

__declspec(dllexport) PFN_vkVoidFunction WINAPI vk_icdGetInstanceProcAddr(void *instance, const char *name)
{
    (void)instance;
    if (name_is(name, "vkCreateInstance")) return (PFN_vkVoidFunction)create_instance;
    if (name_is(name, "vkEnumerateInstanceExtensionProperties")) return (PFN_vkVoidFunction)enum_instance_ext;
    if (name_is(name, "vk_icdGetInstanceProcAddr")) return (PFN_vkVoidFunction)vk_icdGetInstanceProcAddr;
    if (name_is(name, "vk_icdNegotiateLoaderICDInterfaceVersion"))
        return (PFN_vkVoidFunction)vk_icdNegotiateLoaderICDInterfaceVersion;
    return 0;
}

__declspec(dllexport) PFN_vkVoidFunction WINAPI vk_icdGetPhysicalDeviceProcAddr(void *device, const char *name)
{
    (void)device; (void)name;
    return 0;
}

BOOL WINAPI DllMain(void *module, DWORD reason, void *reserved)
{
    (void)module; (void)reason; (void)reserved;
    return TRUE;
}
