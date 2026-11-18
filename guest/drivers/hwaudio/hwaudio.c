/* HyperWin speakers and microphone: a PortCls WaveRT driver over the shared window (audio/README.md).
 *
 * Windows' audio engine gets its stream buffers straight out of HW_AUDIO, so it mixes into memory
 * the phone reads, and reads the microphone from memory the phone writes. No copy happens on this
 * side. The phone's AAudio callbacks move the cursors in the control page (include/hw_audio.h), and
 * those cursors are the stream position this driver reports, so Windows runs at the speed of the
 * phone's audio clock. A 1 ms high-resolution timer per running stream watches the cursor and
 * signals the audio engine's event each time it crosses into the next period (event mode).
 *
 * Four PortCls subdevices on one root-enumerated device (ROOT\HwAudio, hwaudio.inf):
 *   WaveRender      pin 0 audio in from Windows, pin 1 bridge out to TopologyRender
 *   TopologyRender  pin 0 bridge in, pin 1 out to the speaker
 *   WaveCapture     pin 0 bridge in from TopologyCapture, pin 1 audio out to Windows
 *   TopologyCapture pin 0 in from the microphone, pin 1 bridge out
 * Formats: 48 kHz stereo, 16-bit PCM or 32-bit float. Windows mixes in float and converts.
 *
 * Plain C with hand-filled COM vtables, so the clang mingw toolchain needs no C++ runtime. */
#define INITGUID
#include <ntddk.h>
typedef WCHAR TCHAR;
typedef struct { ULONG FifoSize, ChipsetDelay, CodecDelay; } KSRTAUDIO_HWLATENCY, *PKSRTAUDIO_HWLATENCY;
typedef struct {
    PVOID Register;
    ULONG Width;
    ULONGLONG Numerator, Denominator;
    ULONG Accuracy;
} KSRTAUDIO_HWREGISTER, *PKSRTAUDIO_HWREGISTER;
#include <portcls.h>
#include <ksmedia.h>
#include "hw_abi.h"
#include "hw_audio.h"

#define TAG 'oaWH'
#define TICK_100NS 10000      /* the cursor watch timer: 1 ms */

static volatile struct hwa_ctl *g_ctl;
static UCHAR *g_buf[2];       /* render, capture: kernel views of the shared buffers */

static volatile struct hwa_stream *dir_stream(int capture) {
    return capture ? &g_ctl->capture : &g_ctl->render;
}

/* ---- Pins, ranges and filters ---- */

/* ks.h only declares these in C; the pins point at them. */
static const GUID cat_audio = {STATICGUIDOF(KSCATEGORY_AUDIO)};
static const GUID node_speaker = {STATICGUIDOF(KSNODETYPE_SPEAKER)};
static const GUID node_microphone = {STATICGUIDOF(KSNODETYPE_MICROPHONE)};

static KSDATARANGE_AUDIO range_s16 = {
    {sizeof(KSDATARANGE_AUDIO), 0, 0, 0, {STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO)},
     {STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM)}, {STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)}},
    HWA_CHANNELS, 16, 16, HWA_RATE, HWA_RATE};
static KSDATARANGE_AUDIO range_f32 = {
    {sizeof(KSDATARANGE_AUDIO), 0, 0, 0, {STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO)},
     {STATICGUIDOF(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)}, {STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)}},
    HWA_CHANNELS, 32, 32, HWA_RATE, HWA_RATE};
static KSDATARANGE range_bridge = {
    sizeof(KSDATARANGE), 0, 0, 0, {STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO)},
    {STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG)}, {STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)}};
static PKSDATARANGE ranges_stream[] = {(PKSDATARANGE)&range_f32, (PKSDATARANGE)&range_s16};
static PKSDATARANGE ranges_bridge[] = {&range_bridge};

#define PIN(flow, comm, ranges, count, cat)                                                         \
    {count, count, 0, NULL,                                                                         \
     {0, NULL, 0, NULL, sizeof(ranges) / sizeof(ranges[0]), ranges, flow, comm, cat, NULL, {0}}}
#define STREAM_PIN(flow) PIN(flow, KSPIN_COMMUNICATION_SINK, ranges_stream, 1, &cat_audio)
#define BRIDGE_PIN(flow, cat) PIN(flow, KSPIN_COMMUNICATION_NONE, ranges_bridge, 0, cat)

