// libhwd3d.so: the phone side of HwD3D (d3d/README.md), in the renderer process (:gpu).
//
// The sgpu server (gpu/host/hwgpu_host.cpp) hands a channel here when its first message is an
// HwD3D hello. Each channel then gets its own thread, which reads requests (include/hw_d3d.h) and
// calls DXVK (built for Android by build.sh). Every object the guest holds is a handle into the
// channel's table, checked for type and generation on every use. The generated thunks
// (gen_thunks.py) read the arguments of most methods; the rest are below.
#include <windows.h>
#include <d3d11_3.h>
#include <d3d9.h>
#include <dxgi.h>

#include <hw_d3d.h>
#include <hwgpu_ring.h>

#include <dirent.h>
#include <dlfcn.h>
#include <mutex>
#include <android/hardware_buffer.h>
#define VK_USE_PLATFORM_ANDROID_KHR 1
#include <vulkan/vulkan.h>
#include <pthread.h>
#include <sched.h>
#include <chrono>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
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
// DXVK, loaded at run time (setup_dxvk): desktop/shell always uses built-in DXVK-Sarek;
// games use HWD3D_DXVK_DIR when set and valid, otherwise the built-in DXVK-Sarek.
namespace {
constexpr unsigned kDxvkAbi = 1;  // dxvk-android.patch's hwd3d_abi_version
struct Dxvk {
    std::string label;
    HRESULT(WINAPI* create_factory)(REFIID, void**) = nullptr;
    PFN_D3D11_CREATE_DEVICE create_device = nullptr;
    void (*set_program)(const char*, const char*) = nullptr;
    void (*set_program_d3d11)(const char*, const char*) = nullptr;
    void (*set_program_d3d9)(const char*, const char*) = nullptr;
    HRESULT(WINAPI* create9ex)(UINT, IDirect3D9Ex**) = nullptr;
    void (*d9_get_vk_handles)(IDirect3DDevice9Ex*, VkInstance*, VkPhysicalDevice*, VkDevice*) = nullptr;
    HRESULT (*d9_blit_to_vkimage)(IDirect3DDevice9Ex*, IDirect3DSurface9*, VkImage, uint32_t, uint32_t, uint32_t) = nullptr;
    void apply_program(const std::string& exe, const std::string& conf) const {
        if (set_program) set_program(exe.c_str(), conf.c_str());
        if (set_program_d3d11) set_program_d3d11(exe.c_str(), conf.c_str());
        if (set_program_d3d9) set_program_d3d9(exe.c_str(), conf.c_str());
    }
};
Dxvk g_dxvk_desktop;
Dxvk g_dxvk_games;
}  // namespace
extern "C" void* hwgpu_host_heap_grant(uint32_t slot, uint64_t size, uint64_t* offset);
extern "C" void hwgpu_host_heap_free(void* p);
extern "C" void hwgpu_host_show_buffer(void* ahb, uint32_t width, uint32_t height);
extern "C" void hwgpu_host_show_game_buffer(uint32_t slot, uint32_t generation, void* ahb, uint32_t width, uint32_t height, int vsync);
extern "C" void hwgpu_host_hide_game_buffer(uint32_t slot);
extern "C" void hwgpu_host_show_window_buffer(uint32_t slot, void* ahb, uint32_t width, uint32_t height, int32_t x, int32_t y, uint32_t cw, uint32_t ch, const uint32_t* crop);
extern "C" void hwgpu_host_set_cursor_shape(uint32_t slot, uint32_t width, uint32_t height, int32_t hot_x, int32_t hot_y,
                                            const void* rgba);

namespace {

void* const BAD = reinterpret_cast<void*>(uintptr_t(1));

// Staged arrays touched by one D3D9 call, so a captured frame can be replayed without the guest.
struct D9StageGrab {
    std::vector<uint64_t> pos;
    std::vector<uint32_t> len;
    std::vector<uint8_t> bytes;
    void clear() { pos.clear(); len.clear(); bytes.clear(); }
};

// Reads a request's arguments. Past the end it reads zeroes and clears ok.
struct Rd {
    const uint8_t *p, *end;
    bool ok = true;
    const uint8_t* stage = nullptr;  // the channel's staging area, when it has one
    uint64_t stage_cap = 0;          // its data bytes
    uint64_t stage_end = 0;          // the furthest staged byte this request used
    uint64_t staged = 0;             // bytes it took from there
    D9StageGrab* d9grab = nullptr;   // when set, staged arrays are copied for the frame sample
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
            const uint8_t* src = stage + HWD3D_STAGING_DATA + at;
            if (d9grab) {
                d9grab->pos.push_back(pos);
                d9grab->len.push_back(uint32_t(bytes));
                d9grab->bytes.insert(d9grab->bytes.end(), src, src + bytes);
            }
            return src;
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

struct Shared;
struct Chan;
void drop_shared_object(uint32_t slot, uint32_t object);
void sync_opened_shares(Chan& c);

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
    const Dxvk* dxvk = nullptr;                // resolved DXVK set (desktop built-in vs games)
    bool is_desktop = true;                    // desktop/shell program (stock driver, no layers, no vsync wait)
    struct D9State* d9 = nullptr;              // Direct3D 9 objects (hwd3d9.inc)
    uint8_t* stage = nullptr;                  // the staging area in the heap (hw_d3d.h)
    uint64_t stage_size = 0;
    uint8_t* readback_ptr() const {
        return (stage && stage_size > HWD3D_STAGING_DATA + HWD3D_QUERY_PAGE + HWD3D_READBACK_SIZE)
                   ? (stage + stage_size - HWD3D_QUERY_PAGE - HWD3D_READBACK_SIZE)
                   : nullptr;
    }
    ID3D11Texture2D* readback_stage = nullptr;
    ID3D11Device* readback_dev = nullptr;
    uint32_t readback_w = 0, readback_h = 0, readback_fmt = 0;
    // Queries ended and not yet answered in the query page.
    struct Pending {
        ID3D11Asynchronous* q;  // a reference of its own
        ID3D11DeviceContext* ctx;
        uint32_t slot, seq;
    };
    std::vector<Pending> pending;
    struct PendingD9 {
        IDirect3DQuery9* q;     // a reference of its own
        uint32_t slot, seq;
    };
    std::vector<PendingD9> pending_d9;
    struct OpenedShare {
        uint32_t handle = 0;
        std::shared_ptr<Shared> shared;
        ID3D11Texture2D* tex = nullptr;
        uint64_t synced_version = 0;
    };
    std::vector<OpenedShare> opened_shares;

