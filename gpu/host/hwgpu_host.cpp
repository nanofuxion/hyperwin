// Phone side of the HyperWin GPU rings. Adapted from SweetCider's sgpu_server.cpp
// (tools/gfxstream-android/sgpu): the same requests and replies, read from and written to the
// shared window instead of a vsock. One thread serves every channel, so gfxstream sees one caller.
//
// Differences from the vsock server:
//   - Fences are not messages. The finished fence id lands in the channel's control page.
//   - Each channel may only use its own context and resource ids (HWGPU_CTX, HWGPU_RES_BASE),
//     so two Windows processes cannot touch each other's objects by id.
//   - SGPU_CMD_SCANOUT puts a ColorBuffer's AHardwareBuffer on a layer of the phone's compositor
//     (one the app process owns and lends to this renderer), so GPU frames never reach the CPU.

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#include "hwgpu_host.h"

#include <hw_d3d.h>

#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <deque>
#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <mutex>
#include <thread>
#include <time.h>
#include <sched.h>
#include <sys/prctl.h>
#include <vector>

#include "gfxstream/virtio-gpu-gfxstream-renderer.h"
#include "gfxstream/virtio-gpu-gfxstream-renderer-unstable.h"
#include "hw_abi.h"
#include "hwgpu_ring.h"
#include "sgpu_proto.h"

#ifdef __ANDROID__
#include <android/hardware_buffer.h>
#include <android/surface_control.h>
#include <dlfcn.h>
#include <poll.h>
#include <unistd.h>
#include <android/log.h>
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "hwgpu", __VA_ARGS__)
#else
#include <stdio.h>
#define LOG(...) (fprintf(stderr, "hwgpu: " __VA_ARGS__), fputc('\n', stderr))
#endif

namespace {

constexpr uint32_t kMaxMessage = 64u << 20;

struct Backing {
    std::unique_ptr<uint8_t[]> data;
    size_t size = 0;
};

struct Mapping {
    uint8_t* hva = nullptr;
    uint64_t size = 0;
    std::vector<uint8_t> last;  // contents at the last SGPU_CMD_SYNC
    int grant = -1;             // in the shared heap: the channel's grant entry; nothing to sync
};

struct Slot {
    uint32_t generation = 0;  // the connection being served, 0 for none
    bool hwd3d = false;       // libhwd3d.so's thread serves this connection, not serve_one
    std::set<uint32_t> resources, contexts;
    // Heap ranges a process still had mapped when this renderer started: kept from reuse until
    // that process lets go of its channel.
    std::vector<std::pair<uint64_t, uint64_t>> quarantined;
};

// The shared heap (HW_HEAP_PA in the guest): host-visible Vulkan memory gfxstream allocates
// through gfxstream_set_host_memory_allocator comes from here, so Windows maps it directly.
uint8_t* g_heap = nullptr;
uint64_t g_heap_size = 0;
std::mutex g_heap_lock;
std::map<uint64_t, uint64_t> g_heap_free, g_heap_used;  // offset -> size

constexpr uint64_t kHeapPage = 4096;

void heap_put(uint64_t off, uint64_t size) {  // g_heap_lock held
    auto next = g_heap_free.lower_bound(off);
    if (next != g_heap_free.end() && off + size == next->first) {
        size += next->second;
        next = g_heap_free.erase(next);
    }
    if (next != g_heap_free.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == off) {
            prev->second += size;
            return;
        }
    }
    g_heap_free[off] = size;
}

bool heap_take(uint64_t off, uint64_t size) {  // g_heap_lock held: carve a known range out
    auto it = g_heap_free.upper_bound(off);
    if (it == g_heap_free.begin()) return false;
    --it;
    uint64_t start = it->first, len = it->second;
    if (off < start || off + size > start + len) return false;
    g_heap_free.erase(it);
    if (off > start) g_heap_free[start] = off - start;
    if (start + len > off + size) g_heap_free[off + size] = start + len - off - size;
    return true;
}

void* heap_alloc(size_t alignment, size_t size) {
    if (!g_heap) return nullptr;
    alignment = std::max<size_t>(alignment, kHeapPage);
    size = (size + kHeapPage - 1) & ~(kHeapPage - 1);
    std::lock_guard<std::mutex> l(g_heap_lock);
    for (auto& [off, len] : g_heap_free) {
        uint64_t at = (off + alignment - 1) & ~uint64_t(alignment - 1);
        if (at + size > off + len) continue;
        heap_take(at, size);
        g_heap_used[at] = size;
        return g_heap + at;
    }
    return nullptr;  // full: gfxstream falls back to private memory, kept in step as before
}

int heap_release(void* p) {
    auto* b = static_cast<uint8_t*>(p);
    if (!g_heap || b < g_heap || b >= g_heap + g_heap_size) return 0;
    std::lock_guard<std::mutex> l(g_heap_lock);
    auto it = g_heap_used.find(uint64_t(b - g_heap));
    if (it != g_heap_used.end()) {
        heap_put(it->first, it->second);
        g_heap_used.erase(it);
    }
    return 1;
}

uint8_t* g_window = nullptr;
bool g_inited = false;
std::atomic<bool> g_run{false};
std::thread g_thread;
std::vector<vgpu_resp_capset_info> g_capsets;
std::map<uint32_t, Backing> g_backing;
std::map<uint32_t, Mapping> g_maps;
Slot g_slots[HWGPU_CHANNELS];

uint64_t now_ns() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return uint64_t(t.tv_sec) * 1000000000ull + uint64_t(t.tv_nsec);
}

void* chan_base(uint32_t slot) { return g_window + hwgpu_chan_off(slot); }

// The channel one request came in on.
struct Link {
    void* chan;
    uint32_t slot;
    uint32_t generation;
};

bool send_msg(const Link& l, const void* head, size_t head_len, const void* tail = nullptr,
              size_t tail_len = 0) {
    uint32_t len = static_cast<uint32_t>(head_len + tail_len);
    return hwgpu_write_all(l.chan, l.generation, 0, &len, sizeof(len)) &&
           hwgpu_write_all(l.chan, l.generation, 0, head, head_len) &&
           (!tail_len || hwgpu_write_all(l.chan, l.generation, 0, tail, tail_len));
}

// HWGPU_TRACE=1 in the environment logs every request; refusals are always logged.
bool g_trace = false;

bool reply(const Link& l, const vgpu_hdr& req, uint32_t type) {
    if (type >= 0x1200) LOG("request 0x%x on channel %u refused: 0x%x", req.type, l.slot, type);
    if (req.flags & SGPU_FLAG_NO_REPLY) return true;  // the guest did not wait for one
    vgpu_hdr h{};
    h.type = type;
    h.ctx_id = req.ctx_id;
    return send_msg(l, &h, sizeof(h));
}

