/* The display half of the adapter (wddm/README.md, phase 6): one screen, one source, one target.
 * Built into hwgpuw.c (it includes this file) and switched on by Parameters\Display = 1, so the
 * driver stays render-only until the screen path is proven.
 *
 * What it does for now: tells Windows there is one monitor, offers a list of modes, and takes
 * what Windows decides (commit, source address, visibility). Where a frame goes is the next step. */
#ifdef HWW_TRACE
static void tr(const char *name);
#else
#define tr(name) ((void)0)
#endif


#define HWD_REFRESH_N 60000u
#define HWD_REFRESH_D 1000u

static __inline uint64_t hwd_cntvct(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

struct hwd_mode { UINT w, h; };

static struct {
    BOOLEAN on;
    UINT native_w, native_h;       /* the largest mode, from Parameters\DisplayWidth/DisplayHeight */
    UINT cur_w, cur_h;             /* what the last commit chose */
    UINT cur_hz;                   /* committed refresh rate in Hz (60/90/120) */
    ULONG cur_period_100ns;        /* 10000000 / cur_hz */
    BOOLEAN visible;
    PHYSICAL_ADDRESS source_addr;
    HANDLE source_alloc;
    UINT commits, source_sets, presents, vsyncs;
    ULONG64 last_addr[4];          /* the last source addresses Windows set (read from outside) */
    ULONG last_flags, last_context_count, last_alloc_ok;
    struct hwgpu_pointer *pointer;  /* the shared pointer block, mapped when the screen half is on */
    volatile struct hwgpu_vsync *vsync; /* phone AChoreographer timeline, mapped at HWGPU_VSYNC_OFF */
    BOOLEAN vsync_calibrated;
    int64_t vsync_off;             /* phone CNTVCT - guest CNTVCT */
} d;

static UINT hwd_phone_hz(void)
{
    if (d.vsync && d.vsync->magic == HWGPU_VSYNC_MAGIC) {
        uint32_t m = d.vsync->hz_milli;
        if (m >= 105000u) return 120u;
        if (m >= 75000u) return 90u;
        if (m >= 45000u) return 60u;
    }
    return 60u;
}

static const struct hwd_mode hwd_extra[] = { { 1920, 1080 }, { 1600, 900 }, { 1366, 768 }, { 1280, 720 }, { 1024, 768 }, { 800, 600 } };

static UINT hwd_mode_list(struct hwd_mode *out, UINT max)
{
    UINT n = 0;
    out[n].w = d.native_w;
    out[n].h = d.native_h;
    n++;
    for (UINT i = 0; i < sizeof hwd_extra / sizeof hwd_extra[0] && n < max; i++) {
        if (hwd_extra[i].w > d.native_w || hwd_extra[i].h > d.native_h) continue;
        if (hwd_extra[i].w == d.native_w && hwd_extra[i].h == d.native_h) continue;
        out[n++] = hwd_extra[i];
    }
    return n;
}

static ULONG hwd_reg_dword(const WCHAR *name, ULONG def)
{
    HANDLE key = NULL;
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING path, vn;
    ULONG got = def;
    RtlInitUnicodeString(&path, g.guard_path);
    InitializeObjectAttributes(&oa, &path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&key, KEY_READ, &oa))) return def;
    RtlInitUnicodeString(&vn, name);
    UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 4];
    ULONG len = 0;
    if (NT_SUCCESS(ZwQueryValueKey(key, &vn, KeyValuePartialInformation, buf, sizeof buf, &len))) {
        KEY_VALUE_PARTIAL_INFORMATION *kv = (KEY_VALUE_PARTIAL_INFORMATION *)buf;
        if (kv->Type == REG_DWORD && kv->DataLength == 4) got = *(ULONG *)kv->Data;
    }
    ZwClose(key);
    return got;
}

/* Hot-path registry tracing (D_Step, D_<fn>, T00..T1F, D_StdAsked) is off by default:
 * writing Parameters on every Present/Allocation stalled both DWM and apps. Set Parameters\TraceReg = 1 to enable. */
static BOOLEAN g_trace_reg;

static void report(const WCHAR *what, ULONG value);

/* A ring of the last 32 events about Windows' own surfaces, in the service's Parameters key as T00..T1F:
 * code << 28 | a << 14 | b. 1 GDI surface asked (type, flags), 2 its size, 3 CreateAllocation (kind, segment set),
 * 4 DestroyAllocation (kind), 5 DescribeAllocation (kind), 6 OpenAllocation (kind). Debug, for the DWM investigation. */
static volatile LONG hwd_nmap, hwd_nunmap, hwd_nops[16];
static volatile ULONG hwd_maplast;

static void hwd_trace(ULONG code, ULONG a, ULONG b)
{
    if (!g_trace_reg) return;
    if (KeGetCurrentIrql() == PASSIVE_LEVEL) {   /* the paging counters are bumped at dispatch level, where the registry is off limits */
        report(L"P_Map", (ULONG)hwd_nmap);
        report(L"P_Unmap", (ULONG)hwd_nunmap);
        report(L"P_MapLast", hwd_maplast);
        report(L"P_Ops", ((ULONG)hwd_nops[0] & 0xffff) | (((ULONG)hwd_nops[1] & 0xffff) << 16));
    }
    static volatile LONG n;
    WCHAR name[] = L"T00";
    ULONG i = (ULONG)InterlockedIncrement(&n) & 31;
    name[1] = (WCHAR)(L"0123456789ABCDEF"[i >> 4]);
    name[2] = (WCHAR)(L"0123456789ABCDEF"[i & 15]);
    report(name, (code << 28) | ((a & 0x3fff) << 14) | (b & 0x3fff));
}


/* Each display entry's last answer goes into Parameters as D_<name>, so a run can be read afterwards. */
static void report(const WCHAR *what, ULONG value);
static void report_bases(void);
#define HWD_STEP(n) do { if (g_trace_reg) { report_bases(); report(L"D_Step", (ULONG)(n)); } } while (0)
static void hwd_log(const char *name, NTSTATUS ns)
{
    if (!g_trace_reg) return;
    WCHAR w[48] = L"D_";
    UINT i = 2;
    for (const char *c = name; *c && i < 46; c++) w[i++] = (WCHAR)(UCHAR)*c;
    w[i] = 0;
    report(w, (ULONG)ns);
}

/* ---- the monitor ---- */

