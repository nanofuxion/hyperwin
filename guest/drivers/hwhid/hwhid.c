/* HyperWin touch screen and Precision Touchpad: a HID minidriver fed over virtio-vsock.
 *
 * Windows sees a 10-finger touch screen and a Windows Precision Touchpad (mouse/README.md), so it
 * does the gestures itself: taps, press and hold, tap and drag, two-finger scroll and pinch, the
 * three and four finger swipes, and edge swipes on the touch screen. It also works on the sign-in
 * and consent screens, which injected touch cannot reach.
 *
 * One root-enumerated device (ROOT\HwHid, hwhid.inf); hidclass.sys is the function driver above
 * this one and makes the HID collections, which Windows' inbox drivers take from there. A system
 * thread listens on vsock port HW_VSOCK_PORT_HID through the stock viosock driver. The Android
 * app connects and sends framed messages (each one length byte, then):
 *   [1] [count] then count x [id] [flags: 1 touching] [x u16] [y u16]
 *         touch screen contacts, x and y 0..32767 over the whole Windows screen
 *   [2] [count] [buttons: 1 left] then count x [id] [flags: 1 touching, 2 a finger, not a palm]
 *       [x u16] [y u16]
 *         touchpad contacts, x and y 0..4095 over the pad
 * A contact lifting is sent once more with touching clear. */
#include <ntddk.h>
#include "hw_abi.h"

#define FILE_DEVICE_SOCKET 0x0801
#define IOCTL_SOCKET_BIND CTL_CODE(FILE_DEVICE_SOCKET, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_SOCKET_LISTEN CTL_CODE(FILE_DEVICE_SOCKET, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define AF_VSOCK 40
#define VMADDR_CID_ANY 0xffffffffu

typedef struct { USHORT family, reserved; ULONG port, cid; } SOCKADDR_VM;
typedef struct { ULONGLONG socket; ULONG type, pad; } VSOCK_PARAMS;

/* hidport.h, which mingw does not have */
#define HID_CTL(n) CTL_CODE(FILE_DEVICE_KEYBOARD, (n), METHOD_NEITHER, FILE_ANY_ACCESS)
#define IOCTL_HID_GET_DEVICE_DESCRIPTOR HID_CTL(0)
#define IOCTL_HID_GET_REPORT_DESCRIPTOR HID_CTL(1)
#define IOCTL_HID_READ_REPORT           HID_CTL(2)
#define IOCTL_HID_WRITE_REPORT          HID_CTL(3)
#define IOCTL_HID_GET_STRING            HID_CTL(4)
#define IOCTL_HID_ACTIVATE_DEVICE       HID_CTL(7)
#define IOCTL_HID_DEACTIVATE_DEVICE     HID_CTL(8)
#define IOCTL_HID_GET_DEVICE_ATTRIBUTES HID_CTL(9)
#define IOCTL_HID_GET_FEATURE  CTL_CODE(FILE_DEVICE_KEYBOARD, 100, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)
#define IOCTL_HID_SET_FEATURE  CTL_CODE(FILE_DEVICE_KEYBOARD, 100, METHOD_IN_DIRECT, FILE_ANY_ACCESS)

typedef struct {
    ULONG Revision;
    PDRIVER_OBJECT DriverObject;
    PUNICODE_STRING RegistryPath;
    ULONG DeviceExtensionSize;
    BOOLEAN DevicesArePolled;
    UCHAR Reserved[3];
} HID_MINIDRIVER_REGISTRATION;
typedef struct { PDEVICE_OBJECT PhysicalDeviceObject, NextDeviceObject; PVOID MiniDeviceExtension; } HID_DEVICE_EXTENSION;
typedef struct { ULONG Size; USHORT VendorID, ProductID, VersionNumber, Reserved[11]; } HID_DEVICE_ATTRIBUTES;
typedef struct { PUCHAR reportBuffer; ULONG reportBufferLen; UCHAR reportId; } HID_XFER_PACKET;
#pragma pack(push, 1)
typedef struct {
    UCHAR bLength, bDescriptorType;
    USHORT bcdHID;
    UCHAR bCountry, bNumDescriptors, bReportType;
    USHORT wReportLength;
} HID_DESCRIPTOR;
#pragma pack(pop)
NTSTATUS NTAPI HidRegisterMinidriver(HID_MINIDRIVER_REGISTRATION *r);