static const PCPIN_DESCRIPTOR pins_wave_render[] = {
    STREAM_PIN(KSPIN_DATAFLOW_IN), BRIDGE_PIN(KSPIN_DATAFLOW_OUT, &cat_audio)};
static const PCPIN_DESCRIPTOR pins_topo_render[] = {
    BRIDGE_PIN(KSPIN_DATAFLOW_IN, &cat_audio), BRIDGE_PIN(KSPIN_DATAFLOW_OUT, &node_speaker)};
static const PCPIN_DESCRIPTOR pins_wave_capture[] = {
    BRIDGE_PIN(KSPIN_DATAFLOW_IN, &cat_audio), STREAM_PIN(KSPIN_DATAFLOW_OUT)};
static const PCPIN_DESCRIPTOR pins_topo_capture[] = {
    BRIDGE_PIN(KSPIN_DATAFLOW_IN, &node_microphone), BRIDGE_PIN(KSPIN_DATAFLOW_OUT, &cat_audio)};

/* Every filter here passes pin 0 straight through to pin 1. */
static const PCCONNECTION_DESCRIPTOR through[] = {{PCFILTER_NODE, 0, PCFILTER_NODE, 1}};

static const GUID cats_wave_render[] = {{STATICGUIDOF(KSCATEGORY_AUDIO)}, {STATICGUIDOF(KSCATEGORY_RENDER)},
                                        {STATICGUIDOF(KSCATEGORY_REALTIME)}};
static const GUID cats_wave_capture[] = {{STATICGUIDOF(KSCATEGORY_AUDIO)}, {STATICGUIDOF(KSCATEGORY_CAPTURE)},
                                         {STATICGUIDOF(KSCATEGORY_REALTIME)}};
static const GUID cats_topo[] = {{STATICGUIDOF(KSCATEGORY_AUDIO)}, {STATICGUIDOF(KSCATEGORY_TOPOLOGY)}};

#define FILTER(pins, cats)                                                                           \
    {0, NULL, sizeof(PCPIN_DESCRIPTOR), 2, pins, sizeof(PCNODE_DESCRIPTOR), 0, NULL, 1, through,      \
     sizeof(cats) / sizeof(cats[0]), cats}
static PCFILTER_DESCRIPTOR filter_desc[4] = {
    FILTER(pins_wave_render, cats_wave_render), FILTER(pins_topo_render, cats_topo),
    FILTER(pins_wave_capture, cats_wave_capture), FILTER(pins_topo_capture, cats_topo)};

/* ---- Streams ---- */

typedef struct {
    const IMiniportWaveRTStreamNotificationVtbl *vtbl;
    LONG refs;
    int capture;
    ULONG format;             /* HWA_FMT_* */
    ULONG frame_bytes;
    ULONG bytes;              /* buffer in use; 0 until allocated */
    ULONG period_bytes;       /* bytes / NotificationCount, 0 without notifications */
    PMDL mdl;
    KSSTATE state;
    PEX_TIMER timer;
    KSPIN_LOCK lock;          /* guards event */
    PKEVENT event;
    ULONGLONG last_period;    /* the period the cursor was in when the timer last looked */
    ULONG epoch;
} Stream;

/* Frames the phone has played or recorded in this stream's current run, 0 until it has seen it. */
static ULONGLONG stream_frames(Stream *s) {
    volatile struct hwa_stream *h = dir_stream(s->capture);
    ULONG seen = __atomic_load_n(&h->seen_epoch, __ATOMIC_ACQUIRE);
    if (seen != s->epoch) return 0;
    ULONGLONG f = __atomic_load_n(&h->frames, __ATOMIC_ACQUIRE);
    return __atomic_load_n(&h->seen_epoch, __ATOMIC_ACQUIRE) == s->epoch ? f : 0;
}

static ULONGLONG stream_bytes(Stream *s) { return stream_frames(s) * s->frame_bytes; }

