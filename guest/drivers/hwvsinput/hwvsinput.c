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
 *   [len] [3] [buttons u16] [lt] [rt] [lx ly rx ry s16] [connected]  gamepad, XInput layout: player 1
 *   [len] [5] [slot] [buttons u16] [lt] [rt] [lx ly rx ry s16] [connected]  the same for players 1 to 4
 *   [len] [8] [slot] [gyro x y z s16] [accel x y z s16]      motion, DualShock 4 units (hwhid.sys)
 *   [len] [9] [kind]                                          player 1 is 0 an Xbox 360 controller, 1 a DualShock 4
 * A second port, HW_VSOCK_PORT_INPUT_BACK, carries messages the other way, to the app:
 *   [len] [6] [slot] [big motor] [small motor] [0]                  rumble
 * The pad device is a small bus: each connected player is a USB Xbox 360 controller on it, which
 * Windows' own xusb22.sys runs (usbpad.c). */
#include <ntddk.h>
#include <kbdmou.h>
#include <ntddkbd.h>
#include <ntddmou.h>
#include "hw_abi.h"
#include "usbpad.h"

#define FILE_DEVICE_SOCKET 0x0801
#define IOCTL_SOCKET_BIND CTL_CODE(FILE_DEVICE_SOCKET, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_SOCKET_LISTEN CTL_CODE(FILE_DEVICE_SOCKET, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define AF_VSOCK 40
#define VMADDR_CID_ANY 0xffffffffu
#define WHEEL_DELTA 120
#define PADS 4

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
    PAD pad[PADS];
    HANDLE back;                   /* the connection back to the app; back_lock guards writes */
    BOOLEAN ds4;                   /* player 1 as a DualShock 4 (message 9) */
    volatile LONG rumble[PADS];    /* 0x10000 | big << 8 | small, waiting for rumble_sender */
    KEVENT rumble_ready;
    PCALLBACK_OBJECT pads;         /* \Callback\HyperWinPad: hwhid.sys's gamepad twin listens */
    KEVENT back_lock;
} g;

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
    } else if (m[0] == 9 && len >= 2) {
        PAD *p = &g.pad[0];
        g.ds4 = m[1] == 1;
        usbpad_update(0, g.ds4, p->connected, p->buttons, p->lt, p->rt, p->lx, p->ly, p->rx, p->ry);
    } else if (m[0] == 8 && len >= 14 && m[1] < PADS) {
        SHORT motion[6];
        for (int i = 0; i < 6; i++) motion[i] = (SHORT)(m[2 + 2 * i] | m[3 + 2 * i] << 8);
        if (g.pads) ExNotifyCallback(g.pads, (PVOID)(ULONG_PTR)(0x100 | m[1]), motion);
    } else if ((m[0] == 3 && len >= 14) || (m[0] == 5 && len >= 15 && m[1] < PADS)) {
        ULONG slot = m[0] == 5 ? m[1] : 0;
        const UCHAR *q = m[0] == 5 ? m + 1 : m;
        PAD *p = &g.pad[slot];
        KIRQL irql = KeAcquireSpinLockRaiseToDpc(&g.lock);
        p->buttons = (USHORT)(q[1] | q[2] << 8);
        p->lt = q[3];
        p->rt = q[4];
        p->lx = (SHORT)(q[5] | q[6] << 8);
        p->ly = (SHORT)(q[7] | q[8] << 8);
        p->rx = (SHORT)(q[9] | q[10] << 8);
        p->ry = (SHORT)(q[11] | q[12] << 8);
        p->connected = q[13];
        p->packet++;
        KeReleaseSpinLock(&g.lock, irql);
        if (g.pads) ExNotifyCallback(g.pads, (PVOID)(ULONG_PTR)slot, (PVOID)(q + 1));
        usbpad_update(slot, slot == 0 && g.ds4, q[13] != 0, p->buttons, p->lt, p->rt, p->lx, p->ly, p->rx, p->ry);
    }
}

/* Sends one message back to the app, if it is listening. */
static void send_back(const UCHAR *msg, ULONG len)
{
    IO_STATUS_BLOCK iosb;
    KeWaitForSingleObject(&g.back_lock, Executive, KernelMode, FALSE, NULL);
    if (g.back) ZwWriteFile(g.back, NULL, NULL, NULL, &iosb, (PVOID)msg, len, NULL, NULL);
    KeSetEvent(&g.back_lock, IO_NO_INCREMENT, FALSE);
}

static NTSTATUS complete(PIRP irp, NTSTATUS status, ULONG_PTR information);

/* \Callback\HyperWinPadOut: a game set the DualShock 4 twin's motors (player 1). */
static void rumble_back(ULONG slot, UCHAR big, UCHAR small);
static VOID twin_rumble(PVOID context, PVOID slot, PVOID motors)
{
    (void)context;
    rumble_back((ULONG)(ULONG_PTR)slot, ((UCHAR *)motors)[0], ((UCHAR *)motors)[1]);
}

/* usbpad.c: a game set a player's motors. xusb22 can send that at DISPATCH_LEVEL, where writing
 * to the socket may not wait, so a thread at PASSIVE_LEVEL sends it on. */
static void rumble_back(ULONG slot, UCHAR big, UCHAR small)
{
    if (slot >= PADS) return;
    __atomic_store_n(&g.rumble[slot], 0x10000 | big << 8 | small, __ATOMIC_SEQ_CST);
    KeSetEvent(&g.rumble_ready, IO_NO_INCREMENT, FALSE);
}

