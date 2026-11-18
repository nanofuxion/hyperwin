// libhwd3d.so: the phone side of HwD3D (d3d/README.md), in the renderer process (:gpu).
//
// The sgpu server (gpu/host/hwgpu_host.cpp) hands a channel here when its first message is an
// HwD3D hello. Each channel then gets its own thread, which reads requests (include/hw_d3d.h) and
// calls DXVK (built for Android by build.sh). Every object the guest holds is a handle into the
// channel's table, checked for type and generation on every use. The generated thunks
// (gen_thunks.py) read the arguments of most methods; the rest are below.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <hw_d3d.h>
#include <hwgpu_ring.h>

#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <map>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#include <sys/system_properties.h>
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "hwd3d", __VA_ARGS__)
#else
#include <stdio.h>
#define LOG(...) (fprintf(stderr, "hwd3d: " __VA_ARGS__), fputc('\n', stderr))
#endif

// libhwgpu.so: the surface swap chains draw on (hwgpu_host.cpp).
extern "C" void* hwgpu_host_d3d_window_acquire(void);
extern "C" void hwgpu_host_d3d_window_release(void* window);
extern "C" void hwgpu_host_d3d_show(int on);
extern "C" int hwgpu_host_vsync(void);

namespace {

void* const BAD = reinterpret_cast<void*>(uintptr_t(1));

// Reads a request's arguments. Past the end it reads zeroes and clears ok.
struct Rd {
    const uint8_t *p, *end;
    bool ok = true;
    const void* take(uint64_t n) {
        uint64_t padded = (n + 3) & ~3ull;
        if (uint64_t(end - p) < padded) {
            ok = false;
            p = end;
            return nullptr;
        }
        const void* at = p;
        p += padded;
        return at;
    }
    void get(void* dst, uint32_t n) {
        const void* s = take(n);
        if (s) memcpy(dst, s, n);
        else memset(dst, 0, n);
    }
    uint32_t u32() {
        uint32_t v;
        get(&v, 4);
        return v;
    }
    // An array: present, count, elements. Null when absent; n gets the count.
    const void* arr(uint32_t el, uint64_t* n) {
        *n = 0;
        if (!u32()) return nullptr;
        uint64_t count = u32();
        const void* at = take(count * el);
        if (at) *n = count;
        return at;
    }
};

// An answer's outputs.
struct Wr {
    std::vector<uint8_t> b;
    void put(const void* v, size_t n) {
        size_t at = b.size();
        b.resize(at + ((n + 3) & ~size_t(3)), 0);
        if (n) memcpy(b.data() + at, v, n);
    }
    void u32(uint32_t v) { put(&v, 4); }
    void arr(const void* v, size_t n) {
        if (v) put(v, n);
    }
};

struct Chan;
using HostFn = int32_t (*)(Chan&, void*, Rd&, Wr&);

#include "hwd3d_host_pre.inc"

int iface_of(const GUID& g) {
    for (int i = 0; i < HI_COUNT; i++)
        if (!memcmp(&g, &hi_iid[i], sizeof g)) return i;
    return -1;
}

bool derives(int have, int want) {
    if (want == HI_IUnknown) return true;
    for (int i = have; i >= 0; i = hi_parent[i])
        if (i == want) return true;
    return false;
}

struct Mapped {
    void* res;  // the record's pointer, an ID3D11Resource
    uint32_t sub, size;
    uint8_t* data;
};

struct Chan {
    void* chan = nullptr;
    uint32_t slot = 0, generation = 0;
    uint64_t received = 0;

    struct Obj {
        void* ptr = nullptr;        // the interface iface names, holding one reference
        IUnknown* ident = nullptr;  // the object's IUnknown, to find it again
        int16_t iface = -1;
        uint16_t gen = 0;
        uint64_t born = 0;
    };
    std::vector<Obj> table = std::vector<Obj>(1);  // index 0 stays empty: handle 0 is null
    std::vector<uint32_t> free_list;
    std::map<std::pair<IUnknown*, int>, uint32_t> found;
    uint64_t births = 0;
    std::vector<Mapped> maps;
    ID3D11DeviceContext* immediate = nullptr;  // the first device's, for unmapping at the end
    void* window = nullptr;                    // the surface, while this program has it
    uint32_t swapchain = 0;

