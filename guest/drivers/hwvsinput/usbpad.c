/* Players as USB Xbox 360 controllers or as DualShock 4s (gamepad/README.md).
 *
 * The pad device hwvsinput.sys reports is a small bus: each connected player is a child device. As
 * an Xbox 360 controller it looks like a wired one on USB (USB\VID_045E&PID_028E), so Windows' own
 * xusb22.sys binds to it. Player 1 can be a DualShock 4 instead (HWPAD\VID_054C&PID_09CC), run by hwhid.sys,
 * whose HID reports carry the phone's motion for gyro aiming; then this file only does its PnP.
 * As an Xbox 360 controller: Then everything that knows the Xbox 360 controller sees it: XInput, but also
 * Windows.Gaming.Input, which Chrome, Edge and many newer games use and which takes only devices
 * xusb22 runs. This file answers the USB requests xusb22 sends (descriptors, configuration,
 * interrupt transfers) the way a real controller does; input reports go up as interrupt IN
 * transfers, and rumble comes back as interrupt OUT transfers.
 *
 * The descriptors, the start-up replies and the control-transfer answers follow ViGEmBus's
 * XusbPdo.cpp (Copyright (c) Nefarius Software Solutions e.U. and Contributors, BSD-3-Clause).
 */
#include <ntddk.h>
#include <usb.h>
#include <usbioctl.h>
#include <usbbusif.h>
#include "usbpad.h"

#define TAG 'pUWH'
#ifndef MdlMappingNoExecute
#define MdlMappingNoExecute 0x40000000
#endif

static const GUID usbdi_guid = {0xb1a96a13, 0x3de0, 0x4574, {0x9b, 0x1, 0xc0, 0x8f, 0xea, 0xb3, 0x18, 0xd6}};
/* GUID_DEVINTERFACE_USB_DEVICE: the USB hub gives every device it enumerates this interface. */
static const GUID usb_device_iface = {0xA5DCBF10, 0x6530, 0x11D2, {0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED}};
static const GUID bus_usb_guid = {0x9d7debbc, 0xc85d, 0x11d1, {0x9e, 0xb4, 0x00, 0x60, 0x08, 0xc3, 0xa1, 0x9a}};

static const UCHAR device_desc[18] = {
    0x12, 0x01, 0x00, 0x02, 0xFF, 0xFF, 0xFF, 0x08, 0x5E, 0x04, 0x8E, 0x02, 0x14, 0x01, 0x01, 0x02, 0x03, 0x01,
};

static const UCHAR config_desc[153] = {
    0x09, 0x02, 0x99, 0x00, 0x04, 0x01, 0x00, 0x80, 0xFA,   /* bus powered; no remote wakeup */
    /* interface 0: the controller, IN 0x81 and OUT 0x01 */
    0x09, 0x04, 0x00, 0x00, 0x02, 0xFF, 0x5D, 0x01, 0x00,
    0x11, 0x21, 0x00, 0x01, 0x01, 0x25, 0x81, 0x14, 0x00, 0x00, 0x00, 0x00, 0x13, 0x01, 0x08, 0x00, 0x00,
    0x07, 0x05, 0x81, 0x03, 0x20, 0x00, 0x04,
    0x07, 0x05, 0x01, 0x03, 0x20, 0x00, 0x08,
    /* interface 1: headset */
    0x09, 0x04, 0x01, 0x00, 0x04, 0xFF, 0x5D, 0x03, 0x00,
    0x1B, 0x21, 0x00, 0x01, 0x01, 0x01, 0x82, 0x40, 0x01, 0x02, 0x20, 0x16, 0x83, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x16, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x07, 0x05, 0x82, 0x03, 0x20, 0x00, 0x02,
    0x07, 0x05, 0x02, 0x03, 0x20, 0x00, 0x04,
    0x07, 0x05, 0x83, 0x03, 0x20, 0x00, 0x40,
    0x07, 0x05, 0x03, 0x03, 0x20, 0x00, 0x10,
    /* interface 2: plug-in module */
    0x09, 0x04, 0x02, 0x00, 0x01, 0xFF, 0x5D, 0x02, 0x00,
    0x09, 0x21, 0x00, 0x01, 0x01, 0x22, 0x84, 0x07, 0x00,
    0x07, 0x05, 0x84, 0x03, 0x20, 0x00, 0x10,
    /* interface 3: security */
    0x09, 0x04, 0x03, 0x00, 0x00, 0xFF, 0xFD, 0x13, 0x04,
    0x06, 0x41, 0x00, 0x01, 0x01, 0x03,
};

