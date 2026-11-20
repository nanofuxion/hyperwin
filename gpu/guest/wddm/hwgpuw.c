/* HyperWin GPU, WDDM variant (wddm/README.md): a render-only display miniport. Windows sees one real
 * graphics adapter, "HyperWin GPU (<the phone's GPU>)". Nothing is drawn here. The adapter exists so
 * that Windows lists it, so that programs find the user-mode drivers (VulkanDriverName and friends)
 * through it, and so that each program can ask the kernel for a ring channel to the phone.
 *
 *   - D3DKMTEscape (include/hw_wddm.h) claims a channel and maps it, and the shared GPU heap ranges the
 *     phone grants it, into the calling process. After that, requests are plain memory writes.
 *   - The scheduler is not in the way: a submission carries at most a marker (a fence the phone is
 *     writing into the channel's control page), polled with a high-resolution timer only while some
 *     are pending, and reported to Windows with DxgkCbNotifyInterrupt.
 *   - A restarted renderer (a driver swap) shows as a new epoch in the ring header. Markers of the old
 *     one are never completed, so Windows' timeout detection resets the adapter.
 *
 * DxgkInitialize is ours: displib.lib is not linked (see wddm/README.md, "Phase 1 findings").
 * The WDK headers are included from gpu/work/wdk at build time and nothing of them is kept. */
#include <ntifs.h>
#include <dispmprt.h>

#include "hw_abi.h"
#define HWGPU_KERNEL
#include "hwgpu_ring.h"
#include "hw_wddm.h"

#define TAG 'wGWH'
#ifndef MdlMappingNoExecute
#define MdlMappingNoExecute 0x40000000
#endif

/* ---- state ---- */

#define MAX_VIEWS 128u
#define MAX_PENDING 256u
#define NO_MARKER 0xFFFFFFFFu   /* a queued buffer with nothing on the phone to wait for */

struct view {
    PMDL mdl;
    PVOID user;
    uint64_t offset, size;
};

/* One channel a process opened. */
struct chan_ctx {
    LIST_ENTRY link;
    PEPROCESS process;     /* referenced */
    HANDLE pid;
    int slot;
    uint32_t generation;
    PMDL mdl;
    PVOID user;
    struct view views[MAX_VIEWS];
};

struct pending {
    BOOLEAN used;
    UINT fence_id, node, engine;
    uint32_t slot, generation, ring;
    uint64_t fence, epoch;
};

struct adapter {
    PDEVICE_OBJECT pdo;
    DXGKRNL_INTERFACE dxgk;
    PVOID ring;            /* kernel mapping of the ring area, offset HWGPU_RING_OFF in the window */
    PVOID heap;            /* kernel mapping of the shared heap */
    FAST_MUTEX lock;
    LIST_ENTRY chans;
    KSPIN_LOCK pending_lock;
    struct pending pend[MAX_PENDING];
    UINT pend_count;
    UINT pend_head;        /* pend is a queue in submission order: fences finish in the order given */
    PEX_TIMER timer;
    BOOLEAN timer_on;
    uint64_t epoch;        /* the renderer's, as the adapter last saw it */
    volatile LONG completing;
    UINT last_fence;
    struct hww_counters counters;
    WCHAR guard_path[260];
};

static struct adapter g;
static BOOLEAN g_notify_set;
static ULONG g_sync_fallbacks;

/* Interrupt notifications must run where the interrupt routine would. There is no interrupt line, and
 * with the screen half on DxgkCbSynchronizeExecution refuses (STATUS_UNSUCCESSFUL) without calling the
 * routine, so then it runs under this driver's own lock at DISPATCH_LEVEL instead. */
static KSPIN_LOCK g_isr_lock;
static void run_synced(PKSYNCHRONIZE_ROUTINE routine, PVOID context)
{
    BOOLEAN ret;
    if (NT_SUCCESS(g.dxgk.DxgkCbSynchronizeExecution(g.dxgk.DeviceHandle, routine, context, 0, &ret))) return;
    g_sync_fallbacks++;
    KIRQL irql;
    KeAcquireSpinLock(&g_isr_lock, &irql);
    routine(context);
    KeReleaseSpinLock(&g_isr_lock, irql);
}

#include "hwdisplay.inc"

static BOOLEAN g_w2;   /* Parameters\\Wddm2 */
static struct { ULONG submits, last_submit, last_flags, resets, preempts, sync_runs; } g_diag;

static void *window(void) { return (UCHAR *)g.ring - HWGPU_RING_OFF; }

/* ---- the crash-loop breaker ---- */

/* A miniport that bugchecks while it starts would stop Windows booting. DriverEntry counts attempts in
 * the service's Parameters key and refuses to load when two went wrong in a row; the count is cleared
 * once the adapter has been up for ten seconds, or stops cleanly. */
static ULONG guard_count(BOOLEAN write, ULONG value)
{
    HANDLE key = NULL;
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING path, name;
    ULONG got = 0;
    RtlInitUnicodeString(&path, g.guard_path);
    InitializeObjectAttributes(&oa, &path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    ULONG disp;
    if (!NT_SUCCESS(ZwCreateKey(&key, KEY_READ | KEY_WRITE, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, &disp))) return 0;
    RtlInitUnicodeString(&name, L"Attempts");
    if (write) {
        ZwSetValueKey(key, &name, 0, REG_DWORD, &value, sizeof value);
        ZwFlushKey(key);   /* a crash right after DriverEntry must still leave the count */
    } else {
        UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 4];
        ULONG len = 0;
        if (NT_SUCCESS(ZwQueryValueKey(key, &name, KeyValuePartialInformation, buf, sizeof buf, &len)) &&
            ((PKEY_VALUE_PARTIAL_INFORMATION)buf)->DataLength == 4)
            got = *(ULONG *)((PKEY_VALUE_PARTIAL_INFORMATION)buf)->Data;
    }
    ZwClose(key);
    return got;
}

/* A status for whoever reads the service's Parameters key: what the last start found. */
static void report(const WCHAR *what, ULONG value)
{
    HANDLE key = NULL;
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING path, name;
    ULONG disp;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;   /* flips can come at interrupt level: no registry there */
    RtlInitUnicodeString(&path, g.guard_path);
    InitializeObjectAttributes(&oa, &path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateKey(&key, KEY_READ | KEY_WRITE, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, &disp))) return;
    RtlInitUnicodeString(&name, (PWSTR)what);
    ZwSetValueKey(key, &name, 0, REG_DWORD, &value, sizeof value);
    ZwFlushKey(key);   /* a crash right after must not lose it */
    ZwClose(key);
}

/* Where dxgkrnl and its memory managers sit this boot, as B_<name>_Hi/Lo, so a stop code's address can be
 * turned into an offset inside the module even when no dump gets written. */
NTSYSAPI NTSTATUS NTAPI ZwQuerySystemInformation(ULONG cls, PVOID buf, ULONG len, PULONG ret);
static BOOLEAN g_mms1_seen;
static void report_bases(void)
{
    if (g_mms1_seen || KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    struct mod { HANDLE section; PVOID mapped, base; ULONG size, flags; USHORT order, init, count, name_at; UCHAR path[256]; };
    ULONG len = 0;
    ZwQuerySystemInformation(11, NULL, 0, &len);
    if (!len) return;
    len += 4096;
    ULONG *buf = ExAllocatePool2(POOL_FLAG_PAGED, len, 'bwgH');
    if (!buf) return;
    if (NT_SUCCESS(ZwQuerySystemInformation(11, buf, len, &len))) {
        struct mod *m = (struct mod *)(buf + 2);
        static const char *want[] = {"dxgkrnl.sys", "dxgmms1.sys", "dxgmms2.sys", "hwgpuw.sys"};
        for (ULONG i = 0; i < buf[0]; i++) {
            const char *n = (const char *)m[i].path + m[i].name_at;
            for (int w = 0; w < 4; w++) {
                const char *a = n, *b = want[w];
                while (*a && (*a | 0x20) == (*b | 0x20)) a++, b++;
                if (*a || *b) continue;
                WCHAR v[24] = L"B_";
                UINT k = 2;
                for (const char *c = want[w]; *c != '.'; c++) v[k++] = (WCHAR)*c;
                v[k++] = L'_';
                v[k + 2] = 0;
                v[k] = L'H', v[k + 1] = L'i';
                report(v, (ULONG)((ULONG64)m[i].base >> 32));
                v[k] = L'L', v[k + 1] = L'o';
                report(v, (ULONG)(ULONG64)m[i].base);
                if (w == 1) g_mms1_seen = TRUE;
            }
        }
    }
    ExFreePoolWithTag(buf, 'bwgH');
}

/* Bring-up trace, built with -DHWW_TRACE: the names of the driver entries dxgkrnl calls, in order, kept in the
 * service's Parameters key as Trace2. */
#ifdef HWW_TRACE
static CHAR g_tr[3000];
static LONG g_trn;
static void tr(const char *name)
{
    if (!strncmp(name, "HwdPresent", 10) || !strncmp(name, "HwdDescribeA", 12) || !strncmp(name, "StdData", 7) || !strncmp(name, "CreateDevice", 12) || !strncmp(name, "DestroyDevice", 13)) return;   /* too frequent */
    LONG at = InterlockedExchangeAdd(&g_trn, 0);
    if (at > (LONG)sizeof g_tr - 64) {   /* keep the last calls: drop the oldest half */
        RtlMoveMemory(g_tr, g_tr + 1500, (SIZE_T)(at - 1500));
        at -= 1500;
    }
    for (ULONG i = 0; name[i] && at < (LONG)sizeof g_tr - 2; i++) g_tr[at++] = name[i];
    g_tr[at++] = ' ';
    g_tr[at] = 0;
    InterlockedExchange(&g_trn, at);
    if (KeGetCurrentIrql() > APC_LEVEL) return;
    HANDLE key = NULL;
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING path, vname;
    ULONG disp;
    RtlInitUnicodeString(&path, g.guard_path);
    InitializeObjectAttributes(&oa, &path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateKey(&key, KEY_READ | KEY_WRITE, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, &disp))) return;
    RtlInitUnicodeString(&vname, L"Trace2");
    ZwSetValueKey(key, &vname, 0, REG_BINARY, g_tr, (ULONG)at + 1);
    ZwClose(key);
}
#else
#define tr(name) ((void)0)
#endif