void on_fence(void*, stream_renderer_fence* f) {
    uint32_t slot = f->ctx_id - 1;
    uint32_t ring = f->ring_idx;
    if (!g_window || slot >= HWGPU_CHANNELS || ring >= HWGPU_FENCE_RINGS) return;
    uint64_t* at = &hwgpu_chan_ctl(chan_base(slot))->fence[ring];
    uint64_t seen = __atomic_load_n(at, __ATOMIC_RELAXED);
    while (f->fence_id > seen &&
           !__atomic_compare_exchange_n(at, &seen, f->fence_id, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
    }
}

void on_debug(void*, stream_renderer_debug* d) {
    if (d && d->message && (g_trace || d->debug_type <= STREAM_RENDERER_DEBUG_WARN)) LOG("gfxstream: %s", d->message);
}

// ---- scanout ----
//
// On Android 16 and later every buffer has a release callback of its own
// (ASurfaceTransaction_setBufferWithRelease), which also comes for a buffer the compositor dropped
// for a newer one. A frame without SGPU_SCANOUT_VSYNC goes to the layer as soon as it arrives: the
// compositor shows the newest at each refresh (mailbox). A vsync frame waits until the one before
// it has been latched (its commit callback), so every one is shown for a refresh, in order, and
// the guest, short of free buffers, draws at the screen's pace. Older Android has only the transaction's completion callback: there frames go one at
// a time, the next applied when the last is reported done, and a frame arriving meanwhile waits
// (replacing one already waiting, which counts as released at once). That is slower, but no buffer
// is ever dropped inside the compositor where its release could not be seen.

bool g_scanout_ok = false;  // ColorBuffers export AHardwareBuffers with this driver
std::atomic<uint32_t> g_scanout_flags{HWGPU_SCANOUT_VSYNC};  // hwgpu_ctl.scanout_flags for every channel
std::map<uint32_t, void*> g_ahb;  // resource -> its AHardwareBuffer, our own reference

struct Frame {
    void* ahb = nullptr;  // a reference of the frame's own
    uint32_t slot = 0, generation = 0, index = 0, seq = 0, width = 0, height = 0;
    bool vsync = false;
};

struct Scan {
    std::mutex lock;
#ifdef __ANDROID__
    ASurfaceControl* layer = nullptr;
#endif
    int view_w = 0, view_h = 0;
    bool visible = false;
    bool inflight = false;    // without release callbacks: a transaction applied, not yet done
    bool has_queued = false, has_shown = false;
    Frame queued, shown;      // shown: the last frame handed to the compositor
    bool committing = false;  // a vsync frame applied, not yet latched
    std::deque<Frame> paced;  // vsync frames waiting for that
    uint64_t frames = 0;
} g_scan;

// Tells the guest it may draw into the frame's buffer again. Counts only go up, so a late release
// of an older showing never takes back a newer one.
void publish_free(const Frame& f) {
    if (!g_window || f.slot >= HWGPU_CHANNELS || f.index >= HWGPU_SCANOUT_BUFFERS) return;
    auto* c = hwgpu_chan_ctl(chan_base(f.slot));
    if (__atomic_load_n(&c->generation, __ATOMIC_ACQUIRE) != f.generation) return;
    uint32_t* at = &c->scanout_free[f.index];
    uint32_t seen = __atomic_load_n(at, __ATOMIC_RELAXED);
    while (int32_t(f.seq - seen) > 0 &&
           !__atomic_compare_exchange_n(at, &seen, f.seq, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
    }
}

// The screen let go of a frame's buffer.
void release_frame(Frame& f) {
    if (!f.ahb) return;
    publish_free(f);
#ifdef __ANDROID__
    AHardwareBuffer_release(static_cast<AHardwareBuffer*>(f.ahb));
#endif
    f.ahb = nullptr;
}

#ifdef __ANDROID__
using SetBufferWithRelease = void (*)(ASurfaceTransaction*, ASurfaceControl*, AHardwareBuffer*, int, void*,
                                      void (*)(void*, int));
SetBufferWithRelease g_set_buffer_with_release = nullptr;  // API 36, looked up at start

void wait_fence(int fd) {
    if (fd < 0) return;
    pollfd p{fd, POLLIN, 0};
    poll(&p, 1, 100);
    close(fd);
}

void apply_locked(Frame f);

// A vsync frame was latched: the next may go.
void on_commit(void*, ASurfaceTransactionStats*) {
    std::lock_guard<std::mutex> l(g_scan.lock);
    g_scan.committing = false;
    if (!g_scan.paced.empty() && g_scan.layer) {
        Frame f = g_scan.paced.front();
        g_scan.paced.pop_front();
        apply_locked(f);
    }
}

void on_buffer_release(void* context, int fence) {
    auto* f = static_cast<Frame*>(context);
    wait_fence(fence);
    release_frame(*f);
    delete f;
}

struct Applied {
    ASurfaceControl* layer;  // acquired for the callback
    bool has_prev;
    Frame prev;
};

void apply_locked(Frame f);

void on_complete(void* context, ASurfaceTransactionStats* stats) {
    auto* a = static_cast<Applied*>(context);
    if (a->has_prev) {
        wait_fence(ASurfaceTransactionStats_getPreviousReleaseFenceFd(stats, a->layer));
        release_frame(a->prev);
    }
    ASurfaceControl_release(a->layer);
    delete a;
    std::lock_guard<std::mutex> l(g_scan.lock);
    g_scan.inflight = false;
    if (g_scan.has_queued && g_scan.layer) {
        g_scan.has_queued = false;
        apply_locked(g_scan.queued);
    }
}

void apply_locked(Frame f) {
    ASurfaceTransaction* t = ASurfaceTransaction_create();
    if (g_set_buffer_with_release) {
        g_set_buffer_with_release(t, g_scan.layer, static_cast<AHardwareBuffer*>(f.ahb), -1, new Frame(f),
                                  on_buffer_release);
        if (f.vsync) {
            ASurfaceTransaction_setOnCommit(t, nullptr, on_commit);
            g_scan.committing = true;
        }
    } else {
        ASurfaceTransaction_setBuffer(t, g_scan.layer, static_cast<AHardwareBuffer*>(f.ahb), -1);
        auto* a = new Applied{g_scan.layer, g_scan.has_shown && g_scan.shown.ahb, g_scan.shown};
        ASurfaceControl_acquire(g_scan.layer);
        ASurfaceTransaction_setOnComplete(t, a, on_complete);
        g_scan.inflight = true;
    }
    ASurfaceTransaction_setBufferTransparency(t, g_scan.layer, ASURFACE_TRANSACTION_TRANSPARENCY_OPAQUE);
    ASurfaceTransaction_setPosition(t, g_scan.layer, 0, 0);
    if (g_scan.view_w > 0 && g_scan.view_h > 0 && f.width && f.height)
        ASurfaceTransaction_setScale(t, g_scan.layer, float(g_scan.view_w) / float(f.width),
                                     float(g_scan.view_h) / float(f.height));
    if (!g_scan.visible) ASurfaceTransaction_setVisibility(t, g_scan.layer, ASURFACE_TRANSACTION_VISIBILITY_SHOW);
    ASurfaceTransaction_apply(t);
    ASurfaceTransaction_delete(t);
    g_scan.shown = f;
    if (g_set_buffer_with_release) g_scan.shown.ahb = nullptr;  // the release callback owns it
    g_scan.has_shown = true;
    g_scan.visible = true;
    g_scan.frames++;
}
#endif

void scan_submit(const Frame& f) {
    std::lock_guard<std::mutex> l(g_scan.lock);
    Frame frame = f;
#ifdef __ANDROID__
    if (g_scan.layer) {
        if (g_set_buffer_with_release) {
            if (frame.vsync && g_scan.committing) g_scan.paced.push_back(frame);
            else apply_locked(frame);
        } else if (!g_scan.inflight) {
            apply_locked(frame);
        } else {
            if (g_scan.has_queued) release_frame(g_scan.queued);
            g_scan.queued = frame;
            g_scan.has_queued = true;
        }
        return;
    }
#endif
    release_frame(frame);  // nowhere to show it (the app has no layer up): done with it at once
}

// What the layer holds stops being seen (hidden, or the layer replaced): the guest may draw into
// it again, though the compositor keeps it until something replaces it.
void forget_shown_locked() {
    for (auto& f : g_scan.paced) release_frame(f);
    g_scan.paced.clear();
    g_scan.committing = false;
    if (g_scan.has_queued) {
        release_frame(g_scan.queued);
        g_scan.has_queued = false;
    }
    if (g_scan.has_shown) {
        publish_free(g_scan.shown);
        if (g_scan.shown.ahb) {
            Frame copy = g_scan.shown;
            g_scan.shown.ahb = nullptr;  // the next completion's "previous" has nothing left to free
            release_frame(copy);
        }
    }
}

// Hides the layer. With a channel, only when that channel's frames are the ones up.
void scan_hide(int slot) {
    std::lock_guard<std::mutex> l(g_scan.lock);
    if (slot >= 0 && !(g_scan.has_shown && int(g_scan.shown.slot) == slot) &&
        !(g_scan.has_queued && int(g_scan.queued.slot) == slot))
        return;
#ifdef __ANDROID__
    if (g_scan.layer && g_scan.visible) {
        ASurfaceTransaction* t = ASurfaceTransaction_create();
        ASurfaceTransaction_setVisibility(t, g_scan.layer, ASURFACE_TRANSACTION_VISIBILITY_HIDE);
        ASurfaceTransaction_apply(t);
        ASurfaceTransaction_delete(t);
    }
#endif
    g_scan.visible = false;
    forget_shown_locked();
}

void drop_ahb(uint32_t res) {
    auto it = g_ahb.find(res);
    if (it == g_ahb.end()) return;
#ifdef __ANDROID__
    AHardwareBuffer_release(static_cast<AHardwareBuffer*>(it->second));
#endif
    g_ahb.erase(it);
}

uint8_t* blob_span(uint32_t res, uint64_t off, uint32_t len) {
    auto m = g_maps.find(res);
    if (m != g_maps.end()) return off + len <= m->second.size ? m->second.hva + off : nullptr;
    auto b = g_backing.find(res);
    if (b != g_backing.end()) return off + len <= b->second.size ? b->second.data.get() + off : nullptr;
    return nullptr;
}

bool alloc_backing(uint32_t res, size_t size, iovec* iov) {
    if (!size || size > kMaxMessage * 4 || g_backing.count(res)) return false;
    Backing b;
    b.data.reset(new (std::nothrow) uint8_t[size]());
    if (!b.data) return false;
    b.size = size;
    iov->iov_base = b.data.get();
    iov->iov_len = size;
    g_backing[res] = std::move(b);
    return true;
}

void clear_grant(uint32_t slot, int grant) {
    if (grant < 0) return;
    auto* c = hwgpu_chan_ctl(chan_base(slot));
    __atomic_store_n(&c->grant[grant].size, 0, __ATOMIC_RELEASE);
    c->grant[grant].offset = 0;
}

void drop_resource(Slot& s, uint32_t res) {
    auto m = g_maps.find(res);
    if (m != g_maps.end()) {
        clear_grant(uint32_t(&s - g_slots), m->second.grant);
        g_maps.erase(m);
        stream_renderer_resource_unmap(res);
    }
    drop_ahb(res);
    stream_renderer_resource_unref(res);
    g_backing.erase(res);
    s.resources.erase(res);
}

// Frees everything one Windows process made.
void drop_slot(Slot& s) {
    scan_hide(int(&s - g_slots));
    for (auto res : std::set<uint32_t>(s.resources)) drop_resource(s, res);
    for (auto c : s.contexts) stream_renderer_context_destroy(c);
    s.contexts.clear();
    s.generation = 0;
    if (!s.quarantined.empty()) {
        std::lock_guard<std::mutex> l(g_heap_lock);
        for (auto [off, size] : s.quarantined) heap_put(off, size);
        s.quarantined.clear();
    }
    auto* c = hwgpu_chan_ctl(chan_base(uint32_t(&s - g_slots)));
    for (uint32_t i = 0; i < HWGPU_GRANTS; i++) clear_grant(uint32_t(&s - g_slots), int(i));
    (void)c;
}

template <typename T>
const T* as(const std::vector<uint8_t>& m) {
    return m.size() >= sizeof(T) ? reinterpret_cast<const T*>(m.data()) : nullptr;
}

bool own_res(const Link& l, uint32_t res) {
    return res >= HWGPU_RES_BASE(l.slot) && res < HWGPU_RES_BASE(l.slot + 1);
}

// Handles one request. Returns false when the connection should close.
bool handle(const Link& l, const std::vector<uint8_t>& m) {
    const auto* hdr = as<vgpu_hdr>(m);
    if (!hdr) return false;
    Slot& slot = g_slots[l.slot];
    const uint32_t ctx = hdr->ctx_id;
    uint32_t resp = VGPU_RESP_OK_NODATA;

    // Everything but the capability queries runs in this channel's context.
    const bool query = hdr->type == SGPU_CMD_HELLO || hdr->type == VGPU_CMD_GET_CAPSET_INFO ||
                       hdr->type == VGPU_CMD_GET_CAPSET;
    if (!query && ctx != 0 && ctx != HWGPU_CTX(l.slot)) return reply(l, *hdr, VGPU_RESP_ERR_INVALID_CONTEXT_ID);
    // Every request that names a resource puts its id right after the header.
    if (!query && hdr->type != VGPU_CMD_CTX_CREATE && hdr->type != VGPU_CMD_CTX_DESTROY &&
        hdr->type != VGPU_CMD_SUBMIT_3D && hdr->type != SGPU_CMD_SYNC) {
        uint32_t res = 0;
        size_t at = hdr->type == VGPU_CMD_TRANSFER_TO_HOST_3D || hdr->type == VGPU_CMD_TRANSFER_FROM_HOST_3D
                        ? offsetof(vgpu_transfer_3d, resource_id)
                        : sizeof(vgpu_hdr);
        if (m.size() < at + 4) return reply(l, *hdr, VGPU_RESP_ERR_INVALID_PARAMETER);
        memcpy(&res, m.data() + at, 4);
        if (!own_res(l, res) && !(hdr->type == SGPU_CMD_SCANOUT && res == 0))  // 0: hide
            return reply(l, *hdr, VGPU_RESP_ERR_INVALID_RESOURCE_ID);
    }

    if (g_trace) LOG("channel %u: request 0x%x ctx %u, %zu bytes", l.slot, hdr->type, hdr->ctx_id, m.size());
    switch (hdr->type) {
    case SGPU_CMD_HELLO: {
        sgpu_hello_resp r{};
        r.hdr.type = SGPU_RESP_OK_HELLO;
        r.hdr.flags = g_scanout_ok ? SGPU_HELLO_SCANOUT : 0;
        r.version = SGPU_VERSION;
        r.num_capsets = static_cast<uint32_t>(g_capsets.size());
        return send_msg(l, &r, sizeof(r));
    }
    case VGPU_CMD_GET_CAPSET_INFO: {
        const auto* c = as<vgpu_get_capset_info>(m);
        if (!c || c->capset_index >= g_capsets.size()) return reply(l, *hdr, VGPU_RESP_ERR_INVALID_PARAMETER);
        return send_msg(l, &g_capsets[c->capset_index], sizeof(vgpu_resp_capset_info));
    }
    case VGPU_CMD_GET_CAPSET: {
        const auto* c = as<vgpu_get_capset>(m);
        const vgpu_resp_capset_info* info = nullptr;
        for (auto& i : g_capsets)
            if (c && i.capset_id == c->capset_id && c->capset_version <= i.capset_max_version) info = &i;
        if (!info) return reply(l, *hdr, VGPU_RESP_ERR_INVALID_PARAMETER);
        std::vector<uint8_t> caps(info->capset_max_size);
        stream_renderer_fill_caps(c->capset_id, c->capset_version, caps.data());
        vgpu_hdr h{};
        h.type = VGPU_RESP_OK_CAPSET;
        return send_msg(l, &h, sizeof(h), caps.data(), caps.size());
    }
    case VGPU_CMD_CTX_CREATE: {
        const auto* c = as<vgpu_ctx_create>(m);
        if (!c || ctx != HWGPU_CTX(l.slot) || slot.contexts.count(ctx))
            return reply(l, *hdr, VGPU_RESP_ERR_INVALID_PARAMETER);
        uint32_t nlen = c->nlen < sizeof(c->debug_name) ? c->nlen : sizeof(c->debug_name);
        if (stream_renderer_context_create(ctx, nlen, c->debug_name, c->context_init))
            resp = VGPU_RESP_ERR_UNSPEC;
        else
            slot.contexts.insert(ctx);
        break;
    }
    case VGPU_CMD_CTX_DESTROY:
        if (slot.contexts.erase(ctx)) stream_renderer_context_destroy(ctx);
        break;
    case VGPU_CMD_CTX_ATTACH_RESOURCE:
    case VGPU_CMD_CTX_DETACH_RESOURCE: {
        const auto* c = as<vgpu_resource_unref>(m);
        if (!c) return reply(l, *hdr, VGPU_RESP_ERR_INVALID_PARAMETER);
        if (hdr->type == VGPU_CMD_CTX_ATTACH_RESOURCE)
            stream_renderer_ctx_attach_resource(static_cast<int>(ctx), static_cast<int>(c->resource_id));
        else
            stream_renderer_ctx_detach_resource(static_cast<int>(ctx), static_cast<int>(c->resource_id));
        break;
    }
    case VGPU_CMD_RESOURCE_CREATE_3D: {
        const auto* c = as<vgpu_resource_create_3d>(m);
        if (!c) return reply(l, *hdr, VGPU_RESP_ERR_INVALID_PARAMETER);
        stream_renderer_resource_create_args a{c->resource_id, c->target, c->format, c->bind,
                                              c->width, c->height, c->depth, c->array_size,
                                              c->last_level, c->nr_samples, c->flags};
        if (stream_renderer_resource_create(&a, nullptr, 0))
            resp = VGPU_RESP_ERR_UNSPEC;
        else
            slot.resources.insert(c->resource_id);
        break;
    }
    case VGPU_CMD_RESOURCE_ATTACH_BACKING: {
        const auto* c = as<vgpu_resource_attach_backing>(m);
        if (!c || m.size() < sizeof(*c) + uint64_t(c->nr_entries) * sizeof(vgpu_mem_entry))
            return reply(l, *hdr, VGPU_RESP_ERR_INVALID_PARAMETER);
        const auto* e = reinterpret_cast<const vgpu_mem_entry*>(c + 1);
        size_t total = 0;
        for (uint32_t i = 0; i < c->nr_entries; i++) total += e[i].length;
        iovec iov;
        if (!alloc_backing(c->resource_id, total, &iov)) return reply(l, *hdr, VGPU_RESP_ERR_OUT_OF_MEMORY);
        if (stream_renderer_resource_attach_iov(static_cast<int>(c->resource_id), &iov, 1)) {
            g_backing.erase(c->resource_id);
            resp = VGPU_RESP_ERR_INVALID_RESOURCE_ID;
        }
        break;
    }
    case VGPU_CMD_RESOURCE_DETACH_BACKING: {
        const auto* c = as<vgpu_resource_unref>(m);
        if (!c) return reply(l, *hdr, VGPU_RESP_ERR_INVALID_PARAMETER);
        iovec* iov = nullptr;
        int n = 0;
        stream_renderer_resource_detach_iov(static_cast<int>(c->resource_id), &iov, &n);
        g_backing.erase(c->resource_id);
        break;
    }
    case VGPU_CMD_RESOURCE_UNREF: {
        const auto* c = as<vgpu_resource_unref>(m);
        if (!c) return reply(l, *hdr, VGPU_RESP_ERR_INVALID_PARAMETER);
        drop_resource(slot, c->resource_id);
        break;
    }
    case VGPU_CMD_TRANSFER_TO_HOST_3D:
    case VGPU_CMD_TRANSFER_FROM_HOST_3D: {
        const auto* t = as<vgpu_transfer_3d>(m);
        if (!t || m.size() < sizeof(*t) + 4) return reply(l, *hdr, VGPU_RESP_ERR_INVALID_PARAMETER);
        uint32_t len;
        memcpy(&len, m.data() + sizeof(*t), 4);
        const bool to_host = hdr->type == VGPU_CMD_TRANSFER_TO_HOST_3D;
        auto b = g_backing.find(t->resource_id);
        if (b == g_backing.end()) return reply(l, *hdr, VGPU_RESP_ERR_INVALID_RESOURCE_ID);
        if (t->offset > b->second.size || len > b->second.size - t->offset ||
            (to_host && m.size() < sizeof(*t) + 4 + uint64_t(len)))
            return reply(l, *hdr, VGPU_RESP_ERR_INVALID_PARAMETER);
        uint8_t* span = b->second.data.get() + t->offset;
        stream_renderer_box box{t->box.x, t->box.y, t->box.z, t->box.w, t->box.h, t->box.d};
        if (to_host) {
            memcpy(span, m.data() + sizeof(*t) + 4, len);
            if (stream_renderer_transfer_write_iov(t->resource_id, ctx, static_cast<int>(t->level), t->stride,
                                                   t->layer_stride, &box, t->offset, nullptr, 0))
                resp = VGPU_RESP_ERR_UNSPEC;
        } else {
            int rc = stream_renderer_transfer_read_iov(t->resource_id, ctx, t->level, t->stride,
                                                       t->layer_stride, &box, t->offset, nullptr, 0);
            if (rc == EAGAIN) return reply(l, *hdr, SGPU_RESP_AGAIN);
            if (rc) return reply(l, *hdr, VGPU_RESP_ERR_UNSPEC);
            sgpu_resp_data r{};
            r.hdr.type = SGPU_RESP_OK_DATA;
            r.len = len;
            if (!send_msg(l, &r, sizeof(r), span, len)) return false;
            goto fence;  // the reply is already sent
        }
        break;
    }
    case VGPU_CMD_SUBMIT_3D: {
        const auto* s = as<vgpu_submit_3d>(m);
        if (!s || m.size() < sizeof(*s) + uint64_t(s->size)) return reply(l, *hdr, VGPU_RESP_ERR_INVALID_PARAMETER);
        stream_renderer_command cmd{ctx, s->size, const_cast<uint8_t*>(m.data() + sizeof(*s)), 0, nullptr};
        if (stream_renderer_submit_cmd(&cmd)) resp = VGPU_RESP_ERR_UNSPEC;
        break;
    }
    case VGPU_CMD_RESOURCE_CREATE_BLOB: {
        const auto* c = as<vgpu_resource_create_blob>(m);
        if (!c || c->nr_entries) return reply(l, *hdr, VGPU_RESP_ERR_INVALID_PARAMETER);
        struct stream_renderer_create_blob cb{c->blob_mem, c->blob_flags, c->blob_id, c->size};
        iovec iov{};
        const bool guest = c->blob_mem == STREAM_BLOB_MEM_GUEST;
        if (guest && !alloc_backing(c->resource_id, c->size, &iov))
            return reply(l, *hdr, VGPU_RESP_ERR_OUT_OF_MEMORY);
        if (stream_renderer_create_blob(ctx, c->resource_id, &cb, guest ? &iov : nullptr, guest ? 1 : 0, nullptr)) {
            g_backing.erase(c->resource_id);
            resp = VGPU_RESP_ERR_UNSPEC;
        } else {
            slot.resources.insert(c->resource_id);
        }
        break;
    }
    case VGPU_CMD_RESOURCE_MAP_BLOB: {
        const auto* c = as<vgpu_map_blob>(m);
        if (!c) return reply(l, *hdr, VGPU_RESP_ERR_INVALID_PARAMETER);
        void* hva = nullptr;
        uint64_t size = 0;
        uint32_t info = 0;
        if (g_maps.count(c->resource_id) || stream_renderer_resource_map(c->resource_id, &hva, &size) || !hva)
            return reply(l, *hdr, VGPU_RESP_ERR_UNSPEC);
        stream_renderer_resource_map_info(c->resource_id, &info);
        auto* b = static_cast<uint8_t*>(hva);
        if (g_heap && b >= g_heap && b + size <= g_heap + g_heap_size && (uint64_t(b - g_heap) & (kHeapPage - 1)) == 0) {
            // In the shared heap: grant the range to this channel, and Windows maps it directly.
            auto* ctl = hwgpu_chan_ctl(l.chan);
            int g = -1;
            for (uint32_t i = 0; i < HWGPU_GRANTS && g < 0; i++)
                if (!__atomic_load_n(&ctl->grant[i].size, __ATOMIC_ACQUIRE)) g = int(i);
            if (g >= 0) {
                sgpu_resp_map_heap r{};
                r.hdr.type = SGPU_RESP_OK_MAP_HEAP;
                r.map_info = info;
                r.offset = uint64_t(b - g_heap);
                r.size = (size + kHeapPage - 1) & ~(kHeapPage - 1);
                ctl->grant[g].offset = r.offset;
                __atomic_store_n(&ctl->grant[g].size, r.size, __ATOMIC_RELEASE);
                Mapping mp;
                mp.hva = b;
                mp.size = size;
                mp.grant = g;
                g_maps[c->resource_id] = std::move(mp);
                return send_msg(l, &r, sizeof(r));
            }
        }
        if (g_heap) LOG("resource %u: %llu bytes outside the shared heap, kept in step by copying", c->resource_id,
                        (unsigned long long)size);
        g_maps[c->resource_id] = Mapping{b, size, std::vector<uint8_t>(b, b + size)};
        vgpu_resp_map_info r{};
        r.hdr.type = VGPU_RESP_OK_MAP_INFO;
        r.map_info = info;
        return send_msg(l, &r, sizeof(r));
    }
    case VGPU_CMD_RESOURCE_UNMAP_BLOB: {
        const auto* c = as<vgpu_resource_unref>(m);
        auto mi = c ? g_maps.find(c->resource_id) : g_maps.end();
        if (mi == g_maps.end()) return reply(l, *hdr, VGPU_RESP_ERR_INVALID_RESOURCE_ID);
        clear_grant(l.slot, mi->second.grant);
        g_maps.erase(mi);
        stream_renderer_resource_unmap(c->resource_id);
        break;
    }
    case SGPU_CMD_BLOB_WRITE:
    case SGPU_CMD_BLOB_READ: {
        const auto* c = as<sgpu_blob_io>(m);
        if (!c) return reply(l, *hdr, VGPU_RESP_ERR_INVALID_PARAMETER);
        uint8_t* span = blob_span(c->resource_id, c->offset, c->len);
        if (!span) return reply(l, *hdr, VGPU_RESP_ERR_INVALID_PARAMETER);
        if (hdr->type == SGPU_CMD_BLOB_WRITE) {
            if (m.size() < sizeof(*c) + uint64_t(c->len)) return reply(l, *hdr, VGPU_RESP_ERR_INVALID_PARAMETER);
            memcpy(span, m.data() + sizeof(*c), c->len);
            break;
        }
        sgpu_resp_data r{};
        r.hdr.type = SGPU_RESP_OK_DATA;
        r.len = c->len;
        return send_msg(l, &r, sizeof(r), span, c->len);
    }
    case SGPU_CMD_SYNC: {
        // Every 5 s: how much mapped memory the syncs walked, and how long they took.
        static uint64_t st_calls, st_pull, st_scan, st_in, st_out, st_ns, st_last;
        const uint64_t t0 = now_ns();
        struct Stat {
            uint64_t t0;
            ~Stat() {
                st_ns += now_ns() - t0;
                if (st_ns && now_ns() - st_last > 5000000000ull) {
                    LOG("sync: %llu calls (%llu pull), walked %llu MB, in %llu KB, out %llu KB, %llu ms",
                        (unsigned long long)st_calls, (unsigned long long)st_pull, (unsigned long long)(st_scan >> 20),
                        (unsigned long long)(st_in >> 10), (unsigned long long)(st_out >> 10),
                        (unsigned long long)(st_ns / 1000000));
                    st_calls = st_pull = st_scan = st_in = st_out = st_ns = 0;
                    st_last = now_ns();
                }
            }
        } stat{t0};
        st_calls++;
        st_in += m.size();
        // Guest changes first, so the host's diff below leaves them out.
        size_t at = sizeof(vgpu_hdr);
        while (at + sizeof(sgpu_sync_entry) <= m.size()) {
            sgpu_sync_entry e;
            memcpy(&e, m.data() + at, sizeof(e));
            at += sizeof(e);
            auto it = g_maps.find(e.resource_id);
            if (!own_res(l, e.resource_id) || at + e.len > m.size() || it == g_maps.end() || it->second.grant >= 0 ||
                e.offset > it->second.size || e.len > it->second.size - e.offset)
                return reply(l, *hdr, VGPU_RESP_ERR_INVALID_PARAMETER);
            memcpy(it->second.hva + e.offset, m.data() + at, e.len);
            __atomic_add_fetch(&hwgpu_hdr(g_window)->copy_from_guest, e.len, __ATOMIC_RELAXED);
            memcpy(it->second.last.data() + e.offset, m.data() + at, e.len);
            at += e.len;
        }
        std::vector<uint8_t> out(sizeof(vgpu_hdr));
        reinterpret_cast<vgpu_hdr*>(out.data())->type = SGPU_RESP_SYNC;
        if (hdr->flags & SGPU_SYNC_PULL) {
            st_pull++;
            for (auto res : slot.resources) {
                auto mi = g_maps.find(res);
                if (mi == g_maps.end()) continue;
                Mapping& mp = mi->second;
                if (mp.grant >= 0) continue;  // shared: nothing to copy
                st_scan += mp.size;
                size_t off = 0;
                while (off < mp.size) {
                    if (out.size() >= SGPU_SYNC_CHUNK) {
                        reinterpret_cast<vgpu_hdr*>(out.data())->flags |= SGPU_SYNC_MORE;
                        break;
                    }
                    size_t n = std::min<size_t>(SGPU_SYNC_PAGE, mp.size - off);
                    if (!memcmp(mp.hva + off, mp.last.data() + off, n)) {
                        off += n;
                        continue;
                    }
                    size_t start = off;
                    while (off < mp.size && off - start < SGPU_SYNC_CHUNK) {
                        n = std::min<size_t>(SGPU_SYNC_PAGE, mp.size - off);
                        if (!memcmp(mp.hva + off, mp.last.data() + off, n)) break;
                        off += n;
                    }
                    memcpy(mp.last.data() + start, mp.hva + start, off - start);
                    sgpu_sync_entry e{res, uint32_t(off - start), start};
                    const auto* eb = reinterpret_cast<const uint8_t*>(&e);
                    out.insert(out.end(), eb, eb + sizeof(e));
                    out.insert(out.end(), mp.last.data() + start, mp.last.data() + off);
                }
            }
        }
        st_out += out.size();
        __atomic_add_fetch(&hwgpu_hdr(g_window)->copy_to_guest, out.size() - sizeof(vgpu_hdr), __ATOMIC_RELAXED);
        return send_msg(l, out.data(), out.size());
    }
    case SGPU_CMD_PRESENT: {
        const auto* p = as<sgpu_present>(m);
        if (!p || p->format != SGPU_PRESENT_BGRA8 || !p->width || !p->height || p->stride < p->width * 4)
            return reply(l, *hdr, VGPU_RESP_ERR_INVALID_PARAMETER);
        uint32_t frame_bytes = p->stride * p->height;
        uint8_t* src = blob_span(p->resource_id, 0, frame_bytes);
        if (!src) return reply(l, *hdr, VGPU_RESP_ERR_INVALID_RESOURCE_ID);
        struct hw_header* wh = reinterpret_cast<struct hw_header*>(g_window);
        uint32_t back = wh->front ^ 1;
        wh->width = p->width;
        wh->height = p->height;
        wh->stride = p->stride;
        wh->format = HW_FORMAT_BGRA;
        uint32_t dst_off = hw_frame_off(wh, back);
        if (uint64_t(dst_off) + frame_bytes > HW_FRAME_END) return reply(l, *hdr, VGPU_RESP_ERR_INVALID_PARAMETER);
        memcpy(g_window + dst_off, src, frame_bytes);
        wh->front = back;
        __atomic_add_fetch(&wh->seq, 1, __ATOMIC_RELEASE);
        break;
    }
    case SGPU_CMD_SCANOUT: {
        const auto* p = as<sgpu_scanout>(m);
        if (!p || p->index >= HWGPU_SCANOUT_BUFFERS) return reply(l, *hdr, VGPU_RESP_ERR_INVALID_PARAMETER);
        if (!g_scanout_ok) return reply(l, *hdr, VGPU_RESP_ERR_UNSPEC);
        if (!p->resource_id) {
            scan_hide(int(l.slot));
            break;
        }
#ifdef __ANDROID__
        auto it = g_ahb.find(p->resource_id);
        if (it == g_ahb.end()) {
            void* ahb = stream_renderer_resource_get_ahb(p->resource_id);
            if (!ahb) return reply(l, *hdr, VGPU_RESP_ERR_INVALID_RESOURCE_ID);  // not an exportable ColorBuffer
            it = g_ahb.emplace(p->resource_id, ahb).first;
        }
        auto* ahb = static_cast<AHardwareBuffer*>(it->second);
        AHardwareBuffer_Desc d{};
        AHardwareBuffer_describe(ahb, &d);
        AHardwareBuffer_acquire(ahb);
        scan_submit(Frame{ahb, l.slot, l.generation, p->index, p->seq, d.width, d.height,
                          (p->flags & SGPU_SCANOUT_VSYNC) != 0});
#endif
        break;
    }
    default:
        LOG("unknown command 0x%x", hdr->type);
        return reply(l, *hdr, VGPU_RESP_ERR_UNSPEC);
    }

    if (!reply(l, *hdr, resp)) return false;
fence:
    if (hdr->flags & VGPU_FLAG_FENCE) {
        stream_renderer_fence f{};
        // Always a context fence, ring 0 when the guest named none: a global fence would come
        // back with no context, and the phone could not tell whose control page it belongs in.
        f.flags = STREAM_RENDERER_FLAG_FENCE | STREAM_RENDERER_FLAG_FENCE_RING_IDX;
        f.fence_id = hdr->fence_id;
        f.ctx_id = HWGPU_CTX(l.slot);
        f.ring_idx = (hdr->flags & VGPU_FLAG_INFO_RING_IDX) ? hdr->ring_idx : 0;
        stream_renderer_create_fence(&f);
    }
    return true;
}

// libhwd3d.so (gpu/host/hwd3d/hwd3d.cpp), loaded on the first HwD3D channel.
std::atomic<int> g_hwd3d_threads{0};
using hwd3d_serve_fn = void (*)(void* chan, uint32_t slot, uint32_t generation, const void* first, size_t first_len);

// Hands channel i's connection, whose first bytes are `first`, to a thread in libhwd3d.so.
bool start_hwd3d(uint32_t i, uint32_t gen, const uint8_t* first, size_t len) {
    static hwd3d_serve_fn serve = [] {
        void* lib = dlopen("libhwd3d.so", RTLD_NOW | RTLD_LOCAL);
        auto fn = lib ? reinterpret_cast<hwd3d_serve_fn>(dlsym(lib, "hwd3d_serve")) : nullptr;
        if (!fn) LOG("no HwD3D: %s", dlerror());
        return fn;
    }();
    if (!serve) return false;
    std::vector<uint8_t> head(first, first + len);
    g_hwd3d_threads++;
    std::thread([i, gen, head = std::move(head)] {
        void* chan = chan_base(i);
        serve(chan, i, gen, head.data(), head.size());
        // Ended by the program closing, or by a bad stream: then stop serving the channel, and
        // its program gets errors until it closes the device, as with sgpu.
        hwgpu_ctl* c = hwgpu_chan_ctl(chan);
        if (hwgpu_alive(c, gen)) __atomic_store_n(&c->host_state, HWGPU_HOST_IDLE, __ATOMIC_RELEASE);
        g_hwd3d_threads--;
    }).detach();
    return true;
}

// Serves one request on a channel when one has started arriving. Returns true when it did.
bool serve_one(uint32_t i) {
    void* chan = chan_base(i);
    hwgpu_ctl* c = hwgpu_chan_ctl(chan);
    Slot& s = g_slots[i];
    const uint32_t state = __atomic_load_n(&c->guest_state, __ATOMIC_ACQUIRE);
    if (state == HWGPU_CLOSING) {
        drop_slot(s);
        __atomic_store_n(&c->host_state, HWGPU_HOST_IDLE, __ATOMIC_RELEASE);
        uint32_t closing = HWGPU_CLOSING;
        __atomic_compare_exchange_n(&c->guest_state, &closing, HWGPU_FREE, false, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE);
        LOG("channel %u closed", i);
        return false;
    }
    if (state != HWGPU_CONNECTED) return false;
    const uint32_t gen = __atomic_load_n(&c->generation, __ATOMIC_ACQUIRE);
    if (s.generation != gen) {
        drop_slot(s);  // a connection this side never saw close
        memset(c->fence, 0, sizeof(c->fence));
        // A new process counts its scanouts from 1: counts the last one left would read as released.
        memset(c->scanout_free, 0, sizeof(c->scanout_free));
        __atomic_store_n(&c->scanout_flags, g_scanout_flags.load(), __ATOMIC_RELEASE);
        s.generation = gen;
        s.hwd3d = false;
        __atomic_store_n(&c->host_state, HWGPU_HOST_READY, __ATOMIC_RELEASE);
        LOG("channel %u open, generation %u", i, gen);
    }
    if (s.hwd3d) return false;
    if (__atomic_load_n(&c->host_state, __ATOMIC_RELAXED) != HWGPU_HOST_READY) return false;  // dropped
    if (__atomic_load_n(&c->tx_head, __ATOMIC_ACQUIRE) == __atomic_load_n(&c->tx_tail, __ATOMIC_RELAXED))
        return false;

    // A request has begun. The guest writes the rest without waiting on anything, so read it whole.
    Link l{chan, i, gen};
    uint8_t prefix[sizeof(uint32_t) + sizeof(vgpu_hdr)];
    uint32_t len = 0;
    std::vector<uint8_t> m;
    bool ok = hwgpu_read_all(chan, gen, 0, prefix, sizeof(prefix));
    uint32_t word1;
    memcpy(&word1, prefix + sizeof(len), sizeof(word1));
    if (ok && word1 == HWD3D_MAGIC) {
        // Not sgpu: an HwD3D program (include/hw_d3d.h). Its channel gets a thread of its own.
        ok = start_hwd3d(i, gen, prefix, sizeof(prefix));
        if (ok) s.hwd3d = true;
    } else if (ok) {
        memcpy(&len, prefix, sizeof(len));
        ok = len >= sizeof(vgpu_hdr) && len <= kMaxMessage;
    }
    if (ok && s.hwd3d) return true;
    if (ok) {
        m.resize(len);
        memcpy(m.data(), prefix + sizeof(len), sizeof(vgpu_hdr));
        ok = hwgpu_read_all(chan, gen, 0, m.data() + sizeof(vgpu_hdr), len - sizeof(vgpu_hdr)) && handle(l, m);
    }
    if (!ok && hwgpu_alive(c, gen)) {
        // A malformed stream cannot be resynchronized. Stop serving it; the guest sees the channel
        // go idle and its process gets errors until it closes the device.
        LOG("channel %u: bad request stream, dropping it", i);
        drop_slot(s);
        s.generation = gen;
        __atomic_store_n(&c->host_state, HWGPU_HOST_IDLE, __ATOMIC_RELEASE);
    }
    return true;
}

void serve_loop() {
    hwgpu_ring_hdr* h = hwgpu_hdr(g_window);
    uint32_t misses = 0;
    // Requests come in bursts while a program draws, each one waiting on the last answer. For
    // 200 us after the last request the loop only yields, so the next is seen within microseconds;
    // after that it sleeps with growing pauses, so a phone's cores are not kept busy (and hot) for
    // nothing. Sleeps are as asked, not rounded up by timer slack.
    prctl(PR_SET_TIMERSLACK, 1);
    uint64_t last = 0;
    while (g_run.load(std::memory_order_relaxed)) {
        __atomic_add_fetch(&h->host_tick, 1, __ATOMIC_RELAXED);
        bool busy = false, open = false;
        for (uint32_t i = 0; i < HWGPU_CHANNELS; i++) {
            busy |= serve_one(i);
            open |= g_slots[i].generation != 0;
        }
        if (busy) {
            misses = 0;
            last = now_ns();
        } else if (open && now_ns() - last < 200000ull) {
            sched_yield();
        } else if (open) {
            hwgpu_backoff(&misses);
        } else {
            hwgpu_pause(2000);  // nobody has the GPU open
        }
    }
}

}  // namespace

extern "C" void gfxstream_set_host_memory_allocator(void* (*alloc)(size_t, size_t), int (*release)(void*));

// The first Vulkan device's name, asked of the phone's own driver, for the adapter's name in Windows.
void gpu_name(char* out, size_t size) {
    snprintf(out, size, "Phone GPU");
#ifdef __ANDROID__
    void* lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return;
    auto create = reinterpret_cast<PFN_vkCreateInstance>(dlsym(lib, "vkCreateInstance"));
    auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
    if (!create || !get) return;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "hwgpu", 1, "hwgpu", 1, VK_API_VERSION_1_0};
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app};
    VkInstance inst = nullptr;
    if (create(&ci, nullptr, &inst) != VK_SUCCESS || !inst) return;
    auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get(inst, "vkEnumeratePhysicalDevices"));
    auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(get(inst, "vkGetPhysicalDeviceProperties"));
    auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(get(inst, "vkDestroyInstance"));
    uint32_t n = 1;
    VkPhysicalDevice dev = nullptr;
    if (enumerate && props && enumerate(inst, &n, &dev) >= 0 && n >= 1 && dev) {
        VkPhysicalDeviceProperties p{};
        props(dev, &p);
        snprintf(out, size, "%s", p.deviceName);
    }
    if (destroy) destroy(inst, nullptr);