/* What the controller answers on its data pipe right after start, before any input. */
static const UCHAR start_replies[6][20] = {
    {0x01, 0x03, 0x0E}, {0x02, 0x03, 0x00}, {0x03, 0x03, 0x03}, {0x08, 0x03, 0x00},
    {0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0xe4, 0xf2, 0xb3, 0xf8, 0x49, 0xf3, 0xb0, 0xfc},
    {0x01, 0x03, 0x03},
};
static const UCHAR start_len[6] = {3, 3, 3, 3, 20, 3};

/* Endpoint, interval, for each interface's pipes as a real controller sets them up. */
static const UCHAR pipes[4][4][2] = {
    {{0x81, 4}, {0x01, 8}},
    {{0x82, 4}, {0x02, 8}, {0x83, 8}, {0x03, 8}},
    {{0x84, 4}},
    {{0}},
};
static const UCHAR pipe_count[4] = {2, 4, 1, 0};
static const UCHAR iface_protocol[4][3] = {{0xFF, 0x5D, 0x01}, {0xFF, 0x5D, 0x03}, {0xFF, 0x5D, 0x02}, {0xFF, 0xFD, 0x13}};

typedef struct {
    ULONG slot;
    BOOLEAN ds4;                   /* a DualShock 4 for hwhid.sys, not an Xbox 360 controller */
    PDEVICE_OBJECT self;
    BOOLEAN present;               /* in the bus's relations; once not, PnP removes it */
    BOOLEAN deleted;
    UCHAR stage;                   /* start-up replies given on the data pipe */
    BOOLEAN said_83;
    BOOLEAN fresh;                 /* report not yet taken by a read */
    UCHAR report[20];
    LIST_ENTRY reads;              /* interrupt IN transfers on 0x81, waiting for input */
    LIST_ENTRY held;               /* IN transfers on the other pipes, which never get data */
    UNICODE_STRING iface;          /* its GUID_DEVINTERFACE_USB_DEVICE, as a hub would register */
    KSPIN_LOCK lock;
} CHILD;

static struct {
    PDEVICE_OBJECT bus;            /* the pad device's PDO, whose children these are */
    PDRIVER_OBJECT driver;
    CHILD *child[USBPAD_SLOTS];
    void (*rumble)(ULONG slot, UCHAR big, UCHAR small);
    KSPIN_LOCK lock;
} u;

void usbpad_init(PDRIVER_OBJECT driver, void (*rumble)(ULONG, UCHAR, UCHAR))
{
    u.driver = driver;
    u.rumble = rumble;
    KeInitializeSpinLock(&u.lock);
}

void usbpad_set_bus(PDEVICE_OBJECT pdo) { u.bus = pdo; }

static CHILD *child_of(PDEVICE_OBJECT d)
{
    for (int i = 0; i < USBPAD_SLOTS; i++)
        if (u.child[i] && u.child[i]->self == d) return u.child[i];
    return NULL;
}

BOOLEAN usbpad_is_child(PDEVICE_OBJECT d) { return child_of(d) != NULL; }

static NTSTATUS finish(PIRP irp, NTSTATUS status, ULONG_PTR info)
{
    irp->IoStatus.Status = status;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

static PURB urb_of(PIRP irp) { return IoGetCurrentIrpStackLocation(irp)->Parameters.Others.Argument1; }

static PVOID transfer_buffer(PVOID buffer, PMDL mdl)
{
    return buffer ? buffer : mdl ? MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority | MdlMappingNoExecute) : NULL;
}

static NTSTATUS urb_done(PIRP irp, PURB urb, NTSTATUS status)
{
    urb->UrbHeader.Status = NT_SUCCESS(status) ? USBD_STATUS_SUCCESS : USBD_STATUS_STALL_PID;
    return finish(irp, status, 0);
}

/* ---- interrupt IN transfers that wait ---- */

static VOID cancel_wait(PDEVICE_OBJECT device, PIRP irp)
{
    CHILD *c = child_of(device);
    KIRQL irql;
    IoReleaseCancelSpinLock(irp->CancelIrql);
    if (c) {
        irql = KeAcquireSpinLockRaiseToDpc(&c->lock);
        RemoveEntryList(&irp->Tail.Overlay.ListEntry);
        KeReleaseSpinLock(&c->lock, irql);
    }
    urb_of(irp)->UrbHeader.Status = USBD_STATUS_CANCELED;
    finish(irp, STATUS_CANCELLED, 0);
}