/* Ten seconds after a good start the count goes back to zero, two minutes with the screen half on (its
 * failures come when the desktop starts, well after the adapter). A thread does it, because a timer
 * callback may not touch the registry; it ends early when the device stops. */
static KEVENT g_guard_stop;
static PETHREAD g_guard_thread;

static BOOLEAN marker_ready(const struct pending *p, uint64_t epoch_now);

/* With the screen half on, the queue's state goes into Parameters as T_<name> every few seconds, so a
 * stall can be read from outside while Windows waits. */
static void diag_write(void)
{
    KIRQL irql;
    struct pending h;
    RtlZeroMemory(&h, sizeof h);
    KeAcquireSpinLock(&g.pending_lock, &irql);
    UINT n = g.pend_count;
    if (n) h = g.pend[g.pend_head];
    KeReleaseSpinLock(&g.pending_lock, irql);
    report(L"T_Pending", n);
    report(L"T_HeadFenceId", h.fence_id);
    report(L"T_HeadSlot", h.slot);
    report(L"T_HeadWant", (ULONG)h.fence);
    report(L"T_HeadEpochOk", h.epoch == g.epoch);
    if (n && h.slot != NO_MARKER && hwgpu_ring_valid(window())) {
        struct hwgpu_ctl *c = hwgpu_chan_ctl((UCHAR *)window() + hwgpu_chan_off(h.slot));
        report(L"T_HeadHave", (ULONG)c->fence[h.ring % HWGPU_FENCE_RINGS]);
        report(L"T_HeadGenOk", c->generation == h.generation);
        report(L"T_HeadState", c->guest_state);
        report(L"T_HeadReady", marker_ready(&h, g.epoch));
    }
    report(L"T_LastFence", g.last_fence);
    report(L"T_Submits", g_diag.submits);
    report(L"T_LastSubmit", g_diag.last_submit);
    report(L"T_LastFlags", g_diag.last_flags);
    report(L"T_Preempts", g_diag.preempts);
    report(L"T_TimerOn", g.timer_on);
    report(L"T_SyncRuns", g_diag.sync_runs);
    report(L"T_SyncFallbacks", g_sync_fallbacks);
    report(L"T_Vsyncs", d.vsyncs);
    report(L"T_Presents", d.presents);
    report(L"T_SourceSets", d.source_sets);
    for (int i = 0; i < 4; i++) {
        WCHAR n[16] = L"T_Addr0";
        n[6] = L'0' + i;
        report(n, (ULONG)d.last_addr[i]);
    }
    report(L"T_SetFlags", d.last_flags);
    report(L"T_SetCtx", d.last_context_count);
    report(L"T_SetAllocOk", d.last_alloc_ok);
}