#endif
}

int hwgpu_host_start(void* window, size_t window_bytes, void* heap, size_t heap_bytes, uint32_t renderer_flags,
                     uint32_t width, uint32_t height) {
    if (!window || window_bytes < HWGPU_RING_OFF + HWGPU_RING_BYTES) return -EINVAL;
    if (g_run.load()) return -EBUSY;
    g_trace = getenv("HWGPU_TRACE") != nullptr;
    if (heap && heap_bytes && !g_heap) {
        g_heap = static_cast<uint8_t*>(heap);
        g_heap_size = heap_bytes & ~(kHeapPage - 1);
        g_heap_free.clear();
        g_heap_used.clear();
        g_heap_free[0] = g_heap_size;
        gfxstream_set_host_memory_allocator(heap_alloc, heap_release);
    }
    if (!g_inited) {
        // See SweetCider's tools/gfxstream-android/android.patch: one thread serves every guest
        // stream, so pipe reads must not block, and each guest thread gets a pipe of its own.
        setenv("GFXSTREAM_PIPE_READ_EAGAIN", "1", 1);
        setenv("GFXSTREAM_PIPE_PER_RESOURCE", "1", 1);
        stream_renderer_param p[] = {
            {STREAM_RENDERER_PARAM_USER_DATA, 0},
            {STREAM_RENDERER_PARAM_RENDERER_FLAGS, renderer_flags},
            {STREAM_RENDERER_PARAM_FENCE_CALLBACK, reinterpret_cast<uintptr_t>(&on_fence)},
            {STREAM_RENDERER_PARAM_DEBUG_CALLBACK, reinterpret_cast<uintptr_t>(&on_debug)},
            {STREAM_RENDERER_PARAM_WIN0_WIDTH, width},
            {STREAM_RENDERER_PARAM_WIN0_HEIGHT, height},
            // Host-visible memory from the shared heap (heap_alloc), imported with VK_EXT_external_memory_host.
            {STREAM_RENDERER_PARAM_RENDERER_FEATURES,
             reinterpret_cast<uintptr_t>(g_heap ? "VulkanAllocateHostMemory:enabled" : "")},
        };
        int err = stream_renderer_init(p, sizeof(p) / sizeof(p[0]));
        if (err) {
            LOG("stream_renderer_init failed (%d)", err);
            return err < 0 ? err : -err;
        }
        for (uint32_t id : {VGPU_CAPSET_GFXSTREAM_VULKAN, VGPU_CAPSET_GFXSTREAM_GLES, VGPU_CAPSET_GFXSTREAM_COMPOSER}) {
            uint32_t ver = 0, size = 0;
            stream_renderer_get_cap_set(id, &ver, &size);
            if (!size) continue;
            vgpu_resp_capset_info i{};
            i.hdr.type = VGPU_RESP_OK_CAPSET_INFO;
            i.capset_id = id;
            i.capset_max_version = ver;
            i.capset_max_size = size;
            g_capsets.push_back(i);
            LOG("capset %u version %u size %u", id, ver, size);
        }
        // Scanout needs ColorBuffers the compositor can take: try one. Without the driver's
        // VK_ANDROID_external_memory_android_hardware_buffer, gfxstream gives them memory that
        // cannot be exported, and Windows keeps drawing frames itself.
        {
            constexpr uint32_t kProbe = 0x7f000001u;  // outside every channel's resource range
            stream_renderer_resource_create_args a{kProbe, 2 /* PIPE_TEXTURE_2D */, 67 /* VIRGL R8G8B8A8 */,
                                                   1u << 1 /* render target */, 64, 64, 1, 1, 0, 0, 0};
            if (!stream_renderer_resource_create(&a, nullptr, 0)) {
                void* ahb = stream_renderer_resource_get_ahb(kProbe);
                g_scanout_ok = ahb != nullptr;
#ifdef __ANDROID__
                if (ahb) AHardwareBuffer_release(static_cast<AHardwareBuffer*>(ahb));
#endif
                stream_renderer_resource_unref(kProbe);
            }
            LOG("scanout %s", g_scanout_ok ? "available" : "unavailable: the driver cannot export ColorBuffers");
        }
#ifdef __ANDROID__
        g_set_buffer_with_release = reinterpret_cast<SetBufferWithRelease>(
            dlsym(RTLD_DEFAULT, "ASurfaceTransaction_setBufferWithRelease"));
        LOG("scanout buffers released %s", g_set_buffer_with_release ? "one by one" : "per transaction");
#endif
        g_inited = true;
    }

    g_window = static_cast<uint8_t*>(window);
    // A new boot's window is all zeros. A renderer restarted under a running Windows (another
    // Vulkan driver, or the last renderer crashed) finds channels the old one served: what they
    // made died with it, so they are marked dropped and their processes get errors. Channels still
    // waiting for their first answer are served as new. The guest owns each channel's state until
    // it closes it, so nothing is wiped.
    for (uint32_t i = 0; i < HWGPU_CHANNELS; i++) {
        hwgpu_ctl* c = hwgpu_chan_ctl(chan_base(i));
        g_slots[i] = Slot{};
        // A process may still have heap ranges mapped from the last renderer: keep them out of
        // use until it lets go of the channel, and let it map no more.
        for (uint32_t g = 0; g < HWGPU_GRANTS; g++) {
            uint64_t size = __atomic_load_n(&c->grant[g].size, __ATOMIC_ACQUIRE), off = c->grant[g].offset;
            if (!size) continue;
            {
                std::lock_guard<std::mutex> hl(g_heap_lock);
                if (g_heap && off <= g_heap_size && size <= g_heap_size - off && heap_take(off, size))
                    g_slots[i].quarantined.emplace_back(off, size);
            }
            clear_grant(i, int(g));
        }
        if (__atomic_load_n(&c->guest_state, __ATOMIC_ACQUIRE) == HWGPU_CONNECTED &&
            __atomic_load_n(&c->host_state, __ATOMIC_ACQUIRE) == HWGPU_HOST_READY) {
            g_slots[i].generation = __atomic_load_n(&c->generation, __ATOMIC_ACQUIRE);
            __atomic_store_n(&c->host_state, HWGPU_HOST_IDLE, __ATOMIC_RELEASE);
            LOG("channel %u was open before this renderer, dropping it", i);
        }
    }
    hwgpu_ring_hdr* h = hwgpu_hdr(g_window);
    h->version = HWGPU_VERSION;
    h->channels = HWGPU_CHANNELS;
    h->tx_bytes = HWGPU_TX_BYTES;
    h->rx_bytes = HWGPU_RX_BYTES;
    // A value no earlier renderer had, so the WDDM adapter can tell a restart (a driver swap).
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    __atomic_store_n(&h->epoch, (uint64_t(ts.tv_sec) << 32) ^ uint64_t(ts.tv_nsec) ^ (uint64_t(getpid()) << 16),
                     __ATOMIC_RELEASE);
    h->copy_to_guest = h->copy_from_guest = 0;
    gpu_name(h->gpu_name, sizeof h->gpu_name);
    __atomic_store_n(&h->magic, HWGPU_MAGIC, __ATOMIC_RELEASE);

    g_run = true;
    g_thread = std::thread(serve_loop);
    return 0;
}

