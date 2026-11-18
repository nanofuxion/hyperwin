// libhwd3d.so: the phone side of HwD3D (d3d/README.md), in the renderer process (:gpu).
//
// The sgpu server (gpu/host/hwgpu_host.cpp) hands a channel here when its first message is an
// HwD3D hello. Each channel then gets its own thread, which reads requests (include/hw_d3d.h) and
// answers the ones that want an answer. Phase 1: hello, and the transport tests' NOP and PING.
#include <hw_d3d.h>
#include <hwgpu_ring.h>

#include <sched.h>
#include <string.h>
#include <time.h>

#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "hwd3d", __VA_ARGS__)
#else
#include <stdio.h>
#define LOG(...) (fprintf(stderr, "hwd3d: " __VA_ARGS__), fputc('\n', stderr))
#endif

namespace {

struct Channel {
    void* chan;
    uint32_t slot, generation;
    uint64_t received = 0;
};

bool send(Channel& c, const void* msg, size_t size) {
    return hwgpu_write_all(c.chan, c.generation, 0, msg, size) != 0;
}

template <typename Reply>
void answer(Reply& r, const hwd3d_msg& req, int32_t status) {
    r.r.hdr = {uint32_t(sizeof(Reply)), HWD3D_MAGIC, req.op, HWD3D_REPLY, req.seq};
    r.r.status = status;
}

// One request. False ends the channel: a broken stream cannot be resynchronized.
bool handle(Channel& c, const std::vector<uint8_t>& m) {
    hwd3d_msg h;
    memcpy(&h, m.data(), sizeof h);
    c.received++;
    switch (h.op) {
    case HWD3D_OP_HELLO: {
        if (m.size() < sizeof(hwd3d_hello)) return false;
        hwd3d_hello hello;
        memcpy(&hello, m.data(), sizeof hello);
        LOG("channel %u: HwD3D hello from pid %u, arch %u, version %u", c.slot, hello.pid, hello.arch, hello.version);
        hwd3d_hello_reply r{};
        answer(r, h, hello.version == HWD3D_VERSION ? 0 : int32_t(0x80004005) /* E_FAIL */);
        r.version = HWD3D_VERSION;
        return (h.flags & HWD3D_NO_REPLY) || send(c, &r, sizeof r);
    }
    case HWD3D_OP_NOP:
        return true;
    case HWD3D_OP_PING: {
        hwd3d_ping_reply r{};
        answer(r, h, 0);
        r.received = c.received;
        return (h.flags & HWD3D_NO_REPLY) || send(c, &r, sizeof r);
    }
    default:
        LOG("channel %u: unknown op 0x%x", c.slot, h.op);
        if (h.flags & HWD3D_NO_REPLY) return true;
        hwd3d_reply r{};
        r.hdr = {uint32_t(sizeof r), HWD3D_MAGIC, h.op, HWD3D_REPLY, h.seq};
        r.status = int32_t(0x80004001);  // E_NOTIMPL
        return send(c, &r, sizeof r);
    }
}

// Reads the rest of a message whose first `have` bytes are in m.
bool read_message(Channel& c, std::vector<uint8_t>& m, size_t have) {
    uint32_t size;
    memcpy(&size, m.data(), sizeof size);
    uint32_t magic;
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
    Channel c{chan, slot, generation};
    std::vector<uint8_t> m(reinterpret_cast<const uint8_t*>(first), reinterpret_cast<const uint8_t*>(first) + first_len);
    bool ok = read_message(c, m, first_len) && handle(c, m);
    while (ok) {
        m.resize(sizeof(hwd3d_msg));
        wait_request(chan, generation);
        if (!hwgpu_read_all(chan, generation, 0, m.data(), sizeof(hwd3d_msg))) break;  // the channel ended
        ok = read_message(c, m, sizeof(hwd3d_msg)) && handle(c, m);
    }
    if (!ok && hwgpu_alive(hwgpu_chan_ctl(chan), generation)) LOG("channel %u: bad HwD3D stream, dropping it", slot);
    LOG("channel %u: HwD3D ends after %llu messages", slot, (unsigned long long)c.received);
}
