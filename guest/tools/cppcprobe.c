/* Read-only CPPC/virtual-cpufreq diagnostics. Run elevated through the guest agent.
 * Build: aarch64-w64-mingw32-clang -O2 -Iinclude -Igpu/include
 *        guest/tools/cppcprobe.c -o build/cppcprobe.exe
 * Optional argument: seconds to sample (default 5, maximum 60).
 * Never maps or writes the virtual frequency device itself. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "hw_abi.h"
#include "hwgpu_ioctl.h"

int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 5;
    if (seconds < 1 || seconds > 60) return 2;
    HANDLE f = CreateFileW(HWGPU_USER_PATH, GENERIC_READ | GENERIC_WRITE,
                          0, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { printf("open failed: %lu\n", GetLastError()); return 1; }
    struct hwgpu_map_out out = {0};
    DWORD got = 0;
    if (!DeviceIoControl(f, IOCTL_HWGPU_SCREEN, NULL, 0, &out, sizeof out, &got, NULL) || !out.user) {
        printf("map failed: %lu\n", GetLastError()); CloseHandle(f); return 1;
    }
    const volatile uint8_t *window = (const volatile uint8_t *)(uintptr_t)out.user;
    const volatile struct hw_header *h = (const volatile struct hw_header *)window;
    if (h->header_bytes < sizeof *h || h->cpu_topo_count > HW_CPPC_MAX_CPUS) {
        printf("old/invalid header: %u bytes\n", h->header_bytes); CloseHandle(f); return 1;
    }
    printf("topology=%u protocol=%u mmio=%llx size=%x\n", h->cpu_topo_count,
           h->virt_cpufreq_proto, (unsigned long long)h->virt_cpufreq_base, h->virt_cpufreq_size);
    for (unsigned cpu = 0; cpu < h->cpu_topo_count; ++cpu)
        printf("cpu%u capacity=%u min=%u max=%u MHz class=%u\n", cpu,
               h->cpu_capacity[cpu], h->cpu_min_mhz[cpu], h->cpu_max_mhz[cpu], h->cpu_eff_class[cpu]);
    for (int sample = 0; sample < seconds * 10; ++sample) {
        const volatile uint64_t *r = (const volatile uint64_t *)(window + HW_CPPC_REGS);
        printf("sample=%d ref=%llu delivered=%llu desired=", sample,
               (unsigned long long)r[0], (unsigned long long)r[1]);
        for (unsigned cpu = 0; cpu < h->cpu_topo_count; ++cpu)
            printf("%s%llu", cpu ? "," : "", (unsigned long long)r[2 + 2 * cpu]);
        printf(" pcc_status=%x\n", *(const volatile uint16_t *)(window + HW_CPPC + 6));
        fflush(stdout);
        const volatile struct hw_cppc_bridge_diag *d = (const volatile struct hw_cppc_bridge_diag *)(window + HW_CPPC + HW_CPPC_BRIDGE_OFF);
        if (d->magic == HW_CPPC_BRIDGE_MAGIC && d->count <= HW_CPPC_MAX_CPUS) {
            printf("bridge samples=%u errors=%u", d->samples, d->accounting_errors);
            for (unsigned cpu = 0; cpu < d->count; ++cpu)
                printf(" cpu%u=%ukHz/%uwrites/load%u/reported%u", cpu, d->khz[cpu], d->writes[cpu], d->load_permille[cpu], d->reported_khz[cpu]);
            printf("\n");
        }
        Sleep(100);
    }
    CloseHandle(f);
    return 0;
}