    static uint32_t handle(uint32_t i, uint16_t gen) { return i | (uint32_t(gen & 0xFFF) << 20); }

    // A handle for an object a call gave out, taking over its reference.
    uint32_t put(void* p, int iface) {
        if (!p) return 0;
        IUnknown* u = static_cast<IUnknown*>(p);
        if (iface < 0 || iface >= HI_COUNT) {
            u->Release();
            return 0;
        }
        IUnknown* ident = nullptr;
        if (FAILED(u->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&ident))) || !ident) ident = u;
        else ident->Release();  // the object stays alive through p
        auto it = found.find({ident, iface});
        if (it != found.end()) {
            u->Release();  // the record holds one already
            return it->second;
        }
        uint32_t i;
        if (!free_list.empty()) {
            i = free_list.back();
            free_list.pop_back();
        } else {
            if (table.size() >= (1u << 20)) {
                u->Release();
                return 0;
            }
            i = uint32_t(table.size());
            table.emplace_back();
        }
        Obj& o = table[i];
        o.ptr = p;
        o.ident = ident;
        o.iface = int16_t(iface);
        o.gen = uint16_t((o.gen + 1) & 0xFFF);
        if (!o.gen) o.gen = 1;
        o.born = ++births;
        uint32_t h = handle(i, o.gen);
        found[{ident, iface}] = h;
        return h;
    }

    // The object behind a handle, as the interface want or one derived from it. Null for 0, BAD
    // for a handle that is stale, made up, or of another type.
    void* obj(uint32_t h, int want) {
        if (!h) return nullptr;
        uint32_t i = h & 0xFFFFF;
        if (i >= table.size() || !table[i].ptr || handle(i, table[i].gen) != h || !derives(table[i].iface, want))
            return BAD;
        return table[i].ptr;
    }

    // An array of handles: present, count, handles.
    template <typename T>
    bool objs(Rd& r, std::vector<T*>& v, int want) {
        if (!r.u32()) return true;
        uint32_t n = r.u32();
        if (n > 4096) return false;
        v.resize(n);
        for (auto& p : v) {
            void* o = obj(r.u32(), want);
            if (o == BAD) return false;
            p = static_cast<T*>(o);
        }
        return r.ok;
    }

    void release(uint32_t h) {
        uint32_t i = h & 0xFFFFF;
        if (!h || i >= table.size() || !table[i].ptr || handle(i, table[i].gen) != h) return;
        Obj& o = table[i];
        found.erase({o.ident, o.iface});
        if (h == swapchain) {
            swapchain = 0;
            hwgpu_host_d3d_show(0);
        }
        void* p = o.ptr;
        o.ptr = nullptr;
        o.ident = nullptr;
        free_list.push_back(i);
        static_cast<IUnknown*>(p)->Release();
    }

    // What the program still held, newest first, as it would have let go.
    void release_all() {
        if (immediate) {
            for (auto& m : maps) immediate->Unmap(static_cast<ID3D11Resource*>(m.res), m.sub);
            maps.clear();
            immediate->Release();
            immediate = nullptr;
        }
        std::vector<std::pair<uint64_t, uint32_t>> live;
        for (uint32_t i = 1; i < table.size(); i++)
            if (table[i].ptr) live.push_back({table[i].born, handle(i, table[i].gen)});
        std::sort(live.rbegin(), live.rend());
        for (auto& l : live) release(l.second);
        if (window) hwgpu_host_d3d_window_release(window);
        window = nullptr;
    }
};

#include "hwd3d_host.inc"

// ---- methods written by hand ----

int32_t hc_IUnknown_QueryInterface(Chan& c, void* self, Rd& r, Wr& w) {
    uint32_t want = r.u32();
    if (!r.ok || want >= HI_COUNT) return E_INVALIDARG;
    void* o = nullptr;
    HRESULT hr = static_cast<IUnknown*>(self)->QueryInterface(hi_iid[want], &o);
    w.u32(SUCCEEDED(hr) ? c.put(o, int(want)) : 0);
    return hr;
}