static void rumble_sender(PVOID context)
{
    (void)context;
    for (;;) {
        KeWaitForSingleObject(&g.rumble_ready, Executive, KernelMode, FALSE, NULL);
        for (ULONG i = 0; i < PADS; i++) {
            LONG v = __atomic_exchange_n(&g.rumble[i], 0, __ATOMIC_SEQ_CST);
            UCHAR m[6] = { 5, 6, (UCHAR)i, (UCHAR)(v >> 8), (UCHAR)v, 0 };
            if (v) send_back(m, sizeof m);
        }
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

/* The way back to the app: accepts its connection on HW_VSOCK_PORT_INPUT_BACK and only writes. */
static void back_listener(PVOID context)
{
    (void)context;
    for (;;) {
        HANDLE listen_h = NULL, conn = NULL, old;
        SOCKADDR_VM addr = { AF_VSOCK, 0, HW_VSOCK_PORT_INPUT_BACK, VMADDR_CID_ANY };
        ULONG backlog = 1;
        NTSTATUS s = open_socket(NULL, &listen_h);
        if (!NT_SUCCESS(s)) { sleep_ms(1000); continue; }
        s = socket_ioctl(listen_h, IOCTL_SOCKET_BIND, &addr, sizeof addr);
        if (NT_SUCCESS(s)) s = socket_ioctl(listen_h, IOCTL_SOCKET_LISTEN, &backlog, sizeof backlog);
        if (!NT_SUCCESS(s)) { ZwClose(listen_h); sleep_ms(1000); continue; }
        while (NT_SUCCESS(open_socket(listen_h, &conn))) {
            KeWaitForSingleObject(&g.back_lock, Executive, KernelMode, FALSE, NULL);
            old = g.back;
            g.back = conn;
            KeSetEvent(&g.back_lock, IO_NO_INCREMENT, FALSE);
            if (old) ZwClose(old);
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
    if (usbpad_is_child(device)) return usbpad_dispatch(device, irp);
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
    if (usbpad_is_child(device)) return usbpad_dispatch(device, irp);
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
        if (NT_SUCCESS(status) && !g.thread) {
            HANDLE t;
            if (NT_SUCCESS(PsCreateSystemThread(&t, THREAD_ALL_ACCESS, NULL, NULL, NULL, listener, NULL))) {
                ZwClose(t);
                g.thread = TRUE;
            }
            if (NT_SUCCESS(PsCreateSystemThread(&t, THREAD_ALL_ACCESS, NULL, NULL, NULL, back_listener, NULL))) ZwClose(t);
            if (NT_SUCCESS(PsCreateSystemThread(&t, THREAD_ALL_ACCESS, NULL, NULL, NULL, rumble_sender, NULL))) ZwClose(t);
        }
        diag(e->kind == KIND_PAD ? 51 : e->mouse ? 48 : 47, NT_SUCCESS(status) ? 1 : 0x100 | ((ULONG)status & 0xff));
        return complete(irp, status, irp->IoStatus.Information);
    }
    if (e->kind == KIND_PAD && st->MinorFunction == IRP_MN_QUERY_DEVICE_RELATIONS &&
        st->Parameters.QueryDeviceRelations.Type == BusRelations) {
        PDEVICE_RELATIONS r = usbpad_relations();
        if (r) {
            irp->IoStatus.Information = (ULONG_PTR)r;
            irp->IoStatus.Status = STATUS_SUCCESS;
        }
        IoSkipCurrentIrpStackLocation(irp);
        return IoCallDriver(lower, irp);
    }
    if (st->MinorFunction == IRP_MN_REMOVE_DEVICE) {
        KIRQL irql = KeAcquireSpinLockRaiseToDpc(&g.lock);
        if (g.kbd == e) g.kbd = NULL;
        if (g.mou == e) g.mou = NULL;
        KeReleaseSpinLock(&g.lock, irql);
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
    if (usbpad_is_child(device)) return usbpad_dispatch(device, irp);
    PORT_EXT *e = device->DeviceExtension;
    IoSkipCurrentIrpStackLocation(irp);
    return PoCallDriver(e->lower, irp);
}

static NTSTATUS pass_down(PDEVICE_OBJECT device, PIRP irp)
{
    if (usbpad_is_child(device)) return usbpad_dispatch(device, irp);
    PORT_EXT *e = device->DeviceExtension;
    IoSkipCurrentIrpStackLocation(irp);
    return IoCallDriver(e->lower, irp);
}

static NTSTATUS create_close(PDEVICE_OBJECT device, PIRP irp)
{
    if (usbpad_is_child(device)) return usbpad_dispatch(device, irp);
    return complete(irp, STATUS_SUCCESS, 0);
}

static NTSTATUS device_control(PDEVICE_OBJECT device, PIRP irp)
{
    PORT_EXT *e = device->DeviceExtension;
    if (usbpad_is_child(device)) return usbpad_dispatch(device, irp);
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
    if (kind == KIND_PAD) usbpad_set_bus(pdo);   /* the players hang off this device */
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
    KeInitializeEvent(&g.back_lock, SynchronizationEvent, TRUE);
    KeInitializeEvent(&g.rumble_ready, SynchronizationEvent, FALSE);
    usbpad_init(driver, rumble_back);
    {
        UNICODE_STRING name;
        OBJECT_ATTRIBUTES oa;
        RtlInitUnicodeString(&name, L"\\Callback\\HyperWinPad");
        InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_PERMANENT, NULL, NULL);
        if (!NT_SUCCESS(ExCreateCallback(&g.pads, &oa, TRUE, TRUE))) g.pads = NULL;
        PCALLBACK_OBJECT out;
        RtlInitUnicodeString(&name, L"\\Callback\\HyperWinPadOut");
        if (NT_SUCCESS(ExCreateCallback(&out, &oa, TRUE, TRUE))) ExRegisterCallback(out, twin_rumble, NULL);
    }
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

/* The players' USB bus, built as part of this file: both use the same inline helpers. */
#include "usbpad.c"
