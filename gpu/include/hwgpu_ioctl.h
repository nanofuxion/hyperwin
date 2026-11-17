/* Direct variant: the one call between a Windows process and hwgpu.sys.
 * Open \\.\HwGpu, send IOCTL_HWGPU_OPEN once. The driver reserves a channel (hwgpu_ring.h) for
 * that handle and maps it into the caller. Closing the handle unmaps it and frees the channel. */
#ifndef HYPERWIN_HWGPU_IOCTL_H
#define HYPERWIN_HWGPU_IOCTL_H

#include <stdint.h>

#define HWGPU_DEVICE_NAME  L"\\Device\\HwGpu"
#define HWGPU_LINK_NAME    L"\\DosDevices\\HwGpu"
#define HWGPU_USER_PATH    L"\\\\.\\HwGpu"

/* CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS) */
#define IOCTL_HWGPU_OPEN 0x00222400u

struct hwgpu_open_out {
    uint64_t chan;        /* user address of the channel: control page, tx, rx */
    uint32_t slot;
    uint32_t generation;
};

#endif