enum {
    ID_TOUCH = 1, ID_TOUCH_MAX = 2, ID_TOUCH_THQA = 3,
    ID_PAD = 4, ID_PAD_CAPS = 5, ID_PAD_THQA = 6, ID_MODE = 7, ID_SWITCH = 8, ID_MOUSE = 9,
};
#define TOUCH_CONTACTS 10
#define PAD_CONTACTS 5
#define TOUCH_REPORT (1 + 6 * TOUCH_CONTACTS + 3)   /* id, contacts, scan time, count */
#define PAD_REPORT (1 + 6 * PAD_CONTACTS + 4)       /* id, contacts, scan time, count, buttons */

/* One finger: tip (and for the pad, confidence) bits, contact id, x, y. */
#define TOUCH_FINGER                                                                              \
    0x09, 0x22, 0xa1, 0x02,             /* Finger, Collection (Logical) */                         \
    0x09, 0x42, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02, /* Tip Switch */       \
    0x95, 0x07, 0x81, 0x03,                                                                       \
    0x09, 0x51, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,      /* Contact Identifier */\
    0x05, 0x01, 0x26, 0xff, 0x7f, 0x75, 0x10, 0x55, 0x0e, 0x65, 0x13,     /* 1/100 inch */         \
    0x35, 0x00, 0x46, 0x4e, 0x02, 0x09, 0x30, 0x81, 0x02,                 /* X, 5.9 in */          \
    0x46, 0x10, 0x01, 0x09, 0x31, 0x81, 0x02,                             /* Y, 2.72 in */         \
    0x05, 0x0d, 0xc0
#define PAD_FINGER                                                                                \
    0x09, 0x22, 0xa1, 0x02,                                                                       \
    0x15, 0x00, 0x25, 0x01, 0x09, 0x47, 0x09, 0x42, 0x95, 0x02, 0x75, 0x01, 0x81, 0x02,           \
    0x95, 0x06, 0x81, 0x03,                                                                       \
    0x09, 0x51, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,                             \
    0x05, 0x01, 0x26, 0xff, 0x0f, 0x75, 0x10, 0x55, 0x0e, 0x65, 0x13,                             \
    0x35, 0x00, 0x46, 0x4e, 0x02, 0x09, 0x30, 0x81, 0x02,                                         \
    0x46, 0x10, 0x01, 0x09, 0x31, 0x81, 0x02,                                                     \
    0x05, 0x0d, 0xc0
#define SCAN_TIME_AND_COUNT                                                                       \
    0x55, 0x0c, 0x66, 0x01, 0x10, 0x47, 0xff, 0xff, 0x00, 0x00, 0x27, 0xff, 0xff, 0x00, 0x00,     \
    0x75, 0x10, 0x95, 0x01, 0x09, 0x56, 0x81, 0x02,                       /* Scan Time, 100 us */  \
    0x65, 0x00, 0x55, 0x00, 0x35, 0x00, 0x45, 0x00,                       /* no unit from here */  \
    0x09, 0x54, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02             /* Contact Count */
#define BLOB_FEATURE(id)                                                                          \
    0x06, 0x00, 0xff, 0x85, id, 0x09, 0xc5, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08,             \
    0x96, 0x00, 0x01, 0xb1, 0x02, 0x05, 0x0d

static const UCHAR report_descriptor[] = {
    /* Touch screen */
    0x05, 0x0d, 0x09, 0x04, 0xa1, 0x01, 0x85, ID_TOUCH,
    TOUCH_FINGER, TOUCH_FINGER, TOUCH_FINGER, TOUCH_FINGER, TOUCH_FINGER,
    TOUCH_FINGER, TOUCH_FINGER, TOUCH_FINGER, TOUCH_FINGER, TOUCH_FINGER,
    SCAN_TIME_AND_COUNT,
    0x85, ID_TOUCH_MAX, 0x09, 0x55, 0x25, TOUCH_CONTACTS, 0x75, 0x08, 0x95, 0x01, 0xb1, 0x02,
    BLOB_FEATURE(ID_TOUCH_THQA),
    0xc0,
    /* Precision Touchpad */
    0x05, 0x0d, 0x09, 0x05, 0xa1, 0x01, 0x85, ID_PAD,
    PAD_FINGER, PAD_FINGER, PAD_FINGER, PAD_FINGER, PAD_FINGER,
    SCAN_TIME_AND_COUNT,
    0x05, 0x09, 0x09, 0x01, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02,   /* Button 1 */
    0x95, 0x07, 0x81, 0x03, 0x05, 0x0d,
    0x85, ID_PAD_CAPS, 0x09, 0x55, 0x09, 0x59, 0x75, 0x04, 0x95, 0x02, 0x25, 0x0f, 0xb1, 0x02,
    BLOB_FEATURE(ID_PAD_THQA),
    0xc0,
    /* Touchpad configuration: input mode, surface and button switches */
    0x05, 0x0d, 0x09, 0x0e, 0xa1, 0x01, 0x85, ID_MODE,
    0x09, 0x22, 0xa1, 0x02, 0x09, 0x52, 0x15, 0x00, 0x25, 0x0a, 0x75, 0x08, 0x95, 0x01, 0xb1, 0x02, 0xc0,
    0x09, 0x22, 0xa1, 0x00, 0x85, ID_SWITCH, 0x09, 0x57, 0x09, 0x58, 0x75, 0x01, 0x95, 0x02, 0x25, 0x01,
    0xb1, 0x02, 0x95, 0x06, 0xb1, 0x03, 0xc0,
    0xc0,
    /* The mouse a touchpad must also offer, for when Windows asks for mouse mode; never sent */
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, ID_MOUSE, 0x09, 0x01, 0xa1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x02, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02,
    0x95, 0x06, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x75, 0x10, 0x95, 0x02,
    0x15, 0x81, 0x25, 0x7f, 0x81, 0x06, 0xc0, 0xc0,
};