static NTSTATUS wait(CHILD *c, LIST_ENTRY *list, PIRP irp)
{
    KIRQL irql = KeAcquireSpinLockRaiseToDpc(&c->lock);
    IoMarkIrpPending(irp);
    InsertTailList(list, &irp->Tail.Overlay.ListEntry);
    IoSetCancelRoutine(irp, cancel_wait);
    if (irp->Cancel && IoSetCancelRoutine(irp, NULL)) {
        RemoveEntryList(&irp->Tail.Overlay.ListEntry);
        KeReleaseSpinLock(&c->lock, irql);
        urb_of(irp)->UrbHeader.Status = USBD_STATUS_CANCELED;
        finish(irp, STATUS_CANCELLED, 0);
        return STATUS_PENDING;
    }
    KeReleaseSpinLock(&c->lock, irql);
    return STATUS_PENDING;
}

static PIRP take(CHILD *c, LIST_ENTRY *list)
{
    PIRP irp = NULL;
    while (!IsListEmpty(list)) {
        PIRP r = CONTAINING_RECORD(list->Flink, IRP, Tail.Overlay.ListEntry);
        RemoveEntryList(&r->Tail.Overlay.ListEntry);
        if (IoSetCancelRoutine(r, NULL)) { irp = r; break; }
        InitializeListHead(&r->Tail.Overlay.ListEntry);   /* being cancelled: its routine removes nothing */
    }
    (void)c;
    return irp;
}

/* Hands the newest report to a waiting read, if there is one. Called with c->lock held; the
 * request completes after it is released. */
static PIRP deliver_locked(CHILD *c)
{
    PIRP irp;
    if (!c->fresh || !(irp = take(c, &c->reads))) return NULL;
    PURB urb = urb_of(irp);
    PUCHAR b = transfer_buffer(urb->UrbBulkOrInterruptTransfer.TransferBuffer, urb->UrbBulkOrInterruptTransfer.TransferBufferMDL);
    ULONG n = urb->UrbBulkOrInterruptTransfer.TransferBufferLength < 20 ? urb->UrbBulkOrInterruptTransfer.TransferBufferLength : 20;
    if (b) RtlCopyMemory(b, c->report, n);
    urb->UrbBulkOrInterruptTransfer.TransferBufferLength = n;
    c->fresh = FALSE;
    return irp;
}

static void cancel_all(CHILD *c)
{
    for (;;) {
        KIRQL irql = KeAcquireSpinLockRaiseToDpc(&c->lock);
        PIRP irp = take(c, &c->reads);
        if (!irp) irp = take(c, &c->held);
        KeReleaseSpinLock(&c->lock, irql);
        if (!irp) return;
        urb_of(irp)->UrbHeader.Status = USBD_STATUS_CANCELED;
        finish(irp, STATUS_CANCELLED, 0);
    }
}

/* ---- the USB requests xusb22 sends ---- */

static NTSTATUS interrupt_transfer(CHILD *c, PIRP irp, PURB urb)
{
    struct _URB_BULK_OR_INTERRUPT_TRANSFER *t = &urb->UrbBulkOrInterruptTransfer;
    ULONG pipe = (ULONG)(ULONG_PTR)t->PipeHandle & 0xff;
    PUCHAR b = transfer_buffer(t->TransferBuffer, t->TransferBufferMDL);
    if (t->TransferFlags & USBD_TRANSFER_DIRECTION_IN) {
        if (pipe == 0x81 && c->stage < 6) {
            ULONG n = start_len[c->stage] < t->TransferBufferLength ? start_len[c->stage] : t->TransferBufferLength;
            if (b) RtlCopyMemory(b, start_replies[c->stage], n);
            t->TransferBufferLength = n;
            c->stage++;
            return urb_done(irp, urb, STATUS_SUCCESS);
        }
        if (pipe == 0x83 && !c->said_83 && t->TransferBufferLength >= 3 && b) {
            b[0] = 0x05; b[1] = 0x03; b[2] = 0x00;
            t->TransferBufferLength = 3;
            c->said_83 = TRUE;
            return urb_done(irp, urb, STATUS_SUCCESS);
        }
        if (pipe != 0x81) return wait(c, &c->held, irp);
        NTSTATUS s = wait(c, &c->reads, irp);
        KIRQL irql = KeAcquireSpinLockRaiseToDpc(&c->lock);
        PIRP ready = deliver_locked(c);
        KeReleaseSpinLock(&c->lock, irql);
        if (ready) urb_done(ready, urb_of(ready), STATUS_SUCCESS);
        return s;
    }
    /* OUT: 01 03 nn sets the LED; 00 08 00 big small 00 00 00 sets the motors */
    if (b && t->TransferBufferLength == 8 && b[0] == 0x00 && u.rumble) u.rumble(c->slot, b[3], b[4]);
    return urb_done(irp, urb, STATUS_SUCCESS);
}