// A region's bytes: row bytes, rows, then the bytes, packed. Checked against what the resource
// needs, so DXVK never reads past them.
bool read_rows(Rd& r, const hwd3d_res& res, uint32_t sub, const uint32_t* box, D3D11_SUBRESOURCE_DATA* out) {
    uint32_t row = r.u32(), rows = r.u32(), want_row, want_rows, slices;
    uint64_t n = 0;
    const void* data = r.arr(1, &n);
    if (!r.ok || !data || !hwd3d_region(&res, sub, box, &want_row, &want_rows, &slices)) return false;
    if (row != want_row || rows != want_rows || n != uint64_t(row) * rows * slices) return false;
    *out = {data, row, row * rows};
    return true;
}

// Create* with first contents: the description, a count of subresources, each one's rows.
template <typename Desc, typename Iface, typename Shape, typename Make>
int32_t create_with_data(Chan& c, Rd& r, Wr& w, int iface, Shape shape, Make make) {
    uint64_t n = 0;
    const void* d = r.arr(sizeof(Desc), &n);
    if (!r.ok || !d || n != 1) return E_INVALIDARG;
    Desc desc;
    memcpy(&desc, d, sizeof desc);
    uint32_t layers = 1;
    hwd3d_res res = shape(desc, &layers);
    uint32_t subs = r.u32();
    std::vector<D3D11_SUBRESOURCE_DATA> init(subs);
    if (subs) {
        if (!res.mips) {
            uint32_t big = std::max({res.width, res.height, res.depth});
            for (res.mips = 1; big > 1; big >>= 1) res.mips++;
        }
        if (subs != res.mips * layers) return E_INVALIDARG;
        for (uint32_t i = 0; i < subs; i++)
            if (!read_rows(r, res, i, nullptr, &init[i])) return E_INVALIDARG;
    }
    bool want = r.u32() != 0;
    if (!r.ok) return E_INVALIDARG;
    Iface* o = nullptr;
    HRESULT hr = make(&desc, subs ? init.data() : nullptr, want ? &o : nullptr);
    w.u32(c.put(o, iface));
    return hr;
}

int32_t hc_ID3D11Device_CreateBuffer(Chan& c, void* self, Rd& r, Wr& w) {
    auto* dev = static_cast<ID3D11Device*>(self);
    return create_with_data<D3D11_BUFFER_DESC, ID3D11Buffer>(
        c, r, w, HI_ID3D11Buffer,
        [](const D3D11_BUFFER_DESC& d, uint32_t*) { return hwd3d_res{1, 0, d.ByteWidth, 1, 1, 1}; },
        [&](const D3D11_BUFFER_DESC* d, const D3D11_SUBRESOURCE_DATA* init, ID3D11Buffer** o) {
            return dev->CreateBuffer(d, init, o);
        });
}

int32_t hc_ID3D11Device_CreateTexture1D(Chan& c, void* self, Rd& r, Wr& w) {
    auto* dev = static_cast<ID3D11Device*>(self);
    return create_with_data<D3D11_TEXTURE1D_DESC, ID3D11Texture1D>(
        c, r, w, HI_ID3D11Texture1D,
        [](const D3D11_TEXTURE1D_DESC& d, uint32_t* layers) {
            *layers = d.ArraySize;
            return hwd3d_res{2, uint32_t(d.Format), d.Width, 1, 1, d.MipLevels};
        },
        [&](const D3D11_TEXTURE1D_DESC* d, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture1D** o) {
            return dev->CreateTexture1D(d, init, o);
        });
}

int32_t hc_ID3D11Device_CreateTexture2D(Chan& c, void* self, Rd& r, Wr& w) {
    auto* dev = static_cast<ID3D11Device*>(self);
    return create_with_data<D3D11_TEXTURE2D_DESC, ID3D11Texture2D>(
        c, r, w, HI_ID3D11Texture2D,
        [](const D3D11_TEXTURE2D_DESC& d, uint32_t* layers) {
            *layers = d.ArraySize;
            return hwd3d_res{3, uint32_t(d.Format), d.Width, d.Height, 1, d.MipLevels};
        },
        [&](const D3D11_TEXTURE2D_DESC* d, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** o) {
            return dev->CreateTexture2D(d, init, o);
        });
}