int hwgpu_host_set_layer(void* layer, int view_width, int view_height) {
    std::lock_guard<std::mutex> l(g_scan.lock);
    g_scan.view_w = view_width;
    g_scan.view_h = view_height;
#ifdef __ANDROID__
    auto* sc = static_cast<ASurfaceControl*>(layer);
    if (sc == g_scan.layer) return 0;  // only a new size
    // A new layer (or none): what the old one showed is gone from the screen with it.
    forget_shown_locked();
    g_scan.has_shown = false;
    g_scan.inflight = false;
    g_scan.visible = false;
    if (g_scan.layer) ASurfaceControl_release(g_scan.layer);
    g_scan.layer = sc;  // the caller's reference passes to us
#else
    (void)layer;
#endif
    return 0;
}

// ---- HwD3D's surface ----

namespace {
std::mutex g_d3d_lock;
#ifdef __ANDROID__
ASurfaceControl* g_d3d_layer = nullptr;
ANativeWindow* g_d3d_window = nullptr;
#endif
bool g_d3d_shown = false;
}  // namespace

void hwgpu_host_set_d3d_layer(void* layer, void* window) {
#ifdef __ANDROID__
    std::lock_guard<std::mutex> l(g_d3d_lock);
    if (g_d3d_layer) ASurfaceControl_release(g_d3d_layer);
    if (g_d3d_window) ANativeWindow_release(g_d3d_window);
    g_d3d_layer = static_cast<ASurfaceControl*>(layer);
    g_d3d_window = static_cast<ANativeWindow*>(window);
    if (g_d3d_layer && g_d3d_shown) {
        ASurfaceTransaction* t = ASurfaceTransaction_create();
        ASurfaceTransaction_setVisibility(t, g_d3d_layer, ASURFACE_TRANSACTION_VISIBILITY_SHOW);
        ASurfaceTransaction_apply(t);
        ASurfaceTransaction_delete(t);
    }
#else
    (void)layer;
    (void)window;
#endif
}

