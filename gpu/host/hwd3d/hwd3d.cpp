// libhwd3d.so: the phone side of HwD3D (d3d/README.md), in the renderer process (:gpu).
//
// The sgpu server (gpu/host/hwgpu_host.cpp) hands a channel here when its first message is an
// HwD3D hello. Each channel then gets its own thread, which reads requests (include/hw_d3d.h) and
// calls DXVK (built for Android by build.sh). Every object the guest holds is a handle into the
// channel's table, checked for type and generation on every use. The generated thunks
// (gen_thunks.py) read the arguments of most methods; the rest are below.
#include <windows.h>
#include <d3d11_3.h>
#include <dxgi.h>

#include <hw_d3d.h>
#include <hwgpu_ring.h>

#include <dlfcn.h>
#include <mutex>
#include <android/hardware_buffer.h>
#define VK_USE_PLATFORM_ANDROID_KHR 1
#include <vulkan/vulkan.h>
#include <sched.h>
#include <chrono>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <string>
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
// DXVK, loaded at run time (setup_dxvk): the version the app's Direct3D setting names, or the shipped one.
namespace {
constexpr unsigned kDxvkAbi = 1;  // dxvk-android.patch's hwd3d_abi_version
struct Dxvk {
    HRESULT(WINAPI* create_factory)(REFIID, void**) = nullptr;
    PFN_D3D11_CREATE_DEVICE create_device = nullptr;
    void (*set_program)(const char*, const char*) = nullptr;
};
Dxvk g_dxvk;
}  // namespace
extern "C" void* hwgpu_host_heap_grant(uint32_t slot, uint64_t size, uint64_t* offset);
extern "C" void hwgpu_host_heap_free(void* p);
extern "C" void hwgpu_host_show_buffer(void* ahb, uint32_t width, uint32_t height);