int32_t hc_ID3D11Device_CreateTexture3D(Chan& c, void* self, Rd& r, Wr& w) {
    auto* dev = static_cast<ID3D11Device*>(self);
    return create_with_data<D3D11_TEXTURE3D_DESC, ID3D11Texture3D>(
        c, r, w, HI_ID3D11Texture3D,
        [](const D3D11_TEXTURE3D_DESC& d, uint32_t*) {
            return hwd3d_res{4, uint32_t(d.Format), d.Width, d.Height, d.Depth, d.MipLevels};
        },
        [&](const D3D11_TEXTURE3D_DESC* d, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture3D** o) {
            return dev->CreateTexture3D(d, init, o);
        });
}

int32_t hc_ID3D11Device_CreateInputLayout(Chan& c, void* self, Rd& r, Wr& w) {
    uint32_t n = r.u32();
    if (n > 32) return E_INVALIDARG;  // D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT
    std::vector<D3D11_INPUT_ELEMENT_DESC> el(n);
    for (auto& e : el) {
        uint64_t len = 0;
        auto* name = static_cast<const char*>(r.arr(1, &len));
        uint32_t f[6];
        r.get(f, sizeof f);
        if (!r.ok || !name || !len || name[len - 1]) return E_INVALIDARG;
        e = {name, f[0], DXGI_FORMAT(f[1]), f[2], f[3], D3D11_INPUT_CLASSIFICATION(f[4]), f[5]};
    }
    uint64_t len = 0;
    const void* code = r.arr(1, &len);
    bool want = r.u32() != 0;
    if (!r.ok) return E_INVALIDARG;
    ID3D11InputLayout* o = nullptr;
    HRESULT hr = static_cast<ID3D11Device*>(self)->CreateInputLayout(el.data(), n, code, len, want ? &o : nullptr);
    w.u32(c.put(o, HI_ID3D11InputLayout));
    return hr;
}

// A resource's shape, as hwd3d_region wants it.
hwd3d_res shape(ID3D11Resource* res) {
    D3D11_RESOURCE_DIMENSION dim;
    res->GetType(&dim);
    switch (dim) {
    case D3D11_RESOURCE_DIMENSION_BUFFER: {
        D3D11_BUFFER_DESC d;
        static_cast<ID3D11Buffer*>(res)->GetDesc(&d);
        return {1, 0, d.ByteWidth, 1, 1, 1};
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
        D3D11_TEXTURE1D_DESC d;
        static_cast<ID3D11Texture1D*>(res)->GetDesc(&d);
        return {2, uint32_t(d.Format), d.Width, 1, 1, d.MipLevels};
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
        D3D11_TEXTURE2D_DESC d;
        static_cast<ID3D11Texture2D*>(res)->GetDesc(&d);
        return {3, uint32_t(d.Format), d.Width, d.Height, 1, d.MipLevels};
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
        D3D11_TEXTURE3D_DESC d;
        static_cast<ID3D11Texture3D*>(res)->GetDesc(&d);
        return {4, uint32_t(d.Format), d.Width, d.Height, d.Depth, d.MipLevels};
    }
    default:
        return {};
    }
}

int32_t hc_ID3D11DeviceContext_UpdateSubresource(Chan& c, void* self, Rd& r, Wr&) {
    void* dst = c.obj(r.u32(), HI_ID3D11Resource);
    uint32_t sub = r.u32();
    uint64_t n = 0;
    auto* box = static_cast<const D3D11_BOX*>(r.arr(sizeof(D3D11_BOX), &n));
    if (!r.ok || !dst || dst == BAD) return E_INVALIDARG;
    auto* res = static_cast<ID3D11Resource*>(dst);
    D3D11_SUBRESOURCE_DATA data;
    if (!read_rows(r, shape(res), sub, reinterpret_cast<const uint32_t*>(box), &data)) return E_INVALIDARG;
    static_cast<ID3D11DeviceContext*>(self)->UpdateSubresource(res, sub, box, data.pSysMem, data.SysMemPitch,
                                                               data.SysMemSlicePitch);
    return 0;
}

