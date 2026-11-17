/* HyperWin keyboard and absolute mouse over virtio-vsock.
 * A keyboard and mouse port driver (the i8042prt role) under the inbox kbdclass and mouclass
 * filters, on two root-enumerated devices: ROOT\HWVSKBD and ROOT\HWVSMOU. It imports nothing but
 * ntoskrnl, so it can start at boot. A system thread listens on vsock port HW_VSOCK_PORT_INPUT
 * through the stock viosock driver; the Android app connects and sends framed messages:
 *   [len] [1] [make lo] [make hi] [flags: 1 break, 2 E0]          keyboard scan code (set 1)
 *   [len] [2] [x lo] [x hi] [y lo] [y hi] [buttons] [wheel]       absolute pointer, 0..65535 */
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

typedef struct {
    PDEVICE_OBJECT self, lower;
    BOOLEAN mouse;
    CONNECT_DATA connect;
} PORT_EXT;

static struct {
    KSPIN_LOCK lock;
    PORT_EXT *kbd, *mou;
    UCHAR buttons;
    BOOLEAN thread;
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

static void on_message(const UCHAR *m, ULONG len)
{
    if (m[0] == 1 && len >= 4) {
        KEYBOARD_INPUT_DATA k;
        RtlZeroMemory(&k, sizeof k);
        k.MakeCode = (USHORT)(m[1] | m[2] << 8);
        k.Flags = (m[3] & 1 ? KEY_BREAK : KEY_MAKE) | (m[3] & 2 ? KEY_E0 : 0);
        deliver(FALSE, &k);
    } else if (m[0] == 2 && len >= 7) {
        MOUSE_INPUT_DATA d;
        UCHAR b = m[5], changed = (UCHAR)(b ^ g.buttons);
        RtlZeroMemory(&d, sizeof d);
        d.Flags = MOUSE_MOVE_ABSOLUTE;
        d.LastX = m[1] | m[2] << 8;
        d.LastY = m[3] | m[4] << 8;
        if (changed & 1) d.ButtonFlags |= b & 1 ? MOUSE_LEFT_BUTTON_DOWN : MOUSE_LEFT_BUTTON_UP;
        if (changed & 2) d.ButtonFlags |= b & 2 ? MOUSE_RIGHT_BUTTON_DOWN : MOUSE_RIGHT_BUTTON_UP;
        if (changed & 4) d.ButtonFlags |= b & 4 ? MOUSE_MIDDLE_BUTTON_DOWN : MOUSE_MIDDLE_BUTTON_UP;
        if (len >= 7 && m[6]) {
            d.ButtonFlags |= MOUSE_WHEEL;
            d.ButtonData = (USHORT)(SHORT)((CHAR)m[6] * WHEEL_DELTA);
        }
        g.buttons = b;
        deliver(TRUE, &d);
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
        if (NT_SUCCESS(status) && !g.thread) {
            HANDLE t;
            if (NT_SUCCESS(PsCreateSystemThread(&t, THREAD_ALL_ACCESS, NULL, NULL, NULL, listener, NULL))) {
                ZwClose(t);
                g.thread = TRUE;
            }
        }
        diag(e->mouse ? 48 : 47, NT_SUCCESS(status) ? 1 : 0x100 | ((ULONG)status & 0xff));
        return complete(irp, status, irp->IoStatus.Information);
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

/* The PDO's hardware ID says which half this device is: ROOT\HWVSMOU is the mouse. */
static BOOLEAN is_mouse(PDEVICE_OBJECT pdo)
{
    WCHAR ids[128];
    ULONG len = 0, i;
    if (!NT_SUCCESS(IoGetDeviceProperty(pdo, DevicePropertyHardwareID, sizeof ids, ids, &len))) return FALSE;
    for (i = 0; i + 3 < len / sizeof(WCHAR); ++i)
        if ((ids[i] == L'M' || ids[i] == L'm') && (ids[i + 1] == L'O' || ids[i + 1] == L'o') &&
            (ids[i + 2] == L'U' || ids[i + 2] == L'u'))
            return TRUE;
    return FALSE;
}

static NTSTATUS add_device(PDRIVER_OBJECT driver, PDEVICE_OBJECT pdo)
{
    BOOLEAN mouse = is_mouse(pdo);
    PDEVICE_OBJECT d;
    PORT_EXT *e;
    KIRQL irql;
    NTSTATUS s = IoCreateDevice(driver, sizeof *e, NULL, mouse ? FILE_DEVICE_MOUSE : FILE_DEVICE_KEYBOARD, 0, FALSE, &d);
    if (!NT_SUCCESS(s)) return s;
    e = d->DeviceExtension;
    RtlZeroMemory(e, sizeof *e);
    e->self = d;
    e->mouse = mouse;
    e->lower = IoAttachDeviceToDeviceStack(d, pdo);
    if (!e->lower) {
        IoDeleteDevice(d);
        return STATUS_NO_SUCH_DEVICE;
    }
    d->Flags |= DO_BUFFERED_IO | DO_POWER_PAGABLE;
    d->Flags &= ~DO_DEVICE_INITIALIZING;
    irql = KeAcquireSpinLockRaiseToDpc(&g.lock);
    if (mouse) g.mou = e; else g.kbd = e;
    KeReleaseSpinLock(&g.lock, irql);
    diag(mouse ? 50 : 49, 1);
    return STATUS_SUCCESS;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry)
{
    (void)registry;
    KeInitializeSpinLock(&g.lock);
    diag(43, 1);
    driver->MajorFunction[IRP_MJ_CREATE] = create_close;
    driver->MajorFunction[IRP_MJ_CLOSE] = create_close;
    driver->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = internal_ioctl;
    driver->MajorFunction[IRP_MJ_PNP] = pnp;
    driver->MajorFunction[IRP_MJ_POWER] = power;
    driver->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = pass_down;
    driver->DriverExtension->AddDevice = add_device;
    return STATUS_SUCCESS;
}
