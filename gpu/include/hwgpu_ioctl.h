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

/* Map a granted heap range into the caller (in: hwgpu_map_in, out: hwgpu_map_out), and unmap it
 * again (in: hwgpu_map_out). Closing the handle unmaps whatever is left. */
#define IOCTL_HWGPU_MAP   0x00222404u
#define IOCTL_HWGPU_UNMAP 0x00222408u

/* The shared window's header and frames ([0, HW_FRAME_END), include/hw_abi.h) mapped into the caller
 * (out: hwgpu_map_out), for the indirect display driver that writes the screen. Only services and
 * elevated administrators may ask (SeImpersonatePrivilege); closing the handle unmaps it. */
#define IOCTL_HWGPU_SCREEN 0x00222410u

struct hwgpu_map_in {
    uint64_t offset;      /* from the start of the heap */
    uint64_t size;
};

struct hwgpu_map_out {
    uint64_t user;        /* address in the caller */
};

struct hwgpu_open_out {
    uint64_t chan;        /* user address of the channel: control page, tx, rx */
    uint32_t slot;
    uint32_t generation;
};

#endif
