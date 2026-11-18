/* d3dping: HwD3D's phase-1 transport test (d3d/README.md). Opens a GPU channel, says hello,
 * sends N requests that want no answer, then one PING that does, and checks the phone counted
 * them all. Build: aarch64-w64-mingw32-clang -O2 -Igpu/include -Iinclude guest/tools/d3dping.c
 *   d3dping [N]          N defaults to 10000 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#include <hw_d3d.h>
#include <hwgpu_ioctl.h>
#include <hwgpu_ring.h>

static void *g_chan;
static uint32_t g_gen, g_seq;

static int send_msg(void *m, uint32_t size, uint16_t op, uint16_t flags)
{
    struct hwd3d_msg *h = m;
    h->size = size;
    h->magic = HWD3D_MAGIC;
    h->op = op;
    h->flags = flags;
    h->seq = ++g_seq;
    return hwgpu_write_all(g_chan, g_gen, 1, m, size);
}

static int recv_msg(void *m, uint32_t size)
{
    if (!hwgpu_read_all(g_chan, g_gen, 1, m, size)) return 0;
    struct hwd3d_msg *h = m;
    return h->magic == HWD3D_MAGIC && h->size == size && (h->flags & HWD3D_REPLY) && h->seq == g_seq;
}

static double ms(LARGE_INTEGER a, LARGE_INTEGER b, LARGE_INTEGER f)
{
    return (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
}

int main(int argc, char **argv)
{
    uint32_t n = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 10) : 10000;
    HANDLE dev = CreateFileW(HWGPU_USER_PATH, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (dev == INVALID_HANDLE_VALUE) { printf("open \\\\.\\HwGpu: error %lu\n", GetLastError()); return 1; }
    struct hwgpu_open_out o;
    DWORD got = 0;
    if (!DeviceIoControl(dev, IOCTL_HWGPU_OPEN, NULL, 0, &o, sizeof o, &got, NULL) || got != sizeof o) {
        printf("IOCTL_HWGPU_OPEN: error %lu\n", GetLastError());
        return 1;
    }
    g_chan = (void *)(uintptr_t)o.chan;
    g_gen = o.generation;
    struct hwgpu_ctl *c = hwgpu_chan_ctl(g_chan);
    for (int i = 0; i < 2000 && __atomic_load_n(&c->host_state, __ATOMIC_ACQUIRE) != HWGPU_HOST_READY; i++) Sleep(1);
    printf("channel %u, generation %u\n", o.slot, o.generation);

    LARGE_INTEGER f, t0, t1, t2;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    struct hwd3d_hello hello = {0};
    hello.version = HWD3D_VERSION;
    hello.pid = GetCurrentProcessId();
    hello.arch = 1;
    struct hwd3d_hello_reply hr;
    if (!send_msg(&hello, sizeof hello, HWD3D_OP_HELLO, 0) || !recv_msg(&hr, sizeof hr)) {
        printf("FAIL: no hello answer\n");
        return 1;
    }
    QueryPerformanceCounter(&t1);
    printf("hello: status 0x%08lx, phone version %u, %.3f ms\n", (unsigned long)hr.r.status, hr.version, ms(t0, t1, f));
    if (hr.r.status != 0) return 1;

    struct hwd3d_msg nop;
    for (uint32_t i = 0; i < n; i++)
        if (!send_msg(&nop, sizeof nop, HWD3D_OP_NOP, HWD3D_NO_REPLY)) { printf("FAIL: send %u\n", i); return 1; }
    struct hwd3d_msg ping;
    struct hwd3d_ping_reply pr;
    if (!send_msg(&ping, sizeof ping, HWD3D_OP_PING, 0) || !recv_msg(&pr, sizeof pr)) {
        printf("FAIL: no ping answer\n");
        return 1;
    }
    QueryPerformanceCounter(&t2);
    double t = ms(t1, t2, f);
    /* The hello, the N requests and the ping itself. */
    uint64_t want = (uint64_t)n + 2;
    printf("%u no-reply requests and a ping: %.3f ms, %.0f requests/s, %.2f us each\n", n, t, (n + 1) * 1000.0 / t,
           t * 1000.0 / (n + 1));
    printf("phone received %llu, expected %llu: %s\n", (unsigned long long)pr.received, (unsigned long long)want,
           pr.received == want ? "PASS" : "FAIL");

    /* Round trips: one waiting request at a time. */
    LARGE_INTEGER r0, r1;
    QueryPerformanceCounter(&r0);
    for (int i = 0; i < 1000; i++)
        if (!send_msg(&ping, sizeof ping, HWD3D_OP_PING, 0) || !recv_msg(&pr, sizeof pr)) { printf("FAIL: ping %d\n", i); return 1; }
    QueryPerformanceCounter(&r1);
    printf("1000 round trips: %.2f us each\n", ms(r0, r1, f));
    CloseHandle(dev);
    return pr.received == want + 1000 ? 0 : 1;
}
