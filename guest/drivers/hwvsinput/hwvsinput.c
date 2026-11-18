/* HyperWin keyboard, absolute mouse and gamepad over virtio-vsock.
 * A keyboard and mouse port driver (the i8042prt role) under the inbox kbdclass and mouclass
 * filters, on two root-enumerated devices: ROOT\HWVSKBD and ROOT\HWVSMOU. A third device, which
 * the driver reports itself the first time it loads, is an Xbox controller to XInput: it offers
 * the XUSB interface xinput1_4.dll looks for (the same calls Microsoft's xusb22.sys answers), so
 * games see a controller without any of Windows' files changing. It imports nothing but
 * ntoskrnl, so it can start at boot. A system thread listens on vsock port HW_VSOCK_PORT_INPUT
 * through the stock viosock driver; the Android app connects and sends framed messages:
 *   [len] [1] [make lo] [make hi] [flags: 1 break, 2 E0]          keyboard scan code (set 1)
 *   [len] [2] [x lo] [x hi] [y lo] [y hi] [buttons] [wheel]       absolute pointer, 0..65535
 *       optionally then [wheel s16] [hwheel s16] in 1/120 steps, which replace [wheel]
 *   [len] [4] [dx s16] [dy s16] [buttons] [wheel s16] [hwheel s16]  relative pointer, mouse counts
 *   [len] [3] [buttons u16] [lt] [rt] [lx ly rx ry s16] [connected]  gamepad, XInput layout */
#include <ntddk.h>
#include <kbdmou.h>
#include <ntddkbd.h>
#include <ntddmou.h>
#include "hw_abi.h"

#define FILE_DEVICE_SOCKET 0x0801
#define IOCTL_SOCKET_BIND CTL_CODE(FILE_DEVICE_SOCKET, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_SOCKET_LISTEN CTL_CODE(FILE_DEVICE_SOCKET, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define AF_VSOCK 40
#define VMADDR_CID_ANY 0xffffffffu
#define WHEEL_DELTA 120

typedef struct {
    USHORT family;
    USHORT reserved;
    ULONG port;
    ULONG cid;
} SOCKADDR_VM;

typedef struct {
    ULONGLONG socket;
    ULONG type;
    ULONG pad;
} VSOCK_PARAMS;

enum { KIND_KBD, KIND_MOU, KIND_PAD };

typedef struct {
    PDEVICE_OBJECT self, lower;
    BOOLEAN mouse;
    int kind;
    CONNECT_DATA connect;
    UNICODE_STRING iface;   /* the pad's XUSB interface */
} PORT_EXT;

/* One controller, in XInput's terms. */
typedef struct {
    UCHAR connected;
    ULONG packet;
    USHORT buttons;
    UCHAR lt, rt;
    SHORT lx, ly, rx, ry;
} PAD;

static struct {
    KSPIN_LOCK lock;
    PORT_EXT *kbd, *mou;
    UCHAR buttons;
    BOOLEAN thread;
    PAD pad;
} g;

/* XUSB, as xinput1_4.dll speaks it (interface class and calls; layouts are packed). */
static const GUID XUSB_INTERFACE = { 0xEC87F1E3, 0xC13B, 0x4100, { 0xB5, 0xF7, 0x8B, 0x84, 0xD5, 0x42, 0x60, 0xCB } };
#define XUSB_GET_INFORMATION   0x80006000u
#define XUSB_GET_CAPABILITIES  0x8000E004u
#define XUSB_GET_LED_STATE     0x8000E008u
#define XUSB_GET_STATE         0x8000E00Cu
#define XUSB_SET_STATE         0x8000A010u
#define XUSB_GET_BATTERY       0x8000E018u
#define XUSB_VERSION           0x0102
#define PAD_VID 0x045E          /* an Xbox 360 controller, what games know best */
#define PAD_PID 0x028E

