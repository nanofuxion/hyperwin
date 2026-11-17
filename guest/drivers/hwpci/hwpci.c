/* PCI bus driver for crosvm's legacy CAM window (pci-host-cam-generic).
 * Bound to ACPI\HWPC0001; see CONTRACT.md for the ACPI node it expects.
 * Children look like pci.sys PDOs: same ID strings, BUS_INTERFACE_STANDARD config access,
 * firmware-assigned BARs and one shared level INTx each. No MSI. */
#include <ntddk.h>
#include <acpiioct.h>
#include "hwpci_shim.h"
#include "hw_abi.h"

/* Progress for the host debug tap (HW_DIAG in the shared window). The window is reserved RAM. */
static volatile ULONG *trace_ring;

/* Trace of completed IRPs: diag[32] count, diag[33..95] entries (irp low 16 bits, major, minor). */
static void trace(PIRP irp, UCHAR tag)
{
    PIO_STACK_LOCATION st;
    ULONG n;
    if (!trace_ring) return;
    st = IoGetCurrentIrpStackLocation(irp);
    n = trace_ring[32]++;
    trace_ring[33 + n % 63] = (ULONG)((ULONG_PTR)irp & 0xffff) << 16 | (ULONG)tag << 12 |
                              (ULONG)(st->MajorFunction & 0xf) << 8 | st->MinorFunction;
}

/* Config-space trace of device 4: diag[32] count, diag[33..95] (write<<31, len<<24, offset<<16, low 16 bits). */
static void cfg_trace(ULONG fnoff, int write, ULONG offset, ULONG length, const UCHAR *data)
{
    ULONG n, v = 0, i;
    if (!trace_ring || fnoff != (4u << 11)) return;
    for (i = 0; i < length && i < 2; ++i) v |= (ULONG)data[i] << (8 * i);
    n = trace_ring[32]++;
    if (n < 160) trace_ring[120 + n] = (ULONG)write << 31 | (length & 0x7f) << 24 | (offset & 0xff) << 16 | v;
}

static void pdo_trace(ULONG dev, UCHAR major, UCHAR minor, NTSTATUS status, ULONG extra)
{
    ULONG n;
    if (!trace_ring || dev != 4) return;
    n = trace_ring[32]++;
    if (n < 160) trace_ring[120 + n] = 0x40000000u | (ULONG)(major & 0x3f) << 24 | (ULONG)minor << 16 |
                                     ((ULONG)status & 0xff) << 8 | (extra & 0xff);
}

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

#define TAG 'icpH'
#define MAX_CHILDREN 64
#define MAX_IRQS 32
#define MAX_ROUTES 128
#define SIG(a, b, c, d) ((ULONG)(a) << 24 | (ULONG)(b) << 16 | (ULONG)(c) << 8 | (ULONG)(d))
#define CMD_MEMORY 0x0002
#define CMD_MASTER 0x0004
#define CMD_INTX_OFF 0x0400

static const GUID guid_bus_if = { 0x496b8280, 0x6f25, 0x11d0, { 0xbe, 0xaf, 0x08, 0x00, 0x2b, 0xe2, 0x09, 0x2f } };
static const GUID guid_pci_bus_if = { 0x496b8281, 0x6f25, 0x11d0, { 0xbe, 0xaf, 0x08, 0x00, 0x2b, 0xe2, 0x09, 0x2f } };
static const GUID guid_bus_type_pci = { 0xc8ebdfb0, 0xb510, 0x11d0, { 0x80, 0xe5, 0x00, 0xa0, 0xc9, 0x25, 0x42, 0xe3 } };

typedef struct {
    BOOLEAN fdo;
} COMMON;

typedef struct {
    COMMON c;
    PDEVICE_OBJECT self, pdo, lower;
    KSPIN_LOCK lock;
    PUCHAR cam;
    ULONG cam_len;
    ULONG nirq, nroute, nchild;
    CM_PARTIAL_RESOURCE_DESCRIPTOR irq[MAX_IRQS];
    struct { UCHAR dev, pin, idx; } route[MAX_ROUTES];
    PDEVICE_OBJECT child[MAX_CHILDREN];
} FDO_EXT;

typedef struct {
    ULONG64 base;
    ULONG len;
    USHORT flags;
} BAR;

typedef struct {
    COMMON c;
    FDO_EXT *fdo;
    PDEVICE_OBJECT self;
    ULONG fnoff;
    UCHAR bus, dev, fn, rev, cls, sub, pi, nbar;
    USHORT ven, did, sven, sdid;
    BOOLEAN decode, has_irq;
    LONG paging;
    BAR bar[6];
    CM_PARTIAL_RESOURCE_DESCRIPTOR irq;
} PDO_EXT;

/* KVM can only emulate MMIO from single-register ldr/str without writeback; force those forms. */
static ULONG cam_rd(FDO_EXT *f, ULONG off)
{
    ULONG v;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(f->cam + off) : "memory");
    return v;
}

static void cam_wr(FDO_EXT *f, ULONG off, ULONG v, ULONG size)
{
    PUCHAR p = f->cam + off;
    if (size == 1) __asm__ volatile("strb %w0, [%1]" : : "r"(v), "r"(p) : "memory");
    else if (size == 2) __asm__ volatile("strh %w0, [%1]" : : "r"(v), "r"(p) : "memory");
    else __asm__ volatile("str %w0, [%1]" : : "r"(v), "r"(p) : "memory");
}

static ULONG rd32(FDO_EXT *f, ULONG fnoff, ULONG reg)
{
    return cam_rd(f, fnoff + (reg & 0xfc));
}

static ULONG fn_offset(ULONG bus, ULONG dev, ULONG fn)
{
    return bus << 16 | dev << 11 | fn << 8;
}

/* Every access is one aligned load or store, so callers above DISPATCH_LEVEL go unlocked. */
static KIRQL cam_lock(FDO_EXT *f)
{
    KIRQL irql = KeGetCurrentIrql();
    return irql > DISPATCH_LEVEL ? irql : KeAcquireSpinLockRaiseToDpc(&f->lock);
}

static void cam_unlock(FDO_EXT *f, KIRQL irql)
{
    if (irql <= DISPATCH_LEVEL) KeReleaseSpinLock(&f->lock, irql);
}

static ULONG cfg_read(FDO_EXT *f, ULONG fnoff, PVOID buffer, ULONG offset, ULONG length)
{
    PUCHAR out = buffer;
    ULONG i, w = 0;
    KIRQL irql;
    if (!f->cam || offset >= 256 || fnoff + 256 > f->cam_len) return 0;
    if (length > 256 - offset) length = 256 - offset;
    irql = cam_lock(f);
    for (i = 0; i < length; ++i) {
        ULONG r = offset + i;
        if (i == 0 || (r & 3) == 0) w = rd32(f, fnoff, r);
        out[i] = (UCHAR)(w >> ((r & 3) * 8));
    }
    cam_unlock(f, irql);
    cfg_trace(fnoff, 0, offset, length, buffer);
    return length;
}

static ULONG cfg_write(FDO_EXT *f, ULONG fnoff, PVOID buffer, ULONG offset, ULONG length)
{
    PUCHAR in = buffer;
    ULONG i = 0;
    KIRQL irql;
    if (!f->cam || offset >= 256 || fnoff + 256 > f->cam_len) return 0;
    if (length > 256 - offset) length = 256 - offset;
    irql = cam_lock(f);
    while (i < length) {
        ULONG r = offset + i, n = 1, v = in[i];
        if (!(r & 3) && length - i >= 4) {
            n = 4;
            v |= (ULONG)in[i + 1] << 8 | (ULONG)in[i + 2] << 16 | (ULONG)in[i + 3] << 24;
        } else if (!(r & 1) && length - i >= 2) {
            n = 2;
            v |= (ULONG)in[i + 1] << 8;
        }
        cam_wr(f, fnoff + r, v, n);
        i += n;
    }
    cam_unlock(f, irql);
    cfg_trace(fnoff, 1, offset, length, buffer);
    return length;
}

static void set_command(PDO_EXT *p, USHORT set, USHORT clear)
{
    USHORT cmd;
    if (cfg_read(p->fdo, p->fnoff, &cmd, 4, 2) != 2) return;
    cmd = (USHORT)((cmd | set) & ~clear);
    cfg_write(p->fdo, p->fnoff, &cmd, 4, 2);
}

