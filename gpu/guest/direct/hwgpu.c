/* HyperWin GPU, direct variant: hands each Windows process its own GPU ring channel.
 *
 * Not a display driver. Windows sees no GPU. The Vulkan driver (vulkan_hwgpu.dll, Mesa's gfxstream
 * encoder) opens \\.\HwGpu, asks for a channel, and from then on writes gfxstream straight into
 * shared RAM that the phone reads. No system call per command, nothing in between.
 *
 * The kernel part only claims, maps and frees. A legacy (non-PnP) driver: one control device,
 * demand or auto start, ntoskrnl imports only.
 */
#include <ntddk.h>
#include "hw_abi.h"
#define HWGPU_KERNEL
#include "hwgpu_ring.h"
#include "hwgpu_ioctl.h"

#define TAG 'upGH'
#ifndef MdlMappingNoExecute
#define MdlMappingNoExecute 0x40000000
#endif

struct open_ctx {
    int slot;           /* -1 until IOCTL_HWGPU_OPEN */
    PMDL mdl;
    PVOID user;         /* mapping in the process that asked */
    PEPROCESS process;
};

static PVOID g_ring;    /* kernel mapping of the ring area, offset HWGPU_RING_OFF in the window */
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
    if (c && c->slot >= 0) {
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

static NTSTATUS on_ioctl(PDEVICE_OBJECT dev, PIRP irp)
{
    (void)dev;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    if (s->Parameters.DeviceIoControl.IoControlCode != IOCTL_HWGPU_OPEN)
        return done(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    if (s->Parameters.DeviceIoControl.OutputBufferLength < sizeof(struct hwgpu_open_out))
        return done(irp, STATUS_BUFFER_TOO_SMALL, 0);
    /* The mapping goes into the current process, so only take calls made from user mode directly. */
    if (irp->RequestorMode != UserMode) return done(irp, STATUS_ACCESS_DENIED, 0);
    struct hwgpu_open_out out;
    RtlZeroMemory(&out, sizeof(out));
    ExAcquireFastMutex(&g_lock);
    NTSTATUS st = open_channel(s->FileObject->FsContext, &out);
    ExReleaseFastMutex(&g_lock);
    if (!NT_SUCCESS(st)) return done(irp, st, 0);
    RtlCopyMemory(irp->AssociatedIrp.SystemBuffer, &out, sizeof(out));
    return done(irp, STATUS_SUCCESS, sizeof(out));
}

static void unload(PDRIVER_OBJECT driver)
{
    UNICODE_STRING link;
    RtlInitUnicodeString(&link, HWGPU_LINK_NAME);
    IoDeleteSymbolicLink(&link);
    if (driver->DeviceObject) IoDeleteDevice(driver->DeviceObject);
    if (g_ring) MmUnmapIoSpace(g_ring, HWGPU_RING_BYTES);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry)
{
    (void)registry;
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = (LONGLONG)(HW_WINDOW_PA + HWGPU_RING_OFF);
    g_ring = MmMapIoSpace(pa, HWGPU_RING_BYTES, MmCached);
    if (!g_ring) return STATUS_INSUFFICIENT_RESOURCES;
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
