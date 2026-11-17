// Phone side of the HyperWin GPU rings. Adapted from SweetCider's sgpu_server.cpp
// (tools/gfxstream-android/sgpu): the same requests and replies, read from and written to the
// shared window instead of a vsock. One thread serves every channel, so gfxstream sees one caller.
//
// Differences from the vsock server:
//   - Fences are not messages. The finished fence id lands in the channel's control page.
//   - Each channel may only use its own context and resource ids (HWGPU_CTX, HWGPU_RES_BASE),
//     so two Windows processes cannot touch each other's objects by id.

#include "hwgpu_host.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <vector>

#include "gfxstream/virtio-gpu-gfxstream-renderer.h"
#include "hw_abi.h"
#include "hwgpu_ring.h"
#include "sgpu_proto.h"

#ifdef __ANDROID__
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
};

struct Slot {
    uint32_t generation = 0;  // the connection being served, 0 for none
    std::set<uint32_t> resources, contexts;
};

uint8_t* g_window = nullptr;
bool g_inited = false;
std::atomic<bool> g_run{false};
std::thread g_thread;
std::vector<vgpu_resp_capset_info> g_capsets;
std::map<uint32_t, Backing> g_backing;
std::map<uint32_t, Mapping> g_maps;
Slot g_slots[HWGPU_CHANNELS];

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

void drop_resource(Slot& s, uint32_t res) {
    if (g_maps.erase(res)) stream_renderer_resource_unmap(res);
    stream_renderer_resource_unref(res);
    g_backing.erase(res);
    s.resources.erase(res);
}

// Frees everything one Windows process made.
void drop_slot(Slot& s) {
    for (auto res : std::set<uint32_t>(s.resources)) drop_resource(s, res);
    for (auto c : s.contexts) stream_renderer_context_destroy(c);
    s.contexts.clear();
    s.generation = 0;
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
        if (!own_res(l, res)) return reply(l, *hdr, VGPU_RESP_ERR_INVALID_RESOURCE_ID);
    }

    if (g_trace) LOG("channel %u: request 0x%x ctx %u, %zu bytes", l.slot, hdr->type, hdr->ctx_id, m.size());
    switch (hdr->type) {
    case SGPU_CMD_HELLO: {
        sgpu_hello_resp r{};
        r.hdr.type = SGPU_RESP_OK_HELLO;
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
        g_maps[c->resource_id] = Mapping{static_cast<uint8_t*>(hva), size,
                                         std::vector<uint8_t>(static_cast<uint8_t*>(hva),
                                                              static_cast<uint8_t*>(hva) + size)};
        vgpu_resp_map_info r{};
        r.hdr.type = VGPU_RESP_OK_MAP_INFO;
        r.map_info = info;
        return send_msg(l, &r, sizeof(r));
    }
    case VGPU_CMD_RESOURCE_UNMAP_BLOB: {
        const auto* c = as<vgpu_resource_unref>(m);
        if (!c || !g_maps.erase(c->resource_id)) return reply(l, *hdr, VGPU_RESP_ERR_INVALID_RESOURCE_ID);
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
        // Guest changes first, so the host's diff below leaves them out.
        size_t at = sizeof(vgpu_hdr);
        while (at + sizeof(sgpu_sync_entry) <= m.size()) {
            sgpu_sync_entry e;
            memcpy(&e, m.data() + at, sizeof(e));
            at += sizeof(e);
            auto it = g_maps.find(e.resource_id);
            if (!own_res(l, e.resource_id) || at + e.len > m.size() || it == g_maps.end() ||
                e.offset > it->second.size || e.len > it->second.size - e.offset)
                return reply(l, *hdr, VGPU_RESP_ERR_INVALID_PARAMETER);
            memcpy(it->second.hva + e.offset, m.data() + at, e.len);
            memcpy(it->second.last.data() + e.offset, m.data() + at, e.len);
            at += e.len;
        }
        std::vector<uint8_t> out(sizeof(vgpu_hdr));
        reinterpret_cast<vgpu_hdr*>(out.data())->type = SGPU_RESP_SYNC;
        if (hdr->flags & SGPU_SYNC_PULL) {
            for (auto res : slot.resources) {
                auto mi = g_maps.find(res);
                if (mi == g_maps.end()) continue;
                Mapping& mp = mi->second;
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
        s.generation = gen;
        __atomic_store_n(&c->host_state, HWGPU_HOST_READY, __ATOMIC_RELEASE);
        LOG("channel %u open, generation %u", i, gen);
    }
    if (__atomic_load_n(&c->host_state, __ATOMIC_RELAXED) != HWGPU_HOST_READY) return false;  // dropped
    if (__atomic_load_n(&c->tx_head, __ATOMIC_ACQUIRE) == __atomic_load_n(&c->tx_tail, __ATOMIC_RELAXED))
        return false;

    // A request has begun. The guest writes the rest without waiting on anything, so read it whole.
    Link l{chan, i, gen};
    uint8_t prefix[sizeof(uint32_t) + sizeof(vgpu_hdr)];
    uint32_t len = 0;
    std::vector<uint8_t> m;
    bool ok = hwgpu_read_all(chan, gen, 0, prefix, sizeof(prefix));
    if (ok) {
        memcpy(&len, prefix, sizeof(len));
        ok = len >= sizeof(vgpu_hdr) && len <= kMaxMessage;
    }
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
    while (g_run.load(std::memory_order_relaxed)) {
        __atomic_add_fetch(&h->host_tick, 1, __ATOMIC_RELAXED);
        bool busy = false, open = false;
        for (uint32_t i = 0; i < HWGPU_CHANNELS; i++) {
            busy |= serve_one(i);
            open |= g_slots[i].generation != 0;
        }
        if (busy) {
            misses = 0;
        } else if (open) {
            hwgpu_backoff(&misses);
        } else {
            hwgpu_pause(2000);  // nobody has the GPU open
        }
    }
}

}  // namespace

int hwgpu_host_start(void* window, size_t window_bytes, uint32_t renderer_flags, uint32_t width,
                     uint32_t height) {
    if (!window || window_bytes < HWGPU_RING_OFF + HWGPU_RING_BYTES) return -EINVAL;
    if (g_run.load()) return -EBUSY;
    g_trace = getenv("HWGPU_TRACE") != nullptr;
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
        g_inited = true;
    }

    g_window = static_cast<uint8_t*>(window);
    memset(g_window + HWGPU_RING_OFF, 0, 0x1000);
    for (uint32_t i = 0; i < HWGPU_CHANNELS; i++) {
        memset(chan_base(i), 0, HWGPU_CTL_BYTES);
        hwgpu_chan_ctl(chan_base(i))->slot = i;
    }
    hwgpu_ring_hdr* h = hwgpu_hdr(g_window);
    h->version = HWGPU_VERSION;
    h->channels = HWGPU_CHANNELS;
    h->tx_bytes = HWGPU_TX_BYTES;
    h->rx_bytes = HWGPU_RX_BYTES;
    __atomic_store_n(&h->magic, HWGPU_MAGIC, __ATOMIC_RELEASE);

    g_run = true;
    g_thread = std::thread(serve_loop);
    return 0;
}

void hwgpu_host_stop(void) {
    if (!g_run.exchange(false)) return;
    g_thread.join();
    for (auto& s : g_slots) drop_slot(s);
    __atomic_store_n(&hwgpu_hdr(g_window)->magic, 0, __ATOMIC_RELEASE);
}