/* Lock held. Decode is off while ones are written so the probe never claims stray addresses. */
static void probe_bars(FDO_EXT *f, PDO_EXT *p)
{
    ULONG o = p->fnoff, cmd = rd32(f, o, 4) & 0xffff, i;
    p->decode = (cmd & CMD_MEMORY) != 0;
    cam_wr(f, o + 4, cmd & ~3u, 2);
    for (i = 0; i < 6; ++i) {
        ULONG reg = 0x10 + i * 4, lo = rd32(f, o, reg), hi = 0, slo, shi = 0xffffffff;
        BOOLEAN is64 = (lo & 7) == 4;
        ULONG64 mask, size, base;
        if (lo & 1) continue;
        if (is64 && i == 5) break;
        cam_wr(f, o + reg, 0xffffffff, 4);
        slo = rd32(f, o, reg);
        cam_wr(f, o + reg, lo, 4);
        if (is64) {
            hi = rd32(f, o, reg + 4);
            cam_wr(f, o + reg + 4, 0xffffffff, 4);
            shi = rd32(f, o, reg + 4);
            cam_wr(f, o + reg + 4, hi, 4);
        }
        mask = (ULONG64)shi << 32 | (slo & ~0xfu);
        size = ~mask + 1;
        base = (ULONG64)hi << 32 | (lo & ~0xfu);
        if (((slo & ~0xfu) || (is64 && shi)) && base && size <= 0xffffffffULL && base + size > base) {
            BAR *b = &p->bar[p->nbar++];
            b->base = base;
            b->len = (ULONG)size;
            b->flags = CM_RESOURCE_MEMORY_READ_WRITE | ((lo & 8) ? CM_RESOURCE_MEMORY_PREFETCHABLE : 0);
        }
        if (is64) ++i;
    }
    cam_wr(f, o + 4, cmd, 2);
}

static void add_child(FDO_EXT *f, ULONG bus, ULONG dev, ULONG fn, const UCHAR *path, ULONG depth)
{
    PDEVICE_OBJECT d;
    PDO_EXT *p;
    ULONG v, pin, i, rdev = dev;
    KIRQL irql;
    if (f->nchild >= MAX_CHILDREN) return;
    if (!NT_SUCCESS(IoCreateDevice(f->self->DriverObject, sizeof *p, NULL, FILE_DEVICE_UNKNOWN,
                                   FILE_AUTOGENERATED_DEVICE_NAME, FALSE, &d)))
        return;
    p = d->DeviceExtension;
    RtlZeroMemory(p, sizeof *p);
    p->fdo = f;
    p->self = d;
    p->bus = (UCHAR)bus;
    p->dev = (UCHAR)dev;
    p->fn = (UCHAR)fn;
    p->fnoff = fn_offset(bus, dev, fn);
    irql = cam_lock(f);
    v = rd32(f, p->fnoff, 0);
    p->ven = (USHORT)v;
    p->did = (USHORT)(v >> 16);
    v = rd32(f, p->fnoff, 8);
    p->rev = (UCHAR)v;
    p->pi = (UCHAR)(v >> 8);
    p->sub = (UCHAR)(v >> 16);
    p->cls = (UCHAR)(v >> 24);
    v = rd32(f, p->fnoff, 0x2c);
    p->sven = (USHORT)v;
    p->sdid = (USHORT)(v >> 16);
    pin = (rd32(f, p->fnoff, 0x3c) >> 8) & 0xff;
    probe_bars(f, p);
    cam_unlock(f, irql);
    if (pin >= 1 && pin <= 4) {
        while (depth--) {
            pin = (pin - 1 + rdev) % 4 + 1;
            rdev = path[depth];
        }
        for (i = 0; i < f->nroute; ++i) {
            if (f->route[i].dev == rdev && f->route[i].pin == pin && f->route[i].idx < f->nirq) {
                p->irq = f->irq[f->route[i].idx];
                p->irq.ShareDisposition = CmResourceShareShared;
                p->has_irq = TRUE;
                break;
            }
        }
    }
    d->Flags |= DO_POWER_PAGABLE;
    d->Flags &= ~DO_DEVICE_INITIALIZING;
    f->child[f->nchild++] = d;
}

static void scan_bus(FDO_EXT *f, ULONG bus, const UCHAR *path, ULONG depth)
{
    ULONG dev, fn;
    if (depth > 8 || ((bus + 1) << 16) > f->cam_len) return;
    for (dev = 0; dev < 32; ++dev) {
        for (fn = 0; fn < 8; ++fn) {
            ULONG o = fn_offset(bus, dev, fn), id, hdr, buses = 0;
            KIRQL irql = cam_lock(f);
            id = rd32(f, o, 0);
            hdr = (rd32(f, o, 0xc) >> 16) & 0xff;
            if ((hdr & 0x7f) == 1 && (USHORT)id != 0xffff && (USHORT)id != 0) {
                buses = rd32(f, o, 0x18);
                if (((buses >> 8) & 0xff) > bus)
                    cam_wr(f, o + 4, (rd32(f, o, 4) & 0xffff) | CMD_MEMORY | CMD_MASTER, 2);
            }
            cam_unlock(f, irql);
            if ((USHORT)id == 0xffff || (USHORT)id == 0) {
                if (fn == 0) break;
                continue;
            }
            if ((hdr & 0x7f) == 0) {
                add_child(f, bus, dev, fn, path, depth);
            } else if ((hdr & 0x7f) == 1 && ((buses >> 8) & 0xff) > bus) {
                UCHAR next[9];
                ULONG i;
                for (i = 0; i < depth; ++i) next[i] = path[i];
                next[depth] = (UCHAR)dev;
                scan_bus(f, (buses >> 8) & 0xff, next, depth + 1);
            }
            if (fn == 0 && !(hdr & 0x80)) break;
        }
    }
}

static void read_routes(FDO_EXT *f)
{
    ULONG outlen = sizeof(ACPI_EVAL_OUTPUT_BUFFER) + MAX_ROUTES * 3 * sizeof(ACPI_METHOD_ARGUMENT);
    ACPI_EVAL_OUTPUT_BUFFER *out = ExAllocatePool2(POOL_FLAG_NON_PAGED, outlen, TAG);
    ACPI_EVAL_INPUT_BUFFER in;
    IO_STATUS_BLOCK iosb;
    KEVENT event;
    PIRP irp;
    NTSTATUS status;
    PUCHAR a, end;
    ULONG vals[3], n = 0;
    if (!out) return;
    in.Signature = SIG('B', 'i', 'e', 'A');
    in.MethodNameAsUlong = SIG('B', 'A', 'T', 'R');
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    irp = IoBuildDeviceIoControlRequest(IOCTL_ACPI_EVAL_METHOD, f->lower, &in, sizeof in, out, outlen,
                                        FALSE, &event, &iosb);
    if (!irp) goto done;
    status = IoCallDriver(f->lower, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = iosb.Status;
    }
    if (!NT_SUCCESS(status) || out->Signature != SIG('B', 'o', 'e', 'A')) goto done;
    a = (PUCHAR)out->Argument;
    end = (PUCHAR)out + (iosb.Information < outlen ? iosb.Information : outlen);
    while (a + 4 <= end && f->nroute < MAX_ROUTES) {
        USHORT type, len;
        ULONG64 v = 0;
        RtlCopyMemory(&type, a, 2);
        RtlCopyMemory(&len, a + 2, 2);
        if (a + 4 + (len < 4 ? 4 : len) > end || type != ACPI_METHOD_ARGUMENT_INTEGER) break;
        RtlCopyMemory(&v, a + 4, len < 8 ? len : 8);
        a += 4 + (len < 4 ? 4 : len);
        vals[n++] = v > 0xff ? 0xff : (ULONG)v;
        if (n == 3) {
            f->route[f->nroute].dev = (UCHAR)vals[0];
            f->route[f->nroute].pin = (UCHAR)vals[1];
            f->route[f->nroute].idx = (UCHAR)vals[2];
            ++f->nroute;
            n = 0;
        }
    }
done:
    ExFreePool(out);
}

/* Windows' ACPI driver gives this device no resources even though its _CRS evaluates.
 * Read _CRS ourselves: the CAM window and the interrupts children inherit. */