    // 3-buffer AHardwareBuffer ring for fullscreen direct scanout (D3D10/11 and D3D9).
    struct ScanRingBuf {
        AHardwareBuffer* ahb = nullptr;
        ID3D11Texture2D* tex11 = nullptr;
        VkDevice vk_dev = VK_NULL_HANDLE;
        VkImage vk_img = VK_NULL_HANDLE;
        VkDeviceMemory vk_mem = VK_NULL_HANDLE;
        uint32_t vk_fmt = 0;
        PFN_vkDestroyImage destroy_image = nullptr;
        PFN_vkFreeMemory free_memory = nullptr;
    };
    ScanRingBuf scan_ring[3];
    uint32_t scan_w = 0, scan_h = 0, scan_idx = 0;
    void* scan_dev = nullptr;

    void free_scan_ring() {
        for (auto& b : scan_ring) {
            if (b.tex11) { b.tex11->Release(); b.tex11 = nullptr; }
            if (b.vk_img && b.destroy_image) { b.destroy_image(b.vk_dev, b.vk_img, nullptr); b.vk_img = VK_NULL_HANDLE; }
            if (b.vk_mem && b.free_memory) { b.free_memory(b.vk_dev, b.vk_mem, nullptr); b.vk_mem = VK_NULL_HANDLE; }
            if (b.ahb) { AHardwareBuffer_release(b.ahb); b.ahb = nullptr; }
            b = ScanRingBuf{};
        }
        scan_w = scan_h = 0;
        scan_dev = nullptr;
    }