static VOID tick(PEX_TIMER timer, PVOID context) {
    Stream *s = context;
    (void)timer;
    if (!s->period_bytes) return;
    ULONGLONG period = stream_bytes(s) / s->period_bytes;
    if (period == s->last_period) return;
    s->last_period = period;
    KIRQL irql;
    KeAcquireSpinLock(&s->lock, &irql);
    if (s->event) KeSetEvent(s->event, 0, FALSE);
    KeReleaseSpinLock(&s->lock, irql);
}

static BOOLEAN guid_eq(const GUID *a, const GUID *b) {
    const ULONGLONG *x = (const ULONGLONG *)a, *y = (const ULONGLONG *)b;
    return x[0] == y[0] && x[1] == y[1];
}

static STDMETHODIMP_(NTSTATUS) st_qi(IMiniportWaveRTStreamNotification *self, REFIID iid, PVOID *out) {
    if (guid_eq(iid, &IID_IUnknown) || guid_eq(iid, &IID_IMiniportWaveRTStream) ||
        guid_eq(iid, &IID_IMiniportWaveRTStreamNotification)) {
        __atomic_add_fetch(&((Stream *)self)->refs, 1, __ATOMIC_ACQ_REL);
        *out = self;
        return STATUS_SUCCESS;
    }
    *out = NULL;
    return STATUS_INVALID_PARAMETER;
}

static STDMETHODIMP_(ULONG) st_addref(IMiniportWaveRTStreamNotification *self) {
    return __atomic_add_fetch(&((Stream *)self)->refs, 1, __ATOMIC_ACQ_REL);
}

static void stream_publish_state(Stream *s, ULONG state) {
    __atomic_store_n(&dir_stream(s->capture)->state, state, __ATOMIC_RELEASE);
}

static STDMETHODIMP_(ULONG) st_release(IMiniportWaveRTStreamNotification *self) {
    Stream *s = (Stream *)self;
    LONG n = __atomic_sub_fetch(&s->refs, 1, __ATOMIC_ACQ_REL);
    if (n) return n;
    if (s->timer) ExDeleteTimer(s->timer, TRUE, TRUE, NULL);
    stream_publish_state(s, HWA_STOP);
    if (s->mdl) IoFreeMdl(s->mdl);
    ExFreePoolWithTag(s, TAG);
    return 0;
}

static STDMETHODIMP_(NTSTATUS) st_set_format(IMiniportWaveRTStreamNotification *self, PKSDATAFORMAT f) {
    (void)self, (void)f;
    return STATUS_NOT_SUPPORTED;
}

static STDMETHODIMP_(NTSTATUS) st_set_state(IMiniportWaveRTStreamNotification *self, KSSTATE state) {
    Stream *s = (Stream *)self;
    volatile struct hwa_stream *h = dir_stream(s->capture);
    if (state == KSSTATE_STOP) {
        ExCancelTimer(s->timer, NULL);
        stream_publish_state(s, HWA_STOP);
    } else if (s->state == KSSTATE_STOP) {
        /* A new run from position 0. The phone resets its cursor when it sees the new epoch. */
        s->epoch = h->epoch + 1;
        s->last_period = 0;
        h->format = s->format;
        h->bytes = s->bytes;
        h->periods = s->period_bytes ? s->bytes / s->period_bytes : 0;
        __atomic_store_n(&h->epoch, s->epoch, __ATOMIC_RELEASE);
    }
    if (state == KSSTATE_RUN) {
        stream_publish_state(s, HWA_RUN);
        ExSetTimer(s->timer, -TICK_100NS, TICK_100NS, NULL);
    } else if (state != KSSTATE_STOP) {
        ExCancelTimer(s->timer, NULL);
        stream_publish_state(s, HWA_PAUSE);
    }
    s->state = state;
    return STATUS_SUCCESS;
}

static STDMETHODIMP_(NTSTATUS) st_get_position(IMiniportWaveRTStreamNotification *self, PKSAUDIO_POSITION pos) {
    Stream *s = (Stream *)self;
    ULONGLONG at = s->bytes ? stream_bytes(s) % s->bytes : 0;
    pos->PlayOffset = at;
    pos->WriteOffset = at;
    return STATUS_SUCCESS;
}

/* The buffer is always the shared one: an MDL over the kernel view of it, which PortCls maps into
 * the audio engine's process. */
