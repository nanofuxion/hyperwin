/* hwusbraw.sys: the guest's raw access to the USB region (include/hw_usb.h), for tests and bring-up of USB
 * passthrough. A driver of its own, installed only when USB is wanted (guest/drivers/hwusbraw/install-usb.cmd), so
 * a fault here can never touch the GPU or the display. Open \\.\HwUsbRaw as SYSTEM. */
#ifndef HYPERWIN_HWUSBRAW_IOCTL_H
#define HYPERWIN_HWUSBRAW_IOCTL_H
#include <stdint.h>

#define HWUSBRAW_DEVICE_NAME L"\\Device\\HwUsbRaw"
#define HWUSBRAW_LINK_NAME L"\\DosDevices\\HwUsbRaw"

/* CTL_CODE(FILE_DEVICE_UNKNOWN, 0xA00, METHOD_BUFFERED, FILE_ANY_ACCESS). Input: struct hwusbraw_io, then the data
 * when writing; output: the data when reading. Copies, because Windows would not map more than a megabyte or two of
 * the region into a program; the real consumer (hwusb.sys) maps it in the kernel. */
#define IOCTL_HWUSBRAW_IO 0x00222800u
#define HWUSBRAW_IO_MAX 0x10000u

struct hwusbraw_io {
    uint64_t offset;      /* from the start of the region */
    uint32_t len;         /* at most HWUSBRAW_IO_MAX */
    uint32_t write;       /* 1: write the data that follows, 0: read len bytes */
};
#endif