// Map: the phone maps for real and keeps it until Unmap. A map that reads sends the contents; the
// guest sends them back at Unmap when it wrote.
int32_t hc_ID3D11DeviceContext_Map(Chan& c, void* self, Rd& r, Wr& w) {
    void* p = c.obj(r.u32(), HI_ID3D11Resource);
    uint32_t sub = r.u32(), type = r.u32(), flags = r.u32();
    HRESULT hr = S_OK;
    if (!r.ok || !p || p == BAD) hr = E_INVALIDARG;
    for (auto& m : c.maps)
        if (m.res == p && m.sub == sub) hr = E_INVALIDARG;  // mapped already
    auto* res = static_cast<ID3D11Resource*>(p);
    auto* ctx = static_cast<ID3D11DeviceContext*>(self);
    D3D11_MAPPED_SUBRESOURCE m{};
    uint64_t size = 0;
    if (SUCCEEDED(hr)) hr = ctx->Map(res, sub, D3D11_MAP(type), flags, &m);
    if (SUCCEEDED(hr)) {
        hwd3d_res s = shape(res);
        uint32_t row, rows, slices;
        if (hwd3d_region(&s, sub, nullptr, &row, &rows, &slices))
            size = s.dim == 1 ? row : s.dim == 4 ? uint64_t(m.DepthPitch) * slices : uint64_t(m.RowPitch) * rows;
        if (!size || size > (256u << 20)) {
            ctx->Unmap(res, sub);
            hr = E_NOTIMPL;
        }
    }
    if (FAILED(hr)) {
        for (int i = 0; i < 4; i++) w.u32(0);
        return hr;
    }
    c.maps.push_back({p, sub, uint32_t(size), static_cast<uint8_t*>(m.pData)});
    bool reads = type == D3D11_MAP_READ || type == D3D11_MAP_READ_WRITE || type == D3D11_MAP_WRITE;
    w.u32(m.RowPitch);
    w.u32(m.DepthPitch);
    w.u32(uint32_t(size));
    w.u32(reads);
    if (reads) w.put(m.pData, size);
    return hr;
}

int32_t hc_ID3D11DeviceContext_Unmap(Chan& c, void* self, Rd& r, Wr&) {
    void* p = c.obj(r.u32(), HI_ID3D11Resource);
    uint32_t sub = r.u32();
    uint64_t n = 0;
    const void* data = r.arr(1, &n);
    if (!r.ok || !p || p == BAD) return E_INVALIDARG;
    for (auto m = c.maps.begin(); m != c.maps.end(); ++m) {
        if (m->res != p || m->sub != sub) continue;
        if (data) memcpy(m->data, data, std::min<uint64_t>(n, m->size));
        static_cast<ID3D11DeviceContext*>(self)->Unmap(static_cast<ID3D11Resource*>(p), sub);
        c.maps.erase(m);
        return 0;
    }
    return E_INVALIDARG;
}

int32_t hc_IDXGIFactory_CreateSwapChain(Chan& c, void* self, Rd& r, Wr& w) {
    void* dev = c.obj(r.u32(), HI_IUnknown);
    uint64_t n = 0;
    auto* d = static_cast<const DXGI_SWAP_CHAIN_DESC*>(r.arr(sizeof(DXGI_SWAP_CHAIN_DESC), &n));
    if (!r.ok || !dev || dev == BAD || !d) return DXGI_ERROR_INVALID_CALL;
    if (c.swapchain) {
        LOG("channel %u: a second swap chain; phase 2 has one surface", c.slot);
        return DXGI_ERROR_INVALID_CALL;
    }
    if (!c.window) c.window = hwgpu_host_d3d_window_acquire();
    if (!c.window) {
        LOG("channel %u: no surface to draw on (the app's screen is not up)", c.slot);
        return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    }
    DXGI_SWAP_CHAIN_DESC desc = *d;
    desc.OutputWindow = static_cast<HWND>(c.window);  // DXVK's Android WSI: a window is an ANativeWindow
    IDXGISwapChain* sc = nullptr;
    HRESULT hr = static_cast<IDXGIFactory*>(self)->CreateSwapChain(static_cast<IUnknown*>(dev), &desc, &sc);
    uint32_t h = c.put(sc, HI_IDXGISwapChain);
    LOG("channel %u: swap chain %ux%u, format %u, %u buffers: 0x%x", c.slot, desc.BufferDesc.Width,
        desc.BufferDesc.Height, unsigned(desc.BufferDesc.Format), desc.BufferCount, unsigned(hr));
    if (h) {
        c.swapchain = h;
        hwgpu_host_d3d_show(1);
    }
    w.u32(h);
    return hr;
}

