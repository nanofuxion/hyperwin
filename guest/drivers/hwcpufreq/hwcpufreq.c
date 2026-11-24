/* Experimental, demand-start CPPC -> crosvm legacy virtual-cpufreq bridge.
 * Not installed by default. Each request must execute on its own guest CPU:
 * the MMIO trap updates the calling crosvm vCPU thread, not an arbitrary thread.
 * Only the discovered legacy protocol is supported. Stop restores zero uclamp
 * minimum (the measured pre-experiment default); no affinity is changed on Android.
 */
#include <ntddk.h>
#include "hw_abi.h"

NTSYSAPI NTSTATUS NTAPI ZwWaitForSingleObject(HANDLE, BOOLEAN, PLARGE_INTEGER);
NTSYSAPI NTSTATUS NTAPI ZwQuerySystemInformation(ULONG, PVOID, ULONG, PULONG);
struct cpu_times { LARGE_INTEGER idle, kernel, user, reserved[2]; ULONG reserved2; };
NTKERNELAPI KAFFINITY NTAPI KeSetSystemAffinityThreadEx(KAFFINITY);
NTKERNELAPI VOID NTAPI KeRevertToUserAffinityThreadEx(KAFFINITY);

static struct {
    volatile LONG stop;
    ULONG count;
    HANDLE threads[HW_CPPC_MAX_CPUS], controller;
    KEVENT work[HW_CPPC_MAX_CPUS], stop_event;
    volatile ULONG target[HW_CPPC_MAX_CPUS];
    struct cpu_times times[HW_CPPC_MAX_CPUS], previous_times[HW_CPPC_MAX_CPUS];
    ULONG max_khz[HW_CPPC_MAX_CPUS], capacity[HW_CPPC_MAX_CPUS];
    volatile UCHAR *cppc, *mmio;
    SIZE_T mmio_size;
} g;

static void write_request(ULONG cpu, ULONG khz)
{
    *(volatile ULONG *)(g.mmio + 8u * cpu) = khz;
    __asm__ volatile("dsb sy" ::: "memory");
    struct hw_cppc_bridge_diag *d = (struct hw_cppc_bridge_diag *)(g.cppc + HW_CPPC_BRIDGE_OFF);
    d->khz[cpu] = khz;
    d->writes[cpu]++;
    /* This is the backend's configured-core frequency estimate, NOT a measured
     * delivered-performance counter for a migrating vCPU. Never feed it to _CPC. */
    d->reported_khz[cpu] = *(volatile ULONG *)(g.mmio + 8u * cpu);
}