static VOID NTAPI guard_thread(PVOID context)
{
    (void)context;
    LONGLONG left = d.on ? 1200000000LL : 100000000LL;
    for (;;) {
        LARGE_INTEGER wait;
        wait.QuadPart = d.on ? -30000000LL : -left;
        if (KeWaitForSingleObject(&g_guard_stop, Executive, KernelMode, FALSE, &wait) != STATUS_TIMEOUT) break;
        if (d.on) diag_write();
        left += wait.QuadPart;
        if (left <= 0) {
            if (left > wait.QuadPart) guard_count(TRUE, 0);   /* only the first time past the mark */
            if (!d.on) break;
        }
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static void guard_start(void)
{
    HANDLE th = NULL;
    KeInitializeEvent(&g_guard_stop, NotificationEvent, FALSE);
    if (NT_SUCCESS(PsCreateSystemThread(&th, THREAD_ALL_ACCESS, NULL, NULL, NULL, guard_thread, NULL))) {
        ObReferenceObjectByHandle(th, THREAD_ALL_ACCESS, NULL, KernelMode, (PVOID *)&g_guard_thread, NULL);
        ZwClose(th);
    }
}

static void guard_stop(void)
{
    if (!g_guard_thread) return;
    KeSetEvent(&g_guard_stop, IO_NO_INCREMENT, FALSE);
    KeWaitForSingleObject(g_guard_thread, Executive, KernelMode, FALSE, NULL);
    ObDereferenceObject(g_guard_thread);
    g_guard_thread = NULL;
    /* A start that reaches a stop did not crash, however soon after it began. */
    guard_count(TRUE, 0);
}

/* ---- mapping into a process ---- */

static PVOID map_user(PVOID kernel, uint64_t size, PMDL *out)
{
    PMDL mdl = IoAllocateMdl(kernel, (ULONG)size, FALSE, FALSE, NULL);
    if (!mdl) return NULL;
    MmBuildMdlForNonPagedPool(mdl);
    PVOID user = NULL;
    __try {
        user = MmMapLockedPagesSpecifyCache(mdl, UserMode, MmCached, NULL, FALSE, NormalPagePriority | MdlMappingNoExecute);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        user = NULL;
    }
    if (!user) {
        IoFreeMdl(mdl);
        return NULL;
    }
    *out = mdl;
    return user;
}

/* Unmaps in the owning process: from its own thread, or attached to it. */
static void unmap_in_process(struct chan_ctx *c, PVOID user, PMDL mdl)
{
    if (user) {
        KAPC_STATE apc;
        BOOLEAN attached = FALSE;
        if (PsGetCurrentProcess() != c->process) {
            KeStackAttachProcess(c->process, &apc);
            attached = TRUE;
        }
        MmUnmapLockedPages(user, mdl);
        if (attached) KeUnstackDetachProcess(&apc);
    }
    if (mdl) IoFreeMdl(mdl);
}

static void free_chan(struct chan_ctx *c)
{
    {
        struct hwgpu_ctl *ctl = hwgpu_chan_ctl((UCHAR *)window() + hwgpu_chan_off((uint32_t)c->slot));
        g.counters.submits_to_phone += __atomic_load_n(&ctl->submits_to_phone, __ATOMIC_RELAXED);
        g.counters.submits_to_guest += __atomic_load_n(&ctl->submits_to_guest, __ATOMIC_RELAXED);
    }
    /* Heap views first: the phone may hand their memory to someone else once the channel is free. */
    for (UINT i = 0; i < MAX_VIEWS; i++) {
        if (c->views[i].user) {
            g.counters.mapped_bytes -= c->views[i].size;
            unmap_in_process(c, c->views[i].user, c->views[i].mdl);
        }
    }
    unmap_in_process(c, c->user, c->mdl);
    g.counters.mapped_bytes -= HWGPU_CHAN_STRIDE;
    hwgpu_release(window(), (uint32_t)c->slot);
    ObDereferenceObject(c->process);
    ExFreePoolWithTag(c, TAG);
}

static struct chan_ctx *find_chan(int slot)
{
    for (PLIST_ENTRY e = g.chans.Flink; e != &g.chans; e = e->Flink) {
        struct chan_ctx *c = CONTAINING_RECORD(e, struct chan_ctx, link);
        if (c->slot == slot && c->process == PsGetCurrentProcess()) return c;
    }
    return NULL;
}

/* A process is going away: its channels and mappings go with it. This runs in that process, as
 * Windows tears down its last thread, while its address space still exists. */
static volatile LONG g_exits;
static VOID NTAPI process_notify(HANDLE parent, HANDLE pid, BOOLEAN create)
{
    (void)parent;
    if (create) return;
    g_exits++;
    LIST_ENTRY dead;
    InitializeListHead(&dead);
    ExAcquireFastMutex(&g.lock);
    for (PLIST_ENTRY e = g.chans.Flink, n; e != &g.chans; e = n) {
        n = e->Flink;
        struct chan_ctx *c = CONTAINING_RECORD(e, struct chan_ctx, link);
        if (c->pid == pid) {
            RemoveEntryList(e);
            InsertTailList(&dead, e);
        }
    }
    for (PLIST_ENTRY e; !IsListEmpty(&dead);) {
        e = RemoveHeadList(&dead);
        free_chan(CONTAINING_RECORD(e, struct chan_ctx, link));
    }
    ExReleaseFastMutex(&g.lock);
}

/* ---- the renderer's epoch ---- */

/* The phone's renderer writes a new epoch into the ring header each time it starts. A different one
 * than the adapter last saw means a driver swap restarted it: every object on the phone is gone. Markers
 * made under the old epoch are never completed (marker_ready), so Windows' timeout detection resets the
 * adapter, DWM rebuilds, and programs see device-lost. Under g.lock or the poll timer. */
static void check_epoch(void)
{
    if (!g.ring || !hwgpu_ring_valid(window())) return;
    uint64_t now = __atomic_load_n(&hwgpu_hdr(window())->epoch, __ATOMIC_ACQUIRE);
    if (g.epoch && now && now != g.epoch) g.counters.resets++;
    if (now) g.epoch = now;
}

/* ---- escapes ---- */

static NTSTATUS esc_open(struct hww_open *out)
{
    if (!g.ring) return STATUS_DEVICE_NOT_READY;
    if (!hwgpu_ring_valid(window())) return STATUS_DEVICE_NOT_READY;  /* the phone has no GPU server */
    struct chan_ctx *c = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof *c, TAG);
    if (!c) return STATUS_INSUFFICIENT_RESOURCES;
    uint32_t gen = 0;
    int slot = hwgpu_claim(window(), &gen);
    if (slot < 0) {
        ExFreePoolWithTag(c, TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    PVOID chan = (UCHAR *)window() + hwgpu_chan_off((uint32_t)slot);
    PMDL mdl = NULL;
    PVOID user = map_user(chan, HWGPU_CHAN_STRIDE, &mdl);
    if (!user) {
        hwgpu_release(window(), (uint32_t)slot);
        ExFreePoolWithTag(c, TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    c->process = PsGetCurrentProcess();
    ObReferenceObject(c->process);
    c->pid = PsGetCurrentProcessId();
    c->slot = slot;
    c->generation = gen;
    c->mdl = mdl;
    c->user = user;
    InsertTailList(&g.chans, &c->link);
    g.counters.channels_opened++;
    g.counters.mapped_bytes += HWGPU_CHAN_STRIDE;
    out->chan = (uint64_t)(ULONG_PTR)user;
    out->slot = (uint32_t)slot;
    out->generation = gen;
    return STATUS_SUCCESS;
}

static NTSTATUS esc_close(uint32_t slot)
{
    struct chan_ctx *c = find_chan((int)slot);
    if (!c) return STATUS_INVALID_PARAMETER;
    RemoveEntryList(&c->link);
    free_chan(c);
    return STATUS_SUCCESS;
}

static NTSTATUS esc_map(struct hww_map *m)
{
    struct chan_ctx *c = find_chan((int)m->slot);
    if (!c || !g.heap) return STATUS_INVALID_PARAMETER;
    if (!m->size || ((m->offset | m->size) & 0xfff) || m->offset > HW_HEAP_SIZE || m->size > HW_HEAP_SIZE - m->offset ||
        m->size > 0xfffff000ull)
        return STATUS_INVALID_PARAMETER;
    /* Only a range the phone granted this channel, matched exactly. */
    struct hwgpu_ctl *ctl = hwgpu_chan_ctl((UCHAR *)window() + hwgpu_chan_off((uint32_t)c->slot));
    BOOLEAN granted = FALSE;
    for (uint32_t i = 0; i < HWGPU_GRANTS; i++) {
        uint64_t sz = __atomic_load_n(&ctl->grant[i].size, __ATOMIC_ACQUIRE);
        if (sz && sz == m->size && ctl->grant[i].offset == m->offset) granted = TRUE;
    }
    if (!granted) return STATUS_ACCESS_DENIED;
    struct view *v = NULL;
    for (UINT i = 0; i < MAX_VIEWS && !v; i++)
        if (!c->views[i].user) v = &c->views[i];
    if (!v) return STATUS_INSUFFICIENT_RESOURCES;
    PMDL mdl = NULL;
    PVOID user = map_user((UCHAR *)g.heap + m->offset, m->size, &mdl);
    if (!user) return STATUS_INSUFFICIENT_RESOURCES;
    v->mdl = mdl;
    v->user = user;
    v->offset = m->offset;
    v->size = m->size;
    g.counters.mapped_bytes += m->size;
    m->user = (uint64_t)(ULONG_PTR)user;
    return STATUS_SUCCESS;
}

static NTSTATUS esc_unmap(struct hww_map *m)
{
    struct chan_ctx *c = find_chan((int)m->slot);
    if (!c) return STATUS_INVALID_PARAMETER;
    for (UINT i = 0; i < MAX_VIEWS; i++) {
        if (c->views[i].user && (uint64_t)(ULONG_PTR)c->views[i].user == m->user) {
            g.counters.mapped_bytes -= c->views[i].size;
            unmap_in_process(c, c->views[i].user, c->views[i].mdl);
            RtlZeroMemory(&c->views[i], sizeof c->views[i]);
            return STATUS_SUCCESS;
        }
    }
    return STATUS_INVALID_PARAMETER;
}

static NTSTATUS NTAPI HwEscape(const HANDLE hAdapter, const DXGKARG_ESCAPE *arg)
{
    tr("Escape");
    (void)hAdapter;
    if (!arg->pPrivateDriverData || arg->PrivateDriverDataSize < sizeof(struct hww_escape)) return STATUS_BUFFER_TOO_SMALL;
    struct hww_escape *e = arg->pPrivateDriverData;
    /* A refusal is reported in the buffer, with codes that say why, so a wrong caller can tell. */
    if (e->magic != HWW_MAGIC) {
        e->status = (int32_t)0xc0de0001;
        return STATUS_SUCCESS;
    }
    if (e->size != sizeof *e) {
        e->status = (int32_t)0xc0de0002;
        return STATUS_SUCCESS;
    }
    /* Mappings go into the calling process, so the call has to come from it. */
    if (ExGetPreviousMode() != UserMode) {
        e->status = (int32_t)0xc0de0003;
        return STATUS_SUCCESS;
    }
    NTSTATUS st = STATUS_INVALID_PARAMETER;
    ExAcquireFastMutex(&g.lock);
    g.counters.escapes++;
    check_epoch();
    switch (e->op) {
    case HWW_ESC_INFO: {
        struct hww_hdr_view { const struct hwgpu_ring_hdr *h; } v = {g.ring ? hwgpu_hdr(window()) : NULL};
        RtlZeroMemory(&e->u, sizeof e->u);
        e->u.info.driver_version = HWW_VERSION;
        e->u.info.channels = HWGPU_CHANNELS;
        e->u.info.heap_size = g.heap ? HW_HEAP_SIZE : 0;
        if (v.h && hwgpu_ring_valid(window())) {
            e->u.info.epoch = __atomic_load_n(&v.h->epoch, __ATOMIC_ACQUIRE);
            RtlCopyMemory(e->u.info.gpu_name, v.h->gpu_name, sizeof e->u.info.gpu_name - 1);
        }
        st = STATUS_SUCCESS;
        break;
    }
    case HWW_ESC_OPEN:
        RtlZeroMemory(&e->u, sizeof e->u);
        st = esc_open(&e->u.open);
        break;
    case HWW_ESC_CLOSE:
        st = esc_close(e->u.slot);
        break;
    case HWW_ESC_MAP:
        st = esc_map(&e->u.map);
        break;
    case HWW_ESC_UNMAP:
        st = esc_unmap(&e->u.map);
        break;
    case HWW_ESC_CHANNEL: {
        uint32_t want = e->u.chan.index, at = 0;
        st = STATUS_NO_MORE_ENTRIES;
        for (PLIST_ENTRY l = g.chans.Flink; l != &g.chans; l = l->Flink, at++) {
            struct chan_ctx *c = CONTAINING_RECORD(l, struct chan_ctx, link);
            if (at != want) continue;
            struct hwgpu_ctl *ctl = hwgpu_chan_ctl((UCHAR *)window() + hwgpu_chan_off((uint32_t)c->slot));
            e->u.chan.slot = (uint32_t)c->slot;
            e->u.chan.pid = (uint32_t)(ULONG_PTR)c->pid;
            e->u.chan.submits_to_phone = __atomic_load_n(&ctl->submits_to_phone, __ATOMIC_RELAXED);
            e->u.chan.submits_to_guest = __atomic_load_n(&ctl->submits_to_guest, __ATOMIC_RELAXED);
            st = STATUS_SUCCESS;
            break;
        }
        break;
    }
    case HWW_ESC_COUNTERS: {
        struct hww_counters k = g.counters;
        if (g.ring && hwgpu_ring_valid(window())) {
            k.phone_copy_to_guest = __atomic_load_n(&hwgpu_hdr(window())->copy_to_guest, __ATOMIC_RELAXED);
            k.phone_copy_from_guest = __atomic_load_n(&hwgpu_hdr(window())->copy_from_guest, __ATOMIC_RELAXED);
        }
        k.markers_pending = g.pend_count;
        /* The closed channels are in g.counters already; add the open ones. */
        for (PLIST_ENTRY l = g.chans.Flink; l != &g.chans; l = l->Flink) {
            struct chan_ctx *c = CONTAINING_RECORD(l, struct chan_ctx, link);
            struct hwgpu_ctl *ctl = hwgpu_chan_ctl((UCHAR *)window() + hwgpu_chan_off((uint32_t)c->slot));
            k.submits_to_phone += __atomic_load_n(&ctl->submits_to_phone, __ATOMIC_RELAXED);
            k.submits_to_guest += __atomic_load_n(&ctl->submits_to_guest, __ATOMIC_RELAXED);
        }
        e->u.counters = k;
        st = STATUS_SUCCESS;
        break;
    }
    }
    ExReleaseFastMutex(&g.lock);
    e->status = st;
    return STATUS_SUCCESS;  /* the call worked; the result is in the buffer */
}

/* ---- markers: fences Windows waits on ---- */

static BOOLEAN NTAPI sync_complete(PVOID context)
{
    struct pending *p = context;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA d;
    RtlZeroMemory(&d, sizeof d);
    d.InterruptType = DXGK_INTERRUPT_DMA_COMPLETED;
    d.DmaCompleted.SubmissionFenceId = p->fence_id;
    d.DmaCompleted.NodeOrdinal = p->node;
    d.DmaCompleted.EngineOrdinal = p->engine;
    g.last_fence = p->fence_id;
    g_diag.sync_runs++;
    g.dxgk.DxgkCbNotifyInterrupt(g.dxgk.DeviceHandle, &d);
    g.dxgk.DxgkCbQueueDpc(g.dxgk.DeviceHandle);
    return TRUE;
}

static void complete_fence(UINT fence_id, UINT node, UINT engine)
{
    struct pending p = {TRUE, fence_id, node, engine};
    run_synced(sync_complete, &p);
    g.counters.markers_done++;
}

static BOOLEAN marker_ready(const struct pending *p, uint64_t epoch_now)
{
    if (p->epoch != epoch_now) return FALSE;  /* the renderer was replaced: never, so Windows resets */
    if (!hwgpu_ring_valid(window())) return FALSE;
    struct hwgpu_ctl *c = hwgpu_chan_ctl((UCHAR *)window() + hwgpu_chan_off(p->slot));
    if (__atomic_load_n(&c->generation, __ATOMIC_ACQUIRE) != p->generation ||
        __atomic_load_n(&c->guest_state, __ATOMIC_ACQUIRE) != HWGPU_CONNECTED)
        return TRUE;  /* the program closed its channel: nothing left to wait for */
    return __atomic_load_n(&c->fence[p->ring % HWGPU_FENCE_RINGS], __ATOMIC_ACQUIRE) >= p->fence;
}

static VOID NTAPI poll_timer(PEX_TIMER timer, PVOID context)
{
    (void)context;
    check_epoch();
    uint64_t epoch_now = g.epoch;
    for (;;) {
        struct pending done = {0};
        KIRQL irql;
        KeAcquireSpinLock(&g.pending_lock, &irql);
        struct pending *head = &g.pend[g.pend_head];
        if (g.pend_count && (head->slot == NO_MARKER || marker_ready(head, epoch_now))) {
            done = *head;
            done.used = TRUE;
            g.pend_head = (g.pend_head + 1) % MAX_PENDING;
            g.pend_count--;
        }
        BOOLEAN more = g.pend_count != 0;
        if (!done.used && !more && g.timer_on) {
            ExSetTimer(timer, 0, 0, NULL);  /* nothing pending: stop polling */
            g.timer_on = FALSE;
        }
        KeReleaseSpinLock(&g.pending_lock, irql);
        if (!done.used) break;
        complete_fence(done.fence_id, done.node, done.engine);
    }
}

static void start_polling(void)
{
    if (g.timer && !g.timer_on) {
        g.timer_on = TRUE;
        LONGLONG due = -10000;  /* 1 ms */
        ExSetTimer(g.timer, due, 10000, NULL);
    }
}

/* ---- the DDIs ---- */

static NTSTATUS NTAPI HwAddDevice(CONST PDEVICE_OBJECT pdo, PVOID *context)
{
    tr("Add");
    g.pdo = pdo;
    *context = &g;
    return STATUS_SUCCESS;
}

static void write_adapter_strings(const char *gpu)
{
    HANDLE key = NULL;
    if (!NT_SUCCESS(IoOpenDeviceRegistryKey(g.pdo, PLUGPLAY_REGKEY_DRIVER, KEY_SET_VALUE, &key))) return;
    WCHAR name[96];
    UNICODE_STRING us;
    ULONG n = 0;
    static const WCHAR prefix[] = L"HyperWin GPU (";
    for (; prefix[n] && n < 80; n++) name[n] = prefix[n];
    for (const char *s = gpu; *s && n < 90; s++) name[n++] = (WCHAR)(UCHAR)*s;
    name[n++] = L')';
    name[n] = 0;
    RtlInitUnicodeString(&us, L"HardwareInformation.AdapterString");
    ZwSetValueKey(key, &us, 0, REG_SZ, name, (n + 1) * sizeof(WCHAR));
    RtlInitUnicodeString(&us, L"HardwareInformation.ChipType");
    ZwSetValueKey(key, &us, 0, REG_SZ, name, (n + 1) * sizeof(WCHAR));
    /* Device Manager and dxdiag show the device's own name, not the adapter string: the same text
     * goes there (the driver description, and the friendly name on the device node). */
    RtlInitUnicodeString(&us, L"DriverDesc");
    ZwSetValueKey(key, &us, 0, REG_SZ, name, (n + 1) * sizeof(WCHAR));
    static const DEVPROPKEY friendly = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
    IoSetDevicePropertyData(g.pdo, &friendly, LOCALE_NEUTRAL, 0, DEVPROP_TYPE_STRING, (n + 1) * sizeof(WCHAR), name);
    static const DEVPROPKEY desc = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 2};
    IoSetDevicePropertyData(g.pdo, &desc, LOCALE_NEUTRAL, 0, DEVPROP_TYPE_STRING, (n + 1) * sizeof(WCHAR), name);
    RtlInitUnicodeString(&us, L"HardwareInformation.DACType");
    static const WCHAR dac[] = L"Phone";
    ZwSetValueKey(key, &us, 0, REG_SZ, (PVOID)dac, sizeof dac);
    RtlInitUnicodeString(&us, L"HardwareInformation.qwMemorySize");
    ULONGLONG mem = HW_HEAP_SIZE;
    ZwSetValueKey(key, &us, 0, REG_QWORD, &mem, sizeof mem);
    ZwClose(key);
}

static NTSTATUS NTAPI HwStartDevice(CONST PVOID ctx, PDXGK_START_INFO info, PDXGKRNL_INTERFACE iface, PULONG sources,
                                    PULONG children)
{
    tr("Start");
    (void)ctx;
    (void)info;
    RtlZeroMemory(&g.dxgk, sizeof g.dxgk);
    RtlCopyMemory(&g.dxgk, iface, min((SIZE_T)iface->Size, sizeof g.dxgk));
    report_bases();
    if (g_w2) {
        DXGK_DEVICE_INFO di;
        RtlZeroMemory(&di, sizeof di);
        NTSTATUS gs = g.dxgk.DxgkCbGetDeviceInformation(g.dxgk.DeviceHandle, &di);
        report(L"DevInfoStatus", (ULONG)gs);
    }
    if (d.on) hwd_size_from_phone();
    *sources = d.on ? 1 : 0;   /* render-only unless Parameters\Display asks for the screen half */
    *children = d.on ? 1 : 0;
    InitializeListHead(&g.chans);
    ExInitializeFastMutex(&g.lock);
    KeInitializeSpinLock(&g.pending_lock);
    KeInitializeSpinLock(&g_isr_lock);
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = (LONGLONG)(HW_WINDOW_PA + HWGPU_RING_OFF);
    g.ring = MmMapIoSpace(pa, HWGPU_RING_BYTES, MmCached);
    pa.QuadPart = (LONGLONG)HW_HEAP_PA;
    g.heap = MmMapIoSpace(pa, (SIZE_T)HW_HEAP_SIZE, MmCached);
    if (!g.ring) return STATUS_INSUFFICIENT_RESOURCES;
    g.epoch = hwgpu_ring_valid(window()) ? hwgpu_hdr(window())->epoch : 0;
    g.timer = ExAllocateTimer(poll_timer, NULL, EX_TIMER_HIGH_RESOLUTION);
    guard_start();
    if (!g_notify_set) {
        NTSTATUS ns = PsSetCreateProcessNotifyRoutine(process_notify, FALSE);
        report(L"ExitHookStatus", (ULONG)ns);
        if (NT_SUCCESS(ns)) g_notify_set = TRUE;
    }
    const char *name = g.ring && hwgpu_ring_valid(window()) && hwgpu_hdr(window())->gpu_name[0] ? hwgpu_hdr(window())->gpu_name : "Phone GPU";
    write_adapter_strings(name);
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwStopDevice(CONST PVOID ctx)
{
    tr("Stop");
    (void)ctx;
    hwd_stop();
    if (g.timer) {
        ExDeleteTimer(g.timer, TRUE, FALSE, NULL);
        g.timer = NULL;
    }
    guard_stop();
    if (g_notify_set) {
        PsSetCreateProcessNotifyRoutine(process_notify, TRUE);
        g_notify_set = FALSE;
    }
    ExAcquireFastMutex(&g.lock);
    while (!IsListEmpty(&g.chans)) {
        PLIST_ENTRY e = RemoveHeadList(&g.chans);
        free_chan(CONTAINING_RECORD(e, struct chan_ctx, link));
    }
    ExReleaseFastMutex(&g.lock);
    if (g.ring) MmUnmapIoSpace(g.ring, HWGPU_RING_BYTES);
    if (g.heap) MmUnmapIoSpace(g.heap, (SIZE_T)HW_HEAP_SIZE);
    g.ring = g.heap = NULL;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwRemoveDevice(CONST PVOID ctx)
{
    tr("Remove");
    (void)ctx;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwDispatchIoRequest(CONST PVOID ctx, ULONG source, PVIDEO_REQUEST_PACKET packet)
{
    tr("Io");
    (void)ctx;
    (void)source;
    (void)packet;
    return STATUS_NOT_SUPPORTED;
}

static BOOLEAN NTAPI HwInterrupt(CONST PVOID ctx, ULONG message)
{
    (void)ctx;
    (void)message;
    return FALSE;  /* no interrupt line: completions come from the polling timer */
}

/* Each interrupt notification (a finished fence, a vsync) is only acted on once the DPC tells dxgkrnl. */
static VOID NTAPI HwDpc(CONST PVOID ctx)
{
    (void)ctx;
    g.dxgk.DxgkCbNotifyDpc(g.dxgk.DeviceHandle);
}

static NTSTATUS NTAPI HwQueryChildRelations(CONST PVOID ctx, PDXGK_CHILD_DESCRIPTOR rel, ULONG size)
{
    tr("ChildRel");
    (void)ctx;
    (void)rel;
    (void)size;
    return STATUS_SUCCESS;  /* none */
}

static NTSTATUS NTAPI HwQueryChildStatus(CONST PVOID ctx, PDXGK_CHILD_STATUS status, BOOLEAN safe)
{
    (void)ctx;
    (void)status;
    (void)safe;
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS NTAPI HwQueryDeviceDescriptor(CONST PVOID ctx, ULONG uid, PDXGK_DEVICE_DESCRIPTOR desc)
{
    (void)ctx;
    (void)uid;
    (void)desc;
    return STATUS_MONITOR_NO_DESCRIPTOR;
}

static NTSTATUS NTAPI HwSetPowerState(CONST PVOID ctx, ULONG uid, DEVICE_POWER_STATE state, POWER_ACTION action)
{
    tr("Power");
    (void)ctx;
    (void)uid;
    (void)state;
    (void)action;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwNotifyAcpiEvent(CONST PVOID ctx, DXGK_EVENT_TYPE type, ULONG event, PVOID arg, PULONG flags)
{
    (void)ctx;
    (void)type;
    (void)event;
    (void)arg;
    if (flags) *flags = 0;
    return STATUS_SUCCESS;
}

static VOID NTAPI HwResetDevice(CONST PVOID ctx) { (void)ctx; }
static VOID NTAPI HwUnload(VOID) {}

static NTSTATUS NTAPI HwQueryInterface(CONST PVOID ctx, PQUERY_INTERFACE qi)
{
    (void)ctx;
    (void)qi;
    return STATUS_NOT_SUPPORTED;
}

static VOID NTAPI HwControlEtw(BOOLEAN enable, ULONG flags, UCHAR level)
{
    (void)enable;
    (void)flags;
    (void)level;
}

/* ---- WDDM 2.0 (Parameters\\Wddm2 = 1), for Direct3D 12 (wddm/README.md, phase 8) ----
 * Windows then hands the driver GPU virtual addresses. The phone owns every object, so the page tables
 * stay empty for now: the entries below are what the memory manager needs in order to start the adapter. */
static BOOLEAN g_w2;

static VOID NTAPI HwSetStablePowerState(const HANDLE hAdapter, const DXGKARG_SETSTABLEPOWERSTATE *p)
{
    (void)hAdapter;
    (void)p;
}

static NTSTATUS NTAPI HwCreateProcess(const HANDLE hAdapter, DXGKARG_CREATEPROCESS *p)
{
    (void)hAdapter;
    tr("CreateProcess");
    p->hKmdProcess = p->hDxgkProcess;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwDestroyProcess(const HANDLE hAdapter, const HANDLE hProcess)
{
    (void)hAdapter;
    (void)hProcess;
    return STATUS_SUCCESS;
}

static VOID NTAPI HwSetRootPageTable(const HANDLE hAdapter, const DXGKARG_SETROOTPAGETABLE *p)
{
    (void)hAdapter;
    (void)p;
    tr("SetRoot");
}

static SIZE_T NTAPI HwGetRootPageTableSize(const HANDLE hAdapter, DXGKARG_GETROOTPAGETABLESIZE *p)
{
    (void)hAdapter;
    p->NumberOfPte = 512;
    return 512 * 8;
}

static NTSTATUS NTAPI HwSubmitCommandVirtual(const HANDLE hAdapter, const DXGKARG_SUBMITCOMMANDVIRTUAL *sc)
{
    (void)hAdapter;
    tr("SubmitVirtual");
    g_diag.submits++;
    complete_fence(sc->SubmissionFenceId, sc->NodeOrdinal, sc->EngineOrdinal);
    return STATUS_SUCCESS;
}

/* What the adapter can do, as the scheduler and memory manager need to hear it. A render-only
 * adapter with no engine of its own to schedule: one node, no preemption, no overlays. */
static NTSTATUS query_caps(DXGK_DRIVERCAPS *caps, ULONG size)
{
    /* The structure grows with each Windows version: ask only for what is set here, and clear what came. */
    ULONG need = max(max((ULONG)(FIELD_OFFSET(DXGK_DRIVERCAPS, WDDMVersion) + sizeof caps->WDDMVersion),
                         (ULONG)(FIELD_OFFSET(DXGK_DRIVERCAPS, GpuEngineTopology) + sizeof caps->GpuEngineTopology)),
                     max((ULONG)(FIELD_OFFSET(DXGK_DRIVERCAPS, PreemptionCaps) + sizeof caps->PreemptionCaps),
                         (ULONG)(FIELD_OFFSET(DXGK_DRIVERCAPS, MemoryManagementCaps) + sizeof caps->MemoryManagementCaps)));
    if (size < need) return STATUS_BUFFER_TOO_SMALL;
    RtlZeroMemory(caps, size);
    caps->HighestAcceptableAddress.QuadPart = ~0ull >> 1;
    caps->MaxAllocationListSlotId = 256;
    caps->SchedulingCaps.MultiEngineAware = 1;
    caps->SchedulingCaps.PreemptionAware = 1;
    caps->PresentationCaps.SupportKernelModeCommandBuffer = 1;
    caps->ApertureSegmentCommitLimit = (SIZE_T)HW_HEAP_SIZE;
    caps->WDDMVersion = DXGKDDI_WDDMv1_3;
    if (g_w2) {
        caps->WDDMVersion = DXGKDDI_WDDMv2;
        caps->MemoryManagementCaps.VirtualAddressingSupported = 1;
        caps->MemoryManagementCaps.GpuMmuSupported = 1;
        /* What the 2.x full-adapter checks ask for, as the compute sample sets them. */
        caps->SupportPerEngineTDR = 1;
        caps->FlipCaps.FlipOnVSyncMmIo = 1;
        caps->FlipCaps.FlipImmediateMmIo = 1;
        caps->FlipCaps.FlipIndependent = 1;
        caps->MaxQueuedFlipOnVSync = 1;
#if DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_5
        if (hwd_reg_dword(L"W2Virtual", 0)) caps->MiscCaps.VirtualGpuOnly = 1;
#endif   /* starts clean, but Windows hides such an adapter from programs */
        if (hwd_reg_dword(L"W2Kit", 0)) {
            caps->HighestAcceptableAddress.QuadPart = (LONGLONG)-1;
            caps->InternalGpuVirtualAddressRangeStart = 0x1000;
            caps->InternalGpuVirtualAddressRangeEnd = 0x1000000;
            caps->SchedulingCaps.NoDmaPatching = 1;
            caps->SchedulingCaps.CancelCommandAware = 1;
        }
    }
    caps->GpuEngineTopology.NbAsymetricProcessingNodes = 1;
    caps->PreemptionCaps.GraphicsPreemptionGranularity = D3DKMDT_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY;
    caps->PreemptionCaps.ComputePreemptionGranularity = D3DKMDT_COMPUTE_PREEMPTION_DMA_BUFFER_BOUNDARY;
    caps->SupportNonVGA = FALSE;
    if (d.on) {
        /* Flips: the new address is taken at once and reported at the next vsync. Without these
         * caps DWM made a primary, set it as the screen's and gave it up again, over and over. */
        /* (no FlipCaps: flips then go through DxgkDdiPresent as DMA buffers, which need no interrupt level) */
    }
    return STATUS_SUCCESS;
}

/* One aperture segment over the GPU heap: system memory the CPU and the phone both see. Windows places
 * allocations that need memory here and has nothing to copy, because the phone keeps the real data. */
static NTSTATUS query_segments(UINT type, const DXGKARG_QUERYADAPTERINFO *qai)
{
    if (type == DXGKQAITYPE_QUERYSEGMENT3) {
        if (qai->OutputDataSize < sizeof(DXGK_QUERYSEGMENTOUT3)) return STATUS_BUFFER_TOO_SMALL;
        DXGK_QUERYSEGMENTOUT3 *out = qai->pOutputData;
        if (!out->pSegmentDescriptor) {
            out->NbSegment = 1;
        } else {
            DXGK_SEGMENTDESCRIPTOR3 *d = out->pSegmentDescriptor;
            RtlZeroMemory(d, sizeof *d);
            d->Flags.Aperture = 1;
            d->Flags.CacheCoherent = 1;
            d->Flags.CpuVisible = 1;
            d->Size = (SIZE_T)HW_HEAP_SIZE;
            d->CommitLimit = (SIZE_T)HW_HEAP_SIZE;
        }
        out->PagingBufferSegmentId = 0;   /* system memory: dxgmms1 faults mapping a paging buffer kept in the aperture */
        out->PagingBufferSize = 64 * 1024;
        out->PagingBufferPrivateDataSize = 0;
        return STATUS_SUCCESS;
    }
    if (qai->OutputDataSize < sizeof(DXGK_QUERYSEGMENTOUT)) return STATUS_BUFFER_TOO_SMALL;
    DXGK_QUERYSEGMENTOUT *out = qai->pOutputData;
    if (!out->pSegmentDescriptor) {
        out->NbSegment = 1;
    } else {
        DXGK_SEGMENTDESCRIPTOR *d = out->pSegmentDescriptor;
        RtlZeroMemory(d, sizeof *d);
        d->Flags.Aperture = 1;
        d->Flags.CacheCoherent = 1;
        d->Flags.CpuVisible = 1;
        d->Size = (SIZE_T)HW_HEAP_SIZE;
        d->CommitLimit = (SIZE_T)HW_HEAP_SIZE;
    }
    out->PagingBufferSegmentId = 0;   /* system memory: dxgmms1 faults mapping a paging buffer kept in the aperture */
    out->PagingBufferSize = 64 * 1024;
    out->PagingBufferPrivateDataSize = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS query_segments4(const DXGKARG_QUERYADAPTERINFO *qai)
{
    if (qai->OutputDataSize < sizeof(DXGK_QUERYSEGMENTOUT4)) return STATUS_BUFFER_TOO_SMALL;
    DXGK_QUERYSEGMENTOUT4 *out = qai->pOutputData;
    out->SegmentDescriptorStride = sizeof(DXGK_SEGMENTDESCRIPTOR4);
    if (!out->pSegmentDescriptor) {
        out->NbSegment = 1;
    } else {
        DXGK_SEGMENTDESCRIPTOR4 *d4 = (DXGK_SEGMENTDESCRIPTOR4 *)out->pSegmentDescriptor;
        RtlZeroMemory(d4, sizeof *d4);
        d4->Flags.Aperture = 1;
        d4->Flags.CacheCoherent = 1;
        d4->Flags.CpuVisible = 1;
        d4->Size = (SIZE_T)HW_HEAP_SIZE;
        d4->CommitLimit = (SIZE_T)HW_HEAP_SIZE;
    }
    out->PagingBufferSegmentId = 0;
    out->PagingBufferSize = 64 * 1024;
    out->PagingBufferPrivateDataSize = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS qai_inner(const HANDLE hAdapter, const DXGKARG_QUERYADAPTERINFO *qai)
{
    switch (qai->Type) {
    case DXGKQAITYPE_DRIVERCAPS:
        return query_caps(qai->pOutputData, qai->OutputDataSize);
    case DXGKQAITYPE_QUERYSEGMENT:
    case DXGKQAITYPE_QUERYSEGMENT3:
        return query_segments(qai->Type, qai);
    case DXGKQAITYPE_QUERYSEGMENT4:
        return query_segments4(qai);
    case DXGKQAITYPE_PHYSICALADAPTERCAPS:
        if (hwd_reg_dword(L"W2NoQ15", 0)) return STATUS_NOT_SUPPORTED;
        if (qai->OutputDataSize < FIELD_OFFSET(DXGK_PHYSICALADAPTERCAPS, Flags) + sizeof(DXGK_PHYSICALADAPTERFLAGS)) return STATUS_BUFFER_TOO_SMALL;
        RtlZeroMemory(qai->pOutputData, qai->OutputDataSize);
        {
            DXGK_PHYSICALADAPTERCAPS *pc = qai->pOutputData;
            pc->NumExecutionNodes = 1;
            pc->PagingNodeIndex = 0;
            pc->DxgkPhysicalAdapterHandle = g.dxgk.DeviceHandle;
            pc->Flags.GpuMmuSupported = 1;
        }
        return STATUS_SUCCESS;
    case DXGKQAITYPE_GPUMMUCAPS: {
        if (qai->OutputDataSize < sizeof(DXGK_GPUMMUCAPS)) return STATUS_BUFFER_TOO_SMALL;
        DXGK_GPUMMUCAPS *mm = qai->pOutputData;
        RtlZeroMemory(mm, sizeof *mm);
        mm->ZeroInPteSupported = 1;
        mm->CacheCoherentMemorySupported = 1;
        mm->PageTableUpdateMode = DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;
        mm->VirtualAddressBitCount = 39;
        mm->LeafPageTableSizeFor64KPagesInBytes = 4096;
        mm->PageTableLevelCount = 3;
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_PAGETABLELEVELDESC: {
        if (qai->OutputDataSize < sizeof(DXGK_PAGE_TABLE_LEVEL_DESC)) return STATUS_BUFFER_TOO_SMALL;
        DXGK_PAGE_TABLE_LEVEL_DESC *lv = qai->pOutputData;
        RtlZeroMemory(lv, sizeof *lv);
        lv->PageTableIndexBitCount = 9;
        lv->PageTableSegmentId = 2;               /* the aperture is segment 1, system memory the next one */
        lv->PagingProcessPageTableSegmentId = 2;
        lv->PageTableSizeInBytes = 4096;
        lv->PageTableAlignmentInBytes = 4096;
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_HISTORYBUFFERPRECISION: {
        if (!g_w2) return STATUS_NOT_SUPPORTED;
        DXGKARG_HISTORYBUFFERPRECISION *hp = qai->pOutputData;
        for (UINT i = 0; i < qai->OutputDataSize / sizeof *hp; i++) hp[i].PrecisionBits = 64;
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_UMDRIVERPRIVATE:
        if (qai->OutputDataSize >= 4) {
            RtlZeroMemory(qai->pOutputData, qai->OutputDataSize);
            *(UINT *)qai->pOutputData = HWW_MAGIC;
            return STATUS_SUCCESS;
        }
        return STATUS_BUFFER_TOO_SMALL;
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

static NTSTATUS NTAPI HwQueryAdapterInfo(const HANDLE hAdapter, const DXGKARG_QUERYADAPTERINFO *qai)
{
    NTSTATUS st = qai_inner(hAdapter, qai);
    static const char hx[] = "0123456789abcdef";
    char nm[16] = {'Q', '0' + (char)((qai->Type / 10) % 10), '0' + (char)(qai->Type % 10), '=',
                   hx[(st >> 28) & 15], hx[(st >> 24) & 15], hx[(st >> 20) & 15], hx[(st >> 16) & 15],
                   hx[(st >> 12) & 15], hx[(st >> 8) & 15], hx[(st >> 4) & 15], hx[st & 15], 0};
    tr(nm);
    return st;
}

static NTSTATUS NTAPI HwCreateDevice(const HANDLE hAdapter, DXGKARG_CREATEDEVICE *cd)
{
    (void)hAdapter;
    tr("CreateDevice");
    cd->hDevice = &g;  /* one device context is all it needs: nothing is kept per device */
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwDestroyDevice(const HANDLE hDevice)
{
    tr("DestroyDevice");
    (void)hDevice;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwCreateContext(const HANDLE hDevice, DXGKARG_CREATECONTEXT *cc)
{
    (void)hDevice;
    tr("CreateContext");
    cc->hContext = &g;
    cc->ContextInfo.DmaBufferSize = 16 * 1024;
    cc->ContextInfo.DmaBufferSegmentSet = 0;           /* system memory */
    cc->ContextInfo.DmaBufferPrivateDataSize = 64;     /* room for a marker */
    cc->ContextInfo.AllocationListSize = 256;
    cc->ContextInfo.PatchLocationListSize = 256;
    if (g_w2) {
        /* With GPU virtual addresses nothing in a DMA buffer is patched. */
        cc->ContextInfo.Caps.NoPatchingRequired = 1;
        cc->ContextInfo.PatchLocationListSize = 0;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwDestroyContext(const HANDLE hContext)
{
    tr("DestroyContext");
    (void)hContext;
    return STATUS_SUCCESS;
}

/* Programs create no kernel allocations through this adapter: the phone owns every object. */
static NTSTATUS NTAPI HwCreateAllocation(const HANDLE hAdapter, DXGKARG_CREATEALLOCATION *ca)
{
    tr("CreateAlloc");
    (void)hAdapter;
    (void)ca;
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS NTAPI HwDestroyAllocation(const HANDLE hAdapter, const DXGKARG_DESTROYALLOCATION *da)
{
    (void)hAdapter;
    (void)da;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwOpenAllocation(const HANDLE hDevice, const DXGKARG_OPENALLOCATION *oa)
{
    (void)hDevice;
    (void)oa;
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS NTAPI HwCloseAllocation(const HANDLE hDevice, const DXGKARG_CLOSEALLOCATION *ca)
{
    (void)hDevice;
    (void)ca;
    return STATUS_SUCCESS;
}

#define NOTSUP(fn, label) \
    static NTSTATUS NTAPI fn(const HANDLE h, PVOID arg) \
    { \
        (void)h; \
        (void)arg; \
        tr(label); \
        return STATUS_NOT_SUPPORTED; \
    }
NOTSUP(HwDescribeAllocation, "Describe")
NOTSUP(HwGetStandardData, "StdData")
NOTSUP(HwRenderDdi, "Render")
NOTSUP(HwPresentDdi, "Present")
NOTSUP(HwRenderKmDdi, "RenderKm")

static NTSTATUS NTAPI HwSuccess(const HANDLE h, PVOID arg)
{
    (void)h;
    (void)arg;
    return STATUS_SUCCESS;
}

/* Paging: the phone keeps every object, so Windows' moves, fills and mappings change nothing. An
 * empty paging buffer is a finished one. */
static NTSTATUS NTAPI HwBuildPagingBuffer(const HANDLE hAdapter, DXGKARG_BUILDPAGINGBUFFER *pb)
{
    tr("BuildPaging");
    (void)hAdapter;
    (void)pb;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwSubmitCommand(const HANDLE hAdapter, const DXGKARG_SUBMITCOMMAND *sc)
{
    tr("Submit");
    (void)hAdapter;
    g_diag.submits++;
    g_diag.last_submit = sc->SubmissionFenceId;
    g_diag.last_flags = sc->Flags.Value | (sc->NodeOrdinal << 24);
    const struct hww_marker *m = (const struct hww_marker *)sc->pDmaBufferPrivateData;
    BOOLEAN marker = FALSE;
    if (!sc->Flags.Paging && m && sc->DmaBufferPrivateDataSize >= sizeof *m &&
        sc->DmaBufferPrivateDataSubmissionEndOffset - sc->DmaBufferPrivateDataSubmissionStartOffset >= sizeof *m) {
        m = (const struct hww_marker *)((const UCHAR *)sc->pDmaBufferPrivateData + sc->DmaBufferPrivateDataSubmissionStartOffset);
        marker = m->magic == HWW_MARKER_MAGIC && m->slot < HWGPU_CHANNELS;
    }
    /* A buffer with nothing to wait for still waits behind the ones before it, and is finished by the
     * polling timer too: finishing it here, inside the submit, never reached Windows. */
    KIRQL irql;
    BOOLEAN queued = FALSE;
    KeAcquireSpinLock(&g.pending_lock, &irql);
    if (g.pend_count < MAX_PENDING) {
        struct pending *p = &g.pend[(g.pend_head + g.pend_count) % MAX_PENDING];
        RtlZeroMemory(p, sizeof *p);
        p->used = TRUE;
        p->fence_id = sc->SubmissionFenceId;
        p->node = sc->NodeOrdinal;
        p->engine = sc->EngineOrdinal;
        p->slot = marker ? m->slot : NO_MARKER;
        if (marker) {
            p->generation = m->generation;
            p->ring = m->ring;
            p->fence = m->fence;
        }
        p->epoch = g.epoch;
        g.pend_count++;
        queued = TRUE;
    }
    KeReleaseSpinLock(&g.pending_lock, irql);
    if (queued) {
        if (marker) g.counters.marker_submits++;
        start_polling();
        return STATUS_SUCCESS;
    }
    complete_fence(sc->SubmissionFenceId, sc->NodeOrdinal, sc->EngineOrdinal);
    return STATUS_SUCCESS;
}

/* Windows asks to preempt when it wants something else to run first. What is queued is dropped and the
 * preemption reported at once: Windows submits the dropped buffers again, and never waits on an answer. */
static BOOLEAN NTAPI sync_preempted(PVOID context)
{
    const DXGKARG_PREEMPTCOMMAND *pc = context;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA d;
    RtlZeroMemory(&d, sizeof d);
    d.InterruptType = DXGK_INTERRUPT_DMA_PREEMPTED;
    d.DmaPreempted.PreemptionFenceId = pc->PreemptionFenceId;
    d.DmaPreempted.LastCompletedFenceId = g.last_fence;
    d.DmaPreempted.NodeOrdinal = pc->NodeOrdinal;
    d.DmaPreempted.EngineOrdinal = pc->EngineOrdinal;
    g.dxgk.DxgkCbNotifyInterrupt(g.dxgk.DeviceHandle, &d);
    g.dxgk.DxgkCbQueueDpc(g.dxgk.DeviceHandle);
    return TRUE;
}

static NTSTATUS NTAPI HwPreemptCommand(const HANDLE hAdapter, const DXGKARG_PREEMPTCOMMAND *pc)
{
    tr("Preempt");
    (void)hAdapter;
    KIRQL irql;
    KeAcquireSpinLock(&g.pending_lock, &irql);
    g.pend_count = 0;
    g.pend_head = 0;
    KeReleaseSpinLock(&g.pending_lock, irql);
    g_diag.preempts++;
    run_synced(sync_preempted, (PVOID)pc);
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwQueryCurrentFence(const HANDLE hAdapter, DXGKARG_QUERYCURRENTFENCE *qf)
{
    tr("CurFence");
    (void)hAdapter;
    qf->CurrentFence = g.last_fence;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwResetFromTimeout(const HANDLE hAdapter)
{
    (void)hAdapter;
    /* What the GPU looked like when Windows gave up on it, for the display bring-up. */
    report(L"T_Submits", g_diag.submits);
    report(L"T_LastSubmit", g_diag.last_submit);
    report(L"T_LastFlags", g_diag.last_flags);
    report(L"T_LastFence", g.last_fence);
    report(L"T_Vsyncs", d.vsyncs);
    report(L"T_VsyncOn", (ULONG)hwd_vsync_on);
    report(L"T_SourceSets", d.source_sets);
    report(L"T_Presents", d.presents);
    report(L"T_SourceAddr", (ULONG)d.source_addr.QuadPart);
    report(L"T_Pending", g.pend_count);
    report(L"T_Resets", ++g_diag.resets);
    report(L"T_Preempts", g_diag.preempts);
    /* Whatever Windows was waiting on is abandoned. */
    KIRQL irql;
    KeAcquireSpinLock(&g.pending_lock, &irql);
    RtlZeroMemory(g.pend, sizeof g.pend);
    g.pend_count = 0;
    g.pend_head = 0;
    KeReleaseSpinLock(&g.pending_lock, irql);
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwRestartFromTimeout(const HANDLE hAdapter)
{
    (void)hAdapter;
    return STATUS_SUCCESS;
}

/* Windows asks for this at the start of every timeout recovery, and a failure here stops the recovery
 * with VIDEO_TDR_FAILURE. There is nothing on this side worth adding to its report. */
static NTSTATUS NTAPI HwCollectDbgInfo(const HANDLE hAdapter, const DXGKARG_COLLECTDBGINFO *di)
{
    (void)hAdapter;
    if (di->pBuffer && di->BufferSize) RtlZeroMemory(di->pBuffer, di->BufferSize);
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwControlInterrupt(const HANDLE hAdapter, const DXGK_INTERRUPT_TYPE type, BOOLEAN enable)
{
    tr("CtlIntr");
    (void)hAdapter;
    return hwd_control_interrupt(type, enable);
}

/* The engine and power DDIs of WDDM 1.2 and later, answered for an adapter with one engine that is
 * always responsive and never powered down. */
/* An engine reset drops everything queued; Windows needs the newest fence it dropped. */
static NTSTATUS NTAPI HwResetEngine(const HANDLE hAdapter, DXGKARG_RESETENGINE *re)
{
    tr("ResetEng");
    (void)hAdapter;
    KIRQL irql;
    KeAcquireSpinLock(&g.pending_lock, &irql);
    re->LastAbortedFenceId = g.pend_count ? g.pend[(g.pend_head + g.pend_count - 1) % MAX_PENDING].fence_id : g.last_fence;
    g.pend_count = 0;
    g.pend_head = 0;
    KeReleaseSpinLock(&g.pending_lock, irql);
    report(L"T_EngineResets", ++g_diag.resets);
    report(L"T_Aborted", re->LastAbortedFenceId);
    report(L"T_LastFence", g.last_fence);
    report(L"T_Submits", g_diag.submits);
    report(L"T_Preempts", g_diag.preempts);
    report(L"T_LastFlags", g_diag.last_flags);
    return STATUS_SUCCESS;
}

/* Windows asks whether an engine answers, and which nodes a reset of it takes down. The phone keeps no
 * engine apart from the channels, so the answer is always: responsive, and none but itself. */
static NTSTATUS NTAPI HwQueryEngineStatus(const HANDLE hAdapter, DXGKARG_QUERYENGINESTATUS *qs)
{
    tr("QEngSt");
    (void)hAdapter;
    tr("EngineStatus");
    RtlZeroMemory(&qs->EngineStatus, sizeof qs->EngineStatus);
    qs->EngineStatus.Responsive = 1;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwQueryDependentEngineGroup(const HANDLE hAdapter, DXGKARG_QUERYDEPENDENTENGINEGROUP *dg)
{
    tr("QDepGrp");
    (void)hAdapter;
    tr("DepGroup");
    dg->DependentNodeOrdinalMask = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwGetNodeMetadata(const HANDLE hAdapter, UINT node, DXGKARG_GETNODEMETADATA *nm)
{
    tr("NodeMeta");
    (void)hAdapter;
    (void)node;
    RtlZeroMemory(nm, sizeof *nm);
    nm->EngineType = DXGK_ENGINE_TYPE_3D;
    if (g_w2) nm->GpuMmuSupported = 1;
    return STATUS_SUCCESS;
}

/* The scheduler calibrates a GPU's clock against the CPU's on a timer. There is no GPU clock here: the
 * phone's work is paced on the phone. Report the CPU's counter as both, at its own frequency. */
static NTSTATUS NTAPI HwCalibrateGpuClock(const HANDLE hAdapter, UINT32 node, UINT32 engine, DXGKARG_CALIBRATEGPUCLOCK *cal)
{
    tr("Calib");
    (void)hAdapter;
    (void)node;
    (void)engine;
    LARGE_INTEGER freq;
    LARGE_INTEGER now = KeQueryPerformanceCounter(&freq);
    cal->GpuFrequency = (ULONGLONG)freq.QuadPart;
    cal->GpuClockCounter = (ULONGLONG)now.QuadPart;
    cal->CpuClockCounter = (ULONGLONG)now.QuadPart;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwCancelCommand(const HANDLE hAdapter, const DXGKARG_CANCELCOMMAND *cc)
{
    tr("Cancel");
    (void)hAdapter;
    (void)cc;
    return STATUS_SUCCESS;
}

/* ---- DxgkInitialize, without displib.lib ---- */

static NTSTATUS call_internal(PDEVICE_OBJECT dev, ULONG code, PVOID out, ULONG out_len)
{
    KEVENT ev;
    IO_STATUS_BLOCK iosb;
    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    PIRP irp = IoBuildDeviceIoControlRequest(code, dev, NULL, 0, out, out_len, TRUE, &ev, &iosb);
    if (!irp) return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS st = IoCallDriver(dev, irp);
    if (st == STATUS_PENDING) {
        KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
        st = iosb.Status;
    }
    return st;
}

typedef NTSTATUS (NTAPI *dxgk_init_fn)(PDRIVER_OBJECT, PUNICODE_STRING, PVOID);

static NTSTATUS HwDxgkInitialize(PDRIVER_OBJECT driver, PUNICODE_STRING registry, DRIVER_INITIALIZATION_DATA *init)
{
    UNICODE_STRING service, device;
    RtlInitUnicodeString(&service, L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\DXGKrnl");
    RtlInitUnicodeString(&device, L"\\Device\\DxgKrnl");
    NTSTATUS load = ZwLoadDriver(&service);
    BOOLEAN loaded_now = load != STATUS_IMAGE_ALREADY_LOADED;
    if (!NT_SUCCESS(load) && load != STATUS_IMAGE_ALREADY_LOADED) return load;
    PFILE_OBJECT file = NULL;
    PDEVICE_OBJECT dev = NULL;
    NTSTATUS st = STATUS_UNSUCCESSFUL;
    for (int tries = 0; tries < 10; tries++) {
        st = IoGetDeviceObjectPointer(&device, GENERIC_READ | GENERIC_WRITE, &file, &dev);
        if (NT_SUCCESS(st) || !loaded_now) break;
        LARGE_INTEGER d;
        d.QuadPart = -50000;  /* 5 ms */
        KeDelayExecutionThread(KernelMode, FALSE, &d);
    }
    if (!NT_SUCCESS(st)) return st;
    dxgk_init_fn fn = NULL;
    st = call_internal(dev, 0x00230047, &fn, sizeof fn);
    if (st == STATUS_INVALID_DEVICE_REQUEST) st = call_internal(dev, 0x0023003F, &fn, sizeof fn);
    ObDereferenceObject(file);
    if (!NT_SUCCESS(st) || !fn) return NT_SUCCESS(st) ? STATUS_UNSUCCESSFUL : st;
    return fn(driver, registry, init);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry)
{
    /* The crash-loop breaker first: two starts in a row that did not finish means the third boot
     * goes without this driver. */
    RtlZeroMemory(g.guard_path, sizeof g.guard_path);
    ULONG chars = registry->Length / sizeof(WCHAR);
    if (chars > 200) return STATUS_INVALID_PARAMETER;
    RtlCopyMemory(g.guard_path, registry->Buffer, registry->Length);
    static const WCHAR sub[] = L"\\Parameters";
    RtlCopyMemory(g.guard_path + chars, sub, sizeof sub);
    ULONG tries = guard_count(FALSE, 0);
    if (tries >= 2) return STATUS_UNSUCCESSFUL;
    guard_count(TRUE, tries + 1);

    DRIVER_INITIALIZATION_DATA init;
    RtlZeroMemory(&init, sizeof init);
    init.Version = DXGKDDI_INTERFACE_VERSION_WDDM1_3;
    init.DxgkDdiAddDevice = HwAddDevice;
    init.DxgkDdiStartDevice = HwStartDevice;
    init.DxgkDdiStopDevice = HwStopDevice;
    init.DxgkDdiRemoveDevice = HwRemoveDevice;
    init.DxgkDdiDispatchIoRequest = HwDispatchIoRequest;
    init.DxgkDdiInterruptRoutine = HwInterrupt;
    init.DxgkDdiDpcRoutine = HwDpc;
    init.DxgkDdiQueryChildRelations = HwQueryChildRelations;
    init.DxgkDdiQueryChildStatus = HwQueryChildStatus;
    init.DxgkDdiQueryDeviceDescriptor = HwQueryDeviceDescriptor;
    init.DxgkDdiSetPowerState = HwSetPowerState;
    init.DxgkDdiNotifyAcpiEvent = HwNotifyAcpiEvent;
    init.DxgkDdiResetDevice = HwResetDevice;
    init.DxgkDdiUnload = HwUnload;
    init.DxgkDdiQueryInterface = HwQueryInterface;
    init.DxgkDdiControlEtwLogging = HwControlEtw;
    init.DxgkDdiQueryAdapterInfo = HwQueryAdapterInfo;
    init.DxgkDdiCreateDevice = HwCreateDevice;
    init.DxgkDdiCreateAllocation = HwCreateAllocation;
    init.DxgkDdiDestroyAllocation = HwDestroyAllocation;
    init.DxgkDdiDescribeAllocation = (PDXGKDDI_DESCRIBEALLOCATION)HwDescribeAllocation;
    init.DxgkDdiGetStandardAllocationDriverData = (PDXGKDDI_GETSTANDARDALLOCATIONDRIVERDATA)HwGetStandardData;
    init.DxgkDdiPatch = (PDXGKDDI_PATCH)HwSuccess;
    init.DxgkDdiSubmitCommand = HwSubmitCommand;
    init.DxgkDdiPreemptCommand = HwPreemptCommand;
    init.DxgkDdiBuildPagingBuffer = HwBuildPagingBuffer;
    init.DxgkDdiResetFromTimeout = HwResetFromTimeout;
    init.DxgkDdiRestartFromTimeout = HwRestartFromTimeout;
    init.DxgkDdiEscape = HwEscape;
    init.DxgkDdiCollectDbgInfo = HwCollectDbgInfo;
    init.DxgkDdiQueryCurrentFence = HwQueryCurrentFence;
    init.DxgkDdiControlInterrupt = HwControlInterrupt;
    init.DxgkDdiDestroyDevice = HwDestroyDevice;
    init.DxgkDdiOpenAllocation = HwOpenAllocation;
    init.DxgkDdiCloseAllocation = HwCloseAllocation;
    init.DxgkDdiRender = (PDXGKDDI_RENDER)HwRenderDdi;
    init.DxgkDdiPresent = (PDXGKDDI_PRESENT)HwPresentDdi;
    init.DxgkDdiCreateContext = HwCreateContext;
    init.DxgkDdiDestroyContext = HwDestroyContext;
    init.DxgkDdiRenderKm = (PDXGKDDI_RENDER)HwRenderKmDdi;
    init.DxgkDdiResetEngine = HwResetEngine;
    init.DxgkDdiCancelCommand = HwCancelCommand;
    init.DxgkDdiGetNodeMetadata = HwGetNodeMetadata;
    init.DxgkDdiCalibrateGpuClock = HwCalibrateGpuClock;
    init.DxgkDdiQueryEngineStatus = HwQueryEngineStatus;
    init.DxgkDdiQueryDependentEngineGroup = HwQueryDependentEngineGroup;
    hwd_setup(&init);
    g_w2 = hwd_reg_dword(L"Wddm2", 0) != 0;
    if (g_w2) {
        init.Version = DXGKDDI_INTERFACE_VERSION_WDDM2_6;
        init.DxgkDdiCreateProcess = HwCreateProcess;
        init.DxgkDdiDestroyProcess = HwDestroyProcess;
        init.DxgkDdiSetRootPageTable = HwSetRootPageTable;
        init.DxgkDdiGetRootPageTableSize = HwGetRootPageTableSize;
        init.DxgkDdiSubmitCommandVirtual = HwSubmitCommandVirtual;
        /* With a GPU address space there is no patching and no physical submit. */
        init.DxgkDdiSetStablePowerState = HwSetStablePowerState;
        /* A WDDM 2.x table also carries the power component, history buffer and virtual machine entries. */
        init.DxgkDdiSetPowerComponentFState = (PDXGKDDISETPOWERCOMPONENTFSTATE)HwSuccess;
        init.DxgkDdiPowerRuntimeControlRequest = (PDXGKDDIPOWERRUNTIMECONTROLREQUEST)HwSuccess;
        init.DxgkDdiFormatHistoryBuffer = (PDXGKDDI_FORMATHISTORYBUFFER)HwSuccess;
    }
    return HwDxgkInitialize(driver, registry, &init);
}