/* Any 256 bytes will do for a device without a signed one; this is Microsoft's sample. */
static const UCHAR thqa_blob[256] = {
    0xfc, 0x28, 0xfe, 0x84, 0x40, 0xcb, 0x9a, 0x87, 0x0d, 0xbe, 0x57, 0x3c, 0xb6, 0x70, 0x09, 0x88, 0x07,
    0x97, 0x2d, 0x2b, 0xe3, 0x38, 0x34, 0xb6, 0x6c, 0xed, 0xb0, 0xf7, 0xe5, 0x9c, 0xf6, 0xc2, 0x2e, 0x84,
    0x1b, 0xe8, 0xb4, 0x51, 0x78, 0x43, 0x1f, 0x28, 0x4b, 0x7c, 0x2d, 0x53, 0xaf, 0xfc, 0x47, 0x70, 0x1b,
    0x59, 0x6f, 0x74, 0x43, 0xc4, 0xf3, 0x47, 0x18, 0x53, 0x1a, 0xa2, 0xa1, 0x71, 0xc7, 0x95, 0x0e, 0x31,
    0x55, 0x21, 0xd3, 0xb5, 0x1e, 0xe9, 0x0c, 0xba, 0xec, 0xb8, 0x89, 0x19, 0x3e, 0xb3, 0xaf, 0x75, 0x81,
    0x9d, 0x53, 0xb9, 0x41, 0x57, 0xf4, 0x6d, 0x39, 0x25, 0x29, 0x7c, 0x87, 0xd9, 0xb4, 0x98, 0x45, 0x7d,
    0xa7, 0x26, 0x9c, 0x65, 0x3b, 0x85, 0x68, 0x89, 0xd7, 0x3b, 0xbd, 0xff, 0x14, 0x67, 0xf2, 0x2b, 0xf0,
    0x2a, 0x41, 0x54, 0xf0, 0xfd, 0x2c, 0x66, 0x7c, 0xf8, 0xc0, 0x8f, 0x33, 0x13, 0x03, 0xf1, 0xd3, 0xc1, 0x0b,
    0x89, 0xd9, 0x1b, 0x62, 0xcd, 0x51, 0xb7, 0x80, 0xb8, 0xaf, 0x3a, 0x10, 0xc1, 0x8a, 0x5b, 0xe8, 0x8a,
    0x56, 0xf0, 0x8c, 0xaa, 0xfa, 0x35, 0xe9, 0x42, 0xc4, 0xd8, 0x55, 0xc3, 0x38, 0xcc, 0x2b, 0x53, 0x5c,
    0x69, 0x52, 0xd5, 0xc8, 0x73, 0x02, 0x38, 0x7c, 0x73, 0xb6, 0x41, 0xe7, 0xff, 0x05, 0xd8, 0x2b, 0x79,
    0x9a, 0xe2, 0x34, 0x60, 0x8f, 0xa3, 0x32, 0x1f, 0x09, 0x78, 0x62, 0xbc, 0x80, 0xe3, 0x0f, 0xbd, 0x65,
    0x20, 0x08, 0x13, 0xc1, 0xe2, 0xee, 0x53, 0x2d, 0x86, 0x7e, 0xa7, 0x5a, 0xc5, 0xd3, 0x7d, 0x98, 0xbe,
    0x31, 0x48, 0x1f, 0xfb, 0xda, 0xaf, 0xa2, 0xa8, 0x6a, 0x89, 0xd6, 0xbf, 0xf2, 0xd3, 0x32, 0x2a, 0x9a,
    0xe4, 0xcf, 0x17, 0xb7, 0xb8, 0xf4, 0xe1, 0x33, 0x08, 0x24, 0x8b, 0xc4, 0x43, 0xa5, 0xe5, 0x24, 0xc2,
};
_Static_assert(sizeof thqa_blob == 256, "the certification blob is 256 bytes");