static NTSTATUS select_configuration(PIRP irp, PURB urb)
{
    struct _URB_SELECT_CONFIGURATION *s = &urb->UrbSelectConfiguration;
    PUCHAR end = (PUCHAR)urb + urb->UrbHeader.Length;
    PUSBD_INTERFACE_INFORMATION info = &s->Interface;
    if (!s->ConfigurationDescriptor) return urb_done(irp, urb, STATUS_SUCCESS);   /* unconfigure */
    s->ConfigurationHandle = (USBD_CONFIGURATION_HANDLE)(ULONG_PTR)0xFFFF0000;
    for (int i = 0; i < 4 && (PUCHAR)info + FIELD_OFFSET(USBD_INTERFACE_INFORMATION, Pipes) <= end; i++) {
        info->Class = iface_protocol[i][0];
        info->SubClass = iface_protocol[i][1];
        info->Protocol = iface_protocol[i][2];
        info->InterfaceHandle = (USBD_INTERFACE_HANDLE)(ULONG_PTR)0xFFFF0000;
        for (ULONG p = 0; p < pipe_count[i] && p < info->NumberOfPipes; p++) {
            USBD_PIPE_INFORMATION *pi = &info->Pipes[p];
            if ((PUCHAR)(pi + 1) > end) break;
            pi->MaximumTransferSize = 0x00400000;
            pi->MaximumPacketSize = 0x20;
            pi->EndpointAddress = pipes[i][p][0];
            pi->Interval = pipes[i][p][1];
            pi->PipeType = UsbdPipeTypeInterrupt;
            pi->PipeHandle = (USBD_PIPE_HANDLE)(ULONG_PTR)(0xFFFF0000 | pipes[i][p][0]);
            pi->PipeFlags = 0;
        }
        if (!info->Length) break;
        info = (PUSBD_INTERFACE_INFORMATION)((PUCHAR)info + info->Length);
    }
    return urb_done(irp, urb, STATUS_SUCCESS);
}

static NTSTATUS select_interface(PIRP irp, PURB urb)
{
    PUSBD_INTERFACE_INFORMATION info = &urb->UrbSelectInterface.Interface;
    ULONG i = info->InterfaceNumber;
    if (i != 1 && i != 2) return urb_done(irp, urb, STATUS_INVALID_PARAMETER);
    info->Class = iface_protocol[i][0];
    info->SubClass = iface_protocol[i][1];
    info->Protocol = iface_protocol[i][2];
    info->InterfaceHandle = (USBD_INTERFACE_HANDLE)(ULONG_PTR)0xFFFF0000;
    info->NumberOfPipes = pipe_count[i];
    for (ULONG p = 0; p < pipe_count[i]; p++) {
        USBD_PIPE_INFORMATION *pi = &info->Pipes[p];
        pi->MaximumTransferSize = 0x00400000;
        pi->MaximumPacketSize = 0x20;
        pi->EndpointAddress = pipes[i][p][0];
        pi->Interval = pipes[i][p][1];
        pi->PipeType = UsbdPipeTypeInterrupt;
        pi->PipeHandle = (USBD_PIPE_HANDLE)(ULONG_PTR)(0xFFFF0000 | pipes[i][p][0]);
        pi->PipeFlags = 0;
    }
    return urb_done(irp, urb, STATUS_SUCCESS);
}

static NTSTATUS get_descriptor(PIRP irp, PURB urb)
{
    struct _URB_CONTROL_DESCRIPTOR_REQUEST *d = &urb->UrbControlDescriptorRequest;
    PUCHAR b = transfer_buffer(d->TransferBuffer, d->TransferBufferMDL);
    const UCHAR *src;
    ULONG size;
    if (d->DescriptorType == USB_DEVICE_DESCRIPTOR_TYPE) src = device_desc, size = sizeof device_desc;
    else if (d->DescriptorType == USB_CONFIGURATION_DESCRIPTOR_TYPE) src = config_desc, size = sizeof config_desc;
    else return urb_done(irp, urb, STATUS_NOT_IMPLEMENTED);   /* strings: none, as on ViGEm's */
    if (!b) return urb_done(irp, urb, STATUS_INVALID_PARAMETER);
    if (size > d->TransferBufferLength) size = d->TransferBufferLength;
    RtlCopyMemory(b, src, size);
    d->TransferBufferLength = size;
    return urb_done(irp, urb, STATUS_SUCCESS);
}

