/* hwusb: a virtual USB host controller for Windows on the UdeCx framework (usb/README.md, phase 3).
 * Devices come from the shared USB region (include/hw_usb.h): each device the phone offers is plugged into the controller.
 * Phase 3a: the framework binding and the controller device only. */
#include <ntddk.h>
#include <wdf.h>
#include <usb.h>
#include <usbdi.h>
#include <wdfusb.h>
#include <udecx.h>
#include "hw_usb.h"

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD HwusbDeviceAdd;

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry)
{
    WDF_DRIVER_CONFIG cfg;
    WDF_DRIVER_CONFIG_INIT(&cfg, HwusbDeviceAdd);
    return WdfDriverCreate(driver, registry, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}

NTSTATUS HwusbDeviceAdd(WDFDRIVER drv, PWDFDEVICE_INIT init)
{
    UNREFERENCED_PARAMETER(drv);
    NTSTATUS st = UdecxInitializeWdfDeviceInit(init);
    if (!NT_SUCCESS(st)) return st;
    WDFDEVICE dev;
    st = WdfDeviceCreate(&init, WDF_NO_OBJECT_ATTRIBUTES, &dev);
    return st;
}