    // Writes the results the GPU has for ended queries into the query page.
    void poll_queries() {
        if (pending.empty() && pending_d9.empty()) return;
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
        for (auto p = pending_d9.begin(); p != pending_d9.end();) {
            uint8_t data[8] = {};
            UINT size = std::min<UINT>(p->q->GetDataSize(), 8);
            if (p->q->GetData(size ? data : nullptr, size, 0) != S_OK) {
                ++p;
                continue;
            }
            hwd3d_query_slot& s = page[p->slot];
            memcpy(s.data, data, size);
            s.size = size;
            __atomic_store_n(&s.seq, p->seq, __ATOMIC_RELEASE);
            p->q->Release();
            p = pending_d9.erase(p);
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
        for (auto it = opened_shares.begin(); it != opened_shares.end();) {
            if (it->handle == h) {
                if (it->tex) it->tex->Release();
                it = opened_shares.erase(it);
            } else {
                ++it;
            }
        }
        drop_shared_object(slot, h);
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
        hwgpu_host_hide_game_buffer(slot);
        idle();
        free_scan_ring();
        if (readback_stage) {
            readback_stage->Release();
            readback_stage = nullptr;
        }
        readback_dev = nullptr;
        for (auto& os : opened_shares)
            if (os.tex) os.tex->Release();
        opened_shares.clear();
        for (auto& p : pending) {
            p.q->Release();
            p.ctx->Release();
        }
        pending.clear();
        for (auto& p : pending_d9) p.q->Release();
        pending_d9.clear();
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
#include "hwd3d9.inc"

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
    uint32_t sub = r.u32(), type = r.u32(), raw_flags = r.u32();
    bool staged_ok = (raw_flags & HWD3D_MAP_STAGED_OK) != 0;
    uint32_t flags = raw_flags & ~HWD3D_MAP_STAGED_OK;
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
    if (reads && staged_ok && c.readback_ptr() && size <= HWD3D_READBACK_SIZE) {
        memcpy(c.readback_ptr(), m.pData, size);
        w.u32(HWD3D_STAGED);
    } else {
        w.u32(reads ? HWD3D_INLINE : 0);
        if (reads) w.put(m.pData, size);
    }
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

// "Limit to screen refresh" (the app's switch) holds a game that asks for no wait to one
// frame per refresh, as the sgpu scanout does. The desktop never waits for VSync.
int32_t hc_IDXGISwapChain_Present(Chan& c, void* self, Rd& r, Wr&) {
    uint32_t sync = r.u32(), flags = r.u32();
    if (!r.ok) return E_INVALIDARG;
    chan_dxvk(c);
    if (c.is_desktop) sync = 0;
    else if (!sync && hwgpu_host_vsync()) sync = 1;
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

// Finds a library matching "<prefix>*.so" in dir (supports both plain libdxvk_dxgi.so and
// version-stamped libdxvk_dxgi-<ver>.so).
std::string find_dxvk_lib(const char* dir, const char* prefix) {
    std::string exact = std::string(dir) + "/" + prefix + ".so";
    if (access(exact.c_str(), R_OK) == 0) return exact;
    DIR* dp = opendir(dir);
    if (!dp) return {};
    size_t plen = strlen(prefix);
    std::string found;
    while (dirent* e = readdir(dp)) {
        const char* n = e->d_name;
        size_t nl = strlen(n);
        if (nl > plen + 3 && strncmp(n, prefix, plen) == 0 && strcmp(n + nl - 3, ".so") == 0) {
            found = std::string(dir) + "/" + n;
            break;
        }
    }
    closedir(dp);
    return found;
}

// Opens a DXVK set from dir (or built-in Sarek *_builtin.so when dir is null) with RTLD_LOCAL
// so two DXVK builds (built-in desktop + optional games build) coexist in one process.
bool load_dxvk_set(const char* dir, Dxvk& out) {
    out = Dxvk{};
    auto open_lib = [&](const char* prefix) -> void* {
        std::string path = dir ? find_dxvk_lib(dir, prefix) : (std::string(prefix) + "_builtin.so");
        if (path.empty()) {
            LOG("DXVK: %s*.so not found in %s", prefix, dir ? dir : "the app");
            return nullptr;
        }
        void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) LOG("DXVK: %s: %s", path.c_str(), dlerror());
        return h;
    };
    void* dxgi = open_lib("libdxvk_dxgi");
    if (!dxgi) return false;
    auto abi = reinterpret_cast<unsigned (*)()>(dlsym(dxgi, "hwd3d_abi_version"));
    if (!abi || abi() != kDxvkAbi) {
        LOG("DXVK in %s was built for HwD3D ABI %u, this renderer speaks %u: refused", dir ? dir : "built-in", abi ? abi() : 0, kDxvkAbi);
        dlclose(dxgi);
        return false;
    }
    void* d3d11 = open_lib("libdxvk_d3d11");
    if (!d3d11) {
        dlclose(dxgi);
        return false;
    }
    out.create_factory = reinterpret_cast<decltype(out.create_factory)>(dlsym(dxgi, "CreateDXGIFactory1"));
    out.create_device = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(dlsym(d3d11, "D3D11CreateDevice"));
    out.set_program = reinterpret_cast<decltype(out.set_program)>(dlsym(dxgi, "hwd3d_set_program"));
    out.set_program_d3d11 = reinterpret_cast<decltype(out.set_program_d3d11)>(dlsym(d3d11, "hwd3d_set_program"));
    if (void* d3d9 = open_lib("libdxvk_d3d9")) {
        out.create9ex = reinterpret_cast<decltype(out.create9ex)>(dlsym(d3d9, "Direct3DCreate9Ex"));
        out.set_program_d3d9 = reinterpret_cast<decltype(out.set_program_d3d9)>(dlsym(d3d9, "hwd3d_set_program"));
        out.d9_get_vk_handles = reinterpret_cast<decltype(out.d9_get_vk_handles)>(dlsym(d3d9, "hwd3d9_get_vk_handles"));
        out.d9_blit_to_vkimage = reinterpret_cast<decltype(out.d9_blit_to_vkimage)>(dlsym(d3d9, "hwd3d9_blit_to_vkimage"));
    }
    out.label = dir ? dir : "built-in DXVK-Sarek";
    return out.create_factory && out.create_device && out.set_program;
}

bool is_desktop_program(const std::string& exe) {
    if (exe.empty()) return true;  // no HWD3D_OP_PROGRAM received -> built-in
    std::string low = exe;
    for (char& ch : low) {
        if (ch >= 'A' && ch <= 'Z') ch = char(ch - 'A' + 'a');
        else if (ch == '/') ch = '\\';
    }
    // Anything under %SystemRoot% (e.g. C:\Windows\), such as dwm.exe, WUDFHost.exe, explorer.exe.
    if (low.size() >= 11 && low[1] == ':' && low.compare(2, 9, "\\windows\\") == 0) return true;
    // Microsoft shell apps outside C:\Windows\ (SystemApps / WindowsApps).
    size_t slash = low.find_last_of('\\');
    std::string base = (slash == std::string::npos) ? low : low.substr(slash + 1);
    static const char* const kShellApps[] = {
        "startmenuexperiencehost.exe",
        "shellexperiencehost.exe",
        "searchhost.exe",
        "textinputhost.exe",
        "systemsettings.exe",
    };
    for (const char* app : kShellApps) {
        if (base == app) return true;
    }
    return false;
}

void setup_dxvk() {
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    static bool done = false;
    if (done) return;
    done = true;
    // Vulkan comes from the same driver wrapper gfxstream uses (Vortek's fixes, the shader cache).
    if (!getenv("HWGPU_VK_LOADER") && getenv("ANDROID_EMU_VK_LOADER_PATH"))
        setenv("HWGPU_VK_LOADER", getenv("ANDROID_EMU_VK_LOADER_PATH"), 1);
    // DXVK writes under $XDG_CACHE_HOME; this process has no home.
    if (!getenv("XDG_CACHE_HOME")) {
        const char* cdir = getenv("HWGPU_VK_CACHE_DIR");
        setenv("XDG_CACHE_HOME", cdir ? cdir : "/data/local/tmp", 1);
    }
    // DXVK logs to files: dxgi.log and d3d11.log in the cache folder. adb shell setprop
    // debug.hwd3d.log info (or debug) says more.
    char level[PROP_VALUE_MAX] = "";
    __system_property_get("debug.hwd3d.log", level);
    if (!getenv("DXVK_LOG_LEVEL")) setenv("DXVK_LOG_LEVEL", level[0] ? level : "warn", 1);
    if (!getenv("DXVK_LOG_PATH")) setenv("DXVK_LOG_PATH", getenv("XDG_CACHE_HOME"), 1);

    load_dxvk_set(nullptr, g_dxvk_desktop);
    const char* dir = getenv("HWD3D_DXVK_DIR");
    if (dir && dir[0]) {
        if (!load_dxvk_set(dir, g_dxvk_games)) {
            LOG("DXVK games (%s) failed to load; falling back to built-in DXVK-Sarek", dir);
            g_dxvk_games = g_dxvk_desktop;
        }
    } else {
        g_dxvk_games = g_dxvk_desktop;
    }
    LOG("DXVK desktop=%s (d3d9=%s), games=%s (d3d9=%s)",
        g_dxvk_desktop.create_factory ? g_dxvk_desktop.label.c_str() : "none",
        g_dxvk_desktop.create9ex ? "yes" : "no",
        g_dxvk_games.create_factory ? g_dxvk_games.label.c_str() : "none",
        g_dxvk_games.create9ex ? "yes" : "no");
}

const Dxvk& chan_dxvk(Chan& c) {
    setup_dxvk();
    if (!c.dxvk) {
        bool desktop = is_desktop_program(c.exe);
        c.is_desktop = desktop;
        c.dxvk = desktop ? &g_dxvk_desktop : &g_dxvk_games;
        LOG("channel %u: '%s' -> %s (%s, vulkan=%s)", c.slot, c.exe.empty() ? "<unknown>" : c.exe.c_str(),
            desktop ? "desktop" : "games", c.dxvk->label.c_str(), desktop ? "stock libvulkan.so" : "games stack");
    }
    return *c.dxvk;
}

int32_t create_factory(Chan& c, Rd& r, Wr& w) {
    uint32_t want = r.u32();
    if (!r.ok || want >= HI_COUNT) return E_INVALIDARG;
    const Dxvk& dx = chan_dxvk(c);
    dx.apply_program(c.exe, c.conf);
    void* f = nullptr;
    if (!dx.create_factory) return E_FAIL;
    HRESULT hr = dx.create_factory(hi_iid[want], &f);
    w.u32(SUCCEEDED(hr) ? c.put(f, int(want)) : 0);
    return hr;
}

int32_t create_device(Chan& c, uint32_t adapter_h, Rd& r, Wr& w) {
    void* adapter = c.obj(adapter_h, HI_IDXGIAdapter);
    uint32_t flags = r.u32();
    uint64_t n = 0;
    auto* levels = static_cast<const D3D_FEATURE_LEVEL*>(r.arr(sizeof(D3D_FEATURE_LEVEL), &n));
    if (!r.ok || adapter == BAD || n > 16) return E_INVALIDARG;
    const Dxvk& dx = chan_dxvk(c);
    dx.apply_program(c.exe, c.conf);
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL level{};
    if (!dx.create_device) return E_FAIL;
    HRESULT hr = dx.create_device(static_cast<IDXGIAdapter*>(adapter),
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
    uint32_t object = 0;  // or sooner, when the owner releases this texture
    bool cpu_fallback = false;
    uint32_t bpp = 4;
    ID3D11Device* owner_dev = nullptr;
    ID3D11DeviceContext* owner_ctx = nullptr;
    ID3D11Texture2D* owner_tex = nullptr;
    ID3D11Texture2D* staging = nullptr;
    std::mutex ahb_lock;
    std::atomic<uint64_t> version{0};
    std::atomic<bool> opened{false};
    ~Shared() {
        if (staging) staging->Release();
        if (owner_tex) owner_tex->Release();
        if (owner_ctx) owner_ctx->Release();
        if (owner_dev) owner_dev->Release();
        if (ahb) AHardwareBuffer_release(ahb);
    }
};
std::mutex g_shared_lock;
std::map<uint32_t, std::shared_ptr<Shared>> g_shared;
uint32_t g_next_share = 1;

// The Vulkan entry DXVK itself uses (vulkan_loader.cpp): stock libvulkan.so for the desktop,
// HWGPU_VK_LOADER (or stock fallback) for games.
PFN_vkGetInstanceProcAddr vk_gipa(bool desktop) {
    static PFN_vkGetInstanceProcAddr gipa_stock, gipa_games;
    if (desktop) {
        if (!gipa_stock)
            if (void* l = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL))
                gipa_stock = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(l, "vkGetInstanceProcAddr"));
        return gipa_stock;
    }
    if (gipa_games) return gipa_games;
    if (const char* wrapper = getenv("HWGPU_VK_LOADER"))
        if (void* l = dlopen(wrapper, RTLD_NOW | RTLD_LOCAL))
            gipa_games = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(l, "vk_icdGetInstanceProcAddr"));
    if (!gipa_games) gipa_games = vk_gipa(true);
    return gipa_games;
}

// An imported image whose texture has gone. DXVK may still have commands in flight that use it (it
// does not own an imported image), so it is destroyed 2 s later, with its D3D11 device held until then.
// Destroying it at once crashed Mali's driver in a later barrier.
// IDXGIVkInteropDevice1's id, spelled out: DXVK's native headers only give __uuidof for their own types.
const GUID kHwVkInteropGuid = {0xe2ef5fa5, 0xdc21, 0x4af7, {0x90, 0xc4, 0xf6, 0x7e, 0xf6, 0xa0, 0x93, 0x24}};
uint64_t now_ns();

struct DeadImage {
    ID3D11Device* owner;   // held, so the Vulkan device outlives the image
    VkDevice dev;
    VkImage image;
    VkDeviceMemory memory;
    PFN_vkDestroyImage destroy_image;
    PFN_vkFreeMemory free_memory;
    uint64_t when;
};
std::mutex g_dead_lock;
std::vector<DeadImage> g_dead;

void destroy_dead(const DeadImage& d) {
    d.destroy_image(d.dev, d.image, nullptr);
    d.free_memory(d.dev, d.memory, nullptr);
    d.owner->Release();
}

// Destroys the images that died long enough ago for their last commands to be done.
void sweep_dead() {
    std::vector<DeadImage> now;
    {
        std::lock_guard<std::mutex> g(g_dead_lock);
        const uint64_t t = now_ns();
        for (auto it = g_dead.begin(); it != g_dead.end();) {
            if (t - it->when > 2000000000ull) {
                now.push_back(*it);
                it = g_dead.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& d : now) destroy_dead(d);
}

// Destroys the VkImage and its memory when DXVK's texture goes, later (see DeadImage).
struct VkKeep final : IUnknown {
    LONG refs = 1;
    ID3D11Device* owner = nullptr;  // a reference, handed to DeadImage
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
            {
                std::lock_guard<std::mutex> g(g_dead_lock);
                g_dead.push_back({owner, dev, image, memory, destroy_image, free_memory, now_ns()});
            }
            delete this;
            sweep_dead();
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
HRESULT wrap_ahb(bool desktop, ID3D11Device* dev, AHardwareBuffer* ahb, const D3D11_TEXTURE2D_DESC& d, ID3D11Texture2D** out) {
    HwVkInteropDevice1* interop = nullptr;
    if (FAILED(dev->QueryInterface(kHwVkInteropGuid, reinterpret_cast<void**>(&interop))) || !interop) return E_NOINTERFACE;
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice vkd;
    interop->GetVulkanHandles(&inst, &phys, &vkd);
    PFN_vkGetInstanceProcAddr gipa = vk_gipa(desktop);
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
            static bool told_no_ahb = false;
            if (!told_no_ahb) {
                told_no_ahb = true;
                LOG("shared surface: the phone's Vulkan device has no Android hardware buffer import");
            }
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
        keep->owner = dev;
        dev->AddRef();
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

void sync_owner_to_ahb(Shared& s) {
    if (!s.cpu_fallback || !s.owner_ctx || !s.owner_tex || !s.staging || !s.ahb) return;
    s.owner_ctx->CopyResource(s.staging, s.owner_tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    HRESULT map_hr = s.owner_ctx->Map(s.staging, 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(map_hr) || !m.pData) {
        static bool told_map = false;
        if (!told_map) {
            told_map = true;
            LOG("shared surface: CPU fallback Map failed (0x%x)", unsigned(map_hr));
        }
        return;
    }
    AHardwareBuffer_Desc hd{};
    AHardwareBuffer_describe(s.ahb, &hd);
    {
        std::lock_guard<std::mutex> lk(s.ahb_lock);
        void* dst = nullptr;
        int lock_rc = AHardwareBuffer_lock(s.ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &dst);
        if (lock_rc == 0 && dst) {
            size_t dst_pitch = size_t(hd.stride) * s.bpp;
            size_t row_bytes = std::min<size_t>(size_t(s.desc.Width) * s.bpp, std::min<size_t>(m.RowPitch, dst_pitch));
            for (uint32_t y = 0; y < s.desc.Height; y++)
                memcpy(static_cast<uint8_t*>(dst) + size_t(y) * dst_pitch,
                       static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch, row_bytes);
            AHardwareBuffer_unlock(s.ahb, nullptr);
            s.version.fetch_add(1, std::memory_order_release);
        } else {
            static bool told_lock = false;
            if (!told_lock) {
                told_lock = true;
                LOG("shared surface: CPU fallback AHardwareBuffer_lock failed (%d)", lock_rc);
            }
        }
    }
    s.owner_ctx->Unmap(s.staging, 0);
}

void sync_opened_shares(Chan& c) {
    if (c.opened_shares.empty() || !c.immediate) return;
    for (auto& os : c.opened_shares) {
        uint64_t v = os.shared->version.load(std::memory_order_acquire);
        if (v == os.synced_version) continue;
        AHardwareBuffer_Desc hd{};
        AHardwareBuffer_describe(os.shared->ahb, &hd);
        std::lock_guard<std::mutex> lk(os.shared->ahb_lock);
        void* src = nullptr;
        if (AHardwareBuffer_lock(os.shared->ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &src) == 0 && src) {
            c.immediate->UpdateSubresource(os.tex, 0, nullptr, src, hd.stride * os.shared->bpp, 0);
            AHardwareBuffer_unlock(os.shared->ahb, nullptr);
            os.synced_version = v;
        }
    }
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
    // A driver added from a file for games (HWGPU_VK_DRIVER, such as PanVK-AI) reads the buffer's layout without the phone's
    // gralloc and can only take a linear one. CPU usage keeps Mali's gralloc from compressing the buffer (AFBC). The desktop
    // always runs on the phone's stock driver, so only game-created shared surfaces need this.
    static const bool custom_driver = getenv("HWGPU_VK_DRIVER") && *getenv("HWGPU_VK_DRIVER");
    chan_dxvk(c);
    if (!c.is_desktop && custom_driver) ad.usage |= AHARDWAREBUFFER_USAGE_CPU_READ_RARELY;
    AHardwareBuffer* ahb = nullptr;
    if (AHardwareBuffer_allocate(&ad, &ahb) != 0) {
        ad.usage &= ~uint64_t(AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY);
        if (AHardwareBuffer_allocate(&ad, &ahb) != 0) {
            LOG("channel %u: no Android hardware buffer for a %ux%u shared surface", c.slot, d.Width, d.Height);
            return E_OUTOFMEMORY;
        }
    }
    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = wrap_ahb(c.is_desktop, dev, ahb, d, &tex);
    std::shared_ptr<Shared> sp;
    bool fallback = false;
    if (FAILED(hr)) {
        AHardwareBuffer_release(ahb);
        ahb = nullptr;
        AHardwareBuffer_Desc cad{
            d.Width, d.Height, 1, fmt,
            AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
                AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY,
            0, 0, 0};
        if (AHardwareBuffer_allocate(&cad, &ahb) != 0) {
            cad.usage &= ~uint64_t(AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY);
            if (AHardwareBuffer_allocate(&cad, &ahb) != 0) {
                cad.usage &= ~uint64_t(AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE);
                if (AHardwareBuffer_allocate(&cad, &ahb) != 0) {
                    LOG("channel %u: no CPU Android hardware buffer for a %ux%u shared surface", c.slot, d.Width, d.Height);
                    return E_OUTOFMEMORY;
                }
            }
        }
        D3D11_TEXTURE2D_DESC td = d;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.CPUAccessFlags = 0;
        td.MiscFlags &= ~UINT(D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                             D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_GDI_COMPATIBLE);
        td.BindFlags |= D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        HRESULT thr = dev->CreateTexture2D(&td, nullptr, &tex);
        if (FAILED(thr) || !tex) {
            AHardwareBuffer_release(ahb);
            return FAILED(thr) ? thr : hr;
        }
        D3D11_TEXTURE2D_DESC sd = td;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        ID3D11Texture2D* staging = nullptr;
        thr = dev->CreateTexture2D(&sd, nullptr, &staging);
        if (FAILED(thr) || !staging) {
            tex->Release();
            AHardwareBuffer_release(ahb);
            return FAILED(thr) ? thr : hr;
        }
        ID3D11DeviceContext* ctx = nullptr;
        dev->GetImmediateContext(&ctx);
        if (!ctx) {
            staging->Release();
            tex->Release();
            AHardwareBuffer_release(ahb);
            return E_FAIL;
        }
        sp = std::make_shared<Shared>();
        sp->ahb = ahb;
        sp->desc = d;
        sp->owner = c.slot;
        sp->cpu_fallback = true;
        sp->bpp = (fmt == AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT) ? 8 : 4;
        sp->owner_dev = dev;
        dev->AddRef();
        sp->owner_ctx = ctx;
        sp->owner_tex = tex;
        tex->AddRef();
        sp->staging = staging;
        fallback = true;
    } else {
        sp = std::make_shared<Shared>();
        sp->ahb = ahb;
        sp->desc = d;
        sp->owner = c.slot;
    }
    uint32_t id;
    {
        std::lock_guard<std::mutex> g(g_shared_lock);
        id = g_next_share++;
        g_shared[id] = sp;
    }
    uint32_t object = c.put(tex, HI_ID3D11Texture2D);
    {
        std::lock_guard<std::mutex> g(g_shared_lock);
        sp->object = object;
    }
    w.u32(object);
    w.u32(id);
    if (fallback) {
        static int fallback_logged = 0;
        if (fallback_logged++ < 8) {
            LOG("channel %u: shared surface %u (CPU readback fallback), %ux%u format %u", c.slot, id, d.Width, d.Height, unsigned(d.Format));
        }
    } else {
        LOG("channel %u: shared surface %u, %ux%u format %u", c.slot, id, d.Width, d.Height, unsigned(d.Format));
    }
    return S_OK;
}

int32_t open_shared(Chan& c, uint32_t device, Rd& r, Wr& w) {
    uint32_t id = r.u32();
    auto* dev = static_cast<ID3D11Device*>(c.obj(device, HI_ID3D11Device));
    if (!r.ok || !dev || dev == BAD) return E_INVALIDARG;
    std::shared_ptr<Shared> sp;
    D3D11_TEXTURE2D_DESC d{};
    {
        std::lock_guard<std::mutex> g(g_shared_lock);
        auto it = g_shared.find(id);
        if (it == g_shared.end()) return E_INVALIDARG;
        sp = it->second;
        d = sp->desc;
    }
    if (!sp->cpu_fallback) {
        chan_dxvk(c);
        AHardwareBuffer* ahb = sp->ahb;
        AHardwareBuffer_acquire(ahb);
        ID3D11Texture2D* tex = nullptr;
        HRESULT hr = wrap_ahb(c.is_desktop, dev, ahb, d, &tex);
        AHardwareBuffer_release(ahb);   // the imported memory holds its own reference
        if (FAILED(hr)) return hr;
        w.u32(c.put(tex, HI_ID3D11Texture2D));
        w.arr(&d, sizeof d);
        LOG("channel %u: opened shared surface %u", c.slot, id);
        return S_OK;
    }
    sp->opened.store(true, std::memory_order_release);
    D3D11_TEXTURE2D_DESC td = d;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.CPUAccessFlags = 0;
    td.MiscFlags &= ~UINT(D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                         D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_GDI_COMPATIBLE);
    td.BindFlags |= D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = dev->CreateTexture2D(&td, nullptr, &tex);
    if (FAILED(hr) || !tex) return FAILED(hr) ? hr : E_FAIL;
    uint32_t obj_h = c.put(tex, HI_ID3D11Texture2D);
    tex->AddRef();
    c.opened_shares.push_back({obj_h, sp, tex, 0});
    sync_opened_shares(c);
    w.u32(obj_h);
    w.arr(&d, sizeof d);
    LOG("channel %u: opened shared surface %u (CPU readback fallback)", c.slot, id);
    return S_OK;
}

// XSSetConstantBuffers1: constant buffers bound from a first constant (Direct3D 11.1). Direct2D binds
// slices of one big buffer this way; the whole buffer from 0 drew wrong colours and no shapes.
int32_t set_cb1(Chan& c, const hwd3d_call& call, Rd& r) {
    auto* ctx = static_cast<ID3D11DeviceContext*>(c.obj(call.object, HI_ID3D11DeviceContext));
    if (!ctx || ctx == BAD) return E_INVALIDARG;
    UINT start = 0, n = 0;
    r.get(&start, 4);
    r.get(&n, 4);
    std::vector<ID3D11Buffer*> b;
    bool bp = c.objs(r, b, HI_ID3D11Buffer);
    if (!r.ok || !bp || n > D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT || (!b.empty() && b.size() != n))
        return E_INVALIDARG;
    std::vector<UINT> first(n), num(n);
    for (UINT i = 0; i < n; i++) r.get(&first[i], 4);
    for (UINT i = 0; i < n; i++) r.get(&num[i], 4);
    if (!r.ok) return E_INVALIDARG;
    ID3D11DeviceContext1* c1 = nullptr;
    if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1))) || !c1) return E_NOINTERFACE;
    ID3D11Buffer* const* bufs = b.empty() ? nullptr : b.data();
    switch (call.method) {
    case 0: c1->VSSetConstantBuffers1(start, n, bufs, first.data(), num.data()); break;
    case 1: c1->PSSetConstantBuffers1(start, n, bufs, first.data(), num.data()); break;
    case 2: c1->GSSetConstantBuffers1(start, n, bufs, first.data(), num.data()); break;
    case 3: c1->HSSetConstantBuffers1(start, n, bufs, first.data(), num.data()); break;
    case 4: c1->DSSetConstantBuffers1(start, n, bufs, first.data(), num.data()); break;
    case 5: c1->CSSetConstantBuffers1(start, n, bufs, first.data(), num.data()); break;
    }
    c1->Release();
    return 0;
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
    if (SUCCEEDED(hr)) {
        std::vector<std::shared_ptr<Shared>> mine;
        {
            std::lock_guard<std::mutex> g(g_shared_lock);
            for (auto& kv : g_shared)
                if (kv.second->owner == c.slot && kv.second->cpu_fallback && kv.second->opened.load(std::memory_order_acquire))
                    mine.push_back(kv.second);
        }
        for (auto& sp : mine) sync_owner_to_ahb(*sp);
    }
    return SUCCEEDED(hr) ? 0 : hr;
}

int32_t scanout(Chan& c, const hwd3d_call& call, Rd& r, Wr& wr) {
    if (call.method == 3) {
        // Windowed D3D10/11 game present via shared staging memory: copy backbuffer to a cached staging
        // texture, write tightly-packed BGRA pixels straight into c.readback_ptr(), and unmap on the host.
        uint32_t src_h = r.u32(), swap_rb = r.u32();
        auto* dev = static_cast<ID3D11Device*>(c.obj(call.object, HI_ID3D11Device));
        auto* src_res = static_cast<ID3D11Resource*>(c.obj(src_h, HI_ID3D11Resource));
        uint8_t* dst_base = c.readback_ptr();
        if (!r.ok || !dev || dev == BAD || !src_res || src_res == BAD || !c.immediate || !dst_base) return E_INVALIDARG;
        ID3D11Texture2D* src_tex = nullptr;
        if (FAILED(src_res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&src_tex))) || !src_tex)
            return E_INVALIDARG;
        D3D11_TEXTURE2D_DESC sd{};
        src_tex->GetDesc(&sd);
        uint64_t dst_pitch = uint64_t(sd.Width) * 4;
        uint64_t total = dst_pitch * sd.Height;
        if (!sd.Width || !sd.Height || total > HWD3D_READBACK_SIZE) {
            src_tex->Release();
            return E_INVALIDARG;
        }
        if (!c.readback_stage || c.readback_dev != dev || c.readback_w != sd.Width || c.readback_h != sd.Height ||
            c.readback_fmt != uint32_t(sd.Format)) {
            if (c.readback_stage) {
                c.readback_stage->Release();
                c.readback_stage = nullptr;
            }
            D3D11_TEXTURE2D_DESC td = {sd.Width, sd.Height, 1, 1, sd.Format, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
            HRESULT chr = dev->CreateTexture2D(&td, nullptr, &c.readback_stage);
            if (FAILED(chr) || !c.readback_stage) {
                src_tex->Release();
                return FAILED(chr) ? chr : E_FAIL;
            }
            c.readback_dev = dev;
            c.readback_w = sd.Width;
            c.readback_h = sd.Height;
            c.readback_fmt = uint32_t(sd.Format);
        }
        c.immediate->CopyResource(c.readback_stage, src_tex);
        src_tex->Release();
        D3D11_MAPPED_SUBRESOURCE m{};
        HRESULT mhr = c.immediate->Map(c.readback_stage, 0, D3D11_MAP_READ, 0, &m);
        if (FAILED(mhr) || !m.pData) return FAILED(mhr) ? mhr : E_FAIL;
        const auto* src_bytes = static_cast<const uint8_t*>(m.pData);
        if (!swap_rb) {
            if (m.RowPitch == dst_pitch) {
                memcpy(dst_base, src_bytes, size_t(total));
            } else {
                for (uint32_t y = 0; y < sd.Height; y++)
                    memcpy(dst_base + size_t(y) * dst_pitch, src_bytes + size_t(y) * m.RowPitch, size_t(dst_pitch));
            }
        } else {
            for (uint32_t y = 0; y < sd.Height; y++) {
                const auto* srow = reinterpret_cast<const uint32_t*>(src_bytes + size_t(y) * m.RowPitch);
                auto* drow = reinterpret_cast<uint32_t*>(dst_base + size_t(y) * dst_pitch);
                for (uint32_t x = 0; x < sd.Width; x++) {
                    uint32_t v = srow[x];
                    drow[x] = (v & 0xFF00FF00u) | ((v & 0x00FF0000u) >> 16) | ((v & 0x000000FFu) << 16);
                }
            }
        }
        c.immediate->Unmap(c.readback_stage, 0);
        wr.u32(uint32_t(dst_pitch));
        wr.u32(sd.Height);
        c.presents++;
        return S_OK;
    }
    if (call.method == 2) {
        hwgpu_host_hide_game_buffer(c.slot);
        return S_OK;
    }
    if (call.method == 1 || call.method == 4) {
        // Fullscreen D3D10/11 game present: copy the backbuffer texture into a 3-buffer AHardwareBuffer
        // scanout ring on the GPU and hand it straight to the phone's layer. Method 4 is a windowed program:
        // the same ring, shown on a layer over the desktop at the window's client rect.
        const bool overlay = call.method == 4;
        uint32_t src_h = r.u32();
        int32_t ov_x = 0, ov_y = 0;
        uint32_t ov_w = 0, ov_h = 0, ov_crop[4] = {0, 0, 0, 0};
        if (overlay) {
            ov_x = int32_t(r.u32());
            ov_y = int32_t(r.u32());
            ov_w = r.u32();
            ov_h = r.u32();
            ov_crop[0] = r.u32();
            ov_crop[1] = r.u32();
            ov_crop[2] = r.u32();
            ov_crop[3] = r.u32();
        }
        auto* dev = static_cast<ID3D11Device*>(c.obj(call.object, HI_ID3D11Device));
        auto* src_res = static_cast<ID3D11Resource*>(c.obj(src_h, HI_ID3D11Resource));
        if (!r.ok || !dev || dev == BAD || !src_res || src_res == BAD || !c.immediate) return E_INVALIDARG;
        ID3D11Texture2D* src_tex = nullptr;
        if (FAILED(src_res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&src_tex))) || !src_tex)
            return E_INVALIDARG;
        D3D11_TEXTURE2D_DESC sd{};
        src_tex->GetDesc(&sd);
        if (!sd.Width || !sd.Height) {
            src_tex->Release();
            return E_INVALIDARG;
        }
        // The phone's layer reads the bytes as RGBA: a BGRA backbuffer would come out with red and blue swapped.
        if (overlay && sd.Format != DXGI_FORMAT_R8G8B8A8_UNORM && sd.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
            sd.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS) {
            src_tex->Release();
            return E_NOTIMPL;
        }
        chan_dxvk(c);
        if (c.scan_dev != dev || c.scan_w != sd.Width || c.scan_h != sd.Height || !c.scan_ring[0].tex11) {
            c.idle();
            c.free_scan_ring();
            D3D11_TEXTURE2D_DESC rd{};
            rd.Width = sd.Width;
            rd.Height = sd.Height;
            rd.MipLevels = 1;
            rd.ArraySize = 1;
            rd.Format = (sd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || sd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
                            ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                            : DXGI_FORMAT_R8G8B8A8_UNORM;
            rd.SampleDesc = {1, 0};
            rd.Usage = D3D11_USAGE_DEFAULT;
            rd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            static const bool custom_driver = getenv("HWGPU_VK_DRIVER") && *getenv("HWGPU_VK_DRIVER");
            for (int i = 0; i < 3; i++) {
                AHardwareBuffer_Desc ad{sd.Width, sd.Height, 1, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
                                        AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                                            AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY |
                                            (!c.is_desktop && custom_driver ? AHARDWAREBUFFER_USAGE_CPU_READ_RARELY : 0),
                                        0, 0, 0};
                if (AHardwareBuffer_allocate(&ad, &c.scan_ring[i].ahb) != 0) {
                    ad.usage &= ~uint64_t(AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY);
                    if (AHardwareBuffer_allocate(&ad, &c.scan_ring[i].ahb) != 0) {
                        c.free_scan_ring();
                        src_tex->Release();
                        return E_OUTOFMEMORY;
                    }
                }
                if (FAILED(wrap_ahb(c.is_desktop, dev, c.scan_ring[i].ahb, rd, &c.scan_ring[i].tex11)) || !c.scan_ring[i].tex11) {
                    c.free_scan_ring();
                    src_tex->Release();
                    return E_FAIL;
                }
            }
            c.scan_dev = dev;
            c.scan_w = sd.Width;
            c.scan_h = sd.Height;
            LOG("channel %u: fullscreen D3D11 scanout ring %ux%u ready", c.slot, sd.Width, sd.Height);
        }
        auto& buf = c.scan_ring[c.scan_idx++ % 3];
        c.immediate->CopyResource(buf.tex11, src_tex);
        src_tex->Release();
        HwVkInteropDevice1* interop = nullptr;
        if (SUCCEEDED(dev->QueryInterface(kHwVkInteropGuid, reinterpret_cast<void**>(&interop))) && interop) {
            interop->FlushRenderingCommands();
            interop->Release();
        } else {
            c.immediate->Flush();
        }
        if (overlay) hwgpu_host_show_window_buffer(c.slot, buf.ahb, sd.Width, sd.Height, ov_x, ov_y, ov_w, ov_h, ov_crop);
        else hwgpu_host_show_game_buffer(c.slot, c.generation, buf.ahb, sd.Width, sd.Height, hwgpu_host_vsync());
        c.presents++;
        return S_OK;
    }
    uint32_t id = r.u32();
    std::shared_ptr<Shared> sp;
    AHardwareBuffer* ahb = nullptr;
    uint32_t w = 0, h = 0;
    {
        std::lock_guard<std::mutex> g(g_shared_lock);
        auto it = g_shared.find(id);
        if (it == g_shared.end()) return E_INVALIDARG;
        sp = it->second;
        ahb = sp->ahb;
        w = sp->desc.Width;
        h = sp->desc.Height;
        AHardwareBuffer_acquire(ahb);
    }
    if (sp->cpu_fallback && sp->owner == c.slot) sync_owner_to_ahb(*sp);
    // The program that drew it waited for its GPU work before presenting (HWD3D_OP_FINISH).
    bool tell = c.presents < 3 || !(c.presents % 600);
    if (tell) LOG("channel %u: screen frame %llu from shared surface %u", c.slot, (unsigned long long)c.presents + 1, id);
    hwgpu_host_show_buffer(ahb, w, h);
    AHardwareBuffer_release(ahb);
    if (tell) LOG("channel %u: screen frame %llu on the layer", c.slot, (unsigned long long)c.presents + 1);
    c.presents++;
    return S_OK;
}

// The owner let go of its texture: no new program can open the surface. Programs that opened it keep
// their own import, and the screen keeps what it shows. Without this every swap chain DWM rebuilt stayed
// in phone memory until DWM's channel closed.
void drop_shared_object(uint32_t slot, uint32_t object) {
    std::shared_ptr<Shared> dead;
    {
        std::lock_guard<std::mutex> g(g_shared_lock);
        for (auto it = g_shared.begin(); it != g_shared.end(); ++it) {
            if (it->second->owner == slot && it->second->object == object) {
                dead = std::move(it->second);
                g_shared.erase(it);
                break;
            }
        }
    }
}

void drop_shared(uint32_t slot) {
    std::vector<std::shared_ptr<Shared>> dead;
    {
        std::lock_guard<std::mutex> g(g_shared_lock);
        for (auto it = g_shared.begin(); it != g_shared.end();) {
            if (it->second->owner == slot) {
                dead.push_back(std::move(it->second));
                it = g_shared.erase(it);
            } else {
                ++it;
            }
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
        {
            size_t slash = c.exe.find_last_of("\\/");
            std::string base = (slash == std::string::npos) ? c.exe : c.exe.substr(slash + 1);
            if (!base.empty()) {
                char tname[16]{};
                strncpy(tname, base.c_str(), 15);
                pthread_setname_np(pthread_self(), tname);
            }
        }
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
    case HWD3D_OP_SET_CB1:
    case HWD3D_OP_D9:
    case HWD3D_OP_CURSOR:
    case HWD3D_OP_FINISH: {
        if (m.size() < sizeof(hwd3d_call)) return false;
        hwd3d_call call;
        memcpy(&call, m.data(), sizeof call);
        Rd r{m.data() + sizeof call, m.data() + m.size()};
        if (c.stage && c.stage_size > HWD3D_STAGING_DATA + HWD3D_QUERY_PAGE + HWD3D_READBACK_SIZE) {
            r.stage = c.stage;
            r.stage_cap = c.stage_size - HWD3D_STAGING_DATA - HWD3D_QUERY_PAGE - HWD3D_READBACK_SIZE;
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
    if (h.op == HWD3D_OP_CALL) sync_opened_shares(c);
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
            st = scanout(c, call, r, w);
        } else if (h.op == HWD3D_OP_CURSOR) {
            uint32_t cw = r.u32(), ch = r.u32();
            int32_t hx = int32_t(r.u32()), hy = int32_t(r.u32());
            uint64_t n = 0;
            const void* rgba = r.arr(4, &n);
            if (r.ok && rgba && cw && ch && cw <= 64 && ch <= 64 && n == uint64_t(cw) * ch)
                hwgpu_host_set_cursor_shape(c.slot, cw, ch, hx, hy, rgba);
            st = S_OK;
        } else if (h.op == HWD3D_OP_FINISH) {
            st = finish(c);
        } else if (h.op == HWD3D_OP_D9) {
            st = d9_call(c, call, r, w);
        } else if (h.op == HWD3D_OP_SET_CB1) {
            st = set_cb1(c, call, r);
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
    if (magic != HWD3D_MAGIC || size < sizeof(hwd3d_msg) || size > HWD3D_MAX_MSG || size % 4 || size < have) {
        uint32_t w2 = 0, w3 = 0;
        if (have >= 16) { memcpy(&w2, m.data() + 8, 4); memcpy(&w3, m.data() + 12, 4); }
        auto* ctl = hwgpu_chan_ctl(c.chan);
        LOG("channel %u: read_message fail: w0=0x%x w1=0x%x w2=0x%x w3=0x%x have=%zu received=%llu tx_h=%llu tx_t=%llu",
            c.slot, size, magic, w2, w3, have, (unsigned long long)c.received,
            (unsigned long long)__atomic_load_n(&ctl->tx_head, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&ctl->tx_tail, __ATOMIC_RELAXED));
        return false;
    }
    m.resize(size);
    if (size != have && !hwgpu_read_all(c.chan, c.generation, 0, m.data() + have, size - have)) {
        LOG("channel %u: read_message body EOF: size=%u have=%zu received=%llu",
            c.slot, size, have, (unsigned long long)c.received);
        return false;
    }
    return true;
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
        else if (!ch.pending.empty() || !ch.pending_d9.empty()) hwgpu_pause(100);
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
    uint32_t prev_op = 0, prev_method = 0, prev_size = 0, prev_seq = 0;
    while (ok) {
        if (m.size() >= sizeof(hwd3d_msg)) {
            hwd3d_msg ph;
            memcpy(&ph, m.data(), sizeof ph);
            prev_op = ph.op; prev_size = ph.size; prev_seq = ph.seq;
            if (m.size() >= sizeof(hwd3d_call)) {
                hwd3d_call pc;
                memcpy(&pc, m.data(), sizeof pc);
                prev_method = pc.method;
            }
        }
        m.resize(sizeof(hwd3d_msg));
        wait_request(c);
        if (!hwgpu_read_all(chan, generation, 0, m.data(), sizeof(hwd3d_msg))) break;  // the channel ended
        ok = read_message(c, m, sizeof(hwd3d_msg)) && handle(c, m);
        c.poll_queries();
    }
    if (!ok && hwgpu_alive(hwgpu_chan_ctl(chan), generation))
        LOG("channel %u: bad HwD3D stream, dropping it (prev op=0x%x method=%u size=%u seq=%u)",
            slot, prev_op, prev_method, prev_size, prev_seq);
    d9_release(c);
    drop_shared(slot);
    c.release_all();
    sweep_dead();
    LOG("channel %u: HwD3D ends after %llu messages", slot, (unsigned long long)c.received);
    if (c.presents) {
        double f = double(c.presents);
        LOG("channel %u: %llu frames; each one %.0f requests, %.0f bytes in the ring, %.1f answers waited for, "
            "%.0f bytes staged in the heap", slot, (unsigned long long)c.presents, c.received / f, c.ring_bytes / f,
            c.answers / f, c.staged_bytes / f);
    }
}