#define QUEUE 64
/* One a device: the touch device, or the gamepad twin. hidclass keeps it as our extension. */
typedef struct {
    BOOLEAN pad;
    KSPIN_LOCK lock;
    LIST_ENTRY reads;                     /* READ_REPORT requests waiting for a report */
    UCHAR queue[QUEUE][TOUCH_REPORT];     /* reports waiting for a request */
    UCHAR queue_len[QUEUE];
    ULONG head, tail;
} DEV;

static struct {
    DEV *touch, *pad;                     /* the devices there are now */
    UCHAR input_mode;                     /* 3 once Windows has put the touchpad in touchpad mode */
    BOOLEAN thread;
} g;

static DEV *dev_of(PDEVICE_OBJECT fdo)
{
    return ((HID_DEVICE_EXTENSION *)fdo->DeviceExtension)->MiniDeviceExtension;
}

static NTSTATUS complete(PIRP irp, NTSTATUS status, ULONG_PTR information)
{
    irp->IoStatus.Status = status;
    irp->IoStatus.Information = information;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

static VOID cancel_read(PDEVICE_OBJECT device, PIRP irp)
{
    DEV *d = dev_of(device);
    KIRQL irql;
    IoReleaseCancelSpinLock(irp->CancelIrql);
    irql = KeAcquireSpinLockRaiseToDpc(&d->lock);
    RemoveEntryList(&irp->Tail.Overlay.ListEntry);
    KeReleaseSpinLock(&d->lock, irql);
    complete(irp, STATUS_CANCELLED, 0);
}

/* Hands a report to a waiting read, or keeps it (dropping the oldest when full). */
static void post(DEV *d, const UCHAR *report, ULONG len)
{
    PIRP irp = NULL;
    KIRQL irql;
    if (!d) return;
    irql = KeAcquireSpinLockRaiseToDpc(&d->lock);
    while (!IsListEmpty(&d->reads)) {
        PIRP r = CONTAINING_RECORD(d->reads.Flink, IRP, Tail.Overlay.ListEntry);
        RemoveEntryList(&r->Tail.Overlay.ListEntry);
        if (IoSetCancelRoutine(r, NULL)) { irp = r; break; }
        InitializeListHead(&r->Tail.Overlay.ListEntry);   /* being cancelled: its routine removes nothing */
    }
    if (!irp) {
        if (d->head - d->tail == QUEUE) d->tail++;
        RtlCopyMemory(d->queue[d->head % QUEUE], report, len);
        d->queue_len[d->head % QUEUE] = (UCHAR)len;
        d->head++;
    }
    KeReleaseSpinLock(&d->lock, irql);
    if (irp) {
        ULONG room = IoGetCurrentIrpStackLocation(irp)->Parameters.DeviceIoControl.OutputBufferLength;
        if (room < len) { complete(irp, STATUS_BUFFER_TOO_SMALL, 0); return; }
        RtlCopyMemory(irp->UserBuffer, report, len);
        complete(irp, STATUS_SUCCESS, len);
    }
}

static NTSTATUS read_report(DEV *d, PIRP irp)
{
    ULONG room = IoGetCurrentIrpStackLocation(irp)->Parameters.DeviceIoControl.OutputBufferLength;
    KIRQL irql = KeAcquireSpinLockRaiseToDpc(&d->lock);
    if (d->head != d->tail) {
        ULONG len = d->queue_len[d->tail % QUEUE];
        UCHAR report[TOUCH_REPORT];
        RtlCopyMemory(report, d->queue[d->tail % QUEUE], len);
        d->tail++;
        KeReleaseSpinLock(&d->lock, irql);
        if (room < len) return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        RtlCopyMemory(irp->UserBuffer, report, len);
        return complete(irp, STATUS_SUCCESS, len);
    }
    IoMarkIrpPending(irp);
    InsertTailList(&d->reads, &irp->Tail.Overlay.ListEntry);
    IoSetCancelRoutine(irp, cancel_read);
    if (irp->Cancel && IoSetCancelRoutine(irp, NULL)) {
        RemoveEntryList(&irp->Tail.Overlay.ListEntry);
        KeReleaseSpinLock(&d->lock, irql);
        complete(irp, STATUS_CANCELLED, 0);
        return STATUS_PENDING;
    }
    KeReleaseSpinLock(&d->lock, irql);
    return STATUS_PENDING;
}

static USHORT scan_time(void)
{
    LARGE_INTEGER f, t = KeQueryPerformanceCounter(&f);
    return (USHORT)(t.QuadPart / (f.QuadPart / 10000));   /* 100 us units, wrapping */
}

static void put16(UCHAR *p, USHORT v) { p[0] = (UCHAR)v; p[1] = (UCHAR)(v >> 8); }

static void on_message(const UCHAR *m, ULONG len)
{
    UCHAR r[TOUCH_REPORT];
    ULONG n, i, max, size;
    if (len < 2 || (m[0] != 1 && m[0] != 2)) return;
    BOOLEAN pad = m[0] == 2;
    const UCHAR *c = m + (pad ? 3 : 2);
    if (pad && len < 3) return;
    n = m[1];
    max = pad ? PAD_CONTACTS : TOUCH_CONTACTS;
    if (n > max) n = max;
    if ((ULONG)(c - m) + 6 * n > len) return;
    size = pad ? PAD_REPORT : TOUCH_REPORT;
    RtlZeroMemory(r, sizeof r);
    r[0] = pad ? ID_PAD : ID_TOUCH;
    for (i = 0; i < n; i++, c += 6) {
        UCHAR *f = r + 1 + 6 * i;
        f[0] = pad ? (UCHAR)((c[1] & 2 ? 1 : 0) | (c[1] & 1 ? 2 : 0)) : (UCHAR)(c[1] & 1);   /* pad: confidence, tip */
        f[1] = c[0];
        f[2] = c[2]; f[3] = c[3];
        f[4] = c[4]; f[5] = c[5];
    }
    put16(r + 1 + 6 * max, scan_time());
    r[3 + 6 * max] = (UCHAR)n;
    if (pad) r[4 + 6 * max] = m[2] & 1;
    post(g.touch, r, size);
}

static NTSTATUS open_socket(HANDLE parent, HANDLE *out)
{
    static const WCHAR name[] = L"\\??\\Viosock";
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    UCHAR eab[sizeof(FILE_FULL_EA_INFORMATION) + sizeof(VSOCK_PARAMS)];
    PFILE_FULL_EA_INFORMATION ea = (PFILE_FULL_EA_INFORMATION)eab;
    VSOCK_PARAMS params = { (ULONGLONG)(ULONG_PTR)parent, 1, 0 };
    RtlZeroMemory(eab, sizeof eab);
    ea->EaNameLength = (UCHAR)(sizeof(FILE_FULL_EA_INFORMATION) - FIELD_OFFSET(FILE_FULL_EA_INFORMATION, EaName) - 1);
    ea->EaValueLength = sizeof(VSOCK_PARAMS);
    RtlCopyMemory(eab + sizeof(FILE_FULL_EA_INFORMATION), &params, sizeof params);
    us.Buffer = (PWCH)name;
    us.Length = sizeof name - sizeof(WCHAR);
    us.MaximumLength = sizeof name;
    InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    return ZwCreateFile(out, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, &oa, &iosb, NULL, 0,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
                        FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, eab, sizeof eab);
}

static NTSTATUS socket_ioctl(HANDLE h, ULONG code, PVOID in, ULONG in_len)
{
    IO_STATUS_BLOCK iosb;
    return ZwDeviceIoControlFile(h, NULL, NULL, NULL, &iosb, code, in, in_len, NULL, 0);
}

static void sleep_ms(LONG ms)
{
    LARGE_INTEGER t;
    t.QuadPart = -10000LL * ms;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}

static VOID listener(PVOID context)
{
    (void)context;
    for (;;) {
        HANDLE listen_h = NULL, conn = NULL;
        SOCKADDR_VM addr = { AF_VSOCK, 0, HW_VSOCK_PORT_HID, VMADDR_CID_ANY };
        ULONG backlog = 1;
        NTSTATUS s = open_socket(NULL, &listen_h);
        if (!NT_SUCCESS(s)) { sleep_ms(1000); continue; }
        s = socket_ioctl(listen_h, IOCTL_SOCKET_BIND, &addr, sizeof addr);
        if (NT_SUCCESS(s)) s = socket_ioctl(listen_h, IOCTL_SOCKET_LISTEN, &backlog, sizeof backlog);
        if (!NT_SUCCESS(s)) { ZwClose(listen_h); sleep_ms(1000); continue; }
        for (;;) {
            UCHAR buf[1024];
            ULONG have = 0;
            if (!NT_SUCCESS(open_socket(listen_h, &conn))) break;
            for (;;) {
                IO_STATUS_BLOCK iosb;
                ULONG used = 0;
                s = ZwReadFile(conn, NULL, NULL, NULL, &iosb, buf + have, sizeof buf - have, NULL, NULL);
                if (!NT_SUCCESS(s) || iosb.Information == 0) break;
                have += (ULONG)iosb.Information;
                while (used < have) {
                    ULONG len = buf[used];
                    if (len == 0) { used = have; break; }
                    if (used + 1 + len > have) break;
                    on_message(buf + used + 1, len);
                    used += 1 + len;
                }
                if (used) {
                    RtlMoveMemory(buf, buf + used, have - used);
                    have -= used;
                }
            }
            ZwClose(conn);
        }
        ZwClose(listen_h);
        sleep_ms(1000);
    }
}

static NTSTATUS get_feature(PIRP irp)
{
    HID_XFER_PACKET *p = irp->UserBuffer;
    ULONG need;
    if (!p || !p->reportBuffer) return complete(irp, STATUS_INVALID_PARAMETER, 0);
    switch (p->reportId) {
    case ID_TOUCH_MAX: need = 2; break;
    case ID_PAD_CAPS: need = 2; break;
    case ID_TOUCH_THQA: case ID_PAD_THQA: need = 1 + sizeof thqa_blob; break;
    case ID_MODE: need = 2; break;
    case ID_SWITCH: need = 2; break;
    default: return complete(irp, STATUS_INVALID_PARAMETER, 0);
    }
    if (p->reportBufferLen < need) return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
    p->reportBuffer[0] = p->reportId;
    switch (p->reportId) {
    case ID_TOUCH_MAX: p->reportBuffer[1] = TOUCH_CONTACTS; break;
    case ID_PAD_CAPS: p->reportBuffer[1] = PAD_CONTACTS | 0 << 4; break;   /* pad type 0: a click-pad */
    case ID_TOUCH_THQA: case ID_PAD_THQA: RtlCopyMemory(p->reportBuffer + 1, thqa_blob, sizeof thqa_blob); break;
    case ID_MODE: p->reportBuffer[1] = g.input_mode; break;
    case ID_SWITCH: p->reportBuffer[1] = 3; break;
    }
    return complete(irp, STATUS_SUCCESS, need);
}

/* ---- The gamepad twin ----
 * A wired Xbox 360 controller also shows Windows a HID game controller, with IG_00 in its IDs, for
 * DirectInput, joy.cpl, SDL and Steam; programs that know XInput skip anything with IG_. This is
 * that for player 1: device Root\VID_045E&PID_028E&IG_00, whose HID IDs take the IG_00 from it.
 * hwvsinput.sys hands each pad update over through the \Callback\HyperWinPad callback object. */
static const UCHAR pad_descriptor[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01,                               /* Gamepad */
    0xa1, 0x00, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x27, 0xff, 0xff, 0x00, 0x00,
    0x75, 0x10, 0x95, 0x02, 0x81, 0x02, 0xc0,                         /* left stick X, Y */
    0xa1, 0x00, 0x09, 0x33, 0x09, 0x34, 0x15, 0x00, 0x27, 0xff, 0xff, 0x00, 0x00,
    0x75, 0x10, 0x95, 0x02, 0x81, 0x02, 0xc0,                         /* right stick Rx, Ry */
    0x09, 0x32, 0x15, 0x00, 0x27, 0xff, 0xff, 0x00, 0x00, 0x75, 0x10, 0x95, 0x01, 0x81, 0x02,   /* triggers, Z */
    0x05, 0x09, 0x19, 0x01, 0x29, 0x0a, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0a, 0x81, 0x02,
    0x75, 0x06, 0x95, 0x01, 0x81, 0x03,                               /* buttons 1 to 10 */
    0x05, 0x01, 0x09, 0x39, 0x15, 0x01, 0x25, 0x08, 0x35, 0x00, 0x46, 0x3b, 0x01, 0x66, 0x14, 0x00,
    0x75, 0x04, 0x95, 0x01, 0x81, 0x42, 0x75, 0x04, 0x95, 0x01, 0x81, 0x03, 0x65, 0x00,   /* hat */
    0xc0,
};
#define PAD_HID_REPORT 13

/* The callback: Argument1 the player (0 to 3), Argument2 the 13 bytes of message 3 after its type. */
static VOID pad_update(PVOID context, PVOID slot, PVOID state)
{
    static const USHORT map[10] = { 0x1000, 0x2000, 0x4000, 0x8000, 0x0100, 0x0200, 0x0020, 0x0010, 0x0040, 0x0080 };
    static const UCHAR hats[16] = { 0, 1, 5, 0, 7, 8, 6, 7, 3, 2, 4, 3, 0, 1, 5, 0 };   /* by up, down, left, right */
    const UCHAR *q = state;
    UCHAR r[PAD_HID_REPORT] = {0};
    USHORT b = (USHORT)(q[0] | q[1] << 8), buttons = 0;
    LONG lx = (SHORT)(q[4] | q[5] << 8), ly = (SHORT)(q[6] | q[7] << 8);
    LONG rx = (SHORT)(q[8] | q[9] << 8), ry = (SHORT)(q[10] | q[11] << 8);
    LONG z = 32768 + ((LONG)q[2] - (LONG)q[3]) * 128;
    (void)context;
    if ((ULONG_PTR)slot != 0 || !g.pad) return;
    if (!q[12]) { lx = ly = rx = ry = 0; z = 32768; b = 0; }   /* disconnected: at rest */
    put16(r + 0, (USHORT)(lx + 32768));
    put16(r + 2, (USHORT)(32767 - ly));
    put16(r + 4, (USHORT)(rx + 32768));
    put16(r + 6, (USHORT)(32767 - ry));
    put16(r + 8, (USHORT)(z < 0 ? 0 : z > 65535 ? 65535 : z));
    for (int i = 0; i < 10; i++)
        if (b & map[i]) buttons |= (USHORT)(1 << i);
    put16(r + 10, buttons);
    r[12] = hats[b & 0xf];
    post(g.pad, r, sizeof r);
}

static NTSTATUS get_string(PIRP irp, PIO_STACK_LOCATION st, BOOLEAN pad)
{
    static const WCHAR maker[] = L"HyperWin", product[] = L"HyperWin Touch", pad_name[] = L"Controller (HyperWin)",
                       serial[] = L"1";
    ULONG id = (ULONG)(ULONG_PTR)st->Parameters.DeviceIoControl.Type3InputBuffer & 0xffff;
    const WCHAR *s = id == 14 ? maker : id == 15 ? (pad ? pad_name : product) : id == 16 ? serial : NULL;
    ULONG bytes;
    if (!s) return complete(irp, STATUS_INVALID_PARAMETER, 0);
    bytes = (ULONG)(wcslen(s) + 1) * sizeof(WCHAR);
    if (st->Parameters.DeviceIoControl.OutputBufferLength < bytes) return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
    RtlCopyMemory(irp->UserBuffer, s, bytes);
    return complete(irp, STATUS_SUCCESS, bytes);
}

static NTSTATUS internal_ioctl(PDEVICE_OBJECT device, PIRP irp)
{
    PIO_STACK_LOCATION st = IoGetCurrentIrpStackLocation(irp);
    ULONG out = st->Parameters.DeviceIoControl.OutputBufferLength;
    DEV *dev = dev_of(device);
    const UCHAR *desc = dev->pad ? pad_descriptor : report_descriptor;
    USHORT desc_len = dev->pad ? sizeof pad_descriptor : sizeof report_descriptor;
    switch (st->Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_HID_GET_DEVICE_DESCRIPTOR: {
        HID_DESCRIPTOR d = { sizeof(HID_DESCRIPTOR), 0x21, 0x0100, 0, 1, 0x22, desc_len };
        if (out < sizeof d) return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        RtlCopyMemory(irp->UserBuffer, &d, sizeof d);
        return complete(irp, STATUS_SUCCESS, sizeof d);
    }
    case IOCTL_HID_GET_REPORT_DESCRIPTOR:
        if (out < desc_len) return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        RtlCopyMemory(irp->UserBuffer, desc, desc_len);
        return complete(irp, STATUS_SUCCESS, desc_len);
    case IOCTL_HID_GET_DEVICE_ATTRIBUTES: {
        HID_DEVICE_ATTRIBUTES *a = irp->UserBuffer;
        if (out < sizeof *a) return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        RtlZeroMemory(a, sizeof *a);
        a->Size = sizeof *a;
        a->VendorID = dev->pad ? 0x045E : 0x1209;    /* the pad: an Xbox 360 controller; else pid.codes */
        a->ProductID = dev->pad ? 0x028E : 0x4857;
        a->VersionNumber = 0x0100;
        return complete(irp, STATUS_SUCCESS, sizeof *a);
    }
    case IOCTL_HID_READ_REPORT:
        return read_report(dev, irp);
    case IOCTL_HID_GET_FEATURE:
        return dev->pad ? complete(irp, STATUS_NOT_SUPPORTED, 0) : get_feature(irp);
    case IOCTL_HID_SET_FEATURE: {
        HID_XFER_PACKET *p = irp->UserBuffer;
        if (!dev->pad && p && p->reportBuffer && p->reportId == ID_MODE && p->reportBufferLen >= 2) g.input_mode = p->reportBuffer[1];
        return complete(irp, STATUS_SUCCESS, 0);
    }
    case IOCTL_HID_GET_STRING:
        return get_string(irp, st, dev->pad);
    case IOCTL_HID_ACTIVATE_DEVICE:
    case IOCTL_HID_DEACTIVATE_DEVICE:
    case IOCTL_HID_WRITE_REPORT:
        return complete(irp, STATUS_SUCCESS, 0);
    default:
        return complete(irp, STATUS_NOT_SUPPORTED, 0);
    }
}

static PDEVICE_OBJECT lower(PDEVICE_OBJECT device)
{
    return ((HID_DEVICE_EXTENSION *)device->DeviceExtension)->NextDeviceObject;
}

static NTSTATUS pnp(PDEVICE_OBJECT device, PIRP irp)
{
    if (IoGetCurrentIrpStackLocation(irp)->MinorFunction == IRP_MN_REMOVE_DEVICE) {
        DEV *d = dev_of(device);
        if (g.touch == d) g.touch = NULL;
        if (g.pad == d) g.pad = NULL;
    }
    IoSkipCurrentIrpStackLocation(irp);
    return IoCallDriver(lower(device), irp);
}

static NTSTATUS power(PDEVICE_OBJECT device, PIRP irp)
{
    PoStartNextPowerIrp(irp);
    IoSkipCurrentIrpStackLocation(irp);
    return PoCallDriver(lower(device), irp);
}

/* Which device this is: the gamepad twin's hardware ID carries IG_. */
static BOOLEAN is_pad(PDEVICE_OBJECT pdo)
{
    WCHAR ids[256];
    ULONG len = 0, i;
    if (!NT_SUCCESS(IoGetDeviceProperty(pdo, DevicePropertyHardwareID, sizeof ids - sizeof(WCHAR), ids, &len))) return FALSE;
    for (i = 0; i + 3 <= len / sizeof(WCHAR); i++)
        if (ids[i] == L'I' && ids[i + 1] == L'G' && ids[i + 2] == L'_') return TRUE;
    return FALSE;
}

static NTSTATUS add_device(PDRIVER_OBJECT driver, PDEVICE_OBJECT fdo)
{
    DEV *d = dev_of(fdo);
    (void)driver;
    RtlZeroMemory(d, sizeof *d);
    KeInitializeSpinLock(&d->lock);
    InitializeListHead(&d->reads);
    d->pad = is_pad(((HID_DEVICE_EXTENSION *)fdo->DeviceExtension)->PhysicalDeviceObject);
    if (d->pad) g.pad = d; else g.touch = d;
    if (!d->pad && !g.thread) {
        HANDLE t;
        if (NT_SUCCESS(PsCreateSystemThread(&t, THREAD_ALL_ACCESS, NULL, NULL, NULL, listener, NULL))) {
            ZwClose(t);
            g.thread = TRUE;
        }
    }
    fdo->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

/* The listener thread never ends, so the driver must never leave memory: the reference taken in
 * DriverEntry keeps it, and a new hwhid.sys takes over at the next restart. */
static VOID unload(PDRIVER_OBJECT driver) { (void)driver; }

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry)
{
    HID_MINIDRIVER_REGISTRATION r;
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    PCALLBACK_OBJECT pads;
    ObReferenceObject(driver);
    RtlInitUnicodeString(&name, L"\\Callback\\HyperWinPad");
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_PERMANENT, NULL, NULL);
    if (NT_SUCCESS(ExCreateCallback(&pads, &oa, TRUE, TRUE))) ExRegisterCallback(pads, pad_update, NULL);
    driver->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = internal_ioctl;
    driver->MajorFunction[IRP_MJ_PNP] = pnp;
    driver->MajorFunction[IRP_MJ_POWER] = power;
    driver->DriverExtension->AddDevice = add_device;
    driver->DriverUnload = unload;
    RtlZeroMemory(&r, sizeof r);
    r.DeviceExtensionSize = sizeof(DEV);
    r.Revision = 1;
    r.DriverObject = driver;
    r.RegistryPath = registry;
    r.DevicesArePolled = FALSE;
    return HidRegisterMinidriver(&r);
}