static void worker(PVOID arg)
{
    ULONG cpu = (ULONG)(ULONG_PTR)arg;
    KAFFINITY old = KeSetSystemAffinityThreadEx((KAFFINITY)1 << cpu);
    while (!g.stop) {
        KeWaitForSingleObject(&g.work[cpu], Executive, KernelMode, FALSE, NULL);
        if (g.stop) break;
        write_request(cpu, g.target[cpu]);
    }
    write_request(cpu, 0);
    KeRevertToUserAffinityThreadEx(old);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* One accounting timer replaces eight independent periodic wakeups. Guest idle
 * accounting is used only to RELEASE requests on idle CPUs, never to invent
 * delivered-performance feedback. Missing/bad accounting fails to zero boost. */
static void controller(PVOID arg)
{
    (void)arg;
    BOOLEAN initialized = FALSE, busy[HW_CPPC_MAX_CPUS] = {0};
    ULONGLONG quiet[HW_CPPC_MAX_CPUS] = {0};
    struct hw_cppc_bridge_diag *d = (struct hw_cppc_bridge_diag *)(g.cppc + HW_CPPC_BRIDGE_OFF);
    while (!g.stop) {
        ULONG bytes = 0;
        NTSTATUS st = ZwQuerySystemInformation(8, g.times, sizeof g.times, &bytes);
        BOOLEAN valid = NT_SUCCESS(st) && bytes == g.count * sizeof(struct cpu_times);
        BOOLEAN any_busy = FALSE;
        if (!valid) d->accounting_errors++;
        for (ULONG cpu = 0; cpu < g.count; ++cpu) {
            ULONG load = ~0u, target = 0;
            ULONGLONG total = (ULONGLONG)g.times[cpu].kernel.QuadPart + g.times[cpu].user.QuadPart;
            ULONGLONG prev = (ULONGLONG)g.previous_times[cpu].kernel.QuadPart + g.previous_times[cpu].user.QuadPart;
            ULONGLONG idle = g.times[cpu].idle.QuadPart, old_idle = g.previous_times[cpu].idle.QuadPart;
            if (valid && initialized && total > prev && idle >= old_idle && idle - old_idle <= total - prev) {
                load = (ULONG)((total - prev - (idle - old_idle)) * 1000u / (total - prev));
                if (load <= 50) {
                    quiet[cpu] += total - prev;
                    if (quiet[cpu] >= 2000000u) busy[cpu] = FALSE; /* 200 ms idle debounce */
                } else {
                    quiet[cpu] = 0;
                    if (load >= 100) busy[cpu] = TRUE;
                }
                ULONGLONG perf = *(volatile ULONGLONG *)(g.cppc + 24u + 16u * cpu);
                if (busy[cpu] && perf && perf <= g.capacity[cpu])
                    target = (ULONG)(perf * g.max_khz[cpu] / g.capacity[cpu]);
            } else if (valid && initialized && total == prev && idle == old_idle) {
                /* No new accounting tick: preserve the last decision. */
                load = d->load_permille[cpu];
                target = g.target[cpu] == ~0u ? 0 : g.target[cpu];
            } else { busy[cpu] = FALSE; quiet[cpu] = 0; }
            d->load_permille[cpu] = load;
            any_busy |= busy[cpu];
            if (target != g.target[cpu]) {
                g.target[cpu] = target;
                KeSetEvent(&g.work[cpu], IO_NO_INCREMENT, FALSE);
            }
        }
        if (valid) RtlCopyMemory(g.previous_times, g.times, sizeof g.times);
        initialized = valid;
        d->samples++;
        LARGE_INTEGER delay; delay.QuadPart = any_busy ? -200000 : -1000000;
        KeWaitForSingleObject(&g.stop_event, Executive, KernelMode, FALSE, &delay);
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static void unload(PDRIVER_OBJECT driver)
{
    (void)driver;
    g.stop = 1;
    KeSetEvent(&g.stop_event, IO_NO_INCREMENT, FALSE);
    for (ULONG i = 0; i < g.count; ++i) KeSetEvent(&g.work[i], IO_NO_INCREMENT, FALSE);
    if (g.controller) { ZwWaitForSingleObject(g.controller, FALSE, NULL); ZwClose(g.controller); g.controller = NULL; }
    for (ULONG i = 0; i < g.count; ++i) if (g.threads[i]) {
        ZwWaitForSingleObject(g.threads[i], FALSE, NULL);
        ZwClose(g.threads[i]); g.threads[i] = NULL;
    }
    if (g.cppc) {
        ((struct hw_cppc_bridge_diag *)(g.cppc + HW_CPPC_BRIDGE_OFF))->magic = 0;
        MmUnmapIoSpace((PVOID)g.cppc, PAGE_SIZE); g.cppc = NULL;
    }
    if (g.mmio) { MmUnmapIoSpace((PVOID)g.mmio, g.mmio_size); g.mmio = NULL; }
}

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING path)
{
    (void)path;
    KeInitializeEvent(&g.stop_event, NotificationEvent, FALSE);
    PHYSICAL_ADDRESS pa; pa.QuadPart = HW_WINDOW_PA;
    struct hw_header *h = MmMapIoSpace(pa, PAGE_SIZE, MmCached);
    if (!h) return STATUS_INSUFFICIENT_RESOURCES;
    ULONG count = h->cpu_topo_count;
    ULONGLONG base = h->virt_cpufreq_base;
    NTSTATUS st = STATUS_NOT_SUPPORTED;
    if (h->magic[0] != 'H' || h->magic[1] != 'Y' || h->header_bytes < sizeof *h ||
        h->virt_cpufreq_proto != 1 || !base || (base & 3) || count < 2 || count > HW_CPPC_MAX_CPUS ||
        count != KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS) || h->virt_cpufreq_size != count * 8u) goto out;
    for (ULONG i = 0; i < count; ++i) {
        if (!h->cpu_capacity[i] || !h->cpu_max_mhz[i]) goto out;
        g.max_khz[i] = (ULONG)h->cpu_max_mhz[i] * 1000u;
        g.capacity[i] = h->cpu_capacity[i];
    }
    g.count = count; g.mmio_size = h->virt_cpufreq_size;
    for (ULONG i = 0; i < count; ++i) {
        KeInitializeEvent(&g.work[i], SynchronizationEvent, FALSE);
        g.target[i] = ~0u; /* First accounting sample explicitly releases every floor. */
    }
    pa.QuadPart = base;
    g.mmio = MmMapIoSpace(pa, g.mmio_size, MmNonCached);
    pa.QuadPart = HW_WINDOW_PA + HW_CPPC;
    g.cppc = MmMapIoSpace(pa, PAGE_SIZE, MmCached);
    if (!g.mmio || !g.cppc) { st = STATUS_INSUFFICIENT_RESOURCES; goto out; }
    struct hw_cppc_bridge_diag *d = (struct hw_cppc_bridge_diag *)(g.cppc + HW_CPPC_BRIDGE_OFF);
    RtlZeroMemory(d, sizeof *d); d->count = count; d->magic = HW_CPPC_BRIDGE_MAGIC;
    OBJECT_ATTRIBUTES attrs;
    InitializeObjectAttributes(&attrs, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    for (ULONG i = 0; i < count; ++i) {
        st = PsCreateSystemThread(&g.threads[i], SYNCHRONIZE, &attrs, NULL, NULL, worker, (PVOID)(ULONG_PTR)i);
        if (!NT_SUCCESS(st)) goto out;
    }
    st = PsCreateSystemThread(&g.controller, SYNCHRONIZE, &attrs, NULL, NULL, controller, NULL);
    if (!NT_SUCCESS(st)) goto out;
    driver->DriverUnload = unload;
    st = STATUS_SUCCESS;
out:
    MmUnmapIoSpace(h, PAGE_SIZE);
    if (!NT_SUCCESS(st)) unload(driver);
    return st;
}