static void diag(ULONG index, ULONG value)
{
    PHYSICAL_ADDRESS pa;
    volatile ULONG *d;
    pa.QuadPart = (LONGLONG)(HW_WINDOW_PA + HW_DIAG);
    d = (volatile ULONG *)MmMapIoSpace(pa, PAGE_SIZE, MmCached);
    if (!d) return;
    d[index] = value;
    MmUnmapIoSpace((PVOID)d, PAGE_SIZE);
}

static void deliver(BOOLEAN mouse, PVOID data)
{
    KIRQL irql = KeAcquireSpinLockRaiseToDpc(&g.lock);
    PORT_EXT *e = mouse ? g.mou : g.kbd;
    if (e && e->connect.ClassService) {
        ULONG consumed = 0;
        PUCHAR start = data, end = start + (mouse ? sizeof(MOUSE_INPUT_DATA) : sizeof(KEYBOARD_INPUT_DATA));
        ((PSERVICE_CALLBACK_ROUTINE)e->connect.ClassService)(e->connect.ClassDeviceObject, start, end, &consumed);
    }
    KeReleaseSpinLock(&g.lock, irql);
}

/* One pointer report. Buttons: bit 0 left, 1 right, 2 middle, 3 back, 4 forward. Wheels in
 * 1/120 steps. The vertical and horizontal wheel share ButtonData, so a report with both is two. */
static void pointer(USHORT flags, LONG x, LONG y, UCHAR b, SHORT wheel, SHORT hwheel)
{
    static const USHORT down[5] = { MOUSE_LEFT_BUTTON_DOWN, MOUSE_RIGHT_BUTTON_DOWN, MOUSE_MIDDLE_BUTTON_DOWN,
                                    MOUSE_BUTTON_4_DOWN, MOUSE_BUTTON_5_DOWN };
    MOUSE_INPUT_DATA d;
    UCHAR changed = (UCHAR)(b ^ g.buttons);
    RtlZeroMemory(&d, sizeof d);
    d.Flags = flags;
    d.LastX = x;
    d.LastY = y;
    for (int i = 0; i < 5; i++)
        if (changed & 1 << i) d.ButtonFlags |= b & 1 << i ? down[i] : (USHORT)(down[i] << 1);
    if (wheel) {
        d.ButtonFlags |= MOUSE_WHEEL;
        d.ButtonData = (USHORT)wheel;
    }
    g.buttons = b;
    deliver(TRUE, &d);
    if (hwheel) {
        RtlZeroMemory(&d, sizeof d);
        d.Flags = flags;
        if (flags == MOUSE_MOVE_ABSOLUTE) d.LastX = x, d.LastY = y;
        d.ButtonFlags = MOUSE_HWHEEL;
        d.ButtonData = (USHORT)hwheel;
        deliver(TRUE, &d);
    }
}

static void on_message(const UCHAR *m, ULONG len)
{
    if (m[0] == 1 && len >= 4) {
        KEYBOARD_INPUT_DATA k;
        RtlZeroMemory(&k, sizeof k);
        k.MakeCode = (USHORT)(m[1] | m[2] << 8);
        k.Flags = (m[3] & 1 ? KEY_BREAK : KEY_MAKE) | (m[3] & 2 ? KEY_E0 : 0);
        deliver(FALSE, &k);
    } else if (m[0] == 2 && len >= 7) {
        /* absolute; extended with a 1/120 wheel and a horizontal wheel, which replace m[6] */
        BOOLEAN fine = len >= 11;
        pointer(MOUSE_MOVE_ABSOLUTE, m[1] | m[2] << 8, m[3] | m[4] << 8, m[5],
                fine ? (SHORT)(m[7] | m[8] << 8) : (SHORT)((CHAR)m[6] * WHEEL_DELTA),
                fine ? (SHORT)(m[9] | m[10] << 8) : 0);
    } else if (m[0] == 4 && len >= 10) {
        /* relative: dx, dy in mouse counts */
        pointer(MOUSE_MOVE_RELATIVE, (SHORT)(m[1] | m[2] << 8), (SHORT)(m[3] | m[4] << 8), m[5],
                (SHORT)(m[6] | m[7] << 8), (SHORT)(m[8] | m[9] << 8));
    } else if (m[0] == 3 && len >= 14) {
        KIRQL irql = KeAcquireSpinLockRaiseToDpc(&g.lock);
        g.pad.buttons = (USHORT)(m[1] | m[2] << 8);
        g.pad.lt = m[3];
        g.pad.rt = m[4];
        g.pad.lx = (SHORT)(m[5] | m[6] << 8);
        g.pad.ly = (SHORT)(m[7] | m[8] << 8);
        g.pad.rx = (SHORT)(m[9] | m[10] << 8);
        g.pad.ry = (SHORT)(m[11] | m[12] << 8);
        g.pad.connected = m[13];
        g.pad.packet++;
        KeReleaseSpinLock(&g.lock, irql);
    }
}

