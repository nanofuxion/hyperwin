/* HyperWin GPU, direct variant: hands each Windows process its own GPU ring channel.
 *
 * Not a display driver. Windows sees no GPU. The Vulkan driver (vulkan_hwgpu.dll, Mesa's gfxstream
 * encoder) opens \\.\HwGpu, asks for a channel, and from then on writes gfxstream straight into
 * shared RAM that the phone reads. No system call per command, nothing in between.
 *
 * It also maps the phone storage slots (include/hw_files.h) into hwfiles.exe.
 *
 * The kernel part only claims, maps and frees. A legacy (non-PnP) driver: one control device,
 * demand or auto start, ntoskrnl imports only.
 */
#include <ntddk.h>
#include "hw_abi.h"
#define HWGPU_KERNEL
#include "hwgpu_ring.h"
#include "hwgpu_ioctl.h"
#include "hw_files.h"

#define TAG 'upGH'
#ifndef MdlMappingNoExecute
#define MdlMappingNoExecute 0x40000000
#endif

#define MAX_VIEWS 256u

/* One heap range mapped into the process. */
struct view {
    PMDL mdl;
    PVOID user;
    uint64_t offset, size;
};

struct open_ctx {
    int slot;           /* -1 until IOCTL_HWGPU_OPEN */
    PMDL mdl;
    PVOID user;         /* mapping in the process that asked */
    PEPROCESS process;
    struct view views[MAX_VIEWS];
    struct view files;  /* the storage slots, IOCTL_HWGPU_FILES */
    struct view screen; /* header and frames, IOCTL_HWGPU_SCREEN */
};

static PVOID g_ring;    /* kernel mapping of the ring area, offset HWGPU_RING_OFF in the window */
static PVOID g_heap;    /* kernel mapping of the shared heap, HW_HEAP_PA, or NULL */
static PVOID g_files;   /* kernel mapping of the storage slots, HWF_OFF in the window */
static PVOID g_screen;  /* kernel mapping of the header and frames, [0, HW_FRAME_END) */
static FAST_MUTEX g_lock;

/* The window base the ring helpers expect, so offsets inside hwgpu_ring.h line up. */
static void *window(void) { return (UCHAR *)g_ring - HWGPU_RING_OFF; }