static NTSTATUS control_transfer(PIRP irp, PURB urb)
{
    struct _URB_CONTROL_TRANSFER *t = &urb->UrbControlTransfer;
    PUCHAR b = transfer_buffer(t->TransferBuffer, t->TransferBufferMDL);
    switch (t->SetupPacket[6]) {   /* by the request's length, as ViGEm tells them apart */
    case 0x04:                     /* the "Xenon" check */
        if (b && t->TransferBufferLength >= 4) { b[0] = 0x31; b[1] = 0x3F; b[2] = 0xCF; b[3] = 0xDC; t->TransferBufferLength = 4; }
        return urb_done(irp, urb, STATUS_SUCCESS);
    case 0x14:
    case 0x08:
        return urb_done(irp, urb, STATUS_UNSUCCESSFUL);   /* must fail, like a real one */
    default:
        return urb_done(irp, urb, STATUS_SUCCESS);
    }
}

static NTSTATUS submit_urb(CHILD *c, PIRP irp)
{
    PURB urb = urb_of(irp);
    switch (urb->UrbHeader.Function) {
    case URB_FUNCTION_SELECT_CONFIGURATION: return select_configuration(irp, urb);
    case URB_FUNCTION_SELECT_INTERFACE: return select_interface(irp, urb);
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE: return get_descriptor(irp, urb);
    case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER: return interrupt_transfer(c, irp, urb);
    case URB_FUNCTION_CONTROL_TRANSFER: return control_transfer(irp, urb);
    case URB_FUNCTION_GET_STATUS_FROM_DEVICE: {
        PUCHAR b = transfer_buffer(urb->UrbControlGetStatusRequest.TransferBuffer, urb->UrbControlGetStatusRequest.TransferBufferMDL);
        if (b && urb->UrbControlGetStatusRequest.TransferBufferLength >= 2) b[0] = b[1] = 0;
        return urb_done(irp, urb, STATUS_SUCCESS);
    }
    case URB_FUNCTION_ABORT_PIPE:
        cancel_all(c);
        return urb_done(irp, urb, STATUS_SUCCESS);
    case URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL:
    case 0x0030:   /* URB_FUNCTION_SYNC_RESET_PIPE */
    case 0x0031:   /* URB_FUNCTION_SYNC_CLEAR_STALL */
        return urb_done(irp, urb, STATUS_SUCCESS);
    default:
        return urb_done(irp, urb, STATUS_INVALID_PARAMETER);
    }
}

static NTSTATUS child_ioctl(CHILD *c, PIRP irp)
{
    PIO_STACK_LOCATION st = IoGetCurrentIrpStackLocation(irp);
    switch (st->Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_INTERNAL_USB_SUBMIT_URB:
        return submit_urb(c, irp);
    case IOCTL_INTERNAL_USB_GET_PORT_STATUS:
        if (st->Parameters.Others.Argument1) *(PULONG)st->Parameters.Others.Argument1 = USBD_PORT_ENABLED | USBD_PORT_CONNECTED;
        return finish(irp, STATUS_SUCCESS, 0);
    case IOCTL_INTERNAL_USB_RESET_PORT:
        return finish(irp, STATUS_SUCCESS, 0);
    default:
        return finish(irp, STATUS_NOT_SUPPORTED, 0);
    }
}

/* ---- the bus interface xusb22's framework asks for ---- */

static VOID WINAPI if_ref(PVOID ctx) { (void)ctx; }
static VOID WINAPI usbdi_version(PVOID ctx, PUSBD_VERSION_INFORMATION v, PULONG caps)
{
    (void)ctx;
    if (v) { v->USBDI_Version = 0x500; v->Supported_USB_Version = 0x200; }
    if (caps) *caps = 0;
}
static NTSTATUS WINAPI bus_time(PVOID ctx, PULONG frame) { (void)ctx; (void)frame; return STATUS_UNSUCCESSFUL; }
static NTSTATUS WINAPI iso_out(PVOID ctx, PURB urb) { (void)ctx; (void)urb; return STATUS_UNSUCCESSFUL; }
static NTSTATUS WINAPI bus_info(PVOID ctx, ULONG level, PVOID buf, PULONG len, PULONG actual)
{
    (void)ctx; (void)level; (void)buf; (void)len; (void)actual;
    return STATUS_UNSUCCESSFUL;
}
static BOOLEAN WINAPI high_speed(PVOID ctx) { (void)ctx; return TRUE; }