void* hwgpu_host_d3d_window_acquire(void) {
#ifdef __ANDROID__
    std::lock_guard<std::mutex> l(g_d3d_lock);
    if (g_d3d_window) ANativeWindow_acquire(g_d3d_window);
    return g_d3d_window;
#else
    return nullptr;
#endif
}

void hwgpu_host_d3d_window_release(void* window) {
#ifdef __ANDROID__
    if (window) ANativeWindow_release(static_cast<ANativeWindow*>(window));
#else
    (void)window;
#endif
}

void hwgpu_host_d3d_show(int on) {
    std::lock_guard<std::mutex> l(g_d3d_lock);
    g_d3d_shown = on != 0;
#ifdef __ANDROID__
    if (!g_d3d_layer) return;
    ASurfaceTransaction* t = ASurfaceTransaction_create();
    ASurfaceTransaction_setVisibility(t, g_d3d_layer, on ? ASURFACE_TRANSACTION_VISIBILITY_SHOW
                                                         : ASURFACE_TRANSACTION_VISIBILITY_HIDE);
    ASurfaceTransaction_apply(t);
    ASurfaceTransaction_delete(t);
#endif
}

void* hwgpu_host_heap_grant(uint32_t slot, uint64_t size, uint64_t* offset) {
    if (!g_window || slot >= HWGPU_CHANNELS) return nullptr;
    auto* b = static_cast<uint8_t*>(heap_alloc(kHeapPage, size));
    if (!b) return nullptr;
    auto* ctl = hwgpu_chan_ctl(chan_base(slot));
    for (uint32_t i = 0; i < HWGPU_GRANTS; i++) {
        if (__atomic_load_n(&ctl->grant[i].size, __ATOMIC_ACQUIRE)) continue;
        ctl->grant[i].offset = uint64_t(b - g_heap);
        __atomic_store_n(&ctl->grant[i].size, (size + kHeapPage - 1) & ~(kHeapPage - 1), __ATOMIC_RELEASE);
        *offset = uint64_t(b - g_heap);
        return b;
    }
    heap_release(b);
    return nullptr;
}