static NTSTATUS stream_buffer(Stream *s, ULONG want, ULONG notifications, PMDL *mdl, ULONG *size,
                              ULONG *offset, MEMORY_CACHING_TYPE *cache) {
    if (s->mdl) return STATUS_DEVICE_BUSY;
    ULONG bytes = want < HWA_BUF_BYTES ? want : HWA_BUF_BYTES;
    if (notifications) bytes -= bytes % (notifications * s->frame_bytes);
    else bytes -= bytes % s->frame_bytes;
    if (!bytes) return STATUS_INVALID_PARAMETER;
    PMDL m = IoAllocateMdl(g_buf[s->capture], bytes, FALSE, FALSE, NULL);
    if (!m) return STATUS_INSUFFICIENT_RESOURCES;
    MmBuildMdlForNonPagedPool(m);
    ULONG *w = (ULONG *)g_buf[s->capture];
    ULONG mark = s->capture ? 0 : s->format == HWA_FMT_F32 ? HWA_MARK_F32 : HWA_MARK_S16;
    for (ULONG i = 0; i < bytes / 4; i++) w[i] = mark;
    s->mdl = m;
    s->bytes = bytes;
    s->period_bytes = notifications ? bytes / notifications : 0;
    *mdl = m;
    *size = bytes;
    *offset = 0;
    *cache = MmCached;
    return STATUS_SUCCESS;
}

static void stream_free(Stream *s, PMDL mdl) {
    if (mdl && mdl == s->mdl) {
        IoFreeMdl(mdl);
        s->mdl = NULL;
        s->bytes = 0;
        s->period_bytes = 0;
    }
}

static STDMETHODIMP_(NTSTATUS) st_alloc(IMiniportWaveRTStreamNotification *self, ULONG want, PMDL *mdl, ULONG *size,
                                        ULONG *offset, MEMORY_CACHING_TYPE *cache) {
    return stream_buffer((Stream *)self, want, 0, mdl, size, offset, cache);
}

static STDMETHODIMP_(VOID) st_free(IMiniportWaveRTStreamNotification *self, PMDL mdl, ULONG size) {
    (void)size;
    stream_free((Stream *)self, mdl);
}

static STDMETHODIMP_(VOID) st_latency(IMiniportWaveRTStreamNotification *self, KSRTAUDIO_HWLATENCY *l) {
    (void)self;
    l->FifoSize = 0;
    l->ChipsetDelay = 0;
    l->CodecDelay = 80000;   /* about 8 ms inside AAudio, in 100 ns units */
}

static STDMETHODIMP_(NTSTATUS) st_no_register(IMiniportWaveRTStreamNotification *self, KSRTAUDIO_HWREGISTER *r) {
    (void)self, (void)r;
    return STATUS_NOT_SUPPORTED;
}

static STDMETHODIMP_(NTSTATUS) st_alloc_notify(IMiniportWaveRTStreamNotification *self, ULONG count, ULONG want,
                                               PMDL *mdl, ULONG *size, ULONG *offset, MEMORY_CACHING_TYPE *cache) {
    if (!count) return STATUS_INVALID_PARAMETER;
    return stream_buffer((Stream *)self, want, count, mdl, size, offset, cache);
}

static STDMETHODIMP_(NTSTATUS) st_register_event(IMiniportWaveRTStreamNotification *self, PKEVENT event) {
    Stream *s = (Stream *)self;
    KIRQL irql;
    KeAcquireSpinLock(&s->lock, &irql);
    NTSTATUS st = s->event ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
    if (!s->event) s->event = event;
    KeReleaseSpinLock(&s->lock, irql);
    return st;
}

static STDMETHODIMP_(NTSTATUS) st_unregister_event(IMiniportWaveRTStreamNotification *self, PKEVENT event) {
    Stream *s = (Stream *)self;
    KIRQL irql;
    KeAcquireSpinLock(&s->lock, &irql);
    NTSTATUS st = s->event == event ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
    if (s->event == event) s->event = NULL;
    KeReleaseSpinLock(&s->lock, irql);
    return st;
}

static const IMiniportWaveRTStreamNotificationVtbl stream_vtbl = {
    st_qi, st_addref, st_release, st_set_format, st_set_state, st_get_position, st_alloc, st_free, st_latency,
    st_no_register, st_no_register, st_alloc_notify, st_free, st_register_event, st_unregister_event};