// "Limit to screen refresh" (the app's switch) holds a program that asks for no wait to one
// frame per refresh, as the sgpu scanout does.
int32_t hc_IDXGISwapChain_Present(Chan&, void* self, Rd& r, Wr&) {
    uint32_t sync = r.u32(), flags = r.u32();
    if (!r.ok) return E_INVALIDARG;
    if (!sync && hwgpu_host_vsync()) sync = 1;
    return static_cast<IDXGISwapChain*>(self)->Present(sync, flags);
}

int32_t hc_IDXGISwapChain_GetDesc(Chan&, void* self, Rd&, Wr& w) {
    DXGI_SWAP_CHAIN_DESC d{};
    HRESULT hr = static_cast<IDXGISwapChain*>(self)->GetDesc(&d);
    d.OutputWindow = nullptr;  // the guest puts its own window back
    w.put(&d, sizeof d);
    return hr;
}

// DXGI_ADAPTER_DESC1 as Windows lays it out: there WCHAR is two bytes, here four.
struct GuestAdapterDesc {
    char16_t description[128];
    uint32_t vendor, device, subsys, revision;
    uint64_t dedicated_video, dedicated_system, shared_system;
    uint32_t luid_low;
    int32_t luid_high;
    uint32_t flags, pad;
};
static_assert(sizeof(GuestAdapterDesc) == 312, "DXGI_ADAPTER_DESC1 in Windows");

int32_t adapter_desc(void* self, Wr& w, size_t size) {
    DXGI_ADAPTER_DESC1 d{};
    IDXGIAdapter1* a1 = nullptr;
    HRESULT hr;
    if (SUCCEEDED(static_cast<IUnknown*>(self)->QueryInterface(__uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&a1)))) {
        hr = a1->GetDesc1(&d);
        a1->Release();
    } else {
        hr = static_cast<IDXGIAdapter*>(self)->GetDesc(reinterpret_cast<DXGI_ADAPTER_DESC*>(&d));
    }
    GuestAdapterDesc g{};
    for (int i = 0; i < 127 && d.Description[i]; i++) g.description[i] = char16_t(d.Description[i]);
    g.vendor = d.VendorId;
    g.device = d.DeviceId;
    g.subsys = d.SubSysId;
    g.revision = d.Revision;
    g.dedicated_video = d.DedicatedVideoMemory;
    g.dedicated_system = d.DedicatedSystemMemory;
    g.shared_system = d.SharedSystemMemory;
    g.luid_low = d.AdapterLuid.LowPart;
    g.luid_high = d.AdapterLuid.HighPart;
    g.flags = d.Flags;
    w.put(&g, size);
    return hr;
}

int32_t hc_IDXGIAdapter_GetDesc(Chan&, void* self, Rd&, Wr& w) { return adapter_desc(self, w, 304); }
int32_t hc_IDXGIAdapter1_GetDesc1(Chan&, void* self, Rd&, Wr& w) { return adapter_desc(self, w, 312); }

// ---- the channel ----

void setup_dxvk() {
    static bool done = false;
    if (done) return;
    done = true;
    // Vulkan comes from the same driver wrapper gfxstream uses (Vortek's fixes, the shader cache).
    if (!getenv("HWGPU_VK_LOADER") && getenv("ANDROID_EMU_VK_LOADER_PATH"))
        setenv("HWGPU_VK_LOADER", getenv("ANDROID_EMU_VK_LOADER_PATH"), 1);
    // DXVK writes under $XDG_CACHE_HOME; this process has no home.
    if (!getenv("XDG_CACHE_HOME")) {
        const char* dir = getenv("HWGPU_VK_CACHE_DIR");
        setenv("XDG_CACHE_HOME", dir ? dir : "/data/local/tmp", 1);
    }
    // DXVK logs to files: dxgi.log and d3d11.log in the cache folder. adb shell setprop
    // debug.hwd3d.log info (or debug) says more.
    char level[PROP_VALUE_MAX] = "";
    __system_property_get("debug.hwd3d.log", level);
    if (!getenv("DXVK_LOG_LEVEL")) setenv("DXVK_LOG_LEVEL", level[0] ? level : "warn", 1);
    if (!getenv("DXVK_LOG_PATH")) setenv("DXVK_LOG_PATH", getenv("XDG_CACHE_HOME"), 1);
}

