/* HyperWin phone storage mapper: maps the storage slots in the shared window (include/hw_files.h)
 * into the SYSTEM service hwfiles.exe.
 *
 * Separate from hwgpu.sys so GPU/display updates and phone storage updates are independent.
 * A legacy (non-PnP) kernel driver: one control device, auto start, ntoskrnl imports only.
 */
#include <ntddk.h>
#include "hw_abi.h"
#define HWGPU_KERNEL
#include "hwgpu_ring.h"
#include "hw_files.h"

_Static_assert(HW_FRAME_BIG + 2ull * HW_MAX_WIDTH * HW_MAX_HEIGHT * 4ull <= HWF_OFF,
               "file slots start above the largest frame pair");
_Static_assert(HWF_OFF + HWF_BYTES <= HWGPU_RING_OFF,
               "file slots end below the GPU rings");

#define TAG 'flWH'
#ifndef MdlMappingNoExecute
#define MdlMappingNoExecute 0x40000000
#endif

struct open_ctx {
    PMDL mdl;
    PVOID user;
    PEPROCESS process;
};

static PVOID g_files;   /* kernel mapping of the storage slots, HWF_OFF in the window */
static FAST_MUTEX g_lock;

static NTSTATUS done(PIRP irp, NTSTATUS st, ULONG_PTR info)
{
    irp->IoStatus.Status = st;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

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

static NTSTATUS on_create(PDEVICE_OBJECT dev, PIRP irp)
{
    (void)dev;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    struct open_ctx *c = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*c), TAG);
    if (!c) return done(irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    RtlZeroMemory(c, sizeof(*c));
    s->FileObject->FsContext = c;
    return done(irp, STATUS_SUCCESS, 0);
}

static NTSTATUS on_cleanup(PDEVICE_OBJECT dev, PIRP irp)
{
    (void)dev;
    struct open_ctx *c = IoGetCurrentIrpStackLocation(irp)->FileObject->FsContext;
    ExAcquireFastMutex(&g_lock);
    if (c && c->user) {
        if (c->process == PsGetCurrentProcess()) MmUnmapLockedPages(c->user, c->mdl);
        if (c->mdl) IoFreeMdl(c->mdl);
        c->user = NULL;
        c->mdl = NULL;
        c->process = NULL;
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

static NTSTATUS on_ioctl(PDEVICE_OBJECT dev, PIRP irp)
{
    (void)dev;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    ULONG code = s->Parameters.DeviceIoControl.IoControlCode;
    ULONG out_len = s->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID buf = irp->AssociatedIrp.SystemBuffer;
    struct open_ctx *c = s->FileObject->FsContext;
    if (irp->RequestorMode != UserMode) return done(irp, STATUS_ACCESS_DENIED, 0);
    if (code != IOCTL_HWFILES_MAP) return done(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    if (out_len < sizeof(struct hwfiles_map_out)) return done(irp, STATUS_BUFFER_TOO_SMALL, 0);
    LUID tcb = {7, 0};  /* SE_TCB_PRIVILEGE: only SYSTEM holds it */
    if (!SeSinglePrivilegeCheck(tcb, UserMode)) return done(irp, STATUS_PRIVILEGE_NOT_HELD, 0);
    struct hwfiles_map_out out;
    NTSTATUS st = STATUS_SUCCESS;
    ExAcquireFastMutex(&g_lock);
    if (!g_files) st = STATUS_DEVICE_NOT_READY;
    else if (c->user) st = STATUS_DEVICE_BUSY;
    else if (!(c->user = map_user(g_files, HWF_BYTES, &c->mdl))) st = STATUS_INSUFFICIENT_RESOURCES;
    else c->process = PsGetCurrentProcess();
    out.user = (uint64_t)(ULONG_PTR)c->user;
    ExReleaseFastMutex(&g_lock);
    if (!NT_SUCCESS(st)) return done(irp, st, 0);
    RtlCopyMemory(buf, &out, sizeof(out));
    return done(irp, STATUS_SUCCESS, sizeof(out));
}

static void unload(PDRIVER_OBJECT driver)
{
    UNICODE_STRING link;
    RtlInitUnicodeString(&link, HWFILES_LINK_NAME);
    IoDeleteSymbolicLink(&link);
    if (driver->DeviceObject) IoDeleteDevice(driver->DeviceObject);
    if (g_files) MmUnmapIoSpace(g_files, HWF_BYTES);
    g_files = NULL;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry)
{
    (void)registry;
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = (LONGLONG)(HW_WINDOW_PA + HWF_OFF);
    g_files = MmMapIoSpace(pa, HWF_BYTES, MmCached);
    if (!g_files) return STATUS_INSUFFICIENT_RESOURCES;
    ExInitializeFastMutex(&g_lock);

    UNICODE_STRING name, link;
    RtlInitUnicodeString(&name, HWFILES_DEVICE_NAME);
    RtlInitUnicodeString(&link, HWFILES_LINK_NAME);
    PDEVICE_OBJECT dev;
    NTSTATUS st = IoCreateDevice(driver, 0, &name, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, FALSE, &dev);
    if (NT_SUCCESS(st)) st = IoCreateSymbolicLink(&link, &name);
    if (!NT_SUCCESS(st)) {
        if (driver->DeviceObject) IoDeleteDevice(driver->DeviceObject);
        MmUnmapIoSpace(g_files, HWF_BYTES);
        g_files = NULL;
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
