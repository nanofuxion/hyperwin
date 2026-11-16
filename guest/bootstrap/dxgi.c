#include <stdint.h>
#if defined(HYPERWIN_NO_WINDOWS)
#include "win.h"
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

/* The factory exists so a privately deployed dxgi.dll loads. Adapter enumeration
 * stays unimplemented: AVF's GPU is the virtio-gpu device, not a DXGI software adapter.
 */
static void *factory_vt[8];
static struct { void **vt; uint32_t refs; } factory;
static int ready;

static HRESULT WINAPI notimpl(void) { return E_NOTIMPL; }
static HRESULT WINAPI qi(void *self, void *iid, void **out)
{
    (void)iid;
    if (!out) return E_INVALIDARG;
    *out = self;
    return S_OK;
}
static uint32_t WINAPI addref(void *self) { (void)self; return 1; }
static uint32_t WINAPI release(void *self) { (void)self; return 1; }

static void boot(void)
{
    int i;
    if (ready) return;
    for (i = 0; i < 8; i++) factory_vt[i] = (void *)notimpl;
    factory_vt[0] = (void *)qi;
    factory_vt[1] = (void *)addref;
    factory_vt[2] = (void *)release;
    factory.vt = factory_vt;
    factory.refs = 1;
    ready = 1;
}

__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory(void *iid, void **out)
{
    (void)iid;
    if (!out) return E_INVALIDARG;
    boot();
    *out = &factory;
    return S_OK;
}

__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory1(void *iid, void **out)
{
    return CreateDXGIFactory(iid, out);
}

__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory2(DWORD flags, void *iid, void **out)
{
    (void)flags;
    return CreateDXGIFactory(iid, out);
}

BOOL WINAPI DllMain(void *module, DWORD reason, void *reserved)
{
    (void)module; (void)reason; (void)reserved;
    return TRUE;
}