int32_t create_factory(Chan& c, Rd& r, Wr& w) {
    uint32_t want = r.u32();
    if (!r.ok || want >= HI_COUNT) return E_INVALIDARG;
    setup_dxvk();
    void* f = nullptr;
    HRESULT hr = CreateDXGIFactory1(hi_iid[want], &f);
    w.u32(SUCCEEDED(hr) ? c.put(f, int(want)) : 0);
    return hr;
}

int32_t create_device(Chan& c, uint32_t adapter_h, Rd& r, Wr& w) {
    void* adapter = c.obj(adapter_h, HI_IDXGIAdapter);
    uint32_t flags = r.u32();
    uint64_t n = 0;
    auto* levels = static_cast<const D3D_FEATURE_LEVEL*>(r.arr(sizeof(D3D_FEATURE_LEVEL), &n));
    if (!r.ok || adapter == BAD || n > 16) return E_INVALIDARG;
    setup_dxvk();
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL level{};
    HRESULT hr = D3D11CreateDevice(static_cast<IDXGIAdapter*>(adapter),
                                   adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                   UINT(n), D3D11_SDK_VERSION, &dev, &level, &ctx);
    LOG("channel %u: D3D11CreateDevice 0x%x, feature level 0x%x", c.slot, unsigned(hr), unsigned(level));
    if (ctx && !c.immediate) {
        c.immediate = ctx;
        ctx->AddRef();
    }
    w.u32(c.put(dev, HI_ID3D11Device));
    w.u32(level);
    w.u32(c.put(ctx, HI_ID3D11DeviceContext));
    return hr;
}

bool send(Chan& c, const void* msg, size_t size) { return hwgpu_write_all(c.chan, c.generation, 0, msg, size) != 0; }

bool answer(Chan& c, const hwd3d_msg& req, int32_t status, const Wr& w) {
    if (req.flags & HWD3D_NO_REPLY) return true;
    hwd3d_reply r{};
    r.hdr = {uint32_t(sizeof r + w.b.size()), HWD3D_MAGIC, req.op, HWD3D_REPLY, req.seq};
    r.status = status;
    return send(c, &r, sizeof r) && (w.b.empty() || send(c, w.b.data(), w.b.size()));
}

