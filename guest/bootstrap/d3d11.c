#if defined(HYPERWIN_NO_WINDOWS)
#include "win.h"
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

HRESULT WINAPI hw_create_device(const void *swap_desc, void **out_swap, void **out_device, void **out_context);

__declspec(dllexport) HRESULT WINAPI D3D11CreateDevice(
    void *adapter, DWORD driver, void *software, DWORD flags,
    const DWORD *levels, DWORD level_count, DWORD sdk,
    void **device, DWORD *level, void **context)
{
    (void)adapter; (void)driver; (void)software; (void)flags;
    (void)levels; (void)level_count; (void)sdk;
    if (level) *level = 0xb000; /* D3D_FEATURE_LEVEL_11_0 */
    return hw_create_device(0, 0, device, context);
}

__declspec(dllexport) HRESULT WINAPI D3D11CreateDeviceAndSwapChain(
    void *adapter, DWORD driver, void *software, DWORD flags,
    const DWORD *levels, DWORD level_count, DWORD sdk,
    const void *swap_desc, void **swapchain, void **device, DWORD *level, void **context)
{
    (void)adapter; (void)driver; (void)software; (void)flags;
    (void)levels; (void)level_count; (void)sdk;
    if (level) *level = 0xb000;
    return hw_create_device(swap_desc, swapchain, device, context);
}

BOOL WINAPI DllMain(void *module, DWORD reason, void *reserved)
{
    (void)module; (void)reason; (void)reserved;
    return TRUE;
}