static NTSTATUS complete(PIRP irp, NTSTATUS status, ULONG_PTR information);

static void put16(UCHAR *p, USHORT v) { p[0] = (UCHAR)v; p[1] = (UCHAR)(v >> 8); }
static void put32(UCHAR *p, ULONG v) { put16(p, (USHORT)v); put16(p + 2, (USHORT)(v >> 16)); }

/* The XUSB calls, answered from the pad state the phone last sent. One controller, index 0. */
static NTSTATUS pad_ioctl(PIRP irp, PIO_STACK_LOCATION st)
{
    ULONG code = st->Parameters.DeviceIoControl.IoControlCode;
    ULONG in = st->Parameters.DeviceIoControl.InputBufferLength;
    ULONG out = st->Parameters.DeviceIoControl.OutputBufferLength;
    UCHAR *b = irp->AssociatedIrp.SystemBuffer;
    USHORT version = in >= 2 ? (USHORT)(b[0] | b[1] << 8) : 0x0100;
    PAD p;
    KIRQL irql = KeAcquireSpinLockRaiseToDpc(&g.lock);
    p = g.pad;
    KeReleaseSpinLock(&g.lock, irql);
    switch (code) {
    case XUSB_GET_INFORMATION:   /* version, controllers, flags (0x80: none), ..., vid, pid */
        if (out < 12) return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        RtlZeroMemory(b, 12);
        put16(b, XUSB_VERSION);
        b[2] = 1;
        put16(b + 8, PAD_VID);
        put16(b + 10, PAD_PID);
        return complete(irp, STATUS_SUCCESS, 12);
    case XUSB_GET_STATE:
        if (version == 0x0100) {   /* status, -, input id, packet, -, buttons, lt, rt, sticks */
            if (out < 20) return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
            RtlZeroMemory(b, 20);
            b[0] = p.connected ? 1 : 0;
            put32(b + 3, p.packet);
            put16(b + 8, p.buttons);
            b[10] = p.lt; b[11] = p.rt;
            put16(b + 12, (USHORT)p.lx); put16(b + 14, (USHORT)p.ly);
            put16(b + 16, (USHORT)p.rx); put16(b + 18, (USHORT)p.ry);
            return complete(irp, STATUS_SUCCESS, 20);
        }
        /* version, status, -, input id, packet, -, -, buttons, lt, rt, sticks, 5 x -, extra buttons */
        if (out < 29) return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        RtlZeroMemory(b, 29);
        put16(b, XUSB_VERSION);
        b[2] = p.connected ? 1 : 0;
        put32(b + 5, p.packet);
        put16(b + 11, p.buttons);
        b[13] = p.lt; b[14] = p.rt;
        put16(b + 15, (USHORT)p.lx); put16(b + 17, (USHORT)p.ly);
        put16(b + 19, (USHORT)p.rx); put16(b + 21, (USHORT)p.ry);
        return complete(irp, STATUS_SUCCESS, 29);
    case XUSB_GET_CAPABILITIES:
        if (version == 0x0101) {   /* what XInput asks for here: version, type, subtype, buttons, ..., motors */
            if (out < 24) return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
            RtlZeroMemory(b, 24);
            put16(b, 0x0101);
            b[2] = 1;                   /* gamepad */
            b[3] = 1;                   /* XINPUT_DEVSUBTYPE_GAMEPAD */
            put16(b + 4, 0xF7FF);
            b[6] = 0xFF; b[7] = 0xFF;
            put16(b + 8, 0xFFC0); put16(b + 10, 0xFFC0); put16(b + 12, 0xFFC0); put16(b + 14, 0xFFC0);
            b[22] = 0xFF; b[23] = 0xFF;
            return complete(irp, STATUS_SUCCESS, 24);
        }
        /* version, type, subtype, flags, vid, pid, product version, -, then what each control reports. */
        if (out < 36) return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        RtlZeroMemory(b, 36);
        put16(b, XUSB_VERSION);
        b[2] = 1;                       /* gamepad */
        b[3] = 1;                       /* XINPUT_DEVSUBTYPE_GAMEPAD */
        put16(b + 6, PAD_VID);
        put16(b + 8, PAD_PID);
        put16(b + 16, 0xF7FF);          /* every button, guide included */
        b[18] = 0xFF; b[19] = 0xFF;     /* triggers */
        put16(b + 20, 0xFFC0); put16(b + 22, 0xFFC0); put16(b + 24, 0xFFC0); put16(b + 26, 0xFFC0);
        b[34] = 0xFF; b[35] = 0xFF;     /* motors: reported, though rumble does not reach the phone yet */
        return complete(irp, STATUS_SUCCESS, 36);
    case XUSB_GET_LED_STATE:            /* version, LED: player 1 */
        if (out < 3) return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        put16(b, XUSB_VERSION);
        b[2] = 6;
        return complete(irp, STATUS_SUCCESS, 3);
    case XUSB_GET_BATTERY:              /* version, type wired, level full */
        if (out < 4) return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        put16(b, XUSB_VERSION);
        b[2] = 1;
        b[3] = 3;
        return complete(irp, STATUS_SUCCESS, 4);
    case XUSB_SET_STATE:                /* LEDs and rumble: accepted */
        return complete(irp, STATUS_SUCCESS, 0);
    default:
        return complete(irp, STATUS_NOT_SUPPORTED, 0);
    }
}

