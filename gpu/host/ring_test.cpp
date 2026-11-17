// Drives hwgpu_host over a window in this process the way a Windows process will over the
// shared window. Checks hello, capsets, a context, a buffer round trip through the renderer, a
// fence in the control page, the id fence between channels, and that closing frees the channel.
//   ring_test [renderer_flags]   default: Vulkan, surfaceless
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "gfxstream/virtio-gpu-gfxstream-renderer.h"
#include "hw_abi.h"
#include "hwgpu_host.h"
#include "hwgpu_ring.h"
#include "sgpu_proto.h"

#define CHECK(c, ...)                                            \
    do {                                                         \
        if (!(c)) {                                              \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                        \
            fputc('\n', stderr);                                 \
            exit(1);                                             \
        }                                                        \
    } while (0)

static uint8_t* g_window;

struct Guest {
    void* chan;
    uint32_t slot, gen;

    explicit Guest() {
        int s = hwgpu_claim(g_window, &gen);
        CHECK(s >= 0, "no free channel");
        slot = uint32_t(s);
        chan = g_window + hwgpu_chan_off(slot);
        for (int i = 0; i < 2000 && __atomic_load_n(&hwgpu_chan_ctl(chan)->host_state, __ATOMIC_ACQUIRE) != HWGPU_HOST_READY; i++)
            hwgpu_pause(1000);
        CHECK(hwgpu_alive(hwgpu_chan_ctl(chan), gen), "phone never readied channel %u", slot);
    }

    std::vector<uint8_t> call(const void* m, size_t len, const void* tail = nullptr, size_t tail_len = 0) {
        uint32_t n = uint32_t(len + tail_len);
        CHECK(hwgpu_write_all(chan, gen, 1, &n, 4) && hwgpu_write_all(chan, gen, 1, m, len) &&
                  (!tail_len || hwgpu_write_all(chan, gen, 1, tail, tail_len)),
              "write on channel %u", slot);
        CHECK(hwgpu_read_all(chan, gen, 1, &n, 4), "read on channel %u", slot);
        std::vector<uint8_t> r(n);
        CHECK(hwgpu_read_all(chan, gen, 1, r.data(), n), "read on channel %u", slot);
        return r;
    }
    uint32_t type(const void* m, size_t len, const void* tail = nullptr, size_t tail_len = 0) {
        return reinterpret_cast<vgpu_hdr*>(call(m, len, tail, tail_len).data())->type;
    }
    void close() {
        hwgpu_release(g_window, slot);
        for (int i = 0; i < 2000 && __atomic_load_n(&hwgpu_chan_ctl(chan)->guest_state, __ATOMIC_ACQUIRE) != HWGPU_FREE; i++)
            hwgpu_pause(1000);
        CHECK(hwgpu_chan_ctl(chan)->guest_state == HWGPU_FREE, "channel %u never freed", slot);
    }
};

static vgpu_hdr H(uint32_t type, uint32_t ctx = 0) {
    vgpu_hdr h{};
    h.type = type;
    h.ctx_id = ctx;
    return h;
}