static NTSTATUS query_interface(CHILD *c, PIRP irp)
{
    PIO_STACK_LOCATION st = IoGetCurrentIrpStackLocation(irp);
    USB_BUS_INTERFACE_USBDI_V1 *i = (USB_BUS_INTERFACE_USBDI_V1 *)st->Parameters.QueryInterface.Interface;
    if (RtlCompareMemory(st->Parameters.QueryInterface.InterfaceType, &usbdi_guid, sizeof(GUID)) != sizeof(GUID) ||
        st->Parameters.QueryInterface.Version > USB_BUSIF_USBDI_VERSION_1 ||
        st->Parameters.QueryInterface.Size < sizeof *i)
        return finish(irp, irp->IoStatus.Status, irp->IoStatus.Information);
    RtlZeroMemory(i, sizeof *i);
    i->Size = sizeof *i;
    i->Version = USB_BUSIF_USBDI_VERSION_1;
    i->BusContext = c;
    i->InterfaceReference = if_ref;
    i->InterfaceDereference = if_ref;
    i->GetUSBDIVersion = usbdi_version;
    i->QueryBusTime = bus_time;
    i->SubmitIsoOutUrb = iso_out;
    i->QueryBusInformation = bus_info;
    i->IsDeviceHighSpeed = high_speed;
    return finish(irp, STATUS_SUCCESS, 0);
}

/* ---- PnP for the children: the bus driver's part ---- */

static PWCHAR pool_string(const WCHAR *s, ULONG bytes)
{
    PWCHAR p = ExAllocatePoolWithTag(PagedPool, bytes, TAG);
    if (p) RtlCopyMemory(p, s, bytes);
    return p;
}

