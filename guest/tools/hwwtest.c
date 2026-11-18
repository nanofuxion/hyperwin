/* hwwtest: talks to the HyperWin GPU adapter through its escape (include/hw_wddm.h).
 *   hwwtest            info, opens a channel, says hello on it (HwD3D's), closes it
 *   hwwtest counters   prints the byte counters (wddm/README.md, "GPU data stays on the phone")
 * Build: aarch64-w64-mingw32-clang -O2 -Iinclude -Igpu/include guest/tools/hwwtest.c */
#include <stdio.h>
#include <string.h>

#include "hww_user.h"
#include "hwgpu_ring.h"
#include "hw_d3d.h"

static void counters(struct hww *w)
{
    struct hww_escape e;
    memset(&e, 0, sizeof e);
    e.op = HWW_ESC_COUNTERS;
    LONG st = hww_call(w, &e);
    if (st < 0) {
        printf("counters: 0x%08lx\n", (unsigned long)st);
        return;
    }
    const struct hww_counters *c = &e.u.counters;
    printf("phone copied into Windows:   %llu bytes\n", (unsigned long long)c->phone_copy_to_guest);
    printf("phone copied out of Windows: %llu bytes\n", (unsigned long long)c->phone_copy_from_guest);
    printf("kernel driver copies:        %llu bytes\n", (unsigned long long)c->kernel_copies);
    printf("escapes %llu, channels opened %llu, mapped %llu bytes\n", (unsigned long long)c->escapes,
           (unsigned long long)c->channels_opened, (unsigned long long)c->mapped_bytes);
    printf("markers pending %llu, done %llu, resets %llu\n", (unsigned long long)c->markers_pending,
           (unsigned long long)c->markers_done, (unsigned long long)c->resets);
}

int main(int argc, char **argv)
{
    struct hww w;
    if (!hww_open(&w)) {
        printf("FAIL: no HyperWin GPU adapter\n");
        return 1;
    }
    if (argc > 1 && !strcmp(argv[1], "counters")) {
        counters(&w);
        hww_close(&w);
        return 0;
    }
    struct hww_escape e;
    memset(&e, 0, sizeof e);
    e.op = HWW_ESC_INFO;
    LONG st = hww_call(&w, &e);
    if (st < 0) {
        printf("FAIL: info 0x%08lx\n", (unsigned long)st);
        return 1;
    }
    printf("driver %u, %u channels, heap %llu MiB, epoch %016llx, gpu \"%s\"\n", e.u.info.driver_version,
           e.u.info.channels, (unsigned long long)(e.u.info.heap_size >> 20), (unsigned long long)e.u.info.epoch,
           e.u.info.gpu_name);
    struct hww_open o;
    st = hww_open_channel(&w, &o);
    if (st < 0) {
        printf("FAIL: open channel 0x%08lx\n", (unsigned long)st);
        return 1;
    }
    printf("channel %u, generation %u at %p\n", o.slot, o.generation, (void *)(uintptr_t)o.chan);
    void *chan = (void *)(uintptr_t)o.chan;
    struct hwgpu_ctl *c = hwgpu_chan_ctl(chan);
    for (int i = 0; i < 2000 && __atomic_load_n(&c->host_state, __ATOMIC_ACQUIRE) != HWGPU_HOST_READY; i++) Sleep(1);
    struct hwd3d_hello h = {{sizeof h, HWD3D_MAGIC, HWD3D_OP_HELLO, 0, 1}, HWD3D_VERSION, GetCurrentProcessId(), 1, 0};
    struct hwd3d_hello_reply r;
    int ok = hwgpu_write_all(chan, o.generation, 1, &h, sizeof h) && hwgpu_read_all(chan, o.generation, 1, &r, sizeof r) &&
             r.r.hdr.magic == HWD3D_MAGIC && r.r.status == 0;
    printf("hello over the escape-mapped channel: %s\n", ok ? "PASS" : "FAIL");
    counters(&w);
    struct hww_escape cl;
    memset(&cl, 0, sizeof cl);
    cl.op = HWW_ESC_CLOSE;
    cl.u.slot = o.slot;
    hww_call(&w, &cl);
    hww_close(&w);
    return ok ? 0 : 1;
}