static NTSTATUS done(PIRP irp, NTSTATUS st, ULONG_PTR info)
{
    irp->IoStatus.Status = st;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

/* Maps one heap range into the current process. User mappings raise instead of failing. */
static PVOID map_user(PVOID kernel, uint64_t size, PMDL *out)
{
    PMDL mdl = IoAllocateMdl(kernel, (ULONG)size, FALSE, FALSE, NULL);
    if (!mdl) return NULL;
    MmBuildMdlForNonPagedPool(mdl);
    PVOID user = NULL;
    __try {
        user = MmMapLockedPagesSpecifyCache(mdl, UserMode, MmCached, NULL, FALSE,
                                            NormalPagePriority | MdlMappingNoExecute);
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

static void unmap_view(struct open_ctx *c, struct view *v)
{
    if (v->user && c->process == PsGetCurrentProcess()) MmUnmapLockedPages(v->user, v->mdl);
    if (v->mdl) IoFreeMdl(v->mdl);
    RtlZeroMemory(v, sizeof(*v));
}

/* A range the phone granted this handle's channel, matched exactly. */
static int granted(struct open_ctx *c, uint64_t offset, uint64_t size)
{
    struct hwgpu_ctl *ctl = hwgpu_chan_ctl((UCHAR *)window() + hwgpu_chan_off((uint32_t)c->slot));
    for (uint32_t i = 0; i < HWGPU_GRANTS; i++) {
        uint64_t g = __atomic_load_n(&ctl->grant[i].size, __ATOMIC_ACQUIRE);
        if (g && g == size && ctl->grant[i].offset == offset) return 1;
    }
    return 0;
}

static NTSTATUS map_heap(struct open_ctx *c, const struct hwgpu_map_in *in, struct hwgpu_map_out *out)
{
    if (c->slot < 0 || !g_heap) return STATUS_DEVICE_NOT_READY;
    if (!in->size || (in->offset | in->size) & 0xfff || in->offset > HW_HEAP_SIZE ||
        in->size > HW_HEAP_SIZE - in->offset || in->size > 0xfffff000ull)
        return STATUS_INVALID_PARAMETER;
    if (!granted(c, in->offset, in->size)) return STATUS_ACCESS_DENIED;
    struct view *v = NULL;
    for (uint32_t i = 0; i < MAX_VIEWS && !v; i++)
        if (!c->views[i].user) v = &c->views[i];
    if (!v) return STATUS_INSUFFICIENT_RESOURCES;
    PMDL mdl = NULL;
    PVOID user = map_user((UCHAR *)g_heap + in->offset, in->size, &mdl);
    if (!user) return STATUS_INSUFFICIENT_RESOURCES;
    v->mdl = mdl;
    v->user = user;
    v->offset = in->offset;
    v->size = in->size;
    out->user = (uint64_t)(ULONG_PTR)user;
    return STATUS_SUCCESS;
}

static NTSTATUS unmap_heap(struct open_ctx *c, const struct hwgpu_map_out *in)
{
    for (uint32_t i = 0; i < MAX_VIEWS; i++) {
        if (c->views[i].user && (uint64_t)(ULONG_PTR)c->views[i].user == in->user) {
            unmap_view(c, &c->views[i]);
            return STATUS_SUCCESS;
        }
    }
    return STATUS_INVALID_PARAMETER;
}

static NTSTATUS on_create(PDEVICE_OBJECT dev, PIRP irp)
{
    (void)dev;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    struct open_ctx *c = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*c), TAG);
    if (!c) return done(irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    RtlZeroMemory(c, sizeof(*c));
    c->slot = -1;
    s->FileObject->FsContext = c;
    return done(irp, STATUS_SUCCESS, 0);
}

/* Runs in the process that owns the handle, while its address space still exists. */
static NTSTATUS on_cleanup(PDEVICE_OBJECT dev, PIRP irp)
{
    (void)dev;
    struct open_ctx *c = IoGetCurrentIrpStackLocation(irp)->FileObject->FsContext;
    ExAcquireFastMutex(&g_lock);
    if (c && c->files.user) unmap_view(c, &c->files);
    if (c && c->screen.user) unmap_view(c, &c->screen);
    if (c && c->slot >= 0) {
        /* Heap views first: the phone may hand their memory to someone else once the channel is free. */
        for (uint32_t i = 0; i < MAX_VIEWS; i++)
            if (c->views[i].user) unmap_view(c, &c->views[i]);
        if (c->user && c->process == PsGetCurrentProcess()) MmUnmapLockedPages(c->user, c->mdl);
        IoFreeMdl(c->mdl);
        hwgpu_release(window(), (uint32_t)c->slot);
        c->slot = -1;
        c->mdl = NULL;
        c->user = NULL;
    }
    ExReleaseFastMutex(&g_lock);
    return done(irp, STATUS_SUCCESS, 0);
}

static NTSTATUS on_close(PDEVICE_OBJECT dev, PIRP irp)
{
    (void)dev;
    struct open_ctx *c = IoGetCurrentIrpStackLocation(irp)->FileObject->FsContext;
    if (c) ExFreePoolWithTag(c, TAG);
    return done(irp, STATUS_SUCCESS, 0);
}

static NTSTATUS open_channel(struct open_ctx *c, struct hwgpu_open_out *out)
{
    if (c->slot >= 0) return STATUS_DEVICE_BUSY;
    if (!hwgpu_ring_valid(window())) return STATUS_DEVICE_NOT_READY;  /* the phone has no GPU server */
    uint32_t gen = 0;
    int slot = hwgpu_claim(window(), &gen);
    if (slot < 0) return STATUS_INSUFFICIENT_RESOURCES;
    PVOID chan = (UCHAR *)window() + hwgpu_chan_off((uint32_t)slot);
    PMDL mdl = IoAllocateMdl(chan, HWGPU_CHAN_STRIDE, FALSE, FALSE, NULL);
    if (!mdl) {
        hwgpu_release(window(), (uint32_t)slot);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    MmBuildMdlForNonPagedPool(mdl);
    /* Cached, like every other mapping of the window: it is ordinary RAM shared with the phone. */
    /* A user-mode mapping raises instead of returning NULL when the process has no room. */
    PVOID user = NULL;
    __try {
        user = MmMapLockedPagesSpecifyCache(mdl, UserMode, MmCached, NULL, FALSE,
                                            NormalPagePriority | MdlMappingNoExecute);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        user = NULL;
    }
    if (!user) {
        IoFreeMdl(mdl);
        hwgpu_release(window(), (uint32_t)slot);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    c->slot = slot;
    c->mdl = mdl;
    c->user = user;
    c->process = PsGetCurrentProcess();
    out->chan = (uint64_t)(ULONG_PTR)user;
    out->slot = (uint32_t)slot;
    out->generation = gen;
    return STATUS_SUCCESS;
}

/* The screen is for services (WUDFHost.exe runs indirect display drivers as LocalService) and for
 * elevated administrators, who could reach it anyway: callers holding SeImpersonatePrivilege. A
 * normal program does not have it. */
static BOOLEAN screen_caller_ok(void)
{
    LUID impersonate = {29, 0};  /* SE_IMPERSONATE_PRIVILEGE */
    return SeSinglePrivilegeCheck(impersonate, UserMode);
}

static NTSTATUS on_ioctl(PDEVICE_OBJECT dev, PIRP irp)
{
    (void)dev;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    ULONG code = s->Parameters.DeviceIoControl.IoControlCode;
    ULONG in_len = s->Parameters.DeviceIoControl.InputBufferLength;
    ULONG out_len = s->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID buf = irp->AssociatedIrp.SystemBuffer;
    struct open_ctx *c = s->FileObject->FsContext;
    /* Mappings go into the current process, so only take calls made from user mode directly. */
    if (irp->RequestorMode != UserMode) return done(irp, STATUS_ACCESS_DENIED, 0);
    if (code == IOCTL_HWGPU_OPEN) {
        if (out_len < sizeof(struct hwgpu_open_out)) return done(irp, STATUS_BUFFER_TOO_SMALL, 0);
        struct hwgpu_open_out out;
        RtlZeroMemory(&out, sizeof(out));
        ExAcquireFastMutex(&g_lock);
        NTSTATUS st = open_channel(c, &out);
        ExReleaseFastMutex(&g_lock);
        if (!NT_SUCCESS(st)) return done(irp, st, 0);
        RtlCopyMemory(buf, &out, sizeof(out));
        return done(irp, STATUS_SUCCESS, sizeof(out));
    }
    if (code == IOCTL_HWGPU_FILES) {
        if (out_len < sizeof(struct hwgpu_map_out)) return done(irp, STATUS_BUFFER_TOO_SMALL, 0);
        LUID tcb = {7, 0};  /* SE_TCB_PRIVILEGE: only SYSTEM holds it */
        if (!SeSinglePrivilegeCheck(tcb, UserMode)) return done(irp, STATUS_PRIVILEGE_NOT_HELD, 0);
        struct hwgpu_map_out out;
        NTSTATUS st = STATUS_SUCCESS;
        ExAcquireFastMutex(&g_lock);
        if (!g_files) st = STATUS_DEVICE_NOT_READY;
        else if (c->files.user) st = STATUS_DEVICE_BUSY;
        else if (!(c->files.user = map_user(g_files, HWF_BYTES, &c->files.mdl))) st = STATUS_INSUFFICIENT_RESOURCES;
        else c->process = PsGetCurrentProcess();
        out.user = (uint64_t)(ULONG_PTR)c->files.user;
        ExReleaseFastMutex(&g_lock);
        if (!NT_SUCCESS(st)) return done(irp, st, 0);
        RtlCopyMemory(buf, &out, sizeof(out));
        return done(irp, STATUS_SUCCESS, sizeof(out));
    }
    if (code == IOCTL_HWGPU_SCREEN) {
        if (out_len < sizeof(struct hwgpu_map_out)) return done(irp, STATUS_BUFFER_TOO_SMALL, 0);
        if (!screen_caller_ok()) return done(irp, STATUS_ACCESS_DENIED, 0);
        struct hwgpu_map_out out;
        NTSTATUS st = STATUS_SUCCESS;
        ExAcquireFastMutex(&g_lock);
        if (!g_screen) st = STATUS_DEVICE_NOT_READY;
        else if (c->screen.user) st = STATUS_DEVICE_BUSY;
        else if (!(c->screen.user = map_user(g_screen, HW_FRAME_END, &c->screen.mdl))) st = STATUS_INSUFFICIENT_RESOURCES;
        else c->process = PsGetCurrentProcess();
        out.user = (uint64_t)(ULONG_PTR)c->screen.user;
        ExReleaseFastMutex(&g_lock);
        if (!NT_SUCCESS(st)) return done(irp, st, 0);
        RtlCopyMemory(buf, &out, sizeof(out));
        return done(irp, STATUS_SUCCESS, sizeof(out));
    }
    if (code == IOCTL_HWGPU_MAP) {
        if (in_len < sizeof(struct hwgpu_map_in) || out_len < sizeof(struct hwgpu_map_out))
            return done(irp, STATUS_BUFFER_TOO_SMALL, 0);
        struct hwgpu_map_in in;
        struct hwgpu_map_out out;
        RtlCopyMemory(&in, buf, sizeof(in));
        RtlZeroMemory(&out, sizeof(out));
        ExAcquireFastMutex(&g_lock);
        NTSTATUS st = c->process == PsGetCurrentProcess() ? map_heap(c, &in, &out) : STATUS_ACCESS_DENIED;
        ExReleaseFastMutex(&g_lock);
        if (!NT_SUCCESS(st)) return done(irp, st, 0);
        RtlCopyMemory(buf, &out, sizeof(out));
        return done(irp, STATUS_SUCCESS, sizeof(out));
    }
    if (code == IOCTL_HWGPU_UNMAP) {
        if (in_len < sizeof(struct hwgpu_map_out)) return done(irp, STATUS_BUFFER_TOO_SMALL, 0);
        struct hwgpu_map_out in;
        RtlCopyMemory(&in, buf, sizeof(in));
        ExAcquireFastMutex(&g_lock);
        NTSTATUS st = c->process == PsGetCurrentProcess() ? unmap_heap(c, &in) : STATUS_ACCESS_DENIED;
        ExReleaseFastMutex(&g_lock);
        return done(irp, st, 0);
    }
    return done(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

static void unload(PDRIVER_OBJECT driver)
{
    UNICODE_STRING link;
    RtlInitUnicodeString(&link, HWGPU_LINK_NAME);
    IoDeleteSymbolicLink(&link);
    if (driver->DeviceObject) IoDeleteDevice(driver->DeviceObject);
    if (g_ring) MmUnmapIoSpace(g_ring, HWGPU_RING_BYTES);
    if (g_heap) MmUnmapIoSpace(g_heap, (SIZE_T)HW_HEAP_SIZE);
    if (g_files) MmUnmapIoSpace(g_files, HWF_BYTES);
    if (g_screen) MmUnmapIoSpace(g_screen, HW_FRAME_END);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry)
{
    (void)registry;
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = (LONGLONG)(HW_WINDOW_PA + HWGPU_RING_OFF);
    g_ring = MmMapIoSpace(pa, HWGPU_RING_BYTES, MmCached);
    if (!g_ring) return STATUS_INSUFFICIENT_RESOURCES;
    /* The heap is optional: firmware or an app from before it leaves Windows without it, and
     * mapped GPU memory is then copied as before. */
    pa.QuadPart = (LONGLONG)HW_HEAP_PA;
    g_heap = MmMapIoSpace(pa, (SIZE_T)HW_HEAP_SIZE, MmCached);
    pa.QuadPart = (LONGLONG)(HW_WINDOW_PA + HWF_OFF);
    g_files = MmMapIoSpace(pa, HWF_BYTES, MmCached);
    pa.QuadPart = (LONGLONG)HW_WINDOW_PA;
    g_screen = MmMapIoSpace(pa, HW_FRAME_END, MmCached);
    ExInitializeFastMutex(&g_lock);

    UNICODE_STRING name, link;
    RtlInitUnicodeString(&name, HWGPU_DEVICE_NAME);
    RtlInitUnicodeString(&link, HWGPU_LINK_NAME);
    PDEVICE_OBJECT dev;
    NTSTATUS st = IoCreateDevice(driver, 0, &name, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, FALSE, &dev);
    if (NT_SUCCESS(st)) st = IoCreateSymbolicLink(&link, &name);
    if (!NT_SUCCESS(st)) {
        if (driver->DeviceObject) IoDeleteDevice(driver->DeviceObject);
        MmUnmapIoSpace(g_ring, HWGPU_RING_BYTES);
        g_ring = NULL;
        return st;
    }
    driver->MajorFunction[IRP_MJ_CREATE] = on_create;
    driver->MajorFunction[IRP_MJ_CLEANUP] = on_cleanup;
    driver->MajorFunction[IRP_MJ_CLOSE] = on_close;
    driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = on_ioctl;
    driver->DriverUnload = unload;
    dev->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