/* The 16-bit PCM or 32-bit float stereo 48 kHz format the engine asks for, or 0. */
static ULONG parse_format(PKSDATAFORMAT f) {
    if (f->FormatSize < sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEX)) return 0;
    WAVEFORMATEX *w = (WAVEFORMATEX *)(f + 1);
    ULONG tag = w->wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE) {
        if (f->FormatSize < sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEXTENSIBLE)) return 0;
        tag = ((WAVEFORMATEXTENSIBLE *)w)->SubFormat.Data1;
    }
    if (w->nChannels != HWA_CHANNELS || w->nSamplesPerSec != HWA_RATE) return 0;
    if (tag == HWA_FMT_S16 && w->wBitsPerSample == 16) return HWA_FMT_S16;
    if (tag == HWA_FMT_F32 && w->wBitsPerSample == 32) return HWA_FMT_F32;
    return 0;
}

/* ---- Filters ---- */

typedef struct {
    const void *vtbl;         /* IMiniportWaveRTVtbl or IMiniportTopologyVtbl */
    int index;                /* into filter_desc: 0 wave render, 1 topo render, 2 wave capture, 3 topo capture */
} Filter;

static STDMETHODIMP_(NTSTATUS) f_qi(PVOID self, REFIID iid, PVOID *out) {
    Filter *f = self;
    int wave = !(f->index & 1);
    if (guid_eq(iid, &IID_IUnknown) || guid_eq(iid, &IID_IMiniport) ||
        (wave && guid_eq(iid, &IID_IMiniportWaveRT)) || (!wave && guid_eq(iid, &IID_IMiniportTopology))) {
        *out = self;
        return STATUS_SUCCESS;
    }
    *out = NULL;
    return STATUS_INVALID_PARAMETER;
}

/* Filters live as long as the driver: counting references is not needed. */
static STDMETHODIMP_(ULONG) f_addref(PVOID self) { (void)self; return 2; }
static STDMETHODIMP_(ULONG) f_release(PVOID self) { (void)self; return 1; }

static STDMETHODIMP_(NTSTATUS) f_desc(PVOID self, PPCFILTER_DESCRIPTOR *out) {
    *out = &filter_desc[((Filter *)self)->index];
    return STATUS_SUCCESS;
}

/* PortCls intersects the ranges above itself. */
static STDMETHODIMP_(NTSTATUS) f_intersect(PVOID self, ULONG pin, PKSDATARANGE range, PKSDATARANGE match,
                                           ULONG len, PVOID format, PULONG format_len) {
    (void)self, (void)pin, (void)range, (void)match, (void)len, (void)format, (void)format_len;
    return STATUS_NOT_IMPLEMENTED;
}

static STDMETHODIMP_(NTSTATUS) f_init(PVOID self, PUNKNOWN adapter, PRESOURCELIST res, PVOID port) {
    (void)self, (void)adapter, (void)res, (void)port;
    return STATUS_SUCCESS;
}