namespace {

void* const BAD = reinterpret_cast<void*>(uintptr_t(1));

// Reads a request's arguments. Past the end it reads zeroes and clears ok.
struct Rd {
    const uint8_t *p, *end;
    bool ok = true;
    const uint8_t* stage = nullptr;  // the channel's staging area, when it has one
    uint64_t stage_cap = 0;          // its data bytes
    uint64_t stage_end = 0;          // the furthest staged byte this request used
    uint64_t staged = 0;             // bytes it took from there
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
    // An array: present, count, elements (or their position in the staging area). Null when
    // absent; n gets the count.
    const void* arr(uint32_t el, uint64_t* n) {
        *n = 0;
        uint32_t how = u32();
        if (!how) return nullptr;
        uint64_t count = u32();
        if (how == HWD3D_STAGED) {
            uint64_t pos;
            get(&pos, 8);
            uint64_t bytes = count * el, at = stage_cap ? pos % stage_cap : 0;
            if (!stage || bytes > stage_cap || at + bytes > stage_cap) {
                ok = false;
                return nullptr;
            }
            stage_end = std::max(stage_end, pos + bytes);
            staged += bytes;
            *n = count;
            return stage + HWD3D_STAGING_DATA + at;
        }
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
    std::string exe, conf;                     // the program this channel draws for, and its dxvk.conf
    uint8_t* stage = nullptr;                  // the staging area in the heap (hw_d3d.h)
    uint64_t stage_size = 0;
    // Queries ended and not yet answered in the query page.
    struct Pending {
        ID3D11Asynchronous* q;  // a reference of its own
        ID3D11DeviceContext* ctx;
        uint32_t slot, seq;
    };
    std::vector<Pending> pending;

    // Writes the results the GPU has for ended queries into the query page.
    void poll_queries() {
        if (pending.empty()) return;
        auto* page = reinterpret_cast<hwd3d_query_slot*>(stage + stage_size - HWD3D_QUERY_PAGE);
        for (auto p = pending.begin(); p != pending.end();) {
            uint8_t data[HWD3D_QUERY_DATA];
            UINT size = std::min<UINT>(p->q->GetDataSize(), HWD3D_QUERY_DATA);
            if (p->ctx->GetData(p->q, size ? data : nullptr, size, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
                ++p;
                continue;
            }
            hwd3d_query_slot& s = page[p->slot];
            memcpy(s.data, data, size);
            s.size = size;
            __atomic_store_n(&s.seq, p->seq, __ATOMIC_RELEASE);
            p->q->Release();
            p->ctx->Release();
            p = pending.erase(p);
        }
    }

    // What crossed, for the log at the end: requests and their bytes, answers, staged bytes.
    uint64_t presents = 0, ring_bytes = 0, answers = 0, staged_bytes = 0;

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

    // Waits until the GPU has finished all this program sent. DXVK's swap chain leaves a
    // callback on its frame-latency fence; destroying it with frames in flight lets the queue
    // thread call into freed memory, so a swap chain only goes once the GPU is idle.
    void idle() {
        if (!immediate) return;
        ID3D11Device* dev = nullptr;
        immediate->GetDevice(&dev);
        D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
        ID3D11Query* q = nullptr;
        if (dev && SUCCEEDED(dev->CreateQuery(&qd, &q))) {
            immediate->End(q);
            immediate->Flush();
            BOOL done = FALSE;
            for (int i = 0; i < 2000 && immediate->GetData(q, &done, sizeof done, 0) != S_OK; i++) usleep(1000);
            q->Release();
        }
        if (dev) dev->Release();
    }

    void release(uint32_t h) {
        uint32_t i = h & 0xFFFFF;
        if (!h || i >= table.size() || !table[i].ptr || handle(i, table[i].gen) != h) return;
        Obj& o = table[i];
        found.erase({o.ident, o.iface});
        if (h == swapchain) {
            swapchain = 0;
            idle();
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
        idle();
        for (auto& p : pending) {
            p.q->Release();
            p.ctx->Release();
        }
        pending.clear();
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
        if (stage) hwgpu_host_heap_free(stage);
        stage = nullptr;
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
    if (r.u32()) {
        // A map the guest answered alone (WRITE_DISCARD, WRITE_NO_OVERWRITE): map, copy, unmap.
        uint32_t type = r.u32(), row = r.u32(), rows = r.u32(), want_row, want_rows, slices;
        uint64_t n = 0;
        const auto* data = static_cast<const uint8_t*>(r.arr(1, &n));
        if (!r.ok || !p || p == BAD || !data || (type != D3D11_MAP_WRITE_DISCARD && type != D3D11_MAP_WRITE_NO_OVERWRITE))
            return E_INVALIDARG;
        auto* res = static_cast<ID3D11Resource*>(p);
        hwd3d_res s = shape(res);
        if (!hwd3d_region(&s, sub, nullptr, &want_row, &want_rows, &slices) || row != want_row || rows != want_rows ||
            n != uint64_t(row) * rows * slices)
            return E_INVALIDARG;
        auto* ctx = static_cast<ID3D11DeviceContext*>(self);
        D3D11_MAPPED_SUBRESOURCE m{};
        HRESULT hr = ctx->Map(res, sub, D3D11_MAP(type), 0, &m);
        if (FAILED(hr)) return hr;
        auto* dst = static_cast<uint8_t*>(m.pData);
        if (s.dim == 1 || m.RowPitch == row) {
            for (uint32_t z = 0; z < slices; z++)
                memcpy(dst + size_t(z) * m.DepthPitch, data + size_t(z) * row * rows, size_t(row) * rows);
        } else {
            for (uint32_t z = 0; z < slices; z++)
                for (uint32_t y = 0; y < rows; y++)
                    memcpy(dst + size_t(z) * m.DepthPitch + size_t(y) * m.RowPitch,
                           data + (size_t(z) * rows + y) * row, row);
        }
        ctx->Unmap(res, sub);
        return 0;
    }
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

// End: a query the guest gave a slot in the query page is watched until the GPU has its result
// (Chan::poll_queries); one without (no staging area) is asked with GetData as before.
int32_t hc_ID3D11DeviceContext_End(Chan& c, void* self, Rd& r, Wr&) {
    void* q = c.obj(r.u32(), HI_ID3D11Asynchronous);
    uint32_t slot = r.u32(), seq = r.u32();
    if (!r.ok || !q || q == BAD) return E_INVALIDARG;
    auto* ctx = static_cast<ID3D11DeviceContext*>(self);
    auto* a = static_cast<ID3D11Asynchronous*>(q);
    ctx->End(a);
    if (seq && c.stage && slot < HWD3D_QUERY_SLOTS) {
        for (auto p = c.pending.begin(); p != c.pending.end(); ++p)
            if (p->slot == slot) {  // ended again before its result came: the new End wins
                p->q->Release();
                p->ctx->Release();
                c.pending.erase(p);
                break;
            }
        a->AddRef();
        ctx->AddRef();
        c.pending.push_back({a, ctx, slot, seq});
    }
    return 0;
}

int32_t hc_ID3D11DeviceContext_GetData(Chan& c, void* self, Rd& r, Wr& w) {
    void* q = c.obj(r.u32(), HI_ID3D11Asynchronous);
    uint32_t size = r.u32(), flags = r.u32();
    if (!r.ok || !q || q == BAD || size > 4096) return E_INVALIDARG;
    std::vector<uint8_t> data(size);
    HRESULT hr = static_cast<ID3D11DeviceContext*>(self)->GetData(static_cast<ID3D11Asynchronous*>(q),
                                                                  size ? data.data() : nullptr, size, flags);
    w.put(data.data(), size);
    return hr;
}

// "Limit to screen refresh" (the app's switch) holds a program that asks for no wait to one
// frame per refresh, as the sgpu scanout does.
int32_t hc_IDXGISwapChain_Present(Chan& c, void* self, Rd& r, Wr&) {
    uint32_t sync = r.u32(), flags = r.u32();
    if (!r.ok) return E_INVALIDARG;
    if (!sync && hwgpu_host_vsync()) sync = 1;
    c.presents++;
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

// Opens DXVK's two libraries from dir (or by name, from the app's own libraries when dir is null),
// dxgi first so d3d11's link to it finds the same copy. False when one is missing or its ABI differs.
bool load_dxvk(const char* dir) {
    auto open = [&](const char* name) -> void* {
        std::string path = dir ? std::string(dir) + "/" + name : std::string(name);
        void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (!h) LOG("DXVK: %s: %s", path.c_str(), dlerror());
        return h;
    };
    void* dxgi = open("libdxvk_dxgi.so");
    if (!dxgi) return false;
    auto abi = reinterpret_cast<unsigned (*)()>(dlsym(dxgi, "hwd3d_abi_version"));
    if (!abi || abi() != kDxvkAbi) {
        LOG("DXVK in %s was built for HwD3D ABI %u, this renderer speaks %u: refused", dir ? dir : "the app", abi ? abi() : 0, kDxvkAbi);
        dlclose(dxgi);
        return false;
    }
    void* d3d11 = open("libdxvk_d3d11.so");
    if (!d3d11) {
        dlclose(dxgi);
        return false;
    }
    g_dxvk.create_factory = reinterpret_cast<decltype(g_dxvk.create_factory)>(dlsym(dxgi, "CreateDXGIFactory1"));
    g_dxvk.create_device = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(dlsym(d3d11, "D3D11CreateDevice"));
    g_dxvk.set_program = reinterpret_cast<decltype(g_dxvk.set_program)>(dlsym(dxgi, "hwd3d_set_program"));
    return g_dxvk.create_factory && g_dxvk.create_device && g_dxvk.set_program;
}

void setup_dxvk() {
    static bool done = false;
    if (done) return;
    done = true;
    const char* dir = getenv("HWD3D_DXVK_DIR");
    if (!(dir && load_dxvk(dir))) {
        if (dir) LOG("DXVK: falling back to the shipped one");
        g_dxvk = Dxvk{};
        load_dxvk(nullptr);
    }
    LOG("DXVK from %s", g_dxvk.create_factory ? (dir && getenv("HWD3D_DXVK_DIR") ? dir : "the app") : "nowhere");
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
    if (g_dxvk.set_program) g_dxvk.set_program(c.exe.c_str(), c.conf.c_str());
    void* f = nullptr;
    if (!g_dxvk.create_factory) return E_FAIL;
    HRESULT hr = g_dxvk.create_factory(hi_iid[want], &f);
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
    if (g_dxvk.set_program) g_dxvk.set_program(c.exe.c_str(), c.conf.c_str());
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL level{};
    if (!g_dxvk.create_device) return E_FAIL;
    HRESULT hr = g_dxvk.create_device(static_cast<IDXGIAdapter*>(adapter),
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


// ---- shared surfaces (wddm/README.md, "Shared resources and DWM") ----
// A texture two programs use (DWM draws the desktop, the screen driver takes it) is an Android hardware
// buffer, imported as a VkImage into each program's own Vulkan device and handed to DXVK. The same buffer
// can later go to the phone's screen as it is.

// DXVK's interop interface (its dxgi_interfaces.h), declared here in the same order.
MIDL_INTERFACE("e2ef5fa5-dc21-4af7-90c4-f67ef6a09323")
HwVkInteropDevice : public IUnknown {
    virtual void STDMETHODCALLTYPE GetVulkanHandles(VkInstance*, VkPhysicalDevice*, VkDevice*) = 0;
    virtual void STDMETHODCALLTYPE GetSubmissionQueue(VkQueue*, uint32_t*) = 0;
    virtual void STDMETHODCALLTYPE TransitionSurfaceLayout(void*, const VkImageSubresourceRange*, VkImageLayout, VkImageLayout) = 0;
    virtual void STDMETHODCALLTYPE FlushRenderingCommands() = 0;
    virtual void STDMETHODCALLTYPE LockSubmissionQueue() = 0;
    virtual void STDMETHODCALLTYPE ReleaseSubmissionQueue() = 0;
};
MIDL_INTERFACE("e2ef5fa5-dc21-4af7-90c4-f67ef6a09324")
HwVkInteropDevice1 : public HwVkInteropDevice {
    virtual void STDMETHODCALLTYPE GetSubmissionQueue1(VkQueue*, uint32_t*, uint32_t*) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateTexture2DFromVkImage(const D3D11_TEXTURE2D_DESC1*, VkImage, ID3D11Texture2D**) = 0;
};

struct Shared {
    AHardwareBuffer* ahb = nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    uint32_t owner = 0;   // the channel slot that made it; it goes when that channel ends
};
std::mutex g_shared_lock;
std::map<uint32_t, Shared> g_shared;
uint32_t g_next_share = 1;

// The Vulkan entry DXVK itself uses (vulkan_loader.cpp in dxvk-android.patch).
PFN_vkGetInstanceProcAddr vk_gipa() {
    static PFN_vkGetInstanceProcAddr gipa;
    if (gipa) return gipa;
    if (const char* wrapper = getenv("HWGPU_VK_LOADER"))
        if (void* l = dlopen(wrapper, RTLD_NOW)) gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(l, "vk_icdGetInstanceProcAddr"));
    if (!gipa)
        if (void* l = dlopen("libvulkan.so", RTLD_NOW)) gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(l, "vkGetInstanceProcAddr"));
    return gipa;
}

// Destroys the VkImage and its memory when DXVK's texture goes (DXVK does not own an imported image).
struct VkKeep final : IUnknown {
    LONG refs = 1;
    VkDevice dev;
    VkImage image;
    VkDeviceMemory memory;
    PFN_vkDestroyImage destroy_image;
    PFN_vkFreeMemory free_memory;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override { *out = nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(__atomic_add_fetch(&refs, 1, __ATOMIC_ACQ_REL)); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG n = __atomic_sub_fetch(&refs, 1, __ATOMIC_ACQ_REL);
        if (!n) {
            destroy_image(dev, image, nullptr);
            free_memory(dev, memory, nullptr);
            delete this;
        }
        return ULONG(n);
    }
};
// {5c1e2b7a-3f0d-4a8e-9b61-0d2f7c4e8a13}: the private data slot VkKeep rides in.
const GUID kVkKeepGuid = {0x5c1e2b7a, 0x3f0d, 0x4a8e, {0x9b, 0x61, 0x0d, 0x2f, 0x7c, 0x4e, 0x8a, 0x13}};

uint32_t ahb_format(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT;
    default:
        return 0;
    }
}

uint32_t ahb_desc_format(AHardwareBuffer* ahb) {
    AHardwareBuffer_Desc d{};
    AHardwareBuffer_describe(ahb, &d);
    return d.format;
}

// The buffer as a texture of this device.
HRESULT wrap_ahb(ID3D11Device* dev, AHardwareBuffer* ahb, const D3D11_TEXTURE2D_DESC& d, ID3D11Texture2D** out) {
    // IDXGIVkInteropDevice1's id, spelled out: DXVK's native headers only give __uuidof for their own types.
    static const GUID kInterop1 = {0xe2ef5fa5, 0xdc21, 0x4af7, {0x90, 0xc4, 0xf6, 0x7e, 0xf6, 0xa0, 0x93, 0x24}};
    HwVkInteropDevice1* interop = nullptr;
    if (FAILED(dev->QueryInterface(kInterop1, reinterpret_cast<void**>(&interop))) || !interop) return E_NOINTERFACE;
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice vkd;
    interop->GetVulkanHandles(&inst, &phys, &vkd);
    PFN_vkGetInstanceProcAddr gipa = vk_gipa();
    auto gdpa = gipa ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(inst, "vkGetDeviceProcAddr")) : nullptr;
#define VKF(name) auto name = gdpa ? reinterpret_cast<PFN_##name>(gdpa(vkd, #name)) : nullptr
    VKF(vkGetAndroidHardwareBufferPropertiesANDROID);
    VKF(vkCreateImage);
    VKF(vkAllocateMemory);
    VKF(vkBindImageMemory);
    VKF(vkDestroyImage);
    VKF(vkFreeMemory);
#undef VKF
    HRESULT hr = E_FAIL;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    do {
        if (!vkGetAndroidHardwareBufferPropertiesANDROID || !vkCreateImage || !vkAllocateMemory || !vkBindImageMemory) {
            LOG("shared surface: the phone's Vulkan device has no Android hardware buffer import");
            break;
        }
        VkAndroidHardwareBufferFormatPropertiesANDROID fp{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
        VkAndroidHardwareBufferPropertiesANDROID props{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID, &fp};
        VkResult vr = vkGetAndroidHardwareBufferPropertiesANDROID(vkd, ahb, &props);
        if (vr != VK_SUCCESS || fp.format == VK_FORMAT_UNDEFINED) {
            LOG("shared surface: buffer properties %d, format %d", int(vr), int(fp.format));
            break;
        }
        static bool told;
        if (!told) {
            told = true;
            LOG("shared surface: Android buffer format %u is Vulkan format %d", unsigned(ahb_desc_format(ahb)), int(fp.format));
        }
        VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, nullptr,
                                            VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = fp.format;
        ci.extent = {d.Width, d.Height, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if ((vr = vkCreateImage(vkd, &ci, nullptr, &image)) != VK_SUCCESS) {
            LOG("shared surface: vkCreateImage %d", int(vr));
            break;
        }
        VkImportAndroidHardwareBufferInfoANDROID imp{VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, nullptr, ahb};
        VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &imp, image, VK_NULL_HANDLE};
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ded, props.allocationSize, 0};
        while (ai.memoryTypeIndex < 32 && !(props.memoryTypeBits & (1u << ai.memoryTypeIndex))) ai.memoryTypeIndex++;
        if ((vr = vkAllocateMemory(vkd, &ai, nullptr, &memory)) != VK_SUCCESS || (vr = vkBindImageMemory(vkd, image, memory, 0)) != VK_SUCCESS) {
            LOG("shared surface: import %d", int(vr));
            break;
        }
        D3D11_TEXTURE2D_DESC1 d1{};
        memcpy(&d1, &d, sizeof d);
        d1.MipLevels = 1;
        d1.ArraySize = 1;
        d1.MiscFlags &= ~UINT(D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX |
                              D3D11_RESOURCE_MISC_GDI_COMPATIBLE);
        hr = interop->CreateTexture2DFromVkImage(&d1, image, out);
        if (FAILED(hr)) {
            LOG("shared surface: DXVK refused the image (0x%x)", unsigned(hr));
            break;
        }
        auto* keep = new VkKeep;
        keep->dev = vkd;
        keep->image = image;
        keep->memory = memory;
        keep->destroy_image = vkDestroyImage;
        keep->free_memory = vkFreeMemory;
        (*out)->SetPrivateDataInterface(kVkKeepGuid, keep);
        keep->Release();
        image = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
    } while (false);
    if (memory && vkFreeMemory) vkFreeMemory(vkd, memory, nullptr);
    if (image && vkDestroyImage) vkDestroyImage(vkd, image, nullptr);
    interop->Release();
    return hr;
}

int32_t create_shared(Chan& c, uint32_t device, Rd& r, Wr& w) {
    D3D11_TEXTURE2D_DESC d;
    r.get(&d, sizeof d);   // zeroes past the end
    auto* dev = static_cast<ID3D11Device*>(c.obj(device, HI_ID3D11Device));
    if (!r.ok || !dev || dev == BAD || !d.Width || !d.Height || d.Width > 16384 || d.Height > 16384) return E_INVALIDARG;
    uint32_t fmt = ahb_format(d.Format);
    if (!fmt || d.SampleDesc.Count > 1 || d.MipLevels > 1 || d.ArraySize > 1) return E_NOTIMPL;   // the guest makes an ordinary texture
    AHardwareBuffer_Desc ad{d.Width, d.Height, 1, fmt,
                            AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                                AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY, 0, 0, 0};
    AHardwareBuffer* ahb = nullptr;
    if (AHardwareBuffer_allocate(&ad, &ahb) != 0) {
        ad.usage &= ~uint64_t(AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY);
        if (AHardwareBuffer_allocate(&ad, &ahb) != 0) {
            LOG("channel %u: no Android hardware buffer for a %ux%u shared surface", c.slot, d.Width, d.Height);
            return E_OUTOFMEMORY;
        }
    }
    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = wrap_ahb(dev, ahb, d, &tex);
    if (FAILED(hr)) {
        AHardwareBuffer_release(ahb);
        return hr;
    }
    uint32_t id;
    {
        std::lock_guard<std::mutex> g(g_shared_lock);
        id = g_next_share++;
        g_shared[id] = Shared{ahb, d, c.slot};
    }
    w.u32(c.put(tex, HI_ID3D11Texture2D));
    w.u32(id);
    LOG("channel %u: shared surface %u, %ux%u format %u", c.slot, id, d.Width, d.Height, unsigned(d.Format));
    return S_OK;
}

int32_t open_shared(Chan& c, uint32_t device, Rd& r, Wr& w) {
    uint32_t id = r.u32();
    auto* dev = static_cast<ID3D11Device*>(c.obj(device, HI_ID3D11Device));
    if (!r.ok || !dev || dev == BAD) return E_INVALIDARG;
    AHardwareBuffer* ahb = nullptr;
    D3D11_TEXTURE2D_DESC d{};
    {
        std::lock_guard<std::mutex> g(g_shared_lock);
        auto it = g_shared.find(id);
        if (it == g_shared.end()) return E_INVALIDARG;
        ahb = it->second.ahb;
        d = it->second.desc;
        AHardwareBuffer_acquire(ahb);
    }
    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = wrap_ahb(dev, ahb, d, &tex);
    AHardwareBuffer_release(ahb);   // the imported memory holds its own reference
    if (FAILED(hr)) return hr;
    w.u32(c.put(tex, HI_ID3D11Texture2D));
    w.arr(&d, sizeof d);
    LOG("channel %u: opened shared surface %u", c.slot, id);
    return S_OK;
}

// Waits until this channel's GPU work is done, so a surface handed on to another program is whole.
int32_t finish(Chan& c) {
    if (!c.immediate) return 0;
    ID3D11Device* dev = nullptr;
    c.immediate->GetDevice(&dev);
    D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
    ID3D11Query* q = nullptr;
    HRESULT hr = dev ? dev->CreateQuery(&qd, &q) : E_FAIL;
    if (dev) dev->Release();
    if (FAILED(hr)) return hr;
    c.immediate->End(q);
    c.immediate->Flush();
    BOOL done = FALSE;
    auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while ((hr = c.immediate->GetData(q, &done, sizeof done, D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE &&
           std::chrono::steady_clock::now() < give_up)
        sched_yield();
    q->Release();
    if (hr == S_FALSE) {
        static bool told;
        if (!told) {
            told = true;
            LOG("channel %u: GPU work not done after 1 s; the surface is shown anyway", c.slot);
        }
        hr = S_OK;
    }
    return SUCCEEDED(hr) ? 0 : hr;
}

int32_t scanout(Chan& c, Rd& r) {
    uint32_t id = r.u32();
    AHardwareBuffer* ahb = nullptr;
    uint32_t w = 0, h = 0;
    {
        std::lock_guard<std::mutex> g(g_shared_lock);
        auto it = g_shared.find(id);
        if (it == g_shared.end()) return E_INVALIDARG;
        ahb = it->second.ahb;
        w = it->second.desc.Width;
        h = it->second.desc.Height;
        AHardwareBuffer_acquire(ahb);
    }
    // The program that drew it waited for its GPU work before presenting (HWD3D_OP_FINISH).
    bool tell = c.presents < 3 || !(c.presents % 600);
    if (tell) LOG("channel %u: screen frame %llu from shared surface %u", c.slot, (unsigned long long)c.presents + 1, id);
    hwgpu_host_show_buffer(ahb, w, h);
    AHardwareBuffer_release(ahb);
    if (tell) LOG("channel %u: screen frame %llu on the layer", c.slot, (unsigned long long)c.presents + 1);
    c.presents++;
    return S_OK;
}

void drop_shared(uint32_t slot) {
    std::lock_guard<std::mutex> g(g_shared_lock);
    for (auto it = g_shared.begin(); it != g_shared.end();) {
        if (it->second.owner == slot) {
            AHardwareBuffer_release(it->second.ahb);
            it = g_shared.erase(it);
        } else {
            ++it;
        }
    }
}

bool send(Chan& c, const void* msg, size_t size) { return hwgpu_write_all(c.chan, c.generation, 0, msg, size) != 0; }

bool answer(Chan& c, const hwd3d_msg& req, int32_t status, const Wr& w) {
    if (req.flags & HWD3D_NO_REPLY) return true;
    hwd3d_reply r{};
    r.hdr = {uint32_t(sizeof r + w.b.size()), HWD3D_MAGIC, req.op, HWD3D_REPLY, req.seq};
    r.status = status;
    return send(c, &r, sizeof r) && (w.b.empty() || send(c, w.b.data(), w.b.size()));
}

int32_t handle_call(Chan& c, const hwd3d_msg& h, const hwd3d_call& call, Rd& r, Wr& w);

// Bytes of the shared GPU heap each HwD3D channel stages its uploads in.
constexpr uint64_t kStaging = 32u << 20;

// One request. False ends the channel: a broken stream cannot be resynchronized.
bool handle(Chan& c, const std::vector<uint8_t>& m) {
    hwd3d_msg h;
    memcpy(&h, m.data(), sizeof h);
    c.received++;
    c.ring_bytes += m.size();
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
    case HWD3D_OP_STAGING: {
        // Asked for when a program first has a big array to send. A program that never does holds no heap.
        hwd3d_hello_reply r{};
        r.r.hdr = {uint32_t(sizeof r), HWD3D_MAGIC, h.op, HWD3D_REPLY, h.seq};
        r.version = HWD3D_VERSION;
        if (!c.stage) {
            uint64_t off = 0;
            c.stage = static_cast<uint8_t*>(hwgpu_host_heap_grant(c.slot, kStaging, &off));
            if (c.stage) {
                c.stage_size = kStaging;
                memset(c.stage, 0, HWD3D_STAGING_DATA);
                r.staging_offset = off;
                r.staging_size = kStaging;
            } else {
                LOG("channel %u: no room in the GPU heap for staging; data rides in the ring", c.slot);
            }
        }
        return (h.flags & HWD3D_NO_REPLY) || send(c, &r, sizeof r);
    }
    case HWD3D_OP_PROGRAM: {
        if (m.size() < sizeof(hwd3d_program)) return false;
        hwd3d_program p;
        memcpy(&p, m.data(), sizeof p);
        uint64_t pad_path = (uint64_t(p.path_bytes) + 3) & ~3ull, pad_conf = (uint64_t(p.conf_bytes) + 3) & ~3ull;
        if (sizeof p + pad_path + pad_conf != m.size() || p.path_bytes > 4096 || p.conf_bytes > (256u << 10)) return false;
        const char* at = reinterpret_cast<const char*>(m.data() + sizeof p);
        c.exe.assign(at, p.path_bytes);  // Windows' path: DXVK's profiles match it with backslashes
        c.conf.assign(at + pad_path, p.conf_bytes);
        LOG("channel %u: program %s%s", c.slot, c.exe.c_str(), c.conf.empty() ? "" : ", with a dxvk.conf");
        return true;
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
    case HWD3D_OP_CREATE_DEVICE:
    case HWD3D_OP_CREATE_SHARED:
    case HWD3D_OP_OPEN_SHARED:
    case HWD3D_OP_SCANOUT:
    case HWD3D_OP_FINISH: {
        if (m.size() < sizeof(hwd3d_call)) return false;
        hwd3d_call call;
        memcpy(&call, m.data(), sizeof call);
        Rd r{m.data() + sizeof call, m.data() + m.size()};
        if (c.stage) {
            r.stage = c.stage;
            r.stage_cap = c.stage_size - HWD3D_STAGING_DATA - HWD3D_QUERY_PAGE;
        }
        int32_t st = handle_call(c, h, call, r, w);
        if (r.stage_end) {
            // The data has been used (DXVK copied it): the guest may write over it.
            auto* sc = reinterpret_cast<hwd3d_staging*>(c.stage);
            if (r.stage_end > __atomic_load_n(&sc->done, __ATOMIC_RELAXED))
                __atomic_store_n(&sc->done, r.stage_end, __ATOMIC_RELEASE);
        }
        c.staged_bytes += r.staged;
        if (h.op == HWD3D_OP_RELEASE) return true;
        c.answers += !(h.flags & HWD3D_NO_REPLY);
        return answer(c, h, st, w);
    }
    default:
        LOG("channel %u: unknown op 0x%x", c.slot, h.op);
        return answer(c, h, int32_t(0x80004001), w);
    }
}

int32_t handle_call(Chan& c, const hwd3d_msg& h, const hwd3d_call& call, Rd& r, Wr& w) {
    {
        int32_t st;
        if (h.op == HWD3D_OP_RELEASE) {
            c.release(call.object);
            return 0;
        } else if (h.op == HWD3D_OP_CREATE_FACTORY) {
            st = create_factory(c, r, w);
        } else if (h.op == HWD3D_OP_CREATE_DEVICE) {
            st = create_device(c, call.object, r, w);
        } else if (h.op == HWD3D_OP_CREATE_SHARED) {
            st = create_shared(c, call.object, r, w);
        } else if (h.op == HWD3D_OP_OPEN_SHARED) {
            st = open_shared(c, call.object, r, w);
        } else if (h.op == HWD3D_OP_SCANOUT) {
            st = scanout(c, r);
        } else if (h.op == HWD3D_OP_FINISH) {
            st = finish(c);
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
        return st;
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
// Ended queries are polled meanwhile: a program spinning on GetData sends nothing, so the answer
// must come while this side waits.
void wait_request(Chan& ch) {
    hwgpu_ctl* c = hwgpu_chan_ctl(ch.chan);
    const uint64_t start = now_ns();
    uint32_t misses = 0;
    while (__atomic_load_n(&c->tx_head, __ATOMIC_ACQUIRE) == __atomic_load_n(&c->tx_tail, __ATOMIC_RELAXED) &&
           hwgpu_alive(c, ch.generation)) {
        ch.poll_queries();
        if (now_ns() - start < 200000ull) sched_yield();
        else if (!ch.pending.empty()) hwgpu_pause(100);
        else hwgpu_backoff(&misses);
    }
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
        wait_request(c);
        if (!hwgpu_read_all(chan, generation, 0, m.data(), sizeof(hwd3d_msg))) break;  // the channel ended
        ok = read_message(c, m, sizeof(hwd3d_msg)) && handle(c, m);
        c.poll_queries();
    }
    if (!ok && hwgpu_alive(hwgpu_chan_ctl(chan), generation)) LOG("channel %u: bad HwD3D stream, dropping it", slot);
    c.release_all();
    drop_shared(slot);
    LOG("channel %u: HwD3D ends after %llu messages", slot, (unsigned long long)c.received);
    if (c.presents) {
        double f = double(c.presents);
        LOG("channel %u: %llu frames; each one %.0f requests, %.0f bytes in the ring, %.1f answers waited for, "
            "%.0f bytes staged in the heap", slot, (unsigned long long)c.presents, c.received / f, c.ring_bytes / f,
            c.answers / f, c.staged_bytes / f);
    }
}