static NTSTATUS crs_fallback(FDO_EXT *f, PHYSICAL_ADDRESS *cam, ULONG *cam_len)
{
    ULONG outlen = 2048, i, len;
    ACPI_EVAL_OUTPUT_BUFFER *out = ExAllocatePool2(POOL_FLAG_NON_PAGED, outlen, TAG);
    ACPI_EVAL_INPUT_BUFFER in;
    IO_STATUS_BLOCK iosb;
    KEVENT event;
    PIRP irp;
    NTSTATUS s = STATUS_INSUFFICIENT_RESOURCES;
    const UCHAR *b;
    if (!out) return s;
    in.Signature = SIG('B', 'i', 'e', 'A');
    in.MethodNameAsUlong = SIG('S', 'R', 'C', '_');
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    irp = IoBuildDeviceIoControlRequest(IOCTL_ACPI_EVAL_METHOD, f->lower, &in, sizeof in, out, outlen,
                                        FALSE, &event, &iosb);
    if (!irp) goto done;
    s = IoCallDriver(f->lower, irp);
    if (s == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        s = iosb.Status;
    }
    if (!NT_SUCCESS(s)) goto done;
    s = STATUS_DEVICE_CONFIGURATION_ERROR;
    if (!out->Count || out->Argument[0].Type != ACPI_METHOD_ARGUMENT_BUFFER) goto done;
    b = out->Argument[0].Data;
    len = out->Argument[0].DataLength;
    for (i = 0; i + 3 <= len;) {
        UCHAR tag = b[i];
        ULONG item;
        if (!(tag & 0x80)) {
            if ((tag & 0x78) == 0x78) break;
            i += 1 + (tag & 7);
            continue;
        }
        item = b[i + 1] | (ULONG)b[i + 2] << 8;
        if (i + 3 + item > len) break;
        if (tag == 0x86 && item >= 9 && !*cam_len) {
            ULONG base, l;
            RtlCopyMemory(&base, b + i + 4, 4);
            RtlCopyMemory(&l, b + i + 8, 4);
            cam->QuadPart = base;
            *cam_len = l > 0x1000000 ? 0x1000000 : l;
        } else if (tag == 0x8a && item >= 43 && b[i + 3] == 0 && !*cam_len) {
            RtlCopyMemory(&cam->QuadPart, b + i + 14, 8);
            {
                ULONGLONG l;
                RtlCopyMemory(&l, b + i + 38, 8);
                *cam_len = l > 0x1000000 ? 0x1000000 : (ULONG)l;
            }
        } else if (tag == 0x89 && item >= 6) {
            ULONG k, count = b[i + 4];
            for (k = 0; k < count && 2 + 4 * k + 4 <= item && f->nirq < MAX_IRQS; ++k) {
                PCM_PARTIAL_RESOURCE_DESCRIPTOR d = &f->irq[f->nirq++];
                ULONG gsiv;
                RtlCopyMemory(&gsiv, b + i + 5 + 4 * k, 4);
                RtlZeroMemory(d, sizeof *d);
                d->Type = CmResourceTypeInterrupt;
                d->ShareDisposition = CmResourceShareShared;
                d->Flags = (b[i + 3] & 2) ? CM_RESOURCE_INTERRUPT_LATCHED : CM_RESOURCE_INTERRUPT_LEVEL_SENSITIVE;
                d->u.Interrupt.Level = gsiv;
                d->u.Interrupt.Vector = gsiv;
                d->u.Interrupt.Affinity = (KAFFINITY)-1;
            }
        }
        i += 3 + item;
    }
    s = *cam_len ? STATUS_SUCCESS : STATUS_DEVICE_CONFIGURATION_ERROR;
done:
    ExFreePool(out);
    return s;
}