static NTSTATUS open_socket(HANDLE parent, HANDLE *out)
{
    static const WCHAR name[] = L"\\??\\Viosock";
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    /* viosock reads the value right after a 3-byte EA name: header (12 bytes), then the params. */
    UCHAR eab[sizeof(FILE_FULL_EA_INFORMATION) + sizeof(VSOCK_PARAMS)];
    PFILE_FULL_EA_INFORMATION ea = (PFILE_FULL_EA_INFORMATION)eab;
    VSOCK_PARAMS params;
    RtlZeroMemory(eab, sizeof eab);
    ea->EaNameLength = (UCHAR)(sizeof(FILE_FULL_EA_INFORMATION) - FIELD_OFFSET(FILE_FULL_EA_INFORMATION, EaName) - 1);
    ea->EaValueLength = sizeof(VSOCK_PARAMS);
    params.socket = (ULONGLONG)(ULONG_PTR)parent;
    params.type = 1;
    params.pad = 0;
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

/* diag[40]: 1 socket, 2 bound, 3 listening, 4 connected; 0x1xxxx/0x2xxxx/0x3xxxx failure + status. diag[44]: messages. */
static void listener(PVOID context)
{
    ULONG messages = 0;
    (void)context;
    for (;;) {
        HANDLE listen_h = NULL, conn = NULL;
        SOCKADDR_VM addr = { AF_VSOCK, 0, HW_VSOCK_PORT_INPUT, VMADDR_CID_ANY };
        ULONG backlog = 1;
        NTSTATUS s = open_socket(NULL, &listen_h);
        if (!NT_SUCCESS(s)) { diag(40, 0x10000 | ((ULONG)s & 0xffff)); sleep_ms(1000); continue; }
        diag(40, 1);
        s = socket_ioctl(listen_h, IOCTL_SOCKET_BIND, &addr, sizeof addr);
        if (NT_SUCCESS(s)) { diag(40, 2); s = socket_ioctl(listen_h, IOCTL_SOCKET_LISTEN, &backlog, sizeof backlog); }
        if (!NT_SUCCESS(s)) { diag(40, 0x20000 | ((ULONG)s & 0xffff)); ZwClose(listen_h); sleep_ms(1000); continue; }
        diag(40, 3);
        for (;;) {
            UCHAR buf[256];
            ULONG have = 0;
            s = open_socket(listen_h, &conn);
            if (!NT_SUCCESS(s)) { diag(40, 0x30000 | ((ULONG)s & 0xffff)); break; }
            diag(40, 4);
            for (;;) {
                IO_STATUS_BLOCK iosb;
                ULONG used = 0;
                s = ZwReadFile(conn, NULL, NULL, NULL, &iosb, buf + have, sizeof buf - have, NULL, NULL);
                if (!NT_SUCCESS(s) || iosb.Information == 0) break;
                have += (ULONG)iosb.Information;
                while (used < have) {
                    ULONG len = buf[used];
                    if (len == 0 || len > 32) { used = have; break; }
                    if (used + 1 + len > have) break;
                    on_message(buf + used + 1, len);
                    used += 1 + len;
                    diag(44, ++messages);
                }
                if (used) {
                    RtlMoveMemory(buf, buf + used, have - used);
                    have -= used;
                }
            }
            ZwClose(conn);
            diag(40, 3);
        }
        ZwClose(listen_h);
        sleep_ms(1000);
    }
}

static NTSTATUS complete(PIRP irp, NTSTATUS status, ULONG_PTR information)
{
    irp->IoStatus.Status = status;
    irp->IoStatus.Information = information;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

static NTSTATUS internal_ioctl(PDEVICE_OBJECT device, PIRP irp)
{
    PORT_EXT *e = device->DeviceExtension;
    PIO_STACK_LOCATION st = IoGetCurrentIrpStackLocation(irp);
    ULONG in = st->Parameters.DeviceIoControl.InputBufferLength;
    ULONG out = st->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID buf = irp->AssociatedIrp.SystemBuffer;
    KIRQL irql;
    switch (st->Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_INTERNAL_KEYBOARD_CONNECT:
    case IOCTL_INTERNAL_MOUSE_CONNECT:
        if (in < sizeof(CONNECT_DATA)) return complete(irp, STATUS_INVALID_PARAMETER, 0);
        if (e->connect.ClassService) return complete(irp, STATUS_SHARING_VIOLATION, 0);
        irql = KeAcquireSpinLockRaiseToDpc(&g.lock);
        e->connect = *(CONNECT_DATA *)st->Parameters.DeviceIoControl.Type3InputBuffer;
        KeReleaseSpinLock(&g.lock, irql);
        diag(e->mouse ? 46 : 45, 1);
        return complete(irp, STATUS_SUCCESS, 0);
    case IOCTL_INTERNAL_KEYBOARD_DISCONNECT:
    case IOCTL_INTERNAL_MOUSE_DISCONNECT:
        irql = KeAcquireSpinLockRaiseToDpc(&g.lock);
        RtlZeroMemory(&e->connect, sizeof e->connect);
        KeReleaseSpinLock(&g.lock, irql);
        return complete(irp, STATUS_SUCCESS, 0);
    case IOCTL_KEYBOARD_QUERY_ATTRIBUTES: {
        KEYBOARD_ATTRIBUTES *a = buf;
        if (out < sizeof *a) return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        RtlZeroMemory(a, sizeof *a);
        a->KeyboardIdentifier.Type = 4;
        a->KeyboardMode = 1;
        a->NumberOfFunctionKeys = 12;
        a->NumberOfIndicators = 3;
        a->NumberOfKeysTotal = 101;
        a->InputDataQueueLength = 100;
        a->KeyRepeatMinimum.Rate = 2;
        a->KeyRepeatMinimum.Delay = 250;
        a->KeyRepeatMaximum.Rate = 30;
        a->KeyRepeatMaximum.Delay = 1000;
        return complete(irp, STATUS_SUCCESS, sizeof *a);
    }
    case IOCTL_KEYBOARD_QUERY_TYPEMATIC: {
        KEYBOARD_TYPEMATIC_PARAMETERS *t = buf;
        if (out < sizeof *t) return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        RtlZeroMemory(t, sizeof *t);
        t->Rate = 30;
        t->Delay = 250;
        return complete(irp, STATUS_SUCCESS, sizeof *t);
    }
    case IOCTL_KEYBOARD_QUERY_INDICATORS: {
        KEYBOARD_INDICATOR_PARAMETERS *i = buf;
        if (out < sizeof *i) return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        RtlZeroMemory(i, sizeof *i);
        return complete(irp, STATUS_SUCCESS, sizeof *i);
    }
    case IOCTL_KEYBOARD_QUERY_INDICATOR_TRANSLATION: {
        KEYBOARD_INDICATOR_TRANSLATION *t = buf;
        if (out < sizeof *t) return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        RtlZeroMemory(t, sizeof *t);
        return complete(irp, STATUS_SUCCESS, sizeof *t);
    }
    case IOCTL_KEYBOARD_SET_INDICATORS:
    case IOCTL_KEYBOARD_SET_TYPEMATIC:
        return complete(irp, STATUS_SUCCESS, 0);
    case IOCTL_MOUSE_QUERY_ATTRIBUTES: {
        MOUSE_ATTRIBUTES *a = buf;
        if (out < sizeof *a) return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        RtlZeroMemory(a, sizeof *a);
        a->MouseIdentifier = WHEELMOUSE_HID_HARDWARE;
        a->NumberOfButtons = 3;
        a->SampleRate = 60;
        a->InputDataQueueLength = 100;
        return complete(irp, STATUS_SUCCESS, sizeof *a);
    }
    default:
        return complete(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

static NTSTATUS start_done(PDEVICE_OBJECT device, PIRP irp, PVOID event)
{
    (void)device; (void)irp;
    KeSetEvent((PKEVENT)event, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS pnp(PDEVICE_OBJECT device, PIRP irp)
{
    PORT_EXT *e = device->DeviceExtension;
    PIO_STACK_LOCATION st = IoGetCurrentIrpStackLocation(irp);
    PDEVICE_OBJECT lower = e->lower;
    NTSTATUS status;
    if (st->MinorFunction == IRP_MN_START_DEVICE) {
        KEVENT event;
        KeInitializeEvent(&event, NotificationEvent, FALSE);
        IoCopyCurrentIrpStackLocationToNext(irp);
        IoSetCompletionRoutine(irp, start_done, &event, TRUE, TRUE, TRUE);
        status = IoCallDriver(lower, irp);
        if (status == STATUS_PENDING) {
            KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
            status = irp->IoStatus.Status;
        }
        if (NT_SUCCESS(status) && e->kind == KIND_PAD && e->iface.Buffer) IoSetDeviceInterfaceState(&e->iface, TRUE);
        if (NT_SUCCESS(status) && !g.thread) {
            HANDLE t;
            if (NT_SUCCESS(PsCreateSystemThread(&t, THREAD_ALL_ACCESS, NULL, NULL, NULL, listener, NULL))) {
                ZwClose(t);
                g.thread = TRUE;
            }
        }
        diag(e->kind == KIND_PAD ? 51 : e->mouse ? 48 : 47, NT_SUCCESS(status) ? 1 : 0x100 | ((ULONG)status & 0xff));
        return complete(irp, status, irp->IoStatus.Information);
    }
    if (st->MinorFunction == IRP_MN_REMOVE_DEVICE) {
        KIRQL irql = KeAcquireSpinLockRaiseToDpc(&g.lock);
        if (g.kbd == e) g.kbd = NULL;
        if (g.mou == e) g.mou = NULL;
        KeReleaseSpinLock(&g.lock, irql);
        if (e->iface.Buffer) {
            IoSetDeviceInterfaceState(&e->iface, FALSE);
            RtlFreeUnicodeString(&e->iface);
        }
        irp->IoStatus.Status = STATUS_SUCCESS;
        IoSkipCurrentIrpStackLocation(irp);
        status = IoCallDriver(lower, irp);
        IoDetachDevice(lower);
        IoDeleteDevice(device);
        return status;
    }
    if (st->MinorFunction == IRP_MN_QUERY_STOP_DEVICE || st->MinorFunction == IRP_MN_QUERY_REMOVE_DEVICE ||
        st->MinorFunction == IRP_MN_SURPRISE_REMOVAL || st->MinorFunction == IRP_MN_CANCEL_STOP_DEVICE ||
        st->MinorFunction == IRP_MN_CANCEL_REMOVE_DEVICE || st->MinorFunction == IRP_MN_STOP_DEVICE)
        irp->IoStatus.Status = STATUS_SUCCESS;
    IoSkipCurrentIrpStackLocation(irp);
    return IoCallDriver(lower, irp);
}

static NTSTATUS power(PDEVICE_OBJECT device, PIRP irp)
{
    PORT_EXT *e = device->DeviceExtension;
    IoSkipCurrentIrpStackLocation(irp);
    return PoCallDriver(e->lower, irp);
}

static NTSTATUS pass_down(PDEVICE_OBJECT device, PIRP irp)
{
    PORT_EXT *e = device->DeviceExtension;
    IoSkipCurrentIrpStackLocation(irp);
    return IoCallDriver(e->lower, irp);
}

static NTSTATUS create_close(PDEVICE_OBJECT device, PIRP irp)
{
    (void)device;
    return complete(irp, STATUS_SUCCESS, 0);
}

/* Programs talk to the pad; the keyboard and mouse only take their class drivers' internal calls. */
static NTSTATUS device_control(PDEVICE_OBJECT device, PIRP irp)
{
    PORT_EXT *e = device->DeviceExtension;
    if (e->kind == KIND_PAD) return pad_ioctl(irp, IoGetCurrentIrpStackLocation(irp));
    IoSkipCurrentIrpStackLocation(irp);
    return IoCallDriver(e->lower, irp);
}

static BOOLEAN has(const WCHAR *ids, ULONG chars, const WCHAR *word)
{
    ULONG i, j;
    for (i = 0; i + 3 <= chars; ++i) {
        for (j = 0; j < 3 && (ids[i + j] | 0x20) == (word[j] | 0x20); ++j) {}
        if (j == 3) return TRUE;
    }
    return FALSE;
}

/* The PDO's hardware ID says which device this is: ROOT\HWVSMOU is the mouse, ROOT\HWVSKBD the
 * keyboard. The pad is the device this driver reported itself, which has no hardware ID. */
static int device_kind(PDEVICE_OBJECT pdo)
{
    WCHAR ids[128];
    ULONG len = 0;
    if (!NT_SUCCESS(IoGetDeviceProperty(pdo, DevicePropertyHardwareID, sizeof ids, ids, &len)) || len < 8)
        return KIND_PAD;
    if (has(ids, len / sizeof(WCHAR), L"MOU")) return KIND_MOU;
    if (has(ids, len / sizeof(WCHAR), L"KBD")) return KIND_KBD;
    return KIND_PAD;
}

static NTSTATUS add_device(PDRIVER_OBJECT driver, PDEVICE_OBJECT pdo)
{
    int kind = device_kind(pdo);
    BOOLEAN mouse = kind == KIND_MOU;
    PDEVICE_OBJECT d;
    PORT_EXT *e;
    KIRQL irql;
    NTSTATUS s = IoCreateDevice(driver, sizeof *e, NULL,
                                kind == KIND_PAD ? FILE_DEVICE_UNKNOWN : mouse ? FILE_DEVICE_MOUSE : FILE_DEVICE_KEYBOARD,
                                FILE_DEVICE_SECURE_OPEN, FALSE, &d);
    if (!NT_SUCCESS(s)) return s;
    e = d->DeviceExtension;
    RtlZeroMemory(e, sizeof *e);
    e->self = d;
    e->mouse = mouse;
    e->kind = kind;
    e->lower = IoAttachDeviceToDeviceStack(d, pdo);
    if (!e->lower) {
        IoDeleteDevice(d);
        return STATUS_NO_SUCH_DEVICE;
    }
    if (kind == KIND_PAD) {
        if (!NT_SUCCESS(IoRegisterDeviceInterface(pdo, &XUSB_INTERFACE, NULL, &e->iface)))
            RtlZeroMemory(&e->iface, sizeof e->iface);
        /* A device the driver reported itself counts as started by it: no start request may come,
         * so the interface goes on here (and again at a start, which is harmless). */
        else
            diag(54, (ULONG)IoSetDeviceInterfaceState(&e->iface, TRUE));
    }
    d->Flags |= DO_BUFFERED_IO | DO_POWER_PAGABLE;
    d->Flags &= ~DO_DEVICE_INITIALIZING;
    if (kind != KIND_PAD) {
        irql = KeAcquireSpinLockRaiseToDpc(&g.lock);
        if (mouse) g.mou = e; else g.kbd = e;
        KeReleaseSpinLock(&g.lock, irql);
    }
    diag(kind == KIND_PAD ? 52 : mouse ? 50 : 49, 1);
    return STATUS_SUCCESS;
}

/* The pad's device: reported once, the first time this driver loads; Windows keeps it from then
 * on and hands it to add_device at every boot. A value under the service key remembers. */
static void report_pad(PDRIVER_OBJECT driver, PUNICODE_STRING registry)
{
    OBJECT_ATTRIBUTES oa;
    HANDLE key;
    UNICODE_STRING name;
    ULONG one = 1, got = 0;
    UCHAR info[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PDEVICE_OBJECT pdo = NULL;
    InitializeObjectAttributes(&oa, registry, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&key, KEY_READ | KEY_WRITE, &oa))) return;
    RtlInitUnicodeString(&name, L"PadReported");
    if (!NT_SUCCESS(ZwQueryValueKey(key, &name, KeyValuePartialInformation, info, sizeof info, &got)) &&
        NT_SUCCESS(IoReportDetectedDevice(driver, InterfaceTypeUndefined, (ULONG)-1, (ULONG)-1, NULL, NULL, FALSE, &pdo)) &&
        NT_SUCCESS(add_device(driver, pdo))) {
        ZwSetValueKey(key, &name, 0, REG_DWORD, &one, sizeof one);
        diag(53, 1);
    }
    ZwClose(key);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry)
{
    KeInitializeSpinLock(&g.lock);
    diag(43, 1);
    driver->MajorFunction[IRP_MJ_CREATE] = create_close;
    driver->MajorFunction[IRP_MJ_CLOSE] = create_close;
    driver->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = internal_ioctl;
    driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = device_control;
    driver->MajorFunction[IRP_MJ_PNP] = pnp;
    driver->MajorFunction[IRP_MJ_POWER] = power;
    driver->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = pass_down;
    driver->DriverExtension->AddDevice = add_device;
    report_pad(driver, registry);
    return STATUS_SUCCESS;
}
