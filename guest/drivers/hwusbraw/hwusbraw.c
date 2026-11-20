/* hwusbraw: raw access to the shared USB region from user mode, as a driver of its own.
 *
 * Not part of the GPU or display drivers on purpose: it is installed by hand for USB tests, starts on demand, and
 * can fail or be removed without the screen noticing. A legacy (non-PnP) driver: one control device, ntoskrnl
 * imports only. Only SYSTEM (SeTcb) may use it. USB passthrough is off by default; so is this driver.
 */
#include <ntddk.h>
#include "hw_abi.h"
#include "hw_usb.h"
#include "hwusbraw_ioctl.h"

static PVOID g_usb;   /* kernel mapping of the USB region, HW_USB_PA */

static NTSTATUS done(PIRP irp, NTSTATUS st, ULONG_PTR info)
{
    irp->IoStatus.Status = st;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

static NTSTATUS on_create(PDEVICE_OBJECT dev, PIRP irp) { (void)dev; return done(irp, STATUS_SUCCESS, 0); }

static NTSTATUS on_ioctl(PDEVICE_OBJECT dev, PIRP irp)
{
    (void)dev;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    ULONG in_len = s->Parameters.DeviceIoControl.InputBufferLength;
    ULONG out_len = s->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID buf = irp->AssociatedIrp.SystemBuffer;
    if (s->Parameters.DeviceIoControl.IoControlCode != IOCTL_HWUSBRAW_IO) return done(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    LUID tcb = {7, 0};   /* SE_TCB_PRIVILEGE: only SYSTEM holds it */
    if (irp->RequestorMode == UserMode && !SeSinglePrivilegeCheck(tcb, UserMode)) return done(irp, STATUS_PRIVILEGE_NOT_HELD, 0);
    if (!g_usb) return done(irp, STATUS_DEVICE_NOT_READY, 0);
    if (in_len < sizeof(struct hwusbraw_io)) return done(irp, STATUS_BUFFER_TOO_SMALL, 0);
    struct hwusbraw_io io;
    RtlCopyMemory(&io, buf, sizeof(io));
    if (io.len > HWUSBRAW_IO_MAX || io.offset > HW_USB_SIZE || io.len > HW_USB_SIZE - io.offset) return done(irp, STATUS_INVALID_PARAMETER, 0);
    if (io.write) {
        if (in_len < sizeof(io) + io.len) return done(irp, STATUS_BUFFER_TOO_SMALL, 0);
        RtlCopyMemory((UCHAR *)g_usb + io.offset, (UCHAR *)buf + sizeof(io), io.len);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        return done(irp, STATUS_SUCCESS, 0);
    }
    if (out_len < io.len) return done(irp, STATUS_BUFFER_TOO_SMALL, 0);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    RtlCopyMemory(buf, (UCHAR *)g_usb + io.offset, io.len);
    return done(irp, STATUS_SUCCESS, io.len);
}

static void unload(PDRIVER_OBJECT driver)
{
    UNICODE_STRING link;
    RtlInitUnicodeString(&link, HWUSBRAW_LINK_NAME);
    IoDeleteSymbolicLink(&link);
    if (driver->DeviceObject) IoDeleteDevice(driver->DeviceObject);
    if (g_usb) MmUnmapIoSpace(g_usb, (SIZE_T)HW_USB_SIZE);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry)
{
    (void)registry;
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = (LONGLONG)HW_USB_PA;
    g_usb = MmMapIoSpace(pa, (SIZE_T)HW_USB_SIZE, MmCached);
    if (!g_usb) return STATUS_INSUFFICIENT_RESOURCES;
    UNICODE_STRING name, link;
    RtlInitUnicodeString(&name, HWUSBRAW_DEVICE_NAME);
    RtlInitUnicodeString(&link, HWUSBRAW_LINK_NAME);
    PDEVICE_OBJECT dev;
    NTSTATUS st = IoCreateDevice(driver, 0, &name, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, FALSE, &dev);
    if (NT_SUCCESS(st)) st = IoCreateSymbolicLink(&link, &name);
    if (!NT_SUCCESS(st)) {
        if (driver->DeviceObject) IoDeleteDevice(driver->DeviceObject);
        MmUnmapIoSpace(g_usb, (SIZE_T)HW_USB_SIZE);
        g_usb = NULL;
        return st;
    }
    driver->MajorFunction[IRP_MJ_CREATE] = on_create;
    driver->MajorFunction[IRP_MJ_CLOSE] = on_create;
    driver->MajorFunction[IRP_MJ_CLEANUP] = on_create;
    driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = on_ioctl;
    driver->DriverUnload = unload;
    dev->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