// One request. False ends the channel: a broken stream cannot be resynchronized.
bool handle(Chan& c, const std::vector<uint8_t>& m) {
    hwd3d_msg h;
    memcpy(&h, m.data(), sizeof h);
    c.received++;
    Wr w;
    switch (h.op) {
    case HWD3D_OP_HELLO: {
        if (m.size() < sizeof(hwd3d_hello)) return false;
        hwd3d_hello hello;
        memcpy(&hello, m.data(), sizeof hello);
        LOG("channel %u: HwD3D hello from pid %u, arch %u, version %u", c.slot, hello.pid, hello.arch, hello.version);
        hwd3d_hello_reply r{};
        r.r.hdr = {uint32_t(sizeof r), HWD3D_MAGIC, h.op, HWD3D_REPLY, h.seq};
        r.r.status = hello.version == HWD3D_VERSION ? 0 : int32_t(0x80004005);  // E_FAIL
        r.version = HWD3D_VERSION;
        return (h.flags & HWD3D_NO_REPLY) || send(c, &r, sizeof r);
    }
    case HWD3D_OP_NOP:
        return true;
    case HWD3D_OP_PING: {
        hwd3d_ping_reply r{};
        r.r.hdr = {uint32_t(sizeof r), HWD3D_MAGIC, h.op, HWD3D_REPLY, h.seq};
        r.received = c.received;
        return (h.flags & HWD3D_NO_REPLY) || send(c, &r, sizeof r);
    }
    case HWD3D_OP_RELEASE:
    case HWD3D_OP_CALL:
    case HWD3D_OP_CREATE_FACTORY:
    case HWD3D_OP_CREATE_DEVICE: {
        if (m.size() < sizeof(hwd3d_call)) return false;
        hwd3d_call call;
        memcpy(&call, m.data(), sizeof call);
        Rd r{m.data() + sizeof call, m.data() + m.size()};
        int32_t st;
        if (h.op == HWD3D_OP_RELEASE) {
            c.release(call.object);
            return true;
        } else if (h.op == HWD3D_OP_CREATE_FACTORY) {
            st = create_factory(c, r, w);
        } else if (h.op == HWD3D_OP_CREATE_DEVICE) {
            st = create_device(c, call.object, r, w);
        } else if (call.method >= HM_COUNT || !hm_table[call.method].fn) {
            static bool told[HM_COUNT];
            if (call.method < HM_COUNT && !told[call.method]) {
                told[call.method] = true;
                LOG("method %u is not done on the phone", call.method);
            }
            st = int32_t(0x80004001);  // E_NOTIMPL
        } else {
            void* self = c.obj(call.object, hm_table[call.method].owner);
            st = (!self || self == BAD) ? int32_t(0x80070057) /* E_INVALIDARG */
                                        : hm_table[call.method].fn(c, self, r, w);
        }
        return answer(c, h, st, w);
    }
    default:
        LOG("channel %u: unknown op 0x%x", c.slot, h.op);
        return answer(c, h, int32_t(0x80004001), w);
    }
}

// Reads the rest of a message whose first `have` bytes are in m.
bool read_message(Chan& c, std::vector<uint8_t>& m, size_t have) {
    uint32_t size, magic;
    memcpy(&size, m.data(), sizeof size);
    memcpy(&magic, m.data() + 4, sizeof magic);
    if (magic != HWD3D_MAGIC || size < sizeof(hwd3d_msg) || size > HWD3D_MAX_MSG || size % 4 || size < have) return false;
    m.resize(size);
    return size == have || hwgpu_read_all(c.chan, c.generation, 0, m.data() + have, size - have);
}

uint64_t now_ns() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return uint64_t(t.tv_sec) * 1000000000ull + uint64_t(t.tv_nsec);
}

// Programs wait on answers in bursts (Map, queries, Present). For 200 us after a request only
// yield, so the next is seen within microseconds; after that hwgpu_read_all's growing sleeps.
void wait_request(void* chan, uint32_t generation) {
    hwgpu_ctl* c = hwgpu_chan_ctl(chan);
    const uint64_t start = now_ns();
    while (__atomic_load_n(&c->tx_head, __ATOMIC_ACQUIRE) == __atomic_load_n(&c->tx_tail, __ATOMIC_RELAXED) &&
           hwgpu_alive(c, generation) && now_ns() - start < 200000ull)
        sched_yield();
}

}  // namespace

extern "C" __attribute__((visibility("default"))) void hwd3d_serve(void* chan, uint32_t slot, uint32_t generation,
                                                                 const void* first, size_t first_len) {
    Chan c;
    c.chan = chan;
    c.slot = slot;
    c.generation = generation;
    std::vector<uint8_t> m(static_cast<const uint8_t*>(first), static_cast<const uint8_t*>(first) + first_len);
    bool ok = read_message(c, m, first_len) && handle(c, m);
    while (ok) {
        m.resize(sizeof(hwd3d_msg));
        wait_request(chan, generation);
        if (!hwgpu_read_all(chan, generation, 0, m.data(), sizeof(hwd3d_msg))) break;  // the channel ended
        ok = read_message(c, m, sizeof(hwd3d_msg)) && handle(c, m);
    }
    if (!ok && hwgpu_alive(hwgpu_chan_ctl(chan), generation)) LOG("channel %u: bad HwD3D stream, dropping it", slot);
    c.release_all();
    LOG("channel %u: HwD3D ends after %llu messages", slot, (unsigned long long)c.received);
}