void hwgpu_host_heap_free(void* p) { heap_release(p); }

int hwgpu_host_vsync(void) { return (g_scanout_flags.load() & HWGPU_SCANOUT_VSYNC) != 0; }

void hwgpu_host_set_vsync(int on) {
    g_scanout_flags = on ? HWGPU_SCANOUT_VSYNC : 0;
    if (!g_window) return;
    for (uint32_t i = 0; i < HWGPU_CHANNELS; i++)
        __atomic_store_n(&hwgpu_chan_ctl(chan_base(i))->scanout_flags, g_scanout_flags.load(), __ATOMIC_RELEASE);
}

void hwgpu_host_set_layer_size(int view_width, int view_height) {
    std::lock_guard<std::mutex> l(g_scan.lock);
    g_scan.view_w = view_width;
    g_scan.view_h = view_height;
}

uint64_t hwgpu_host_scanout_frames(void) {
    std::lock_guard<std::mutex> l(g_scan.lock);
    return g_scan.frames;
}

void hwgpu_host_stop(void) {
    if (!g_run.exchange(false)) return;
    g_thread.join();
    // HwD3D threads end when their channel stops being served.
    for (uint32_t i = 0; i < HWGPU_CHANNELS; i++)
        if (g_slots[i].hwd3d) __atomic_store_n(&hwgpu_chan_ctl(chan_base(i))->host_state, HWGPU_HOST_IDLE, __ATOMIC_RELEASE);
    while (g_hwd3d_threads.load()) hwgpu_pause(1000);
    for (auto& s : g_slots) drop_slot(s);
    __atomic_store_n(&hwgpu_hdr(g_window)->magic, 0, __ATOMIC_RELEASE);
}