static NTSTATUS NTAPI HwdQueryChildRelations(CONST PVOID ctx, PDXGK_CHILD_DESCRIPTOR rel, ULONG size)
{
    tr("HwdQueryChil");
    HWD_STEP(900);
    (void)ctx;
    if (size < 2 * sizeof(DXGK_CHILD_DESCRIPTOR)) return STATUS_BUFFER_TOO_SMALL;
    RtlZeroMemory(rel, size);
    rel[0].ChildDeviceType = TypeVideoOutput;
    rel[0].ChildCapabilities.HpdAwareness = HpdAwarenessAlwaysConnected;
    rel[0].ChildCapabilities.Type.VideoOutput.InterfaceTechnology = D3DKMDT_VOT_HD15;
    rel[0].ChildCapabilities.Type.VideoOutput.MonitorOrientationAwareness = D3DKMDT_MOA_NONE;
    rel[0].ChildCapabilities.Type.VideoOutput.SupportsSdtvModes = FALSE;
    rel[0].ChildUid = 0;   /* the video present target id is the child uid */
    rel[0].AcpiUid = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwdQueryChildStatus(CONST PVOID ctx, PDXGK_CHILD_STATUS st, BOOLEAN nonDestructive)
{
    tr("HwdQueryChil");
    HWD_STEP(901);
    (void)ctx;
    (void)nonDestructive;
    if (st->ChildUid != 0) return STATUS_INVALID_PARAMETER;
    switch (st->Type) {
    case StatusConnection:
        st->HotPlug.Connected = TRUE;
        return STATUS_SUCCESS;
    case StatusRotation:
        st->Rotation.Angle = 0;
        return STATUS_SUCCESS;
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

/* No EDID: Windows then asks RecommendMonitorModes, and the monitor shows as a generic one. */
static NTSTATUS NTAPI HwdQueryDeviceDescriptor_impl(CONST PVOID ctx, ULONG uid, PDXGK_DEVICE_DESCRIPTOR desc)
{
    tr("HwdQueryDevi");
    (void)ctx;
    (void)uid;
    (void)desc;
    return STATUS_MONITOR_NO_DESCRIPTOR;
}

/* ---- mode sets ---- */

static void hwd_signal(D3DKMDT_VIDEO_SIGNAL_INFO *s, UINT w, UINT h, UINT hz)
{
    if (!hz) hz = 60u;
    RtlZeroMemory(s, sizeof *s);
    s->VideoStandard = D3DKMDT_VSS_OTHER;
    s->TotalSize.cx = w;
    s->TotalSize.cy = h;
    s->ActiveSize.cx = w;
    s->ActiveSize.cy = h;
    s->VSyncFreq.Numerator = hz * 1000u;
    s->VSyncFreq.Denominator = 1000u;
    s->HSyncFreq.Numerator = hz * h;
    s->HSyncFreq.Denominator = 1;
    s->PixelRate = (SIZE_T)w * h * hz;
    s->ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE;
}

static void hwd_source_format(D3DKMDT_VIDPN_SOURCE_MODE *m, UINT w, UINT h)
{
    m->Type = D3DKMDT_RMT_GRAPHICS;
    m->Format.Graphics.PrimSurfSize.cx = w;
    m->Format.Graphics.PrimSurfSize.cy = h;
    m->Format.Graphics.VisibleRegionSize.cx = w;
    m->Format.Graphics.VisibleRegionSize.cy = h;
    m->Format.Graphics.Stride = w * 4;
    m->Format.Graphics.PixelFormat = D3DDDIFMT_A8R8G8B8;
    m->Format.Graphics.ColorBasis = D3DKMDT_CB_SRGB;
    m->Format.Graphics.PixelValueAccessMode = D3DKMDT_PVAM_DIRECT;
}

/* A fresh source mode set with our modes in it, or only the one size when the other side is decided. */
static NTSTATUS hwd_build_source_set(const DXGK_VIDPN_INTERFACE *vi, D3DKMDT_HVIDPN hv, UINT only_w, UINT only_h)
{
    D3DKMDT_HVIDPNSOURCEMODESET hset;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE *si;
    NTSTATUS ns = vi->pfnCreateNewSourceModeSet(hv, 0, &hset, &si);
    HWD_STEP(10);
    if (!NT_SUCCESS(ns)) return ns;
    struct hwd_mode list[8];
    UINT n = hwd_mode_list(list, 8);
    for (UINT i = 0; i < n; i++) {
        if (only_w && (list[i].w != only_w || list[i].h != only_h)) continue;
        D3DKMDT_VIDPN_SOURCE_MODE *m;
        ns = si->pfnCreateNewModeInfo(hset, &m);
    HWD_STEP(11);
        if (!NT_SUCCESS(ns)) break;
        hwd_source_format(m, list[i].w, list[i].h);
        ns = si->pfnAddMode(hset, m);
    HWD_STEP(12);
        if (!NT_SUCCESS(ns)) { si->pfnReleaseModeInfo(hset, m); break; }
    }
    if (!NT_SUCCESS(ns)) return ns;
    return vi->pfnAssignSourceModeSet(hv, 0, hset);
}

static NTSTATUS hwd_build_target_set(const DXGK_VIDPN_INTERFACE *vi, D3DKMDT_HVIDPN hv, UINT only_w, UINT only_h, UINT only_hz)
{
    static const UINT rates[3] = { 120u, 90u, 60u };
    D3DKMDT_HVIDPNTARGETMODESET hset;
    const DXGK_VIDPNTARGETMODESET_INTERFACE *ti;
    NTSTATUS ns = vi->pfnCreateNewTargetModeSet(hv, 0, &hset, &ti);
    HWD_STEP(20);
    if (!NT_SUCCESS(ns)) return ns;
    UINT pref_hz = hwd_phone_hz();
    struct hwd_mode list[8];
    UINT n = hwd_mode_list(list, 8);
    for (UINT i = 0; i < n; i++) {
        if (only_w && (list[i].w != only_w || list[i].h != only_h)) continue;
        UINT nr = (i == 0 && !only_hz) ? 3u : 1u;
        for (UINT r = 0; r < nr; r++) {
            UINT hz = only_hz ? only_hz : ((i == 0) ? rates[r] : 60u);
            D3DKMDT_VIDPN_TARGET_MODE *m;
            ns = ti->pfnCreateNewModeInfo(hset, &m);
            HWD_STEP(21);
            if (!NT_SUCCESS(ns)) break;
            hwd_signal(&m->VideoSignalInfo, list[i].w, list[i].h, hz);
            m->Preference = (i == 0 && hz == pref_hz) ? D3DKMDT_MP_PREFERRED : D3DKMDT_MP_NOTPREFERRED;
            ns = ti->pfnAddMode(hset, m);
            HWD_STEP(22);
            if (!NT_SUCCESS(ns)) { ti->pfnReleaseModeInfo(hset, m); break; }
        }
        if (!NT_SUCCESS(ns)) break;
    }
    if (!NT_SUCCESS(ns)) return ns;
    return vi->pfnAssignTargetModeSet(hv, 0, hset);
}

static void hwd_path_defaults(D3DKMDT_VIDPN_PRESENT_PATH *p)
{
    p->VidPnSourceId = 0;
    p->VidPnTargetId = 0;
    p->ImportanceOrdinal = D3DKMDT_VPPI_PRIMARY;
    p->ContentTransformation.Scaling = D3DKMDT_VPPS_IDENTITY;
    RtlZeroMemory(&p->ContentTransformation.ScalingSupport, sizeof p->ContentTransformation.ScalingSupport);
    p->ContentTransformation.ScalingSupport.Identity = 1;
    p->ContentTransformation.Rotation = D3DKMDT_VPPR_IDENTITY;
    RtlZeroMemory(&p->ContentTransformation.RotationSupport, sizeof p->ContentTransformation.RotationSupport);
    p->ContentTransformation.RotationSupport.Identity = 1;
    p->VisibleFromActiveTLOffset.cx = 0;
    p->VisibleFromActiveTLOffset.cy = 0;
    p->VisibleFromActiveBROffset.cx = 0;
    p->VisibleFromActiveBROffset.cy = 0;
    p->VidPnTargetColorBasis = D3DKMDT_CB_SRGB;
    p->VidPnTargetColorCoeffDynamicRanges.FirstChannel = 8;
    p->VidPnTargetColorCoeffDynamicRanges.SecondChannel = 8;
    p->VidPnTargetColorCoeffDynamicRanges.ThirdChannel = 8;
    p->VidPnTargetColorCoeffDynamicRanges.FourthChannel = 0;
    p->Content = D3DKMDT_VPPC_GRAPHICS;
    p->CopyProtection.CopyProtectionType = D3DKMDT_VPPMT_NOPROTECTION;
    p->CopyProtection.APSTriggerBits = 0;
    RtlZeroMemory(&p->CopyProtection.CopyProtectionSupport, sizeof p->CopyProtection.CopyProtectionSupport);
    p->GammaRamp.Type = D3DDDI_GAMMARAMP_DEFAULT;
    p->GammaRamp.DataSize = 0;
}

static NTSTATUS NTAPI HwdIsSupportedVidPn_impl(const HANDLE hAdapter, DXGKARG_ISSUPPORTEDVIDPN *arg)
{
    tr("HwdIsSupport");
    (void)hAdapter;
    const DXGK_VIDPN_INTERFACE *vi;
    D3DKMDT_HVIDPNTOPOLOGY ht;
    const DXGK_VIDPNTOPOLOGY_INTERFACE *ti;
    arg->IsVidPnSupported = TRUE;
    NTSTATUS ns = g.dxgk.DxgkCbQueryVidPnInterface(arg->hDesiredVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vi);
    if (!NT_SUCCESS(ns)) return ns;
    ns = vi->pfnGetTopology(arg->hDesiredVidPn, &ht, &ti);
    if (!NT_SUCCESS(ns)) return ns;
    SIZE_T paths = 0;
    ns = ti->pfnGetNumPaths(ht, &paths);
    if (!NT_SUCCESS(ns)) return ns;
    if (paths > 1) arg->IsVidPnSupported = FALSE;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwdRecommendFunctionalVidPn_impl(const HANDLE hAdapter, const DXGKARG_RECOMMENDFUNCTIONALVIDPN *arg)
{
    tr("HwdRecommend");
    (void)hAdapter;
    const DXGK_VIDPN_INTERFACE *vi;
    D3DKMDT_HVIDPN hv = arg->hRecommendedFunctionalVidPn;
    NTSTATUS ns = g.dxgk.DxgkCbQueryVidPnInterface(hv, DXGK_VIDPN_INTERFACE_VERSION_V1, &vi);
    if (!NT_SUCCESS(ns)) return ns;
    D3DKMDT_HVIDPNTOPOLOGY ht;
    const DXGK_VIDPNTOPOLOGY_INTERFACE *ti;
    ns = vi->pfnGetTopology(hv, &ht, &ti);
    if (!NT_SUCCESS(ns)) return ns;
    D3DKMDT_VIDPN_PRESENT_PATH *path;
    ns = ti->pfnCreateNewPathInfo(ht, &path);
    HWD_STEP(30);
    if (!NT_SUCCESS(ns)) return ns;
    hwd_path_defaults(path);
    ns = ti->pfnAddPath(ht, path);
    HWD_STEP(31);
    if (!NT_SUCCESS(ns)) { ti->pfnReleasePathInfo(ht, path); return ns; }
    ns = hwd_build_source_set(vi, hv, d.native_w, d.native_h);
    if (!NT_SUCCESS(ns)) return ns;
    return hwd_build_target_set(vi, hv, d.native_w, d.native_h, hwd_phone_hz());
}

static BOOLEAN hwd_pinned_source(const DXGK_VIDPN_INTERFACE *vi, D3DKMDT_HVIDPN hv, UINT *w, UINT *h)
{
    D3DKMDT_HVIDPNSOURCEMODESET hset;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE *si;
    if (!NT_SUCCESS(vi->pfnAcquireSourceModeSet(hv, 0, &hset, &si))) return FALSE;
    const D3DKMDT_VIDPN_SOURCE_MODE *m = NULL;
    BOOLEAN got = FALSE;
    if (NT_SUCCESS(si->pfnAcquirePinnedModeInfo(hset, &m)) && m) {
        *w = m->Format.Graphics.PrimSurfSize.cx;
        *h = m->Format.Graphics.PrimSurfSize.cy;
        si->pfnReleaseModeInfo(hset, m);
        got = TRUE;
    }
    vi->pfnReleaseSourceModeSet(hv, hset);
    return got;
}

static BOOLEAN hwd_pinned_target(const DXGK_VIDPN_INTERFACE *vi, D3DKMDT_HVIDPN hv, UINT *w, UINT *h, UINT *hz)
{
    D3DKMDT_HVIDPNTARGETMODESET hset;
    const DXGK_VIDPNTARGETMODESET_INTERFACE *ti;
    if (!NT_SUCCESS(vi->pfnAcquireTargetModeSet(hv, 0, &hset, &ti))) return FALSE;
    const D3DKMDT_VIDPN_TARGET_MODE *m = NULL;
    BOOLEAN got = FALSE;
    if (NT_SUCCESS(ti->pfnAcquirePinnedModeInfo(hset, &m)) && m) {
        *w = m->VideoSignalInfo.ActiveSize.cx;
        *h = m->VideoSignalInfo.ActiveSize.cy;
        if (hz) {
            UINT num = m->VideoSignalInfo.VSyncFreq.Numerator;
            UINT den = m->VideoSignalInfo.VSyncFreq.Denominator;
            *hz = (den && num) ? (num + den / 2) / den : 60u;
        }
        ti->pfnReleaseModeInfo(hset, m);
        got = TRUE;
    }
    vi->pfnReleaseTargetModeSet(hv, hset);
    return got;
}

/* Windows has a VidPN with some constraints already pinned; fill what is open with every mode we
 * have, or the one mode if the other side already decided. */
static NTSTATUS NTAPI HwdEnumVidPnCofuncModality_impl(const HANDLE hAdapter, const DXGKARG_ENUMVIDPNCOFUNCMODALITY *arg)
{
    tr("HwdEnumVidPn");
    (void)hAdapter;
    const DXGK_VIDPN_INTERFACE *vi;
    D3DKMDT_HVIDPN hv = arg->hConstrainingVidPn;
    NTSTATUS ns = g.dxgk.DxgkCbQueryVidPnInterface(hv, DXGK_VIDPN_INTERFACE_VERSION_V1, &vi);
    if (!NT_SUCCESS(ns)) return ns;
    UINT sw = 0, sh = 0, tw = 0, th = 0;
    BOOLEAN sp = hwd_pinned_source(vi, hv, &sw, &sh);
    BOOLEAN tp = hwd_pinned_target(vi, hv, &tw, &th, NULL);
    UINT w = 0, h = 0;
    if (sp) { w = sw; h = sh; } else if (tp) { w = tw; h = th; }
    /* A side with a pinned mode is Windows' decision and stays as it is. Pinning is never ours to do:
     * a pin from here made dxgkrnl stop answering. */
    if (!sp) {
        ns = hwd_build_source_set(vi, hv, w, h);
        if (!NT_SUCCESS(ns)) return ns;
    }
    if (!tp) {
        ns = hwd_build_target_set(vi, hv, w, h, 0);
        if (!NT_SUCCESS(ns)) return ns;
    }
    /* The path's scaling and rotation are only ever identity, so nothing there to widen. */
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwdRecommendMonitorModes_impl(const HANDLE hAdapter, const DXGKARG_RECOMMENDMONITORMODES *arg)
{
    static const UINT rates[3] = { 120u, 90u, 60u };
    tr("HwdRecommend");
    (void)hAdapter;
    UINT pref_hz = hwd_phone_hz();
    struct hwd_mode list[8];
    UINT n = hwd_mode_list(list, 8);
    for (UINT i = 0; i < n; i++) {
        UINT nr = (i == 0) ? 3u : 1u;
        for (UINT r = 0; r < nr; r++) {
            UINT hz = (i == 0) ? rates[r] : 60u;
            D3DKMDT_MONITOR_SOURCE_MODE *m;
            NTSTATUS ns = arg->pMonitorSourceModeSetInterface->pfnCreateNewModeInfo(arg->hMonitorSourceModeSet, &m);
            if (!NT_SUCCESS(ns)) return ns;
            hwd_signal(&m->VideoSignalInfo, list[i].w, list[i].h, hz);
            m->ColorBasis = D3DKMDT_CB_SRGB;
            m->ColorCoeffDynamicRanges.FirstChannel = 8;
            m->ColorCoeffDynamicRanges.SecondChannel = 8;
            m->ColorCoeffDynamicRanges.ThirdChannel = 8;
            m->ColorCoeffDynamicRanges.FourthChannel = 0;
            m->Origin = D3DKMDT_MCO_DRIVER;
            m->Preference = (i == 0 && hz == pref_hz) ? D3DKMDT_MP_PREFERRED : D3DKMDT_MP_NOTPREFERRED;
            ns = arg->pMonitorSourceModeSetInterface->pfnAddMode(arg->hMonitorSourceModeSet, m);
            if (!NT_SUCCESS(ns)) {
                arg->pMonitorSourceModeSetInterface->pfnReleaseModeInfo(arg->hMonitorSourceModeSet, m);
                return ns;
            }
        }
    }
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwdRecommendVidPnTopology(const HANDLE hAdapter, const DXGKARG_RECOMMENDVIDPNTOPOLOGY *arg)
{
    tr("HwdRecommend");
    HWD_STEP(905);
    (void)hAdapter;
    (void)arg;
    return STATUS_GRAPHICS_NO_RECOMMENDED_VIDPN_TOPOLOGY;
}

static NTSTATUS NTAPI HwdCommitVidPn_impl(const HANDLE hAdapter, const DXGKARG_COMMITVIDPN *arg)
{
    tr("HwdCommitVid");
    (void)hAdapter;
    const DXGK_VIDPN_INTERFACE *vi;
    NTSTATUS ns = g.dxgk.DxgkCbQueryVidPnInterface(arg->hFunctionalVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vi);
    if (!NT_SUCCESS(ns)) return ns;
    UINT w = 0, h = 0, tw = 0, th = 0, hz = 0;
    if (hwd_pinned_source(vi, arg->hFunctionalVidPn, &w, &h)) {
        d.cur_w = w;
        d.cur_h = h;
    }
    if (hwd_pinned_target(vi, arg->hFunctionalVidPn, &tw, &th, &hz) && hz >= 30 && hz <= 240) {
        d.cur_hz = hz;
        d.cur_period_100ns = 10000000u / hz;
    }
    d.commits++;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwdUpdateActiveVidPnPresentPath_impl(const HANDLE hAdapter, const DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *arg)
{
    tr("HwdUpdateAct");
    (void)hAdapter;
    (void)arg;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwdSetVidPnSourceAddress_impl(const HANDLE hAdapter, const DXGKARG_SETVIDPNSOURCEADDRESS *arg)
{
    tr("HwdSetVidPnS");
    (void)hAdapter;
    d.last_addr[d.source_sets & 3] = (ULONG64)arg->PrimaryAddress.QuadPart;
    d.last_flags = arg->Flags.Value;
    d.last_context_count = arg->ContextCount;
    d.last_alloc_ok = arg->hAllocation != NULL;
    d.source_addr = arg->PrimaryAddress;
    d.source_alloc = arg->hAllocation;
    d.source_sets++;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwdSetVidPnSourceVisibility_impl(const HANDLE hAdapter, const DXGKARG_SETVIDPNSOURCEVISIBILITY *arg)
{
    tr("HwdSetVidPnS");
    (void)hAdapter;
    d.visible = arg->Visible;
    return STATUS_SUCCESS;
}

/* Where the beam would be: sawtooth at the committed refresh rate (d.cur_hz). */
static NTSTATUS NTAPI HwdGetScanLine(const HANDLE hAdapter, DXGKARG_GETSCANLINE *arg)
{
    tr("HwdGetScanLi");
    (void)hAdapter;
    LARGE_INTEGER freq, now;
    now = KeQueryPerformanceCounter(&freq);
    UINT h = d.cur_h ? d.cur_h : d.native_h;
    UINT hz = d.cur_hz ? d.cur_hz : 60u;
    UINT total = h + h / 20 + 1;
    ULONGLONG ticks_per_frame = (freq.QuadPart > 0) ? ((ULONGLONG)freq.QuadPart / hz) : 166667ULL;
    if (!ticks_per_frame) ticks_per_frame = 1;
    UINT line = (UINT)(((ULONGLONG)now.QuadPart % ticks_per_frame) * total / ticks_per_frame);
    arg->InVerticalBlank = line >= h;
    arg->ScanLine = line >= h ? 0 : line;
    return STATUS_SUCCESS;
}

/* The pointer goes to the phone (hwgpu_ring.h, struct hwgpu_pointer), which shows it on a layer of its
 * own: Windows then leaves it out of every frame. Called at any IRQL up to DISPATCH: no paging here. */
static void hwd_pointer_publish(void)
{
    struct hwgpu_pointer *p = d.pointer;
    p->screen_w = d.cur_w ? d.cur_w : d.native_w;
    p->screen_h = d.cur_h ? d.cur_h : d.native_h;
    p->magic = HWGPU_POINTER_MAGIC;
    KeMemoryBarrier();
    InterlockedIncrement((volatile LONG *)&p->seq);
}

static NTSTATUS NTAPI HwdSetPointerPosition(const HANDLE hAdapter, const DXGKARG_SETPOINTERPOSITION *arg)
{
    tr("HwdSetPointe");
    (void)hAdapter;
    struct hwgpu_pointer *p = d.pointer;
    if (!p) return STATUS_SUCCESS;
    p->x = arg->X;
    p->y = arg->Y;
    p->visible = arg->Flags.Visible ? 1u : 0u;
    hwd_pointer_publish();
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwdSetPointerShape(const HANDLE hAdapter, const DXGKARG_SETPOINTERSHAPE *arg)
{
    tr("HwdSetPointe");
    (void)hAdapter;
    struct hwgpu_pointer *p = d.pointer;
    if (!p || !arg->pPixels) return STATUS_NOT_SUPPORTED;
    const UCHAR *src = (const UCHAR *)arg->pPixels;
    UINT w = arg->Width, pitch = arg->Pitch;
    if (arg->Flags.Monochrome) {
        /* An AND mask over an XOR mask, one bit a pixel: Height counts both. */
        UINT h = arg->Height / 2;
        if (!w || !h || w > HWGPU_POINTER_MAX || h > HWGPU_POINTER_MAX) return STATUS_NOT_SUPPORTED;
        for (UINT y = 0; y < h; y++) {
            for (UINT x = 0; x < w; x++) {
                UINT and_bit = (src[y * pitch + x / 8] >> (7 - x % 8)) & 1;
                UINT xor_bit = (src[(y + h) * pitch + x / 8] >> (7 - x % 8)) & 1;
                /* Transparent, black, white; inverting pixels have no blend here: black. */
                p->rgba[y * w + x] = and_bit && !xor_bit ? 0u : (!and_bit && xor_bit ? 0xffffffffu : 0xff000000u);
            }
        }
        p->width = w;
        p->height = h;
    } else if (arg->Flags.Color || arg->Flags.MaskedColor) {
        UINT h = arg->Height;
        if (!w || !h || w > HWGPU_POINTER_MAX || h > HWGPU_POINTER_MAX) return STATUS_NOT_SUPPORTED;
        for (UINT y = 0; y < h; y++) {
            const ULONG *row = (const ULONG *)(src + y * pitch);
            for (UINT x = 0; x < w; x++) {
                ULONG c = row[x];  /* B, G, R, A in memory */
                UINT a = c >> 24, r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
                if (arg->Flags.MaskedColor) a = a ? 0 : 0xff;  /* 0: shown, 0xFF: inverts, left out */
                else { r = r * a / 255; g = g * a / 255; b = b * a / 255; }
                p->rgba[y * w + x] = r | (g << 8) | (b << 16) | ((ULONG)a << 24);
            }
        }
        p->width = w;
        p->height = h;
    } else {
        return STATUS_NOT_SUPPORTED;
    }
    p->hot_x = (LONG)arg->XHot;
    p->hot_y = (LONG)arg->YHot;
    KeMemoryBarrier();
    InterlockedIncrement((volatile LONG *)&p->shape_seq);
    hwd_pointer_publish();
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwdIsSupportedVidPn(const HANDLE hAdapter, DXGKARG_ISSUPPORTEDVIDPN *arg)
{
    tr("HwdIsSupport");
    HWD_STEP(1000 + __COUNTER__);
    NTSTATUS ns = HwdIsSupportedVidPn_impl(hAdapter, arg);
    hwd_log("IsSupportedVidPn", ns);
    return ns;
}

static NTSTATUS NTAPI HwdRecommendFunctionalVidPn(const HANDLE hAdapter, const DXGKARG_RECOMMENDFUNCTIONALVIDPN *arg)
{
    tr("HwdRecommend");
    HWD_STEP(1000 + __COUNTER__);
    NTSTATUS ns = HwdRecommendFunctionalVidPn_impl(hAdapter, arg);
    hwd_log("RecommendFunctionalVidPn", ns);
    return ns;
}

static NTSTATUS NTAPI HwdEnumVidPnCofuncModality(const HANDLE hAdapter, const DXGKARG_ENUMVIDPNCOFUNCMODALITY *arg)
{
    tr("HwdEnumVidPn");
    HWD_STEP(1000 + __COUNTER__);
    NTSTATUS ns = HwdEnumVidPnCofuncModality_impl(hAdapter, arg);
    hwd_log("EnumVidPnCofuncModality", ns);
    return ns;
}

static NTSTATUS NTAPI HwdRecommendMonitorModes(const HANDLE hAdapter, const DXGKARG_RECOMMENDMONITORMODES *arg)
{
    tr("HwdRecommend");
    HWD_STEP(1000 + __COUNTER__);
    NTSTATUS ns = HwdRecommendMonitorModes_impl(hAdapter, arg);
    hwd_log("RecommendMonitorModes", ns);
    return ns;
}

static NTSTATUS NTAPI HwdCommitVidPn(const HANDLE hAdapter, const DXGKARG_COMMITVIDPN *arg)
{
    tr("HwdCommitVid");
    HWD_STEP(1000 + __COUNTER__);
    NTSTATUS ns = HwdCommitVidPn_impl(hAdapter, arg);
    hwd_log("CommitVidPn", ns);
    return ns;
}

static NTSTATUS NTAPI HwdSetVidPnSourceAddress(const HANDLE hAdapter, const DXGKARG_SETVIDPNSOURCEADDRESS *arg)
{
    /* Runs at DIRQL when FlipOnVSyncMmIo=1: no pageable/registry calls here. */
    return HwdSetVidPnSourceAddress_impl(hAdapter, arg);
}

static NTSTATUS NTAPI HwdSetVidPnSourceVisibility(const HANDLE hAdapter, const DXGKARG_SETVIDPNSOURCEVISIBILITY *arg)
{
    tr("HwdSetVidPnS");
    HWD_STEP(1000 + __COUNTER__);
    NTSTATUS ns = HwdSetVidPnSourceVisibility_impl(hAdapter, arg);
    hwd_log("SetVidPnSourceVisibility", ns);
    return ns;
}

static NTSTATUS NTAPI HwdUpdateActiveVidPnPresentPath(const HANDLE hAdapter, const DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *arg)
{
    tr("HwdUpdateAct");
    HWD_STEP(1000 + __COUNTER__);
    NTSTATUS ns = HwdUpdateActiveVidPnPresentPath_impl(hAdapter, arg);
    hwd_log("UpdateActiveVidPnPresentPath", ns);
    return ns;
}

/* ---- surfaces ----
 * With the screen half on, Windows makes allocations here: its own (the desktop's primary, shadow and
 * staging surfaces) and the user-mode driver's flippable ones. Each is a small record; the pixels of a
 * program's surface live on the phone. Windows' own get system memory through the aperture segment, so
 * what the CPU draws into them before DWM runs has somewhere to go. */
struct hwd_alloc {
    struct hww_alloc a;
    D3DDDI_RATIONAL refresh;
    PMDL mdl;                 /* a window bitmap: the pages it is mapped with (MapApertureSegment), until unmapped */
    ULONG mdl_page, map_page, map_pages;
};
static volatile LONG64 hwd_next_id;

static NTSTATUS NTAPI HwdGetStandardData_impl(const HANDLE hAdapter, DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *arg)
{
    tr("HwdGetStanda");
    (void)hAdapter;
    if (g_trace_reg) {   /* what Windows asks of a driver for its own surfaces: counts per type, for the DWM investigation */
        static volatile LONG asked[16];
        UINT t = (UINT)arg->StandardAllocationType;
        if (t < 16) report(L"D_StdAsked", (ULONG)InterlockedIncrement(&asked[t]) | (t << 24));
        else report(L"D_StdAsked", t);
    }
    arg->ResourcePrivateDriverDataSize = 0;
    if (!arg->pAllocationPrivateDriverData) {
        arg->AllocationPrivateDriverDataSize = sizeof(struct hww_alloc);
        return STATUS_SUCCESS;
    }
    if (arg->AllocationPrivateDriverDataSize < sizeof(struct hww_alloc)) return STATUS_BUFFER_TOO_SMALL;
    struct hww_alloc *a = arg->pAllocationPrivateDriverData;
    RtlZeroMemory(a, sizeof *a);
    a->magic = HWW_ALLOC_MAGIC;
    a->id = (uint64_t)InterlockedIncrement64(&hwd_next_id) | (1ull << 63);
    switch (arg->StandardAllocationType) {
    case D3DKMDT_STANDARDALLOCATION_SHAREDPRIMARYSURFACE:
        a->kind = HWW_ALLOC_PRIMARY;
        a->width = arg->pCreateSharedPrimarySurfaceData->Width;
        a->height = arg->pCreateSharedPrimarySurfaceData->Height;
        a->format = arg->pCreateSharedPrimarySurfaceData->Format;
        a->source = arg->pCreateSharedPrimarySurfaceData->VidPnSourceId;
        return STATUS_SUCCESS;
    case D3DKMDT_STANDARDALLOCATION_SHADOWSURFACE:
        a->kind = HWW_ALLOC_SHADOW;
        a->width = arg->pCreateShadowSurfaceData->Width;
        a->height = arg->pCreateShadowSurfaceData->Height;
        a->format = arg->pCreateShadowSurfaceData->Format;
        arg->pCreateShadowSurfaceData->Pitch = a->width * 4;
        return STATUS_SUCCESS;
    case D3DKMDT_STANDARDALLOCATION_GDISURFACE:   /* DWM's redirection surfaces: shared RAM, CPU visible */
    hwd_trace(1, (ULONG)arg->pCreateGdiSurfaceData->Type, (ULONG)arg->pCreateGdiSurfaceData->Flags.Value);
    hwd_trace(2, arg->pCreateGdiSurfaceData->Width, arg->pCreateGdiSurfaceData->Height);
        a->kind = HWW_ALLOC_SHADOW;
        a->width = arg->pCreateGdiSurfaceData->Width;
        a->height = arg->pCreateGdiSurfaceData->Height;
        a->format = arg->pCreateGdiSurfaceData->Format;
        arg->pCreateGdiSurfaceData->Pitch = a->width * 4;
        return STATUS_SUCCESS;
    case D3DKMDT_STANDARDALLOCATION_STAGINGSURFACE:
        a->kind = HWW_ALLOC_STAGING;
        a->width = arg->pCreateStagingSurfaceData->Width;
        a->height = arg->pCreateStagingSurfaceData->Height;
        a->format = D3DDDIFMT_A8R8G8B8;
        arg->pCreateStagingSurfaceData->Pitch = a->width * 4;
        return STATUS_SUCCESS;
    default:
        report(L"D_StdTypeRefused", (ULONG)arg->StandardAllocationType);
        return STATUS_NOT_SUPPORTED;
    }
}

static NTSTATUS NTAPI HwdCreateAllocation_impl(const HANDLE hAdapter, DXGKARG_CREATEALLOCATION *ca)
{
    tr("HwdCreateAll");
    (void)hAdapter;
    for (UINT i = 0; i < ca->NumAllocations; i++) {
        DXGK_ALLOCATIONINFO *ai = &ca->pAllocationInfo[i];
        const struct hww_alloc *in = ai->pPrivateDriverData;
        if (!in || ai->PrivateDriverDataSize < sizeof *in || in->magic != HWW_ALLOC_MAGIC || !in->width || !in->height ||
            in->width > 16384 || in->height > 16384)
            return STATUS_INVALID_PARAMETER;
        struct hwd_alloc *al = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof *al, 'lAwH');
        if (!al) return STATUS_NO_MEMORY;
        RtlZeroMemory(al, sizeof *al);
        al->a = *in;
        al->refresh.Numerator = (d.cur_hz ? d.cur_hz : 60u) * 1000u;
        al->refresh.Denominator = HWD_REFRESH_D;
        ai->Alignment = 0;
        /* A surface the Direct3D driver made lives on the phone: Windows only needs a token of memory for it.
         * Windows' own surfaces (primary, shadow, staging) are drawn by the CPU and need their pixels. */
        ai->Size = in->kind == HWW_ALLOC_UMD ? 0x10000
                                             : (((SIZE_T)in->width * in->height * 4 + 0xfff) & ~(SIZE_T)0xfff);
        ai->PitchAlignedSize = 0;
        ai->HintedBank.Value = 0;
        ai->PreferredSegment.Value = 0;
        ai->SupportedReadSegmentSet = 1;
        ai->SupportedWriteSegmentSet = 1;
        ai->EvictionSegmentSet = 0;
        ai->MaximumRenamingListLength = 0;
        ai->hAllocation = al;
        if (in->kind != HWW_ALLOC_UMD) hwd_trace(3, in->kind, ai->SupportedReadSegmentSet);
        ai->Flags.Value = 0;
        ai->Flags.CpuVisible = 1;
        if (in->kind == HWW_ALLOC_SHADOW) ai->Flags.Cached = 1;
        ai->pAllocationUsageHint = NULL;
        ai->AllocationPriority = D3DDDI_ALLOCATIONPRIORITY_NORMAL;
    }
    if (ca->Flags.Resource && !ca->hResource) ca->hResource = (HANDLE)(ULONG_PTR)1;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwdDestroyAllocation_impl(const HANDLE hAdapter, const DXGKARG_DESTROYALLOCATION *da)
{
    tr("HwdDestroyAl");
    (void)hAdapter;
    for (UINT i = 0; i < da->NumAllocations; i++) {
        if (da->pAllocationList[i] == d.source_alloc) d.source_alloc = NULL;
        if (da->pAllocationList[i]) {
            struct hwd_alloc *al = (struct hwd_alloc *)da->pAllocationList[i];
            if (al->a.kind != HWW_ALLOC_UMD) {
                KIRQL irql;
                KeAcquireSpinLock(&g_mdl_lock, &irql);
                al->mdl = NULL;
                al->map_pages = 0;
                KeReleaseSpinLock(&g_mdl_lock, irql);
                hwd_trace(4, al->a.kind, 0);
            }
            ExFreePoolWithTag(da->pAllocationList[i], 'lAwH');
        }
    }
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwdDescribeAllocation_impl(const HANDLE hAdapter, DXGKARG_DESCRIBEALLOCATION *da)
{
    tr("HwdDescribeA");
    (void)hAdapter;
    const struct hwd_alloc *al = da->hAllocation;
    if (!al) return STATUS_INVALID_PARAMETER;
    da->Width = al->a.width;
    da->Height = al->a.height;
    da->Format = al->a.kind == HWW_ALLOC_UMD ? D3DDDIFMT_A8R8G8B8 : (D3DDDIFORMAT)al->a.format;
    da->MultisampleMethod.NumSamples = 1;
    da->MultisampleMethod.NumQualityLevels = 0;
    da->RefreshRate = al->refresh;
    da->PrivateDriverFormatAttribute = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwdOpenAllocation_impl(const HANDLE hDevice, const DXGKARG_OPENALLOCATION *oa)
{
    tr("HwdOpenAlloc");
    (void)hDevice;
    for (UINT i = 0; i < oa->NumAllocations; i++) {
        DXGKARGCB_GETHANDLEDATA gh;
        RtlZeroMemory(&gh, sizeof gh);
        gh.hObject = oa->pOpenAllocation[i].hAllocation;
        gh.Type = DXGK_HANDLE_ALLOCATION;
        PVOID al = g.dxgk.DxgkCbGetHandleData(&gh);
        if (!al) return STATUS_INVALID_HANDLE;
        oa->pOpenAllocation[i].hDeviceSpecificAllocation = al;
        hwd_trace(6, ((struct hwd_alloc *)al)->a.kind, 0);
    }
    return STATUS_SUCCESS;
}

/* A present asks for a DMA buffer that copies or flips. The copy or flip itself is the phone's: the
 * user-mode driver tells its renderer. Here the buffer only has to exist and move through the queue. */
static NTSTATUS NTAPI HwdPresent_impl(const HANDLE hContext, DXGKARG_PRESENT *pr)
{
    tr("HwdPresent_i");
    (void)hContext;
    /* A flip comes with no room in its buffer, and needs none: SetVidPnSourceAddress does the work. */
    if (pr->DmaSize >= 16) {
        RtlZeroMemory(pr->pDmaBuffer, 16);
        pr->pDmaBuffer = (UCHAR *)pr->pDmaBuffer + 16;
    }
    if (pr->pDmaBufferPrivateData && pr->DmaBufferPrivateDataSize >= sizeof(struct hww_marker)) {
        RtlZeroMemory(pr->pDmaBufferPrivateData, sizeof(struct hww_marker));
        pr->pDmaBufferPrivateData = (UCHAR *)pr->pDmaBufferPrivateData + sizeof(struct hww_marker);
    }
    d.presents++;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwdQueryDeviceDescriptor(CONST PVOID ctx, ULONG uid, PDXGK_DEVICE_DESCRIPTOR desc)
{
    tr("HwdQueryDevi");
    HWD_STEP(1000 + __COUNTER__);
    NTSTATUS ns = HwdQueryDeviceDescriptor_impl(ctx, uid, desc);
    hwd_log("QueryDeviceDescriptor", ns);
    return ns;
}

static NTSTATUS NTAPI HwdGetStandardData(const HANDLE hAdapter, DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *arg)
{
    tr("HwdGetStanda");
    HWD_STEP(1000 + __COUNTER__);
    NTSTATUS ns = HwdGetStandardData_impl(hAdapter, arg);
    hwd_log("GetStandardData", ns);
    return ns;
}

static NTSTATUS NTAPI HwdCreateAllocation(const HANDLE hAdapter, DXGKARG_CREATEALLOCATION *ca)
{
    tr("HwdCreateAll");
    HWD_STEP(1000 + __COUNTER__);
    NTSTATUS ns = HwdCreateAllocation_impl(hAdapter, ca);
    hwd_log("CreateAllocation", ns);
    return ns;
}

static NTSTATUS NTAPI HwdDestroyAllocation(const HANDLE hAdapter, const DXGKARG_DESTROYALLOCATION *da)
{
    tr("HwdDestroyAl");
    HWD_STEP(1000 + __COUNTER__);
    NTSTATUS ns = HwdDestroyAllocation_impl(hAdapter, da);
    hwd_log("DestroyAllocation", ns);
    return ns;
}

static NTSTATUS NTAPI HwdDescribeAllocation(const HANDLE hAdapter, DXGKARG_DESCRIBEALLOCATION *da)
{
    tr("HwdDescribeA");
    HWD_STEP(1000 + __COUNTER__);
    NTSTATUS ns = HwdDescribeAllocation_impl(hAdapter, da);
    hwd_log("DescribeAllocation", ns);
    return ns;
}

static NTSTATUS NTAPI HwdOpenAllocation(const HANDLE hDevice, const DXGKARG_OPENALLOCATION *oa)
{
    tr("HwdOpenAlloc");
    HWD_STEP(1000 + __COUNTER__);
    NTSTATUS ns = HwdOpenAllocation_impl(hDevice, oa);
    hwd_log("OpenAllocation", ns);
    return ns;
}

static NTSTATUS NTAPI HwdPresent(const HANDLE hContext, DXGKARG_PRESENT *pr)
{
    tr("HwdPresent");
    HWD_STEP(1000 + __COUNTER__);
    NTSTATUS ns = HwdPresent_impl(hContext, pr);
    hwd_log("Present", ns);
    return ns;
}

/* ---- vsync ----
 * A flip is finished when the screen reports a vsync with the new address. Phase-locked to the phone's
 * AChoreographer timeline (struct hwgpu_vsync) via a one-shot high-resolution timer re-armed on each tick. */
static PEX_TIMER hwd_vsync_timer;
static volatile LONG hwd_vsync_on;

static void hwd_calibrate_vsync(void)
{
    volatile struct hwgpu_vsync *v = d.vsync;
    if (!v || v->magic != HWGPU_VSYNC_MAGIC || !v->freq) return;
    v->calibrate = 1;
    LARGE_INTEGER slp;
    slp.QuadPart = -200000LL;  /* 20 ms for host thread to enter calibrate spin */
    KeDelayExecutionThread(KernelMode, FALSE, &slp);
    int64_t best_off = 0;
    uint64_t best_rtt = ~0ull;
    for (int i = 0; i < 50; i++) {
        uint32_t ask = v->ask + 1;
        uint64_t g1 = hwd_cntvct();
        v->guest_t = g1;
        KeMemoryBarrier();
        v->ask = ask;
        uint64_t limit = g1 + v->freq / 100;
        while (v->answer != ask && hwd_cntvct() < limit) {}
        uint64_t g2 = hwd_cntvct();
        if (v->answer != ask) continue;
        if (g2 - g1 < best_rtt) {
            best_rtt = g2 - g1;
            best_off = (int64_t)(v->host_t - (g1 + g2) / 2);
        }
    }
    v->calibrate = 0;
    if (best_rtt != ~0ull) {
        d.vsync_off = best_off;
        KeMemoryBarrier();
        d.vsync_calibrated = TRUE;
        report(L"T_VsyncCalUs", (ULONG)(best_rtt * 1000000ull / v->freq));
        report(L"T_VsyncPhoneHz", v->hz_milli);
    }
}

static LONGLONG hwd_next_vsync_100ns(void)
{
    ULONG fallback = d.cur_period_100ns ? d.cur_period_100ns : 166667u;
    volatile struct hwgpu_vsync *v = d.vsync;
    if (!d.vsync_calibrated || !v || v->magic != HWGPU_VSYNC_MAGIC) return (LONGLONG)fallback;
    uint64_t last = 0, period = 0;
    uint32_t freq = 0;
    for (int tries = 0; tries < 4; tries++) {
        uint32_t s1 = v->seq;
        if (s1 & 1) continue;
        KeMemoryBarrier();
        last = v->last;
        period = v->period;
        freq = v->freq;
        KeMemoryBarrier();
        if (v->seq == s1) break;
        freq = 0;
    }
    if (!freq || !period || !last) return (LONGLONG)fallback;
    UINT hz = d.cur_hz ? d.cur_hz : 60u;
    uint64_t want_period = (uint64_t)freq / hz;
    /* Use the exact measured phone period when it matches the committed mode; otherwise step by the committed mode aligned to last. */
    uint64_t diff = (want_period > period) ? (want_period - period) : (period - want_period);
    uint64_t step = (diff * 4 < period) ? period : want_period;
    if (!step) return (LONGLONG)fallback;
    uint64_t now_host = (uint64_t)((int64_t)hwd_cntvct() + d.vsync_off);
    uint64_t next_host;
    if (now_host >= last) {
        uint64_t k = (now_host - last) / step + 1;
        next_host = last + k * step;
    } else {
        next_host = last;
    }
    uint64_t delta_ticks = next_host - now_host;
    LONGLONG due = (LONGLONG)(delta_ticks * 10000000ull / freq);
    LONGLONG min_due = 10000LL; /* 1 ms floor */
    LONGLONG max_due = (LONGLONG)fallback * 2LL;
    if (due < min_due) due = min_due;
    if (due > max_due) due = max_due;
    return due;
}

static BOOLEAN NTAPI hwd_vsync_sync(PVOID context)
{
    (void)context;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA n;
    RtlZeroMemory(&n, sizeof n);
    n.InterruptType = DXGK_INTERRUPT_CRTC_VSYNC;
    n.CrtcVsync.VidPnTargetId = 0;
    n.CrtcVsync.PhysicalAddress = d.source_addr;
    g.dxgk.DxgkCbNotifyInterrupt(g.dxgk.DeviceHandle, &n);
    g.dxgk.DxgkCbQueueDpc(g.dxgk.DeviceHandle);
    return TRUE;
}

static VOID NTAPI hwd_vsync_tick(PEX_TIMER timer, PVOID context)
{
    (void)context;
    if (!hwd_vsync_on) return;
    run_synced(hwd_vsync_sync, NULL);
    d.vsyncs++;
    if (hwd_vsync_on) ExSetTimer(timer, -hwd_next_vsync_100ns(), 0, NULL);
}

static NTSTATUS hwd_control_interrupt(DXGK_INTERRUPT_TYPE type, BOOLEAN enable)
{
    if (!d.on || type != DXGK_INTERRUPT_CRTC_VSYNC) return STATUS_SUCCESS;
    if (!hwd_vsync_timer) {
        hwd_vsync_timer = ExAllocateTimer(hwd_vsync_tick, NULL, EX_TIMER_HIGH_RESOLUTION);
        if (!hwd_vsync_timer) return STATUS_NO_MEMORY;
    }
    LONG prev = InterlockedExchange(&hwd_vsync_on, enable ? 1 : 0);
    if (enable && !prev) {
        ExSetTimer(hwd_vsync_timer, -hwd_next_vsync_100ns(), 0, NULL);
    } else if (!enable && prev) {
        ExCancelTimer(hwd_vsync_timer, NULL);
    }
    return STATUS_SUCCESS;
}

static void hwd_stop(void)
{
    InterlockedExchange(&hwd_vsync_on, 0);
    if (hwd_vsync_timer) {
        ExDeleteTimer(hwd_vsync_timer, TRUE, TRUE, NULL);
        hwd_vsync_timer = NULL;
    }
}

/* Called from DriverEntry: read the switch, and fill the display entries only when it is on. */
/* The screen's size is the one chosen in the app (the window header's want_*), as the indirect display
 * had it; DisplayWidth/DisplayHeight in Parameters are only the fallback. */
static void hwd_size_from_phone(void)
{
    if (!d.pointer) {
        PHYSICAL_ADDRESS pp;
        pp.QuadPart = (LONGLONG)(HW_WINDOW_PA + HWGPU_POINTER_OFF);
        d.pointer = MmMapIoSpace(pp, sizeof(struct hwgpu_pointer), MmCached);
        if (d.pointer) {
            d.pointer->magic = 0;  /* until Windows hands over the pointer */
            d.pointer->visible = 0;
        }
    }
    if (!d.vsync) {
        PHYSICAL_ADDRESS vp;
        vp.QuadPart = (LONGLONG)(HW_WINDOW_PA + HWGPU_VSYNC_OFF);
        d.vsync = MmMapIoSpace(vp, sizeof(struct hwgpu_vsync), MmCached);
    }
    UINT hz = hwd_phone_hz();
    d.cur_hz = hz;
    d.cur_period_100ns = 10000000u / hz;
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = (LONGLONG)HW_WINDOW_PA;
    const struct hw_header *h = MmMapIoSpace(pa, PAGE_SIZE, MmCached);
    if (!h) return;
    if (RtlCompareMemory(h->magic, HW_MAGIC, 8) == 8 && h->screen_want_width >= 640 && h->screen_want_width <= 7680 &&
        h->screen_want_height >= 480 && h->screen_want_height <= 4320) {
        d.native_w = d.cur_w = h->screen_want_width & ~1u;
        d.native_h = d.cur_h = h->screen_want_height & ~1u;
    }
    MmUnmapIoSpace((PVOID)h, PAGE_SIZE);
}

static void hwd_setup(DRIVER_INITIALIZATION_DATA *init)
{
    /* Allocations whatever the switch: shared surfaces (DWM's, for the screen driver) need them. */
    init->DxgkDdiCreateAllocation = HwdCreateAllocation;
    init->DxgkDdiDestroyAllocation = HwdDestroyAllocation;
    init->DxgkDdiDescribeAllocation = HwdDescribeAllocation;
    init->DxgkDdiOpenAllocation = HwdOpenAllocation;
    init->DxgkDdiPresent = HwdPresent;   /* a shared surface's present: a fenced no-op, the phone shows it */
    /* Windows asks for its own kinds of surfaces (GDI surfaces for DWM's window bitmaps) through this one even
     * without a screen; without it a render-only adapter cannot hold them. */
    init->DxgkDdiGetStandardAllocationDriverData = HwdGetStandardData;
    g_trace_reg = hwd_reg_dword(L"TraceReg", 0) != 0;
    d.on = hwd_reg_dword(L"Display", 0) != 0;
    if (!d.on) return;
    d.native_w = hwd_reg_dword(L"DisplayWidth", 1280);
    d.native_h = hwd_reg_dword(L"DisplayHeight", 720);
    if (d.native_w < 640 || d.native_w > 7680) d.native_w = 1280;
    if (d.native_h < 480 || d.native_h > 4320) d.native_h = 720;
    d.cur_w = d.native_w;
    d.cur_h = d.native_h;
    init->DxgkDdiQueryChildRelations = HwdQueryChildRelations;
    init->DxgkDdiQueryChildStatus = HwdQueryChildStatus;
    init->DxgkDdiQueryDeviceDescriptor = HwdQueryDeviceDescriptor;
    init->DxgkDdiIsSupportedVidPn = HwdIsSupportedVidPn;
    init->DxgkDdiRecommendFunctionalVidPn = HwdRecommendFunctionalVidPn;
    init->DxgkDdiEnumVidPnCofuncModality = HwdEnumVidPnCofuncModality;
    init->DxgkDdiSetVidPnSourceAddress = HwdSetVidPnSourceAddress;
    init->DxgkDdiSetVidPnSourceVisibility = HwdSetVidPnSourceVisibility;
    init->DxgkDdiCommitVidPn = HwdCommitVidPn;
    init->DxgkDdiUpdateActiveVidPnPresentPath = HwdUpdateActiveVidPnPresentPath;
    init->DxgkDdiRecommendMonitorModes = HwdRecommendMonitorModes;
    init->DxgkDdiRecommendVidPnTopology = HwdRecommendVidPnTopology;
    init->DxgkDdiGetScanLine = HwdGetScanLine;
    init->DxgkDdiSetPointerPosition = HwdSetPointerPosition;
    init->DxgkDdiSetPointerShape = HwdSetPointerShape;
    init->DxgkDdiGetStandardAllocationDriverData = HwdGetStandardData;
}
