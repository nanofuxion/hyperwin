/* The Direct3D 12 driver part (wddm/README.md phase 8). Microsoft's D3D12Core.dll loads this DLL by the
 * adapter's fourth UserModeDriverName and calls OpenAdapter12. So far this only answers the opening
 * questions and writes them to the log, to learn what the runtime asks before a device exists. */
#ifdef _WIN64
#include <utility>
#ifndef __out
#define __out
#endif
#include <d3d12umddi.h>

namespace d12 {

struct Adapter {
    D3D12DDI_HRTADAPTER rt;
    const D3DDDI_ADAPTERCALLBACKS *cb;
};

static SIZE_T APIENTRY CalcPrivateDeviceSize(D3D12DDI_HADAPTER, const D3D12DDIARG_CALCPRIVATEDEVICESIZE *a)
{
    ulog("hwd3d_umd: Direct3D 12 CalcPrivateDeviceSize, interface 0x%x version 0x%x flags 0x%x\n", a->Interface, a->Version, (unsigned)a->Flags);
    return 64;
}

static HRESULT APIENTRY CreateDevice(D3D12DDI_HADAPTER, const D3D12DDIARG_CREATEDEVICE_0003 *a)
{
    ulog("hwd3d_umd: Direct3D 12 CreateDevice, interface 0x%x version 0x%x flags 0x%x: not done yet\n", a->Interface, a->Version, (unsigned)a->Flags);
    return E_NOTIMPL;
}

static HRESULT APIENTRY CloseAdapter(D3D12DDI_HADAPTER h)
{
    ulog("hwd3d_umd: Direct3D 12 CloseAdapter\n");
    delete static_cast<Adapter *>(h.pDrvPrivate);
    umd_adapter_closed();
    return S_OK;
}

static HRESULT APIENTRY GetSupportedVersions(D3D12DDI_HADAPTER, UINT32 *n, UINT64 *v)
{
    static const UINT64 kVersions[] = {D3D12DDI_SUPPORTED_0043};
    ulog("hwd3d_umd: Direct3D 12 GetSupportedVersions, room for %u, list %p\n", n ? *n : 0, (void *)v);
    const UINT32 have = sizeof kVersions / sizeof kVersions[0];
    if (v) {
        if (*n < have) return E_INVALIDARG;
        memcpy(v, kVersions, sizeof kVersions);
    }
    *n = have;
    return S_OK;
}

static HRESULT APIENTRY GetCaps(D3D12DDI_HADAPTER, const D3D12DDIARG_GETCAPS *g)
{
    ulog("hwd3d_umd: Direct3D 12 GetCaps %u, %u bytes, info %p\n", (unsigned)g->Type, g->DataSize, g->pInfo);
    if (g->pData && g->DataSize) memset(g->pData, 0, g->DataSize);
    return S_OK;
}

static HRESULT APIENTRY GetOptionalDDITables(D3D12DDI_HADAPTER, UINT32 *n, D3D12DDI_TABLE_REQUEST *)
{
    ulog("hwd3d_umd: Direct3D 12 GetOptionalDDITables, room for %u\n", n ? *n : 0);
    if (n) *n = 0;
    return S_OK;
}

/* Every entry of every table exists from the start: one not written yet names itself in the log when
 * first called and returns zero. One stub per table kind and slot, since a stub cannot see who called it.
 * (64-bit only: there the caller cleans up, so one shape fits every entry.) */
#include "umd12_names.inc"
enum { kKinds12 = 10, kSlots12 = 128 };
static const unsigned kKindTypes12[kKinds12] = {0, 1, 2, 3, 8, 9, 20, 24, 26, 27};
static const char *const *g_names12[kKinds12];
static unsigned g_count12[kKinds12];
static LONG g_seen12[kKinds12 * kSlots12];
template <int I> struct Stub12 {
    static INT_PTR fn()
    {
        if (!InterlockedExchange(&g_seen12[I], 1)) {
            const int k = I / kSlots12, n = I % kSlots12;
            ulog("hwd3d_umd: Direct3D 12 %s (table type %u, entry %d) is not done yet\n",
                 g_names12[k] && (unsigned)n < g_count12[k] ? g_names12[k][n] : "?", kKindTypes12[k], n);
        }
        return 0;
    }
};
template <size_t... I> static void *const *stubs12(std::index_sequence<I...>)
{
    static void *const t[] = {reinterpret_cast<void *>(&Stub12<I>::fn)...};
    return t;
}

static HRESULT APIENTRY FillDDITable(D3D12DDI_HADAPTER, D3D12DDI_TABLE_TYPE type, VOID *table, SIZE_T size, UINT num, D3D12DDI_HRTTABLE)
{
    const unsigned count = (unsigned)(size / sizeof(void *));
    int k = -1;
    for (int i = 0; i < kKinds12; i++) if (kKindTypes12[i] == (unsigned)type) k = i;
    const char *name = "?";
    if (k >= 0) {
        for (const auto &t : kTables12) {
            if (t.type != (unsigned)type || t.count != count) continue;
            g_names12[k] = t.names;
            g_count12[k] = t.count;
            name = t.table;
        }
    }
    ulog("hwd3d_umd: Direct3D 12 FillDDITable type %u, %u entries, table %u: %s\n", (unsigned)type, count, num, name);
    if (k < 0 || count > kSlots12) return E_NOTIMPL;
    void *const *st = stubs12(std::make_index_sequence<kKinds12 * kSlots12>());
    void **out = static_cast<void **>(table);
    for (unsigned i = 0; i < count; i++) out[i] = st[k * kSlots12 + i];
    return S_OK;
}

static VOID APIENTRY DestroyDevice(D3D12DDI_HDEVICE) { ulog("hwd3d_umd: Direct3D 12 DestroyDevice\n"); }

} // namespace d12

#endif

extern "C" HRESULT APIENTRY OpenAdapter12(void *arg)
{
#ifndef _WIN64
    (void)arg;
    return E_NOTIMPL;   /* 32-bit Direct3D 12 programs are rare: not served yet */
#else
    D3D12DDIARG_OPENADAPTER *a = static_cast<D3D12DDIARG_OPENADAPTER *>(arg);
    ulog("hwd3d_umd: OpenAdapter12\n");
    if (!a || !a->pAdapterFuncs) return E_INVALIDARG;
    d12::Adapter *ad = new d12::Adapter{a->hRTAdapter, a->pAdapterCallbacks};
    a->hAdapter.pDrvPrivate = ad;
    a->pAdapterFuncs->pfnCalcPrivateDeviceSize = d12::CalcPrivateDeviceSize;
    a->pAdapterFuncs->pfnCreateDevice = d12::CreateDevice;
    a->pAdapterFuncs->pfnCloseAdapter = d12::CloseAdapter;
    a->pAdapterFuncs->pfnGetSupportedVersions = d12::GetSupportedVersions;
    a->pAdapterFuncs->pfnGetCaps = d12::GetCaps;
    a->pAdapterFuncs->pfnGetOptionalDDITables = d12::GetOptionalDDITables;
    a->pAdapterFuncs->pfnFillDDITable = d12::FillDDITable;
    a->pAdapterFuncs->pfnDestroyDevice = d12::DestroyDevice;
    umd_adapter_opened();
    return S_OK;
#endif
}