static NTSTATUS query_id(CHILD *c, PIRP irp)
{
    static const WCHAR dev_id[] = L"USB\\VID_045E&PID_028E";
    static const WCHAR hw_ids[] = L"USB\\VID_045E&PID_028E&REV_0114\0USB\\VID_045E&PID_028E\0";
    static const WCHAR compat_ids[] = L"USB\\MS_COMP_XUSB10\0USB\\Class_FF&SubClass_5D&Prot_01\0USB\\Class_FF&SubClass_5D\0USB\\Class_FF\0";
    static const WCHAR ds4_id[] = L"HWPAD\\VID_054C&PID_09CC";
    static const WCHAR ds4_hw[] = L"HWPAD\\VID_054C&PID_09CC\0";
    WCHAR inst[] = L"HWPAD0";
    PWCHAR p = NULL;
    if (c->ds4) {
        switch (IoGetCurrentIrpStackLocation(irp)->Parameters.QueryId.IdType) {
        case BusQueryDeviceID: p = pool_string(ds4_id, sizeof ds4_id); break;
        case BusQueryHardwareIDs: p = pool_string(ds4_hw, sizeof ds4_hw); break;
        case BusQueryInstanceID: inst[2] = L'4'; inst[3] = L'D'; inst[4] = L'S'; inst[5] = (WCHAR)(L'1' + c->slot); p = pool_string(inst, sizeof inst); break;
        default: return finish(irp, irp->IoStatus.Status, irp->IoStatus.Information);
        }
        return p ? finish(irp, STATUS_SUCCESS, (ULONG_PTR)p) : finish(irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    }
    switch (IoGetCurrentIrpStackLocation(irp)->Parameters.QueryId.IdType) {
    case BusQueryDeviceID: p = pool_string(dev_id, sizeof dev_id); break;
    case BusQueryHardwareIDs: p = pool_string(hw_ids, sizeof hw_ids); break;
    case BusQueryCompatibleIDs: p = pool_string(compat_ids, sizeof compat_ids); break;
    case BusQueryInstanceID: inst[5] = (WCHAR)(L'1' + c->slot); p = pool_string(inst, sizeof inst); break;
    default: return finish(irp, irp->IoStatus.Status, irp->IoStatus.Information);
    }
    return p ? finish(irp, STATUS_SUCCESS, (ULONG_PTR)p) : finish(irp, STATUS_INSUFFICIENT_RESOURCES, 0);
}

static NTSTATUS child_pnp(CHILD *c, PIRP irp)
{
    PIO_STACK_LOCATION st = IoGetCurrentIrpStackLocation(irp);
    switch (st->MinorFunction) {
    case IRP_MN_QUERY_ID:
        return query_id(c, irp);
    case IRP_MN_QUERY_DEVICE_TEXT: {
        static const WCHAR desc[] = L"HyperWin Xbox 360 Controller", ds4[] = L"HyperWin DualShock 4", where[] = L"HyperWin";
        if (st->Parameters.QueryDeviceText.DeviceTextType == DeviceTextDescription)
            return finish(irp, STATUS_SUCCESS, (ULONG_PTR)(c->ds4 ? pool_string(ds4, sizeof ds4) : pool_string(desc, sizeof desc)));
        if (st->Parameters.QueryDeviceText.DeviceTextType == DeviceTextLocationInformation)
            return finish(irp, STATUS_SUCCESS, (ULONG_PTR)pool_string(where, sizeof where));
        return finish(irp, irp->IoStatus.Status, irp->IoStatus.Information);
    }
    case IRP_MN_QUERY_CAPABILITIES: {
        PDEVICE_CAPABILITIES caps = st->Parameters.DeviceCapabilities.Capabilities;
        caps->Removable = TRUE;
        caps->SurpriseRemovalOK = TRUE;
        caps->UniqueID = FALSE;
        caps->Address = c->slot;
        caps->UINumber = c->slot;
        caps->DeviceState[PowerSystemWorking] = PowerDeviceD0;
        for (int s = PowerSystemSleeping1; s < PowerSystemMaximum; s++) caps->DeviceState[s] = PowerDeviceD3;
        caps->DeviceWake = PowerDeviceUnspecified;
        caps->SystemWake = PowerSystemUnspecified;
        return finish(irp, STATUS_SUCCESS, 0);
    }
    case IRP_MN_QUERY_BUS_INFORMATION: {
        PPNP_BUS_INFORMATION bi = ExAllocatePoolWithTag(PagedPool, sizeof *bi, TAG);
        if (!bi) return finish(irp, STATUS_INSUFFICIENT_RESOURCES, 0);
        bi->BusTypeGuid = bus_usb_guid;
        bi->LegacyBusType = PNPBus;
        bi->BusNumber = 0;
        return finish(irp, STATUS_SUCCESS, (ULONG_PTR)bi);
    }
    case IRP_MN_QUERY_DEVICE_RELATIONS:
        if (st->Parameters.QueryDeviceRelations.Type == TargetDeviceRelation) {
            PDEVICE_RELATIONS r = ExAllocatePoolWithTag(PagedPool, sizeof *r, TAG);
            if (!r) return finish(irp, STATUS_INSUFFICIENT_RESOURCES, 0);
            r->Count = 1;
            r->Objects[0] = c->self;
            ObReferenceObject(c->self);
            return finish(irp, STATUS_SUCCESS, (ULONG_PTR)r);
        }
        return finish(irp, irp->IoStatus.Status, irp->IoStatus.Information);
    case IRP_MN_QUERY_INTERFACE:
        if (c->ds4) return finish(irp, irp->IoStatus.Status, irp->IoStatus.Information);
        return query_interface(c, irp);
    case IRP_MN_START_DEVICE:
        c->stage = 0;
        c->said_83 = FALSE;
        if (!c->ds4 && !c->iface.Buffer && !NT_SUCCESS(IoRegisterDeviceInterface(c->self, &usb_device_iface, NULL, &c->iface)))
            RtlZeroMemory(&c->iface, sizeof c->iface);
        if (c->iface.Buffer) IoSetDeviceInterfaceState(&c->iface, TRUE);
        return finish(irp, STATUS_SUCCESS, 0);
    case IRP_MN_STOP_DEVICE:
    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_QUERY_REMOVE_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:
        if (st->MinorFunction == IRP_MN_SURPRISE_REMOVAL) cancel_all(c);
        return finish(irp, STATUS_SUCCESS, 0);
    case IRP_MN_REMOVE_DEVICE:
        cancel_all(c);
        if (c->iface.Buffer) IoSetDeviceInterfaceState(&c->iface, FALSE);
        if (!c->present) {          /* gone from the bus: the device object goes too */
            KIRQL irql = KeAcquireSpinLockRaiseToDpc(&u.lock);
            u.child[c->slot] = NULL;
            KeReleaseSpinLock(&u.lock, irql);
            if (c->iface.Buffer) RtlFreeUnicodeString(&c->iface);
            finish(irp, STATUS_SUCCESS, 0);
            IoDeleteDevice(c->self);
            return STATUS_SUCCESS;
        }
        return finish(irp, STATUS_SUCCESS, 0);
    case IRP_MN_QUERY_RESOURCES:
    case IRP_MN_QUERY_RESOURCE_REQUIREMENTS:
    default:
        return finish(irp, irp->IoStatus.Status, irp->IoStatus.Information);
    }
}

NTSTATUS usbpad_dispatch(PDEVICE_OBJECT device, PIRP irp)
{
    CHILD *c = child_of(device);
    PIO_STACK_LOCATION st = IoGetCurrentIrpStackLocation(irp);
    switch (st->MajorFunction) {
    case IRP_MJ_PNP:
        return child_pnp(c, irp);
    case IRP_MJ_POWER:
        PoStartNextPowerIrp(irp);
        /* No waking: a wait-wake completed at once would read as "the device woke up". */
        if (st->MinorFunction == IRP_MN_WAIT_WAKE) return finish(irp, STATUS_NOT_SUPPORTED, 0);
        return finish(irp, STATUS_SUCCESS, 0);
    case IRP_MJ_INTERNAL_DEVICE_CONTROL:
        if (c->ds4) return finish(irp, STATUS_NOT_SUPPORTED, 0);
        return child_ioctl(c, irp);
    case IRP_MJ_CREATE:
    case IRP_MJ_CLOSE:
        return finish(irp, STATUS_SUCCESS, 0);
    default:
        return finish(irp, irp->IoStatus.Status, irp->IoStatus.Information);
    }
}

/* The bus's children, for IRP_MN_QUERY_DEVICE_RELATIONS (BusRelations) on the pad device. */
PDEVICE_RELATIONS usbpad_relations(void)
{
    ULONG n = 0;
    PDEVICE_RELATIONS r = ExAllocatePoolWithTag(PagedPool, sizeof(DEVICE_RELATIONS) + USBPAD_SLOTS * sizeof(PDEVICE_OBJECT), TAG);
    if (!r) return NULL;
    KIRQL irql = KeAcquireSpinLockRaiseToDpc(&u.lock);
    for (int i = 0; i < USBPAD_SLOTS; i++) {
        if (!u.child[i] || !u.child[i]->present) continue;
        r->Objects[n++] = u.child[i]->self;
        ObReferenceObject(u.child[i]->self);
    }
    KeReleaseSpinLock(&u.lock, irql);
    r->Count = n;
    return r;
}

/* A player's state from the phone, at PASSIVE_LEVEL: plugs the controller in or out, and hands
 * xusb22 the new report. */
void usbpad_update(ULONG slot, BOOLEAN ds4, BOOLEAN connected, USHORT buttons, UCHAR lt, UCHAR rt, SHORT lx, SHORT ly,
                   SHORT rx, SHORT ry)
{
    CHILD *c;
    BOOLEAN changed = FALSE;
    if (slot >= USBPAD_SLOTS || !u.bus) return;
    c = u.child[slot];
    if (c && c->ds4 != ds4) {      /* the other kind: unplug it; the new one comes once PnP removed it */
        if (c->present) {
            c->present = FALSE;
            IoInvalidateDeviceRelations(u.bus, BusRelations);
        }
        return;
    }
    if (connected && !c) {
        PDEVICE_OBJECT d;
        if (!NT_SUCCESS(IoCreateDevice(u.driver, sizeof(CHILD), NULL, FILE_DEVICE_BUS_EXTENDER,
                                       FILE_AUTOGENERATED_DEVICE_NAME | FILE_DEVICE_SECURE_OPEN, FALSE, &d))) return;
        c = d->DeviceExtension;
        RtlZeroMemory(c, sizeof *c);
        c->slot = slot;
        c->ds4 = ds4;
        c->self = d;
        c->present = TRUE;
        InitializeListHead(&c->reads);
        InitializeListHead(&c->held);
        KeInitializeSpinLock(&c->lock);
        d->Flags |= DO_POWER_PAGABLE;
        d->Flags &= ~DO_DEVICE_INITIALIZING;
        u.child[slot] = c;
        changed = TRUE;
    } else if (connected && c && !c->present) {
        c->present = TRUE;                 /* back before PnP took the old one away */
        changed = TRUE;
    } else if (!connected && c && c->present) {
        c->present = FALSE;
        changed = TRUE;
    }
    if (changed) IoInvalidateDeviceRelations(u.bus, BusRelations);
    if (!connected || !c || ds4) return;   /* a DualShock 4's reports come from hwhid.sys */
    UCHAR r[20] = {0x00, 0x14};
    r[2] = (UCHAR)buttons; r[3] = (UCHAR)(buttons >> 8);
    r[4] = lt; r[5] = rt;
    r[6] = (UCHAR)lx; r[7] = (UCHAR)((USHORT)lx >> 8);
    r[8] = (UCHAR)ly; r[9] = (UCHAR)((USHORT)ly >> 8);
    r[10] = (UCHAR)rx; r[11] = (UCHAR)((USHORT)rx >> 8);
    r[12] = (UCHAR)ry; r[13] = (UCHAR)((USHORT)ry >> 8);
    KIRQL irql = KeAcquireSpinLockRaiseToDpc(&c->lock);
    RtlCopyMemory(c->report, r, sizeof r);
    c->fresh = TRUE;
    PIRP ready = deliver_locked(c);
    KeReleaseSpinLock(&c->lock, irql);
    if (ready) urb_done(ready, urb_of(ready), STATUS_SUCCESS);
}