/* Diagnostic: 20 s after start, record each child's Windows problem code (diag[16 + i]). */
static void watch_children(PVOID context)
{
    static const DEVPROPKEY problem = { { 0x4340a6c5, 0x93fa, 0x4706, { 0x97, 0x2c, 0x7b, 0x64, 0x80, 0x08, 0xa5, 0xa7 } }, 3 };
    static const DEVPROPKEY status = { { 0x4340a6c5, 0x93fa, 0x4706, { 0x97, 0x2c, 0x7b, 0x64, 0x80, 0x08, 0xa5, 0xa7 } }, 12 };
    FDO_EXT *f = context;
    LARGE_INTEGER delay;
    ULONG round, i;
    for (round = 0; round < 6; ++round) {
        delay.QuadPart = -100000000LL;
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
        for (i = 0; i < f->nchild && i < 12; ++i) {
            ULONG code = 0xffff, st = 0, req = 0;
            DEVPROPTYPE type;
            IoGetDevicePropertyData(f->child[i], &problem, 0, 0, sizeof code, &code, &req, &type);
            IoGetDevicePropertyData(f->child[i], &status, 0, 0, sizeof st, &st, &req, &type);
            diag(16 + i, (code & 0xff) << 24 | (st & 0xffffff));
        }
        diag(31, round + 1);
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static BOOLEAN NTAPI probe_isr(PKINTERRUPT interrupt, PVOID context)
{
    (void)interrupt; (void)context;
    return FALSE;
}

/* Diagnostic: can the system connect one of this bus's INTx lines at all? diag[114] = status. */
static void probe_connect(FDO_EXT *f, PCM_RESOURCE_LIST raw, PCM_RESOURCE_LIST tr)
{
    ULONG i;
    for (i = 0; i < tr->List[0].PartialResourceList.Count; ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR d = &tr->List[0].PartialResourceList.PartialDescriptors[i];
        PKINTERRUPT obj = NULL;
        NTSTATUS s;
        if (d->Type != CmResourceTypeInterrupt) continue;
        s = IoConnectInterrupt(&obj, probe_isr, f, NULL, d->u.Interrupt.Vector, (KIRQL)d->u.Interrupt.Level,
                               (KIRQL)d->u.Interrupt.Level,
                               (d->Flags & CM_RESOURCE_INTERRUPT_LATCHED) ? Latched : LevelSensitive, TRUE,
                               d->u.Interrupt.Affinity, FALSE);
        diag(114, (ULONG)s);
        if (NT_SUCCESS(s)) IoDisconnectInterrupt(obj);
        (void)raw;
        return;
    }
}

static NTSTATUS fdo_start(FDO_EXT *f, PIO_STACK_LOCATION stack)
{
    PCM_RESOURCE_LIST raw = stack->Parameters.StartDevice.AllocatedResources;
    PCM_RESOURCE_LIST tr = stack->Parameters.StartDevice.AllocatedResourcesTranslated;
    PHYSICAL_ADDRESS cam = { 0 };
    ULONG cam_len = 0, i;
    UCHAR path[1];
    if (f->cam) return STATUS_SUCCESS;
    diag(0, raw && raw->Count ? 0x100 | raw->List[0].PartialResourceList.Count : 0x200);
    if (!raw || !tr || !raw->Count || !tr->Count) {
        NTSTATUS s = crs_fallback(f, &cam, &cam_len);
        diag(2, NT_SUCCESS(s) ? 0x20 : 0x30);
        if (!NT_SUCCESS(s)) return s;
        goto map;
    }
    for (i = 0; i < tr->List[0].PartialResourceList.Count; ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR d = &tr->List[0].PartialResourceList.PartialDescriptors[i];
        if (d->Type == CmResourceTypeDeviceSpecific) break;
        if (d->Type == CmResourceTypeMemory && !cam_len) {
            cam = d->u.Memory.Start;
            cam_len = d->u.Memory.Length;
        }
    }
    for (i = 0; i < tr->List[0].PartialResourceList.Count && i < 9; ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR d = &tr->List[0].PartialResourceList.PartialDescriptors[i];
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = &raw->List[0].PartialResourceList.PartialDescriptors[i];
        if (d->Type == CmResourceTypeInterrupt)
            diag(104 + i, r->u.Interrupt.Vector << 24 | (d->u.Interrupt.Level & 0xff) << 16 | (d->u.Interrupt.Vector & 0xffff));
    }
    probe_connect(f, raw, tr);
    for (i = 0; i < raw->List[0].PartialResourceList.Count; ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR d = &raw->List[0].PartialResourceList.PartialDescriptors[i];
        if (d->Type == CmResourceTypeDeviceSpecific) break;
        if (d->Type == CmResourceTypeInterrupt && f->nirq < MAX_IRQS) f->irq[f->nirq++] = *d;
    }
map:
    if (cam_len > 0x1000000) cam_len = 0x1000000;
    diag(2, 3);
    if (cam_len < 0x10000) { diag(2, 0x31); return STATUS_DEVICE_CONFIGURATION_ERROR; }
    f->cam = MmMapIoSpace(cam, cam_len, MmNonCached);
    if (!f->cam) { diag(2, 0x32); return STATUS_INSUFFICIENT_RESOURCES; }
    f->cam_len = cam_len;
    read_routes(f);
    scan_bus(f, 0, path, 0);
    diag(3, f->nchild | f->nirq << 8 | f->nroute << 16);
    for (i = 0; i < f->nchild && i < 12; ++i) {
        PDO_EXT *c = f->child[i]->DeviceExtension;
        if (c->did == 0x1042) diag(28 + (c->dev & 3), (ULONG)c->rev << 24 | (ULONG)c->sub << 16 | (ULONG)c->pi << 8 | c->dev);
        diag(4 + i, (ULONG)c->dev << 27 | (ULONG)(c->has_irq ? 1 : 0) << 26 | (ULONG)c->nbar << 22 |
                    (ULONG)(c->did & 0xfff) << 8 | c->cls);
    }
    diag(2, 4);
    {
        HANDLE t;
        if (NT_SUCCESS(PsCreateSystemThread(&t, THREAD_ALL_ACCESS, NULL, NULL, NULL, watch_children, f))) ZwClose(t);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS complete(PIRP irp, NTSTATUS status)
{
    irp->IoStatus.Status = status;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

static NTSTATUS sync_done(PDEVICE_OBJECT device, PIRP irp, PVOID event)
{
    (void)device; (void)irp;
    KeSetEvent((PKEVENT)event, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS forward_sync(FDO_EXT *f, PIRP irp)
{
    KEVENT event;
    NTSTATUS status;
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetCompletionRoutine(irp, sync_done, &event, TRUE, TRUE, TRUE);
    status = IoCallDriver(f->lower, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = irp->IoStatus.Status;
    }
    return status;
}

static NTSTATUS pass_down(FDO_EXT *f, PIRP irp)
{
    IoSkipCurrentIrpStackLocation(irp);
    return IoCallDriver(f->lower, irp);
}

#ifndef HWPCI_OWN_ARBITER
#define HWPCI_OWN_ARBITER 1
#endif
#ifndef HWPCI_OWN_TRANSLATOR
#define HWPCI_OWN_TRANSLATOR 1
#endif
static ULONG diag15;
static const GUID guid_arbiter_if = { 0xe644f185, 0x8c0e, 0x11d0, { 0xbe, 0xcf, 0x08, 0x00, 0x2b, 0xe2, 0x09, 0x2f } };

static void arb_ref(PVOID context) { (void)context; }

/* Interrupt arbiter for this bus. The bus already owns every INTx line in its _CRS (shared), and
 * Windows has no arbiter that will place a non-PCI-enumerated child's line, so grant each child the
 * exact line it asks for. Translation to the GIC still goes through ACPI and the HAL below us. */
static NTSTATUS NTAPI arb_handler(PVOID context, ARBITER_ACTION action, PARBITER_PARAMETERS params)
{
    PLIST_ENTRY head, e;
    (void)context;
    switch (action) {
    case ArbiterActionTestAllocation:
    case ArbiterActionRetestAllocation:
    case ArbiterActionBootAllocation:
        diag(15, diag15 |= 2);
        head = action == ArbiterActionBootAllocation ? params->Parameters.BootAllocation.ArbitrationList
                                                     : params->Parameters.TestAllocation.ArbitrationList;
        for (e = head->Flink; e != head; e = e->Flink) {
            PARBITER_LIST_ENTRY a = CONTAINING_RECORD(e, ARBITER_LIST_ENTRY, ListEntry);
            PIO_RESOURCE_DESCRIPTOR alt = a->Alternatives;
            ULONG k;
            for (k = 0; k < a->AlternativeCount && alt[k].Type != CmResourceTypeInterrupt; ++k) {}
            if (k == a->AlternativeCount) { a->Result = ArbiterResultNullRequest; continue; }
            alt += k;
            RtlZeroMemory(a->Assignment, sizeof *a->Assignment);
            a->Assignment->Type = CmResourceTypeInterrupt;
            a->Assignment->ShareDisposition = alt->ShareDisposition;
            a->Assignment->Flags = alt->Flags;
            a->Assignment->u.Interrupt.Level = alt->u.Interrupt.MinimumVector;
            a->Assignment->u.Interrupt.Vector = alt->u.Interrupt.MinimumVector;
            a->Assignment->u.Interrupt.Affinity = (KAFFINITY)-1;
            a->SelectedAlternative = alt;
            a->Result = ArbiterResultSuccess;
        }
        return STATUS_SUCCESS;
    case ArbiterActionQueryConflict:
        *params->Parameters.QueryConflict.ConflictCount = 0;
        *params->Parameters.QueryConflict.Conflicts = NULL;
        return STATUS_SUCCESS;
    case ArbiterActionQueryAllocatedResources:
        *params->Parameters.QueryAllocatedResources.AllocatedResources = NULL;
        return STATUS_SUCCESS;
    default:
        return STATUS_SUCCESS;
    }
}

static const GUID guid_translator_if = { 0x6c154a92, 0xaacf, 0x11d0, { 0x8d, 0x2a, 0x00, 0xa0, 0xc9, 0x06, 0xb2, 0x44 } };

/* Child interrupts are the bus's own _CRS lines, so this level translates nothing. Without it the
 * child's PCIBus line goes through ACPI's PCI translator and comes out as a bogus vector. */
static NTSTATUS NTAPI tr_resources(PVOID context, PCM_PARTIAL_RESOURCE_DESCRIPTOR source,
                                   RESOURCE_TRANSLATION_DIRECTION direction, ULONG count,
                                   IO_RESOURCE_DESCRIPTOR alternatives[], PDEVICE_OBJECT pdo,
                                   PCM_PARTIAL_RESOURCE_DESCRIPTOR target)
{
    (void)context; (void)direction; (void)count; (void)alternatives; (void)pdo;
    *target = *source;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI tr_requirements(PVOID context, PIO_RESOURCE_DESCRIPTOR source, PDEVICE_OBJECT pdo,
                                      PULONG count, PIO_RESOURCE_DESCRIPTOR *target)
{
    PIO_RESOURCE_DESCRIPTOR t = ExAllocatePool2(POOL_FLAG_PAGED, sizeof *t, TAG);
    (void)context; (void)pdo;
    if (!t) return STATUS_INSUFFICIENT_RESOURCES;
    *t = *source;
    *count = 1;
    *target = t;
    return STATUS_SUCCESS;
}

static NTSTATUS fdo_pnp(FDO_EXT *f, PIRP irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS status;
    ULONG i;
    switch (stack->MinorFunction) {
    case IRP_MN_START_DEVICE:
        status = forward_sync(f, irp);
        if (NT_SUCCESS(status)) status = fdo_start(f, stack);
        return complete(irp, status);
    case IRP_MN_QUERY_STOP_DEVICE:
        return complete(irp, STATUS_UNSUCCESSFUL);
    case IRP_MN_QUERY_INTERFACE:
        if (HWPCI_OWN_ARBITER && RtlCompareMemory(stack->Parameters.QueryInterface.InterfaceType, &guid_arbiter_if, sizeof(GUID)) == sizeof(GUID) &&
            (ULONG_PTR)stack->Parameters.QueryInterface.InterfaceSpecificData == CmResourceTypeInterrupt &&
            stack->Parameters.QueryInterface.Size >= sizeof(ARBITER_INTERFACE)) {
            PARBITER_INTERFACE a = (PARBITER_INTERFACE)stack->Parameters.QueryInterface.Interface;
            a->Size = sizeof *a;
            a->Version = 1;
            a->Context = f;
            a->InterfaceReference = arb_ref;
            a->InterfaceDereference = arb_ref;
            a->ArbiterHandler = arb_handler;
            a->Flags = 0;
            diag(15, diag15 |= 1);
            return complete(irp, STATUS_SUCCESS);
        }
        if (HWPCI_OWN_TRANSLATOR && RtlCompareMemory(stack->Parameters.QueryInterface.InterfaceType, &guid_translator_if, sizeof(GUID)) == sizeof(GUID) &&
            (ULONG_PTR)stack->Parameters.QueryInterface.InterfaceSpecificData == CmResourceTypeInterrupt &&
            stack->Parameters.QueryInterface.Size >= sizeof(TRANSLATOR_INTERFACE)) {
            PTRANSLATOR_INTERFACE t = (PTRANSLATOR_INTERFACE)stack->Parameters.QueryInterface.Interface;
            t->Size = sizeof *t;
            t->Version = 1;
            t->Context = f;
            t->InterfaceReference = arb_ref;
            t->InterfaceDereference = arb_ref;
            t->TranslateResources = tr_resources;
            t->TranslateResourceRequirements = tr_requirements;
            diag(15, diag15 |= 4);
            return complete(irp, STATUS_SUCCESS);
        }
        return pass_down(f, irp);
    case IRP_MN_QUERY_DEVICE_RELATIONS:
        if (stack->Parameters.QueryDeviceRelations.Type == BusRelations && f->cam) {
            PDEVICE_RELATIONS old = (PDEVICE_RELATIONS)irp->IoStatus.Information, r;
            ULONG prev = old ? old->Count : 0, n = prev + f->nchild;
            r = ExAllocatePool2(POOL_FLAG_PAGED, sizeof(DEVICE_RELATIONS) + (n ? n - 1 : 0) * sizeof(PDEVICE_OBJECT), TAG);
            if (!r) return complete(irp, STATUS_INSUFFICIENT_RESOURCES);
            for (i = 0; i < prev; ++i) r->Objects[i] = old->Objects[i];
            for (i = 0; i < f->nchild; ++i) {
                ObReferenceObject(f->child[i]);
                r->Objects[prev + i] = f->child[i];
            }
            r->Count = n;
            if (old) ExFreePool(old);
            irp->IoStatus.Information = (ULONG_PTR)r;
            irp->IoStatus.Status = STATUS_SUCCESS;
        }
        return pass_down(f, irp);
    case IRP_MN_REMOVE_DEVICE: {
        PDEVICE_OBJECT lower = f->lower;
        irp->IoStatus.Status = STATUS_SUCCESS;
        status = pass_down(f, irp);
        for (i = 0; i < f->nchild; ++i) IoDeleteDevice(f->child[i]);
        f->nchild = 0;
        if (f->cam) MmUnmapIoSpace(f->cam, f->cam_len);
        f->cam = NULL;
        IoDetachDevice(lower);
        IoDeleteDevice(f->self);
        return status;
    }
    case IRP_MN_SURPRISE_REMOVAL:
    case IRP_MN_QUERY_REMOVE_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_STOP_DEVICE:
        irp->IoStatus.Status = STATUS_SUCCESS;
        return pass_down(f, irp);
    default:
        return pass_down(f, irp);
    }
}

static PWCHAR put_str(PWCHAR w, const WCHAR *s)
{
    while (*s) *w++ = *s++;
    return w;
}

static PWCHAR put_hex(PWCHAR w, ULONG v, int digits)
{
    while (digits--) *w++ = L"0123456789ABCDEF"[(v >> (digits * 4)) & 15];
    return w;
}

static PWCHAR put_dec(PWCHAR w, ULONG v)
{
    WCHAR t[10];
    int n = 0;
    do t[n++] = (WCHAR)(L'0' + v % 10); while (v /= 10);
    while (n) *w++ = t[--n];
    return w;
}

static PWCHAR put_vd(PWCHAR w, PDO_EXT *p)
{
    w = put_str(w, L"PCI\\VEN_");
    w = put_hex(w, p->ven, 4);
    w = put_str(w, L"&DEV_");
    return put_hex(w, p->did, 4);
}

static PWCHAR put_subsys(PWCHAR w, PDO_EXT *p)
{
    w = put_str(w, L"&SUBSYS_");
    w = put_hex(w, p->sdid, 4);
    return put_hex(w, p->sven, 4);
}

static PWCHAR put_rev(PWCHAR w, PDO_EXT *p)
{
    w = put_str(w, L"&REV_");
    return put_hex(w, p->rev, 2);
}

static PWCHAR put_cc(PWCHAR w, PDO_EXT *p, const WCHAR *prefix, BOOLEAN progif)
{
    w = put_str(w, prefix);
    w = put_hex(w, p->cls, 2);
    w = put_hex(w, p->sub, 2);
    return progif ? put_hex(w, p->pi, 2) : w;
}

static PWCHAR put_ven(PWCHAR w, PDO_EXT *p)
{
    w = put_str(w, L"PCI\\VEN_");
    return put_hex(w, p->ven, 4);
}

/* Returns the number of WCHARs written including terminators, 0 if the query is not answered. */
static ULONG build_id(PDO_EXT *p, BUS_QUERY_ID_TYPE type, PWCHAR b)
{
    PWCHAR w = b;
    int i;
    switch (type) {
    case BusQueryDeviceID:
        w = put_rev(put_subsys(put_vd(w, p), p), p);
        break;
    case BusQueryHardwareIDs:
        w = put_rev(put_subsys(put_vd(w, p), p), p);
        *w++ = 0;
        w = put_subsys(put_vd(w, p), p);
        *w++ = 0;
        w = put_cc(put_vd(w, p), p, L"&CC_", TRUE);
        *w++ = 0;
        w = put_cc(put_vd(w, p), p, L"&CC_", FALSE);
        *w++ = 0;
        break;
    case BusQueryCompatibleIDs:
        w = put_rev(put_vd(w, p), p);
        *w++ = 0;
        w = put_vd(w, p);
        *w++ = 0;
        for (i = 1; i >= 0; --i) {
            w = put_cc(put_ven(w, p), p, L"&CC_", (BOOLEAN)i);
            *w++ = 0;
        }
        w = put_ven(w, p);
        *w++ = 0;
        for (i = 1; i >= 0; --i) {
            w = put_cc(w, p, L"PCI\\CC_", (BOOLEAN)i);
            *w++ = 0;
        }
        break;
    case BusQueryInstanceID:
        w = put_str(w, L"3&0&0&");
        w = put_hex(w, p->bus, 2);
        w = put_hex(w, (ULONG)p->dev << 3 | p->fn, 2);
        break;
    default:
        return 0;
    }
    *w++ = 0;
    return (ULONG)(w - b);
}

static NTSTATUS return_wstr(PIRP irp, const WCHAR *s, ULONG chars)
{
    PWCHAR out = ExAllocatePool2(POOL_FLAG_PAGED, chars * sizeof(WCHAR), TAG);
    if (!out) return STATUS_INSUFFICIENT_RESOURCES;
    RtlCopyMemory(out, s, chars * sizeof(WCHAR));
    irp->IoStatus.Information = (ULONG_PTR)out;
    return STATUS_SUCCESS;
}

static void if_ref(PVOID context) { (void)context; }

static BOOLEAN NTAPI if_translate(PVOID context, PHYSICAL_ADDRESS bus_address, ULONG length,
                                  PULONG address_space, PPHYSICAL_ADDRESS translated)
{
    (void)context; (void)length;
    if (*address_space != 0) return FALSE;
    *translated = bus_address;
    return TRUE;
}

/* Diagnostic: log every call into the disk's DMA adapter (slot, low byte of return). Slots with
 * more than eight arguments are left alone because this forwarder only passes registers. */
typedef ULONG_PTR (*FWD8)(ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR);
static PVOID dma_orig[48];
#define DMA_WRAP(n) static ULONG_PTR dma_w##n(ULONG_PTR a, ULONG_PTR b, ULONG_PTR c, ULONG_PTR d, ULONG_PTR e, \
    ULONG_PTR f, ULONG_PTR g, ULONG_PTR h) { ULONG_PTR r = ((FWD8)dma_orig[n])(a, b, c, d, e, f, g, h); \
    pdo_trace(4, 0x3d, n, (NTSTATUS)r, (ULONG)(r >> 24)); return r; }
DMA_WRAP(0) DMA_WRAP(1) DMA_WRAP(2) DMA_WRAP(3) DMA_WRAP(4) DMA_WRAP(5) DMA_WRAP(6) DMA_WRAP(7) DMA_WRAP(8)
DMA_WRAP(9) DMA_WRAP(10) DMA_WRAP(11) DMA_WRAP(12) DMA_WRAP(14) DMA_WRAP(15) DMA_WRAP(16) DMA_WRAP(17)
DMA_WRAP(19) DMA_WRAP(20) DMA_WRAP(21) DMA_WRAP(25) DMA_WRAP(26) DMA_WRAP(27) DMA_WRAP(28)
DMA_WRAP(29) DMA_WRAP(30) DMA_WRAP(31) DMA_WRAP(32) DMA_WRAP(33)
/* AllocateCommonBufferEx(adapter, PPHYSICAL_ADDRESS max, ULONG length, PPHYSICAL_ADDRESS logical, BOOLEAN cache, node) */
static ULONG_PTR dma_w18(ULONG_PTR a, ULONG_PTR b, ULONG_PTR c, ULONG_PTR d, ULONG_PTR e, ULONG_PTR f, ULONG_PTR g, ULONG_PTR h)
{
    ULONG_PTR r = ((FWD8)dma_orig[18])(a, b, c, d, e, f, g, h);
    ULONGLONG max = b ? (ULONGLONG)((PPHYSICAL_ADDRESS)b)->QuadPart : 0;
    pdo_trace(4, 0x3c, 18, (NTSTATUS)(r != 0), (ULONG)e);
    pdo_trace(4, 0x3c, 0xa0, (NTSTATUS)(max >> 32), (ULONG)(max >> 24));
    pdo_trace(4, 0x3c, 0xa1, (NTSTATUS)(c >> 16), (ULONG)(c >> 8));
    pdo_trace(4, 0x3c, 0xa2, (NTSTATUS)(ULONG)f, (ULONG)(b != 0));
    return r;
}

static const PVOID dma_wrappers[48] = {
    dma_w0, dma_w1, dma_w2, dma_w3, dma_w4, dma_w5, dma_w6, dma_w7, dma_w8, dma_w9, dma_w10, dma_w11, dma_w12,
    NULL, dma_w14, dma_w15, dma_w16, dma_w17, dma_w18, dma_w19, dma_w20, dma_w21, NULL, NULL, NULL, dma_w25,
    dma_w26, dma_w27, dma_w28, dma_w29, dma_w30, dma_w31, dma_w32, dma_w33,
};

static void dma_instrument(struct _DMA_ADAPTER *a)
{
    ULONG size = a->DmaOperations->Size, n = (size - sizeof(ULONG_PTR)) / sizeof(PVOID), i;
    PVOID *ops = ExAllocatePool2(POOL_FLAG_NON_PAGED, size, TAG);
    const PVOID *src = (const PVOID *)((const UCHAR *)a->DmaOperations + sizeof(ULONG_PTR));
    if (!ops) return;
    RtlCopyMemory(ops, a->DmaOperations, size);
    for (i = 0; i < n && i < 48; ++i) {
        dma_orig[i] = src[i];
        if (dma_wrappers[i] && src[i]) ((PVOID *)((UCHAR *)ops + sizeof(ULONG_PTR)))[i] = dma_wrappers[i];
    }
    pdo_trace(4, 0x3d, 0xff, (NTSTATUS)size, n);
    a->DmaOperations = (PDMA_OPERATIONS)ops;
}

static struct _DMA_ADAPTER *NTAPI if_dma(PVOID context, struct _DEVICE_DESCRIPTION *description, PULONG map_registers)
{
    PDO_EXT *p = context;
    struct _DMA_ADAPTER *a;
    /* The HAL has no PCI DMA domain for this bus; describe the device as a platform device. */
    description->InterfaceType = Internal;
    description->BusNumber = 0;
    a = IoGetDmaAdapter(p->fdo->pdo, description, map_registers);
    pdo_trace(p->dev, 0x3f, (UCHAR)description->Version, a ? 1 : 0, (ULONG)description->InterfaceType);
    pdo_trace(p->dev, 0x3c, 0xb0, description->Master | description->ScatterGather << 1 | description->Dma32BitAddresses << 2 |
              description->Dma64BitAddresses << 3 | description->IgnoreCount << 4, (ULONG)description->DmaWidth);
    pdo_trace(p->dev, 0x3c, 0xb1, (NTSTATUS)(description->MaximumLength >> 16), description->Version >= 3 ? *(PULONG)((PUCHAR)description + sizeof(DEVICE_DESCRIPTION)) : 0xff);
    if (a && p->dev == 4) dma_instrument(a);
    return a;
}

static ULONG NTAPI if_get(PVOID context, ULONG space, PVOID buffer, ULONG offset, ULONG length)
{
    PDO_EXT *p = context;
    pdo_trace(p->dev, 0x3e, (UCHAR)space, (NTSTATUS)offset, length);
    return space != PCI_WHICHSPACE_ROM ? cfg_read(p->fdo, p->fnoff, buffer, offset, length) : 0;
}

static ULONG NTAPI if_set(PVOID context, ULONG space, PVOID buffer, ULONG offset, ULONG length)
{
    PDO_EXT *p = context;
    return space != PCI_WHICHSPACE_ROM ? cfg_write(p->fdo, p->fnoff, buffer, offset, length) : 0;
}

static ULONG slot_offset(FDO_EXT *f, ULONG bus, ULONG slot)
{
    PCI_SLOT_NUMBER s;
    s.u.AsULONG = slot;
    return bus < 256 ? fn_offset(bus, s.u.bits.DeviceNumber, s.u.bits.FunctionNumber) : f->cam_len;
}

static ULONG NTAPI pci_read(PVOID context, ULONG bus, ULONG slot, PVOID buffer, ULONG offset, ULONG length)
{
    FDO_EXT *f = context;
    return cfg_read(f, slot_offset(f, bus, slot), buffer, offset, length);
}

static ULONG NTAPI pci_write(PVOID context, ULONG bus, ULONG slot, PVOID buffer, ULONG offset, ULONG length)
{
    FDO_EXT *f = context;
    return cfg_write(f, slot_offset(f, bus, slot), buffer, offset, length);
}

static void NTAPI pci_pin_to_line(PVOID context, PPCI_COMMON_CONFIG data) { (void)context; (void)data; }

static void NTAPI pci_line_to_pin(PVOID context, PPCI_COMMON_CONFIG a, PPCI_COMMON_CONFIG b)
{
    (void)context; (void)a; (void)b;
}

static const GUID guid_present_if = { 0xd1b82c26, 0xbf49, 0x45ef, { 0xb2, 0x16, 0x71, 0xcb, 0xd7, 0x88, 0x9b, 0x57 } };

static BOOLEAN NTAPI present(USHORT ven, USHORT dev, UCHAR rev, USHORT sven, USHORT sdid, ULONG flags)
{
    (void)rev; (void)sven; (void)sdid; (void)flags;
    return ven == 0x1af4 || (ven == 0x8086 && dev == 0x1237);
}

static BOOLEAN NTAPI present_ex(PVOID context, PPCI_DEVICE_PRESENCE_PARAMETERS q)
{
    PDO_EXT *p = context;
    if (q->Flags & PCI_USE_VENDEV_IDS) {
        if (q->VendorID != p->ven || q->DeviceID != p->did) return FALSE;
    }
    if ((q->Flags & PCI_USE_SUBSYSTEM_IDS) && (q->SubVendorID != p->sven || q->SubSystemID != p->sdid)) return FALSE;
    if ((q->Flags & PCI_USE_REVISION) && q->RevisionID != p->rev) return FALSE;
    if ((q->Flags & PCI_USE_CLASS_SUBCLASS) && (q->BaseClass != p->cls || q->SubClass != p->sub)) return FALSE;
    if ((q->Flags & PCI_USE_PROGIF) && q->ProgIf != p->pi) return FALSE;
    return TRUE;
}

static NTSTATUS query_interface(PDO_EXT *p, PIO_STACK_LOCATION stack, NTSTATUS status)
{
    const GUID *g = stack->Parameters.QueryInterface.InterfaceType;
    USHORT size = stack->Parameters.QueryInterface.Size;
    if (RtlCompareMemory(g, &guid_bus_if, sizeof(GUID)) == sizeof(GUID)) {
        PBUS_INTERFACE_STANDARD i = (PBUS_INTERFACE_STANDARD)stack->Parameters.QueryInterface.Interface;
        if (size < sizeof *i) return STATUS_INVALID_PARAMETER;
        i->Size = sizeof *i;
        i->Version = 1;
        i->Context = p;
        i->InterfaceReference = if_ref;
        i->InterfaceDereference = if_ref;
        i->TranslateBusAddress = if_translate;
        i->GetDmaAdapter = if_dma;
        i->SetBusData = if_set;
        i->GetBusData = if_get;
        return STATUS_SUCCESS;
    }
    if (RtlCompareMemory(g, &guid_pci_bus_if, sizeof(GUID)) == sizeof(GUID)) {
        PPCI_BUS_INTERFACE_STANDARD i = (PPCI_BUS_INTERFACE_STANDARD)stack->Parameters.QueryInterface.Interface;
        if (size < sizeof *i) return STATUS_INVALID_PARAMETER;
        RtlZeroMemory(i, sizeof *i);
        i->Size = sizeof *i;
        i->Version = PCI_BUS_INTERFACE_STANDARD_VERSION;
        i->Context = p->fdo;
        i->InterfaceReference = if_ref;
        i->InterfaceDereference = if_ref;
        i->ReadConfig = pci_read;
        i->WriteConfig = pci_write;
        i->PinToLine = pci_pin_to_line;
        i->LineToPin = pci_line_to_pin;
        return STATUS_SUCCESS;
    }
    if (RtlCompareMemory(g, &guid_present_if, sizeof(GUID)) == sizeof(GUID)) {
        PPCI_DEVICE_PRESENT_INTERFACE i = (PPCI_DEVICE_PRESENT_INTERFACE)stack->Parameters.QueryInterface.Interface;
        if (size < FIELD_OFFSET(PCI_DEVICE_PRESENT_INTERFACE, IsDevicePresentEx)) return STATUS_INVALID_PARAMETER;
        i->Size = size < sizeof *i ? (USHORT)FIELD_OFFSET(PCI_DEVICE_PRESENT_INTERFACE, IsDevicePresentEx) : (USHORT)sizeof *i;
        i->Version = PCI_DEVICE_PRESENT_INTERFACE_VERSION;
        i->Context = p;
        i->InterfaceReference = if_ref;
        i->InterfaceDereference = if_ref;
        i->IsDevicePresent = present;
        if (size >= sizeof *i) i->IsDevicePresentEx = present_ex;
        return STATUS_SUCCESS;
    }
    return status;
}

static NTSTATUS query_requirements(PDO_EXT *p, PIRP irp)
{
    BOOLEAN with_irq = p->has_irq;
    ULONG n = p->nbar + (with_irq ? 1 : 0), i;
    ULONG size = sizeof(IO_RESOURCE_REQUIREMENTS_LIST) + (n ? n - 1 : 0) * sizeof(IO_RESOURCE_DESCRIPTOR);
    PIO_RESOURCE_REQUIREMENTS_LIST r;
    PIO_RESOURCE_DESCRIPTOR d;
    if (!n) return irp->IoStatus.Status;
    r = ExAllocatePool2(POOL_FLAG_PAGED, size, TAG);
    if (!r) return STATUS_INSUFFICIENT_RESOURCES;
    r->ListSize = size;
    r->InterfaceType = PCIBus;
    r->BusNumber = p->bus;
    r->SlotNumber = (ULONG)p->dev | (ULONG)p->fn << 5;
    r->AlternativeLists = 1;
    r->List[0].Version = 1;
    r->List[0].Revision = 1;
    r->List[0].Count = n;
    d = r->List[0].Descriptors;
    for (i = 0; i < p->nbar; ++i, ++d) {
        d->Type = CmResourceTypeMemory;
        d->ShareDisposition = CmResourceShareDeviceExclusive;
        d->Flags = p->bar[i].flags;
        d->u.Memory.Length = p->bar[i].len;
        d->u.Memory.Alignment = 1;
        d->u.Memory.MinimumAddress.QuadPart = (LONGLONG)p->bar[i].base;
        d->u.Memory.MaximumAddress.QuadPart = (LONGLONG)(p->bar[i].base + p->bar[i].len - 1);
    }
    if (with_irq) {
        d->Type = CmResourceTypeInterrupt;
        d->ShareDisposition = CmResourceShareShared;
        d->Flags = p->irq.Flags;
        d->u.Interrupt.MinimumVector = p->irq.u.Interrupt.Vector;
        d->u.Interrupt.MaximumVector = p->irq.u.Interrupt.Vector;
    }
    irp->IoStatus.Information = (ULONG_PTR)r;
    return STATUS_SUCCESS;
}

static NTSTATUS query_resources(PDO_EXT *p, PIRP irp)
{
    ULONG n = p->nbar + (p->has_irq ? 1 : 0), i;
    ULONG size = sizeof(CM_RESOURCE_LIST) + (n ? n - 1 : 0) * sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR);
    PCM_RESOURCE_LIST r;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR d;
    if (!p->decode || !p->nbar) return irp->IoStatus.Status;
    r = ExAllocatePool2(POOL_FLAG_PAGED, size, TAG);
    if (!r) return STATUS_INSUFFICIENT_RESOURCES;
    r->Count = 1;
    r->List[0].InterfaceType = PCIBus;
    r->List[0].BusNumber = p->bus;
    r->List[0].PartialResourceList.Version = 1;
    r->List[0].PartialResourceList.Revision = 1;
    r->List[0].PartialResourceList.Count = n;
    d = r->List[0].PartialResourceList.PartialDescriptors;
    for (i = 0; i < p->nbar; ++i, ++d) {
        d->Type = CmResourceTypeMemory;
        d->ShareDisposition = CmResourceShareDeviceExclusive;
        d->Flags = p->bar[i].flags;
        d->u.Memory.Start.QuadPart = (LONGLONG)p->bar[i].base;
        d->u.Memory.Length = p->bar[i].len;
    }
    if (p->has_irq) *d = p->irq;
    irp->IoStatus.Information = (ULONG_PTR)r;
    return STATUS_SUCCESS;
}

static NTSTATUS query_capabilities(PDO_EXT *p, PIO_STACK_LOCATION stack)
{
    PDEVICE_CAPABILITIES c = stack->Parameters.DeviceCapabilities.Capabilities;
    int i;
    if (c->Version != 1 || c->Size < sizeof *c) return STATUS_UNSUCCESSFUL;
    c->DeviceState[PowerSystemWorking] = PowerDeviceD0;
    for (i = PowerSystemSleeping1; i < PowerSystemMaximum; ++i) c->DeviceState[i] = PowerDeviceD3;
    c->SystemWake = PowerSystemUnspecified;
    c->DeviceWake = PowerDeviceUnspecified;
    c->DeviceD1 = FALSE;
    c->DeviceD2 = FALSE;
    c->LockSupported = FALSE;
    c->EjectSupported = FALSE;
    c->Removable = FALSE;
    c->DockDevice = FALSE;
    c->UniqueID = TRUE;
    c->SilentInstall = FALSE;
    c->RawDeviceOK = FALSE;
    c->SurpriseRemovalOK = FALSE;
    c->Address = (ULONG)p->dev << 16 | p->fn;
    c->UINumber = p->dev;
    return STATUS_SUCCESS;
}

/* IoConnectInterruptEx(CONNECT_LINE_BASED), which storport uses, reads this property. Windows' interrupt
 * arbiter writes it; children of this bus are arbitrated here instead, so copy the matching entries from
 * the bus device's own property. Layout: ULONG count, ULONG pad, then 0x58-byte vector records with the
 * translated vector at offset 4. */
static const DEVPROPKEY interrupt_connection_key =
    { { 0xf0e20f09, 0xd97a, 0x49a9, { 0x80, 0x46, 0xbb, 0x6e, 0x22, 0xe6, 0xbb, 0x2e } }, 2 };
#define VECTOR_RECORD 0x58

static void set_connection_data(PDO_EXT *p, PCM_RESOURCE_LIST translated)
{
    UCHAR parent[8 + VECTOR_RECORD * MAX_IRQS], child[8 + VECTOR_RECORD * 4];
    ULONG size = 0, count, n = 0, i, k;
    DEVPROPTYPE type = 0;
    NTSTATUS s = IoGetDevicePropertyData(p->fdo->pdo, &interrupt_connection_key, 0, 0, sizeof parent, parent, &size, &type);
    diag(116, (ULONG)s);
    diag(117, size | type << 16);
    if (!NT_SUCCESS(s) || size < 8 || !translated || !translated->Count) return;
    count = *(PULONG)parent;
    for (k = 0; k < translated->List[0].PartialResourceList.Count && n < 4; ++k) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR d = &translated->List[0].PartialResourceList.PartialDescriptors[k];
        if (d->Type != CmResourceTypeInterrupt) continue;
        for (i = 0; i < count && 8 + (i + 1) * VECTOR_RECORD <= size; ++i) {
            PUCHAR rec = parent + 8 + i * VECTOR_RECORD;
            if (*(PULONG)(rec + 4) != d->u.Interrupt.Vector) continue;
            RtlCopyMemory(child + 8 + n * VECTOR_RECORD, rec, VECTOR_RECORD);
            ++n;
            break;
        }
    }
    *(PULONG)child = n;
    *(PULONG)(child + 4) = 0;
    s = IoSetDevicePropertyData(p->self, &interrupt_connection_key, 0, 0, type, 8 + n * VECTOR_RECORD, child);
    diag(118 + (p->dev & 7), (ULONG)s | n << 28);
}

static NTSTATUS pdo_pnp(PDO_EXT *p, PIRP irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS status = irp->IoStatus.Status;
    WCHAR buf[512];
    ULONG n;
    switch (stack->MinorFunction) {
    case IRP_MN_START_DEVICE:
        set_command(p, CMD_MEMORY | CMD_MASTER, CMD_INTX_OFF);
        status = STATUS_SUCCESS;
        {
            static ULONG started;
            PCM_RESOURCE_LIST t = stack->Parameters.StartDevice.AllocatedResourcesTranslated;
            started |= 1u << p->dev;
            diag(12, started);
            if (p->has_irq) set_connection_data(p, t);
            if (p->dev == 5) {
                IO_CONNECT_INTERRUPT_PARAMETERS cp;
                PKINTERRUPT obj = NULL;
                NTSTATUS cs;
                RtlZeroMemory(&cp, sizeof cp);
                cp.Version = CONNECT_LINE_BASED;
                cp.LineBased.PhysicalDeviceObject = p->self;
                cp.LineBased.InterruptObject = &obj;
                cp.LineBased.ServiceRoutine = probe_isr;
                cp.LineBased.ServiceContext = p;
                cs = IoConnectInterruptEx(&cp);
                diag(115, (ULONG)cs);
                if (NT_SUCCESS(cs)) {
                    IO_DISCONNECT_INTERRUPT_PARAMETERS dp;
                    RtlZeroMemory(&dp, sizeof dp);
                    dp.Version = CONNECT_LINE_BASED;
                    dp.ConnectionContext.InterruptObject = obj;
                    IoDisconnectInterruptEx(&dp);
                }
            }
            if (p->dev == 4 && t && t->Count) {
                ULONG k, m = t->List[0].PartialResourceList.Count;
                PCM_RESOURCE_LIST rw = stack->Parameters.StartDevice.AllocatedResources;
                for (k = 0; k < m && k < 2; ++k) {
                    PCM_PARTIAL_RESOURCE_DESCRIPTOR d = &t->List[0].PartialResourceList.PartialDescriptors[k];
                    PCM_PARTIAL_RESOURCE_DESCRIPTOR r = &rw->List[0].PartialResourceList.PartialDescriptors[k];
                    diag(96 + 4 * k, (ULONG)d->Type << 24 | (ULONG)d->ShareDisposition << 16 | d->Flags);
                    diag(97 + 4 * k, d->Type == CmResourceTypeMemory ? d->u.Memory.Start.LowPart : d->u.Interrupt.Vector);
                    diag(98 + 4 * k, d->Type == CmResourceTypeMemory ? d->u.Memory.Length : d->u.Interrupt.Level);
                    diag(99 + 4 * k, r->Type == CmResourceTypeMemory ? r->u.Memory.Start.LowPart : r->u.Interrupt.Vector);
                }
            }
            diag(13 + (p->dev & 1), (ULONG)p->dev << 24 | (t && t->Count ? t->List[0].PartialResourceList.Count : 0xff));
        }
        break;
    case IRP_MN_STOP_DEVICE:
    case IRP_MN_REMOVE_DEVICE:
        set_command(p, 0, CMD_MEMORY | CMD_MASTER);
        status = STATUS_SUCCESS;
        break;
    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_QUERY_REMOVE_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:
        status = STATUS_SUCCESS;
        break;
    case IRP_MN_QUERY_CAPABILITIES:
        status = query_capabilities(p, stack);
        break;
    case IRP_MN_QUERY_ID:
        n = build_id(p, stack->Parameters.QueryId.IdType, buf);
        if (n) status = return_wstr(irp, buf, n);
        break;
    case IRP_MN_QUERY_DEVICE_TEXT:
        if (stack->Parameters.QueryDeviceText.DeviceTextType == DeviceTextDescription) {
            PWCHAR w = put_str(buf, L"PCI Device");
            *w++ = 0;
            status = return_wstr(irp, buf, (ULONG)(w - buf));
        } else if (stack->Parameters.QueryDeviceText.DeviceTextType == DeviceTextLocationInformation) {
            PWCHAR w = put_dec(put_str(buf, L"PCI bus "), p->bus);
            w = put_dec(put_str(w, L", device "), p->dev);
            w = put_dec(put_str(w, L", function "), p->fn);
            *w++ = 0;
            status = return_wstr(irp, buf, (ULONG)(w - buf));
        }
        break;
    case IRP_MN_QUERY_BUS_INFORMATION: {
        PPNP_BUS_INFORMATION b = ExAllocatePool2(POOL_FLAG_PAGED, sizeof *b, TAG);
        if (!b) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        b->BusTypeGuid = guid_bus_type_pci;
        b->LegacyBusType = PCIBus;
        b->BusNumber = p->bus;
        irp->IoStatus.Information = (ULONG_PTR)b;
        status = STATUS_SUCCESS;
        break;
    }
    case IRP_MN_QUERY_INTERFACE:
        status = query_interface(p, stack, status);
        break;
    case IRP_MN_QUERY_RESOURCE_REQUIREMENTS:
        status = query_requirements(p, irp);
        break;
    case IRP_MN_READ_CONFIG:
    case IRP_MN_WRITE_CONFIG: {
        ULONG done;
        if (stack->Parameters.ReadWriteConfig.WhichSpace == PCI_WHICHSPACE_ROM) {
            status = STATUS_NOT_SUPPORTED;
            break;
        }
        done = stack->MinorFunction == IRP_MN_READ_CONFIG
            ? cfg_read(p->fdo, p->fnoff, stack->Parameters.ReadWriteConfig.Buffer,
                       stack->Parameters.ReadWriteConfig.Offset, stack->Parameters.ReadWriteConfig.Length)
            : cfg_write(p->fdo, p->fnoff, stack->Parameters.ReadWriteConfig.Buffer,
                        stack->Parameters.ReadWriteConfig.Offset, stack->Parameters.ReadWriteConfig.Length);
        irp->IoStatus.Information = done;
        status = STATUS_SUCCESS;
        break;
    }
    case IRP_MN_QUERY_RESOURCES:
        status = query_resources(p, irp);
        break;
    case IRP_MN_QUERY_DEVICE_RELATIONS:
        if (stack->Parameters.QueryDeviceRelations.Type == TargetDeviceRelation) {
            PDEVICE_RELATIONS r = ExAllocatePool2(POOL_FLAG_PAGED, sizeof *r, TAG);
            if (!r) {
                status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }
            ObReferenceObject(p->self);
            r->Count = 1;
            r->Objects[0] = p->self;
            irp->IoStatus.Information = (ULONG_PTR)r;
            status = STATUS_SUCCESS;
        }
        break;
    case IRP_MN_DEVICE_USAGE_NOTIFICATION:
        if (stack->Parameters.UsageNotification.Type == DeviceUsageTypePaging) {
            p->paging += stack->Parameters.UsageNotification.InPath ? 1 : -1;
            if (p->paging > 0) p->self->Flags &= ~DO_POWER_PAGABLE;
            else p->self->Flags |= DO_POWER_PAGABLE;
        }
        status = STATUS_SUCCESS;
        break;
    default:
        break;
    }
    if (stack->MinorFunction == IRP_MN_QUERY_INTERFACE) {
        const UCHAR *gb = (const UCHAR *)stack->Parameters.QueryInterface.InterfaceType;
        pdo_trace(p->dev, IRP_MJ_PNP, 0x80 | (status == STATUS_SUCCESS), gb[1], gb[0]);
        pdo_trace(p->dev, IRP_MJ_PNP, 0x90, gb[3], gb[2]);
        pdo_trace(p->dev, IRP_MJ_PNP, 0xa0, stack->Parameters.QueryInterface.Size >> 8, stack->Parameters.QueryInterface.Size);
    } else {
        pdo_trace(p->dev, IRP_MJ_PNP, stack->MinorFunction, status, 0);
    }
    return complete(irp, status);
}

static NTSTATUS dispatch_pnp(PDEVICE_OBJECT device, PIRP irp)
{
    COMMON *c = device->DeviceExtension;
    return c->fdo ? fdo_pnp((FDO_EXT *)c, irp) : pdo_pnp((PDO_EXT *)c, irp);
}

static NTSTATUS dispatch_power(PDEVICE_OBJECT device, PIRP irp)
{
    COMMON *c = device->DeviceExtension;
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS status = irp->IoStatus.Status;
    if (c->fdo) {
        IoSkipCurrentIrpStackLocation(irp);
        return PoCallDriver(((FDO_EXT *)c)->lower, irp);
    }
    switch (stack->MinorFunction) {
    case IRP_MN_SET_POWER:
        if (stack->Parameters.Power.Type == DevicePowerState)
            PoSetPowerState(device, DevicePowerState, stack->Parameters.Power.State);
        status = STATUS_SUCCESS;
        break;
    case IRP_MN_QUERY_POWER:
        status = STATUS_SUCCESS;
        break;
    case IRP_MN_WAIT_WAKE:
        status = STATUS_NOT_SUPPORTED;
        break;
    default:
        break;
    }
    pdo_trace(((PDO_EXT *)c)->dev, IRP_MJ_POWER, stack->MinorFunction, status, 0);
    return complete(irp, status);
}

static NTSTATUS dispatch_other(PDEVICE_OBJECT device, PIRP irp)
{
    COMMON *c = device->DeviceExtension;
    if (c->fdo) return pass_down((FDO_EXT *)c, irp);
    pdo_trace(((PDO_EXT *)c)->dev, IoGetCurrentIrpStackLocation(irp)->MajorFunction, IoGetCurrentIrpStackLocation(irp)->MinorFunction, irp->IoStatus.Status, 0);
    return complete(irp, irp->IoStatus.Status);
}

static NTSTATUS add_device(PDRIVER_OBJECT driver, PDEVICE_OBJECT pdo)
{
    PDEVICE_OBJECT d;
    FDO_EXT *f;
    NTSTATUS status = IoCreateDevice(driver, sizeof *f, NULL, FILE_DEVICE_BUS_EXTENDER, FILE_DEVICE_SECURE_OPEN,
                                     FALSE, &d);
    if (!NT_SUCCESS(status)) return status;
    f = d->DeviceExtension;
    RtlZeroMemory(f, sizeof *f);
    f->c.fdo = TRUE;
    f->self = d;
    f->pdo = pdo;
    KeInitializeSpinLock(&f->lock);
    diag(2, 2);
    f->lower = IoAttachDeviceToDeviceStack(d, pdo);
    if (!f->lower) {
        IoDeleteDevice(d);
        return STATUS_NO_SUCH_DEVICE;
    }
    d->Flags |= f->lower->Flags & DO_POWER_PAGABLE;
    d->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry)
{
    (void)registry;
    diag(2, 1);
    {
        PHYSICAL_ADDRESS pa;
        pa.QuadPart = (LONGLONG)(HW_WINDOW_PA + HW_DIAG);
        trace_ring = (volatile ULONG *)MmMapIoSpace(pa, PAGE_SIZE, MmCached);
    }
    driver->MajorFunction[IRP_MJ_PNP] = dispatch_pnp;
    driver->MajorFunction[IRP_MJ_POWER] = dispatch_power;
    driver->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = dispatch_other;
    driver->DriverExtension->AddDevice = add_device;
    return STATUS_SUCCESS;
}
