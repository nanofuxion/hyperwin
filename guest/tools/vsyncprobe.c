/* vsyncprobe: reads the phone's refresh timeline (vsync/README.md phase 1, struct hwgpu_vsync in
 * hwgpu_ring.h), measures the clock offset between Windows' counter and the phone's, and prints the
 * next refreshes as Windows sees them. Run as SYSTEM or a service account (the screen mapping needs
 * the impersonate privilege), e.g. through the guest agent.
 * Build: aarch64-w64-mingw32-clang -O2 -Iinclude -Igpu/include guest/tools/vsyncprobe.c -o build/vsyncprobe.exe */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include "hwgpu_ioctl.h"
#include "hwgpu_ring.h"

static inline uint64_t counter(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

int main(void)
{
    HANDLE f = CreateFileW(HWGPU_USER_PATH, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { printf("no \\\\.\\HwGpu: %lu\n", GetLastError()); return 1; }
    struct hwgpu_map_out out = {0};
    DWORD got = 0;
    if (!DeviceIoControl(f, IOCTL_HWGPU_SCREEN, NULL, 0, &out, sizeof out, &got, NULL) || !out.user) {
        printf("screen mapping refused: %lu\n", GetLastError());
        return 1;
    }
    volatile struct hwgpu_vsync *v = (volatile struct hwgpu_vsync *)((volatile uint8_t *)(uintptr_t)out.user + HWGPU_VSYNC_OFF);
    if (v->magic != HWGPU_VSYNC_MAGIC) { printf("no vsync block (magic %08x)\n", v->magic); return 1; }
    printf("phone: %u.%03u Hz, period %llu ticks, counter %u Hz, %llu refreshes so far\n", v->hz_milli / 1000,
           v->hz_milli % 1000, (unsigned long long)v->period, v->freq, (unsigned long long)v->count);

    /* Clock offset: the round with the shortest turn-around wins. */
    v->calibrate = 1;
    Sleep(20);
    int64_t best_off = 0;
    uint64_t best_rtt = ~0ull;
    for (int i = 0; i < 50; i++) {
        uint32_t ask = v->ask + 1;
        uint64_t g1 = counter();
        v->guest_t = g1;
        MemoryBarrier();
        v->ask = ask;
        uint64_t limit = g1 + v->freq / 100;
        while (v->answer != ask && counter() < limit) {}
        uint64_t g2 = counter();
        if (v->answer != ask) continue;
        if (g2 - g1 < best_rtt) {
            best_rtt = g2 - g1;
            best_off = (int64_t)(v->host_t - (g1 + g2) / 2);
        }
    }
    v->calibrate = 0;
    if (best_rtt == ~0ull) { printf("the phone did not answer the clock mailbox\n"); return 1; }
    printf("offset %lld ticks (phone - Windows), best turn-around %.1f us\n", (long long)best_off,
           best_rtt * 1e6 / v->freq);

    /* The next 30 refreshes, as Windows' counter, and how far each is from one period after the last. */
    uint64_t last_count = v->count, prev = 0;
    double worst = 0, sum = 0;
    int n = 0;
    while (n < 30) {
        uint32_t s1 = v->seq;
        if (s1 & 1 || v->count == last_count) { YieldProcessor(); continue; }
        uint64_t t = v->last, period = v->period, cnt = v->count;
        if (v->seq != s1) continue;
        uint64_t seen = counter() + best_off;   /* now, in phone ticks */
        last_count = cnt;
        if (prev) {
            double gap_ms = (double)(t - prev) * 1000.0 / v->freq;
            double off_ms = ((double)(t - prev) - (double)period * (double)((t - prev + period / 2) / period)) * 1000.0 / v->freq;
            double late_ms = (double)(seen - t) * 1000.0 / v->freq;
            printf("refresh %llu: %.3f ms after the last, %+.3f ms off the period, seen %.3f ms after it\n",
                   (unsigned long long)cnt, gap_ms, off_ms, late_ms);
            if (off_ms < 0) off_ms = -off_ms;
            if (off_ms > worst) worst = off_ms;
            sum += off_ms;
            n++;
        }
        prev = t;
    }
    printf("spacing off the period: worst %.3f ms, mean %.3f ms\n", worst, sum / n);
    return 0;
}