int main(int argc, char** argv) {
    uint32_t flags = argc > 1 ? strtoul(argv[1], nullptr, 0)
                              : STREAM_RENDERER_FLAGS_USE_VK_BIT | STREAM_RENDERER_FLAGS_USE_SURFACELESS_BIT;
    g_window = static_cast<uint8_t*>(calloc(1, HW_WINDOW_SIZE));
    CHECK(g_window, "window");
    CHECK(hwgpu_host_start(g_window, HW_WINDOW_SIZE, nullptr, 0, flags, 256, 256) == 0, "hwgpu_host_start");

    Guest a;
    sgpu_hello hello{H(SGPU_CMD_HELLO), SGPU_VERSION, 0};
    auto r = a.call(&hello, sizeof(hello));
    auto* hr = reinterpret_cast<sgpu_hello_resp*>(r.data());
    CHECK(hr->hdr.type == SGPU_RESP_OK_HELLO && hr->num_capsets > 0, "hello 0x%x, %u capsets", hr->hdr.type,
          hr->num_capsets);
    printf("channel %u: hello, %u capsets\n", a.slot, hr->num_capsets);

    uint32_t vk = 0;
    for (uint32_t i = 0; i < hr->num_capsets; i++) {
        vgpu_get_capset_info q{H(VGPU_CMD_GET_CAPSET_INFO), i, 0};
        auto m = a.call(&q, sizeof(q));
        auto ci = *reinterpret_cast<vgpu_resp_capset_info*>(m.data());
        CHECK(ci.hdr.type == VGPU_RESP_OK_CAPSET_INFO, "capset info 0x%x", ci.hdr.type);
        vgpu_get_capset g{H(VGPU_CMD_GET_CAPSET), ci.capset_id, ci.capset_max_version};
        auto c = a.call(&g, sizeof(g));
        CHECK(c.size() == sizeof(vgpu_hdr) + ci.capset_max_size, "capset %u size %zu", ci.capset_id, c.size());
        printf("capset %u: version %u, %u bytes\n", ci.capset_id, ci.capset_max_version, ci.capset_max_size);
        if (ci.capset_id == VGPU_CAPSET_GFXSTREAM_VULKAN) vk = ci.capset_id;
    }
    CHECK(vk, "no Vulkan capset");

    const uint32_t ctx = HWGPU_CTX(a.slot);
    vgpu_ctx_create cc{H(VGPU_CMD_CTX_CREATE, ctx), 9, vk, "ring-test"};
    CHECK(a.type(&cc, sizeof(cc)) == VGPU_RESP_OK_NODATA, "ctx create");

    // A 1 MiB buffer: bigger than half a ring, so the stream wraps.
    const uint32_t res = HWGPU_RES_BASE(a.slot) + 7, size = 1u << 20;
    vgpu_resource_create_3d rc{H(VGPU_CMD_RESOURCE_CREATE_3D), res, 0 /* PIPE_BUFFER */, 64 /* R8_UNORM */,
                               0x10, size, 1, 1, 1, 0, 0, 0, 0};
    CHECK(a.type(&rc, sizeof(rc)) == VGPU_RESP_OK_NODATA, "resource create");
    struct {
        vgpu_resource_attach_backing a;
        vgpu_mem_entry e;
    } ab{{H(VGPU_CMD_RESOURCE_ATTACH_BACKING), res, 1}, {0, size, 0}};
    CHECK(a.type(&ab, sizeof(ab)) == VGPU_RESP_OK_NODATA, "attach backing");
    vgpu_resource_unref at{H(VGPU_CMD_CTX_ATTACH_RESOURCE, ctx), res, 0};
    CHECK(a.type(&at, sizeof(at)) == VGPU_RESP_OK_NODATA, "ctx attach");

    std::vector<uint8_t> pat(size);
    for (uint32_t i = 0; i < size; i++) pat[i] = uint8_t(i * 31 + 7);
    struct {
        vgpu_transfer_3d t;
        uint32_t len;
    } __attribute__((packed)) tw{{H(VGPU_CMD_TRANSFER_TO_HOST_3D, ctx), {0, 0, 0, size, 1, 1}, 0, res, 0, 0, 0}, size};
    tw.t.hdr.flags = VGPU_FLAG_FENCE;
    tw.t.hdr.fence_id = 42;
    for (int round = 0; round < 3; round++)
        CHECK(a.type(&tw, sizeof(tw), pat.data(), size) == VGPU_RESP_OK_NODATA, "transfer to host");

    std::vector<uint8_t> zero(size);
    sgpu_blob_io bw{H(SGPU_CMD_BLOB_WRITE), res, size, 0};
    CHECK(a.type(&bw, sizeof(bw), zero.data(), size) == VGPU_RESP_OK_NODATA, "clear backing");
    tw.t.hdr.flags = 0;
    tw.t.hdr.type = VGPU_CMD_TRANSFER_FROM_HOST_3D;
    auto d = a.call(&tw, sizeof(tw));
    auto* dr = reinterpret_cast<sgpu_resp_data*>(d.data());
    CHECK(dr->hdr.type == SGPU_RESP_OK_DATA && d.size() == sizeof(*dr) + size, "transfer from host 0x%x", dr->hdr.type);
    CHECK(memcmp(d.data() + sizeof(*dr), pat.data(), size) == 0, "round trip data differs");
    printf("buffer round trip through the renderer: %u bytes match\n", size);

    for (int i = 0; i < 1000 && __atomic_load_n(&hwgpu_chan_ctl(a.chan)->fence[0], __ATOMIC_ACQUIRE) != 42; i++)
        hwgpu_pause(1000);
    CHECK(hwgpu_chan_ctl(a.chan)->fence[0] == 42, "fence 42 not in the control page (%llu)",
          (unsigned long long)hwgpu_chan_ctl(a.chan)->fence[0]);
    printf("fence 42 in the control page\n");

    // Present: write a solid red 4x2 frame into a resource and present it.
    {
        const uint32_t pres = HWGPU_RES_BASE(a.slot) + 20;
        const uint32_t pw = 4, ph = 2, pstride = pw * 4, pbytes = pstride * ph;
        vgpu_resource_create_3d prc{H(VGPU_CMD_RESOURCE_CREATE_3D), pres, 0, 64, 0x10, pbytes, 1, 1, 1, 0, 0, 0, 0};
        CHECK(a.type(&prc, sizeof(prc)) == VGPU_RESP_OK_NODATA, "present resource create");
        struct { vgpu_resource_attach_backing a; vgpu_mem_entry e; }
            pab{{H(VGPU_CMD_RESOURCE_ATTACH_BACKING), pres, 1}, {0, pbytes, 0}};
        CHECK(a.type(&pab, sizeof(pab)) == VGPU_RESP_OK_NODATA, "present attach backing");
        // Fill with solid red BGRA (0x00, 0x00, 0xFF, 0xFF).
        std::vector<uint8_t> red(pbytes);
        for (uint32_t i = 0; i < pw * ph; i++) { red[i*4] = 0; red[i*4+1] = 0; red[i*4+2] = 0xFF; red[i*4+3] = 0xFF; }
        sgpu_blob_io pbw{H(SGPU_CMD_BLOB_WRITE), pres, pbytes, 0};
        CHECK(a.type(&pbw, sizeof(pbw), red.data(), pbytes) == VGPU_RESP_OK_NODATA, "fill red");
        // Present it.
        struct hw_header* wh = reinterpret_cast<struct hw_header*>(g_window);
        uint32_t old_seq = wh->seq;
        sgpu_present sp{H(SGPU_CMD_PRESENT), pres, pw, ph, pstride, SGPU_PRESENT_BGRA8};
        CHECK(a.type(&sp, sizeof(sp)) == VGPU_RESP_OK_NODATA, "present");
        CHECK(wh->seq == old_seq + 1, "seq bumped");
        CHECK(wh->width == pw && wh->height == ph, "frame dimensions");
        uint32_t off = hw_frame_off(wh, wh->front);
        CHECK(memcmp(g_window + off, red.data(), pbytes) == 0, "frame pixels match");
        printf("present: 4x2 red frame in the shared window, seq %u\n", wh->seq);
    }

    // A second process may not touch the first one's resource or context.
    Guest b;
    CHECK(b.slot != a.slot, "same slot twice");
    vgpu_resource_unref steal{H(VGPU_CMD_RESOURCE_UNREF), res, 0};
    CHECK(b.type(&steal, sizeof(steal)) == VGPU_RESP_ERR_INVALID_RESOURCE_ID, "channel %u freed another's resource", b.slot);
    vgpu_ctx_create cc2{H(VGPU_CMD_CTX_CREATE, ctx), 9, vk, "ring-test"};
    CHECK(b.type(&cc2, sizeof(cc2)) == VGPU_RESP_ERR_INVALID_CONTEXT_ID, "channel %u used another's context", b.slot);
    printf("channel %u refused channel %u's ids\n", b.slot, a.slot);

    a.close();
    b.close();
    Guest again;
    CHECK(again.gen > 1 || again.slot != a.slot, "reuse");
    again.close();
    printf("channels freed and reused\n");

    hwgpu_host_stop();
    printf("PASS\n");
    return 0;
}