static STDMETHODIMP_(NTSTATUS) f_new_stream(PVOID self, PMINIPORTWAVERTSTREAM *out, PPORTWAVERTSTREAM port,
                                            ULONG pin, BOOLEAN capture, PKSDATAFORMAT format) {
    Filter *f = self;
    (void)port;
    *out = NULL;
    if (pin != (capture ? 1u : 0u) || capture != (f->index == 2)) return STATUS_INVALID_PARAMETER;
    ULONG fmt = parse_format(format);
    if (!fmt) return STATUS_NO_MATCH;
    Stream *s = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*s), TAG);
    if (!s) return STATUS_INSUFFICIENT_RESOURCES;
    memset(s, 0, sizeof(*s));
    s->vtbl = &stream_vtbl;
    s->refs = 1;
    s->capture = capture;
    s->format = fmt;
    s->frame_bytes = HWA_FRAME_BYTES(fmt);
    s->state = KSSTATE_STOP;
    s->epoch = dir_stream(capture)->epoch;
    KeInitializeSpinLock(&s->lock);
    s->timer = ExAllocateTimer(tick, s, EX_TIMER_HIGH_RESOLUTION);
    if (!s->timer) {
        ExFreePoolWithTag(s, TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    *out = (PMINIPORTWAVERTSTREAM)s;
    return STATUS_SUCCESS;
}

static STDMETHODIMP_(NTSTATUS) f_dma(PVOID self, PDEVICE_DESCRIPTION d) {
    (void)self;
    memset(d, 0, sizeof(*d));
    d->Master = TRUE;
    d->ScatterGather = TRUE;
    d->Dma32BitAddresses = TRUE;
    d->InterfaceType = PCIBus;
    d->MaximumLength = 0xffffffff;
    return STATUS_SUCCESS;
}

typedef STDMETHODIMP_(NTSTATUS) (*init_fn)(PVOID, PUNKNOWN, PRESOURCELIST, PVOID);
static const IMiniportWaveRTVtbl wave_vtbl = {
    (void *)f_qi, (void *)f_addref, (void *)f_release, (void *)f_desc, (void *)f_intersect,
    (void *)f_init, (void *)f_new_stream, (void *)f_dma};
static const IMiniportTopologyVtbl topo_vtbl = {
    (void *)f_qi, (void *)f_addref, (void *)f_release, (void *)f_desc, (void *)f_intersect, (void *)f_init};

static Filter filters[4] = {{&wave_vtbl, 0}, {&topo_vtbl, 1}, {&wave_vtbl, 2}, {&topo_vtbl, 3}};
static const CLSID *filter_port[4] = {&CLSID_PortWaveRT, &CLSID_PortTopology, &CLSID_PortWaveRT, &CLSID_PortTopology};
static PWCHAR filter_name[4] = {L"WaveRender", L"TopologyRender", L"WaveCapture", L"TopologyCapture"};

/* ---- Adapter ---- */

static BOOLEAN map_window(void) {
    if (g_ctl) return TRUE;
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = (LONGLONG)(HW_WINDOW_PA + HWA_OFF);
    UCHAR *base = MmMapIoSpace(pa, HWA_END - HWA_OFF, MmCached);
    if (!base) return FALSE;
    g_buf[0] = base + (HWA_RENDER_OFF - HWA_OFF);
    g_buf[1] = base + (HWA_CAPTURE_OFF - HWA_OFF);
    /* Before Windows the boot code may have left a test tone header here: start clean. */
    g_ctl = (volatile struct hwa_ctl *)base;
    memset((void *)g_ctl, 0, HWA_CTL_BYTES);
    g_ctl->version = HWA_VERSION;
    g_ctl->rate = HWA_RATE;
    g_ctl->channels = HWA_CHANNELS;
    __atomic_store_n(&g_ctl->magic, HWA_MAGIC, __ATOMIC_RELEASE);
    return TRUE;
}

static NTSTATUS start_device(PDEVICE_OBJECT dev, PIRP irp, PRESOURCELIST res) {
    if (!map_window()) return STATUS_INSUFFICIENT_RESOURCES;
    PPORT ports[4] = {0};
    NTSTATUS st = STATUS_SUCCESS;
    for (int i = 0; i < 4 && NT_SUCCESS(st); i++) {
        st = PcNewPort(&ports[i], filter_port[i]);
        if (NT_SUCCESS(st)) st = ports[i]->lpVtbl->Init(ports[i], dev, irp, (PUNKNOWN)&filters[i], NULL, res);
        if (NT_SUCCESS(st)) st = PcRegisterSubdevice(dev, filter_name[i], (PUNKNOWN)ports[i]);
    }
    if (NT_SUCCESS(st)) st = PcRegisterPhysicalConnection(dev, (PUNKNOWN)ports[0], 1, (PUNKNOWN)ports[1], 0);
    if (NT_SUCCESS(st)) st = PcRegisterPhysicalConnection(dev, (PUNKNOWN)ports[3], 1, (PUNKNOWN)ports[2], 0);
    for (int i = 0; i < 4; i++)
        if (ports[i]) ports[i]->lpVtbl->Release(ports[i]);
    return st;
}

static NTSTATUS add_device(PDRIVER_OBJECT drv, PDEVICE_OBJECT pdo) {
    return PcAddAdapterDevice(drv, pdo, (PCPFNSTARTDEVICE)start_device, 4, 0);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg) {
    return PcInitializeAdapterDriver(drv, reg, add_device);
}
