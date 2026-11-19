/* The display half of the adapter (wddm/README.md, phase 6): one screen, one source, one target.
 * Built into hwgpuw.c (it includes this file) and switched on by Parameters\Display = 1, so the
 * driver stays render-only until the screen path is proven.
 *
 * What it does for now: tells Windows there is one monitor, offers a list of modes, and takes
 * what Windows decides (commit, source address, visibility). Where a frame goes is the next step. */

#define HWD_REFRESH_N 60000u
#define HWD_REFRESH_D 1000u

struct hwd_mode { UINT w, h; };

static struct {
    BOOLEAN on;
    UINT native_w, native_h;       /* the largest mode, from Parameters\DisplayWidth/DisplayHeight */
    UINT cur_w, cur_h;             /* what the last commit chose */
    BOOLEAN visible;
    PHYSICAL_ADDRESS source_addr;
    HANDLE source_alloc;
    UINT commits, source_sets, presents, vsyncs;
} d;

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

/* Each display entry's last answer goes into Parameters as D_<name>, so a run can be read afterwards. */
static void report(const WCHAR *what, ULONG value);
#define HWD_STEP(n) report(L"D_Step", (ULONG)(n))
static void hwd_log(const char *name, NTSTATUS ns)
{
    WCHAR w[48] = L"D_";
    UINT i = 2;
    for (const char *c = name; *c && i < 46; c++) w[i++] = (WCHAR)(UCHAR)*c;
    w[i] = 0;
    report(w, (ULONG)ns);
}

/* ---- the monitor ---- */

static NTSTATUS NTAPI HwdQueryChildRelations(CONST PVOID ctx, PDXGK_CHILD_DESCRIPTOR rel, ULONG size)
{
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
    (void)ctx;
    (void)uid;
    (void)desc;
    return STATUS_MONITOR_NO_DESCRIPTOR;
}

/* ---- mode sets ---- */

static void hwd_signal(D3DKMDT_VIDEO_SIGNAL_INFO *s, UINT w, UINT h)
{
    RtlZeroMemory(s, sizeof *s);
    s->VideoStandard = D3DKMDT_VSS_OTHER;
    s->TotalSize.cx = w;
    s->TotalSize.cy = h;
    s->ActiveSize.cx = w;
    s->ActiveSize.cy = h;
    s->VSyncFreq.Numerator = HWD_REFRESH_N;
    s->VSyncFreq.Denominator = HWD_REFRESH_D;
    s->HSyncFreq.Numerator = (UINT)((UINT64)HWD_REFRESH_N * h / HWD_REFRESH_D);
    s->HSyncFreq.Denominator = 1;
    s->PixelRate = (SIZE_T)w * h * 60;
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

static NTSTATUS hwd_build_target_set(const DXGK_VIDPN_INTERFACE *vi, D3DKMDT_HVIDPN hv, UINT only_w, UINT only_h)
{
    D3DKMDT_HVIDPNTARGETMODESET hset;
    const DXGK_VIDPNTARGETMODESET_INTERFACE *ti;
    NTSTATUS ns = vi->pfnCreateNewTargetModeSet(hv, 0, &hset, &ti);
    HWD_STEP(20);
    if (!NT_SUCCESS(ns)) return ns;
    struct hwd_mode list[8];
    UINT n = hwd_mode_list(list, 8);
    for (UINT i = 0; i < n; i++) {
        if (only_w && (list[i].w != only_w || list[i].h != only_h)) continue;
        D3DKMDT_VIDPN_TARGET_MODE *m;
        ns = ti->pfnCreateNewModeInfo(hset, &m);
    HWD_STEP(21);
        if (!NT_SUCCESS(ns)) break;
        hwd_signal(&m->VideoSignalInfo, list[i].w, list[i].h);
        m->Preference = i == 0 ? D3DKMDT_MP_PREFERRED : D3DKMDT_MP_NOTPREFERRED;
        ns = ti->pfnAddMode(hset, m);
    HWD_STEP(22);
        if (!NT_SUCCESS(ns)) { ti->pfnReleaseModeInfo(hset, m); break; }
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
    return hwd_build_target_set(vi, hv, d.native_w, d.native_h);
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

static BOOLEAN hwd_pinned_target(const DXGK_VIDPN_INTERFACE *vi, D3DKMDT_HVIDPN hv, UINT *w, UINT *h)
{
    D3DKMDT_HVIDPNTARGETMODESET hset;
    const DXGK_VIDPNTARGETMODESET_INTERFACE *ti;
    if (!NT_SUCCESS(vi->pfnAcquireTargetModeSet(hv, 0, &hset, &ti))) return FALSE;
    const D3DKMDT_VIDPN_TARGET_MODE *m = NULL;
    BOOLEAN got = FALSE;
    if (NT_SUCCESS(ti->pfnAcquirePinnedModeInfo(hset, &m)) && m) {
        *w = m->VideoSignalInfo.ActiveSize.cx;
        *h = m->VideoSignalInfo.ActiveSize.cy;
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
    (void)hAdapter;
    const DXGK_VIDPN_INTERFACE *vi;
    D3DKMDT_HVIDPN hv = arg->hConstrainingVidPn;
    NTSTATUS ns = g.dxgk.DxgkCbQueryVidPnInterface(hv, DXGK_VIDPN_INTERFACE_VERSION_V1, &vi);
    if (!NT_SUCCESS(ns)) return ns;
    UINT sw = 0, sh = 0, tw = 0, th = 0;
    BOOLEAN sp = hwd_pinned_source(vi, hv, &sw, &sh);
    BOOLEAN tp = hwd_pinned_target(vi, hv, &tw, &th);
    UINT w = 0, h = 0;
    if (sp) { w = sw; h = sh; } else if (tp) { w = tw; h = th; }
    /* A side with a pinned mode is Windows' decision and stays as it is. Pinning is never ours to do:
     * a pin from here made dxgkrnl stop answering. */
    if (!sp) {
        ns = hwd_build_source_set(vi, hv, w, h);
        if (!NT_SUCCESS(ns)) return ns;
    }
    if (!tp) {
        ns = hwd_build_target_set(vi, hv, w, h);
        if (!NT_SUCCESS(ns)) return ns;
    }
    /* The path's scaling and rotation are only ever identity, so nothing there to widen. */
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwdRecommendMonitorModes_impl(const HANDLE hAdapter, const DXGKARG_RECOMMENDMONITORMODES *arg)
{
    (void)hAdapter;
    struct hwd_mode list[8];
    UINT n = hwd_mode_list(list, 8);
    for (UINT i = 0; i < n; i++) {
        D3DKMDT_MONITOR_SOURCE_MODE *m;
        NTSTATUS ns = arg->pMonitorSourceModeSetInterface->pfnCreateNewModeInfo(arg->hMonitorSourceModeSet, &m);
        if (!NT_SUCCESS(ns)) return ns;
        hwd_signal(&m->VideoSignalInfo, list[i].w, list[i].h);
        m->ColorBasis = D3DKMDT_CB_SRGB;
        m->ColorCoeffDynamicRanges.FirstChannel = 8;
        m->ColorCoeffDynamicRanges.SecondChannel = 8;
        m->ColorCoeffDynamicRanges.ThirdChannel = 8;
        m->ColorCoeffDynamicRanges.FourthChannel = 0;
        m->Origin = D3DKMDT_MCO_DRIVER;
        m->Preference = i == 0 ? D3DKMDT_MP_PREFERRED : D3DKMDT_MP_NOTPREFERRED;
        ns = arg->pMonitorSourceModeSetInterface->pfnAddMode(arg->hMonitorSourceModeSet, m);
        if (!NT_SUCCESS(ns)) {
            arg->pMonitorSourceModeSetInterface->pfnReleaseModeInfo(arg->hMonitorSourceModeSet, m);
            return ns;
        }
    }
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwdRecommendVidPnTopology(const HANDLE hAdapter, const DXGKARG_RECOMMENDVIDPNTOPOLOGY *arg)
{
    HWD_STEP(905);
    (void)hAdapter;
    (void)arg;
    return STATUS_GRAPHICS_NO_RECOMMENDED_VIDPN_TOPOLOGY;
}

static NTSTATUS NTAPI HwdCommitVidPn_impl(const HANDLE hAdapter, const DXGKARG_COMMITVIDPN *arg)
{
    (void)hAdapter;
    const DXGK_VIDPN_INTERFACE *vi;
    NTSTATUS ns = g.dxgk.DxgkCbQueryVidPnInterface(arg->hFunctionalVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vi);
    if (!NT_SUCCESS(ns)) return ns;
    UINT w = 0, h = 0;
    if (hwd_pinned_source(vi, arg->hFunctionalVidPn, &w, &h)) {
        d.cur_w = w;
        d.cur_h = h;
    }
    d.commits++;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwdUpdateActiveVidPnPresentPath_impl(const HANDLE hAdapter, const DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *arg)
{
    (void)hAdapter;
    (void)arg;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwdSetVidPnSourceAddress_impl(const HANDLE hAdapter, const DXGKARG_SETVIDPNSOURCEADDRESS *arg)
{
    (void)hAdapter;
    d.source_addr = arg->PrimaryAddress;
    d.source_alloc = arg->hAllocation;
    d.source_sets++;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwdSetVidPnSourceVisibility_impl(const HANDLE hAdapter, const DXGKARG_SETVIDPNSOURCEVISIBILITY *arg)
{
    (void)hAdapter;
    d.visible = arg->Visible;
    return STATUS_SUCCESS;
}

/* Where the beam would be: a 60 Hz sawtooth from the system clock. */
static NTSTATUS NTAPI HwdGetScanLine(const HANDLE hAdapter, DXGKARG_GETSCANLINE *arg)
{
    HWD_STEP(902);
    (void)hAdapter;
    LARGE_INTEGER now;
    KeQueryPerformanceCounter(&now);
    UINT h = d.cur_h ? d.cur_h : d.native_h;
    UINT total = h + h / 20 + 1;
    UINT line = (UINT)((now.QuadPart / 4000) % total);
    arg->InVerticalBlank = line >= h;
    arg->ScanLine = line >= h ? 0 : line;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwdSetPointerPosition(const HANDLE hAdapter, const DXGKARG_SETPOINTERPOSITION *arg)
{
    HWD_STEP(903);
    (void)hAdapter;
    (void)arg;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwdSetPointerShape(const HANDLE hAdapter, const DXGKARG_SETPOINTERSHAPE *arg)
{
    HWD_STEP(904);
    (void)hAdapter;
    (void)arg;
    return STATUS_NOT_SUPPORTED;   /* the cursor is drawn into the frame until the cursor layer exists */
}

static NTSTATUS NTAPI HwdIsSupportedVidPn(const HANDLE hAdapter, DXGKARG_ISSUPPORTEDVIDPN *arg)
{
    HWD_STEP(1000 + __COUNTER__);
    NTSTATUS ns = HwdIsSupportedVidPn_impl(hAdapter, arg);
    hwd_log("IsSupportedVidPn", ns);
    return ns;
}

static NTSTATUS NTAPI HwdRecommendFunctionalVidPn(const HANDLE hAdapter, const DXGKARG_RECOMMENDFUNCTIONALVIDPN *arg)
{
    HWD_STEP(1000 + __COUNTER__);
    NTSTATUS ns = HwdRecommendFunctionalVidPn_impl(hAdapter, arg);
    hwd_log("RecommendFunctionalVidPn", ns);
    return ns;
}

static NTSTATUS NTAPI HwdEnumVidPnCofuncModality(const HANDLE hAdapter, const DXGKARG_ENUMVIDPNCOFUNCMODALITY *arg)
{
    HWD_STEP(1000 + __COUNTER__);
    NTSTATUS ns = HwdEnumVidPnCofuncModality_impl(hAdapter, arg);
    hwd_log("EnumVidPnCofuncModality", ns);
    return ns;
}

static NTSTATUS NTAPI HwdRecommendMonitorModes(const HANDLE hAdapter, const DXGKARG_RECOMMENDMONITORMODES *arg)
{
    HWD_STEP(1000 + __COUNTER__);
    NTSTATUS ns = HwdRecommendMonitorModes_impl(hAdapter, arg);
    hwd_log("RecommendMonitorModes", ns);
    return ns;
}

static NTSTATUS NTAPI HwdCommitVidPn(const HANDLE hAdapter, const DXGKARG_COMMITVIDPN *arg)
{
    HWD_STEP(1000 + __COUNTER__);
    NTSTATUS ns = HwdCommitVidPn_impl(hAdapter, arg);
    hwd_log("CommitVidPn", ns);
    return ns;
}

static NTSTATUS NTAPI HwdSetVidPnSourceAddress(const HANDLE hAdapter, const DXGKARG_SETVIDPNSOURCEADDRESS *arg)
{
    HWD_STEP(1000 + __COUNTER__);
    NTSTATUS ns = HwdSetVidPnSourceAddress_impl(hAdapter, arg);
    hwd_log("SetVidPnSourceAddress", ns);
    return ns;
}

static NTSTATUS NTAPI HwdSetVidPnSourceVisibility(const HANDLE hAdapter, const DXGKARG_SETVIDPNSOURCEVISIBILITY *arg)
{
    HWD_STEP(1000 + __COUNTER__);
    NTSTATUS ns = HwdSetVidPnSourceVisibility_impl(hAdapter, arg);
    hwd_log("SetVidPnSourceVisibility", ns);
    return ns;
}

static NTSTATUS NTAPI HwdUpdateActiveVidPnPresentPath(const HANDLE hAdapter, const DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *arg)
{
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
};
static volatile LONG64 hwd_next_id;

static NTSTATUS NTAPI HwdGetStandardData_impl(const HANDLE hAdapter, DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *arg)
{
    (void)hAdapter;
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
    case D3DKMDT_STANDARDALLOCATION_STAGINGSURFACE:
        a->kind = HWW_ALLOC_STAGING;
        a->width = arg->pCreateStagingSurfaceData->Width;
        a->height = arg->pCreateStagingSurfaceData->Height;
        a->format = D3DDDIFMT_A8R8G8B8;
        arg->pCreateStagingSurfaceData->Pitch = a->width * 4;
        return STATUS_SUCCESS;
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

static NTSTATUS NTAPI HwdCreateAllocation_impl(const HANDLE hAdapter, DXGKARG_CREATEALLOCATION *ca)
{
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
        al->refresh.Numerator = HWD_REFRESH_N;
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
        ai->Flags.Value = 0;
        ai->Flags.CpuVisible = 1;
        ai->pAllocationUsageHint = NULL;
        ai->AllocationPriority = D3DDDI_ALLOCATIONPRIORITY_NORMAL;
    }
    if (ca->Flags.Resource && !ca->hResource) ca->hResource = (HANDLE)(ULONG_PTR)1;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwdDestroyAllocation_impl(const HANDLE hAdapter, const DXGKARG_DESTROYALLOCATION *da)
{
    (void)hAdapter;
    for (UINT i = 0; i < da->NumAllocations; i++) {
        if (da->pAllocationList[i] == d.source_alloc) d.source_alloc = NULL;
        if (da->pAllocationList[i]) ExFreePoolWithTag(da->pAllocationList[i], 'lAwH');
    }
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwdDescribeAllocation_impl(const HANDLE hAdapter, DXGKARG_DESCRIBEALLOCATION *da)
{
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
    (void)hDevice;
    for (UINT i = 0; i < oa->NumAllocations; i++) {
        DXGKARGCB_GETHANDLEDATA gh;
        RtlZeroMemory(&gh, sizeof gh);
        gh.hObject = oa->pOpenAllocation[i].hAllocation;
        gh.Type = DXGK_HANDLE_ALLOCATION;
        PVOID al = g.dxgk.DxgkCbGetHandleData(&gh);
        if (!al) return STATUS_INVALID_HANDLE;
        oa->pOpenAllocation[i].hDeviceSpecificAllocation = al;
    }
    return STATUS_SUCCESS;
}

/* A present asks for a DMA buffer that copies or flips. The copy or flip itself is the phone's: the
 * user-mode driver tells its renderer. Here the buffer only has to exist and move through the queue. */
static NTSTATUS NTAPI HwdPresent_impl(const HANDLE hContext, DXGKARG_PRESENT *pr)
{
    (void)hContext;
    if (pr->DmaSize < 16) return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    RtlZeroMemory(pr->pDmaBuffer, 16);
    pr->pDmaBuffer = (UCHAR *)pr->pDmaBuffer + 16;
    if (pr->pDmaBufferPrivateData && pr->DmaBufferPrivateDataSize >= sizeof(struct hww_marker)) {
        RtlZeroMemory(pr->pDmaBufferPrivateData, sizeof(struct hww_marker));
        pr->pDmaBufferPrivateData = (UCHAR *)pr->pDmaBufferPrivateData + sizeof(struct hww_marker);
    }
    d.presents++;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI HwdQueryDeviceDescriptor(CONST PVOID ctx, ULONG uid, PDXGK_DEVICE_DESCRIPTOR desc)
{
    HWD_STEP(1000 + __COUNTER__);
    NTSTATUS ns = HwdQueryDeviceDescriptor_impl(ctx, uid, desc);
    hwd_log("QueryDeviceDescriptor", ns);
    return ns;
}

static NTSTATUS NTAPI HwdGetStandardData(const HANDLE hAdapter, DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *arg)
{
    HWD_STEP(1000 + __COUNTER__);
    NTSTATUS ns = HwdGetStandardData_impl(hAdapter, arg);
    hwd_log("GetStandardData", ns);
    return ns;
}

static NTSTATUS NTAPI HwdCreateAllocation(const HANDLE hAdapter, DXGKARG_CREATEALLOCATION *ca)
{
    HWD_STEP(1000 + __COUNTER__);
    NTSTATUS ns = HwdCreateAllocation_impl(hAdapter, ca);
    hwd_log("CreateAllocation", ns);
    return ns;
}

static NTSTATUS NTAPI HwdDestroyAllocation(const HANDLE hAdapter, const DXGKARG_DESTROYALLOCATION *da)
{
    HWD_STEP(1000 + __COUNTER__);
    NTSTATUS ns = HwdDestroyAllocation_impl(hAdapter, da);
    hwd_log("DestroyAllocation", ns);
    return ns;
}

static NTSTATUS NTAPI HwdDescribeAllocation(const HANDLE hAdapter, DXGKARG_DESCRIBEALLOCATION *da)
{
    HWD_STEP(1000 + __COUNTER__);
    NTSTATUS ns = HwdDescribeAllocation_impl(hAdapter, da);
    hwd_log("DescribeAllocation", ns);
    return ns;
}

static NTSTATUS NTAPI HwdOpenAllocation(const HANDLE hDevice, const DXGKARG_OPENALLOCATION *oa)
{
    HWD_STEP(1000 + __COUNTER__);
    NTSTATUS ns = HwdOpenAllocation_impl(hDevice, oa);
    hwd_log("OpenAllocation", ns);
    return ns;
}

static NTSTATUS NTAPI HwdPresent(const HANDLE hContext, DXGKARG_PRESENT *pr)
{
    HWD_STEP(1000 + __COUNTER__);
    NTSTATUS ns = HwdPresent_impl(hContext, pr);
    hwd_log("Present", ns);
    return ns;
}

/* ---- vsync ----
 * A flip is finished when the screen reports a vsync with the new address. Until the phone's own vsync
 * timeline drives this (vsync/README.md), a 60 Hz timer stands in for it, running only while Windows
 * has the interrupt enabled. Without it dxgkrnl's flip queue never moves and its watchdog fires. */
static PEX_TIMER hwd_vsync_timer;
static volatile LONG hwd_vsync_on;

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
    (void)timer;
    (void)context;
    if (!hwd_vsync_on) return;
    BOOLEAN ret;
    g.dxgk.DxgkCbSynchronizeExecution(g.dxgk.DeviceHandle, hwd_vsync_sync, NULL, 0, &ret);
    d.vsyncs++;
}

static NTSTATUS hwd_control_interrupt(DXGK_INTERRUPT_TYPE type, BOOLEAN enable)
{
    if (!d.on || type != DXGK_INTERRUPT_CRTC_VSYNC) return STATUS_SUCCESS;
    if (!hwd_vsync_timer) {
        hwd_vsync_timer = ExAllocateTimer(hwd_vsync_tick, NULL, EX_TIMER_HIGH_RESOLUTION);
        if (!hwd_vsync_timer) return STATUS_NO_MEMORY;
        ExSetTimer(hwd_vsync_timer, -166667, 166667, NULL);   /* 100 ns units: 60 Hz */
    }
    InterlockedExchange(&hwd_vsync_on, enable ? 1 : 0);
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
static void hwd_setup(DRIVER_INITIALIZATION_DATA *init)
{
    /* Allocations whatever the switch: shared surfaces (DWM's, for the screen driver) need them. */
    init->DxgkDdiCreateAllocation = HwdCreateAllocation;
    init->DxgkDdiDestroyAllocation = HwdDestroyAllocation;
    init->DxgkDdiDescribeAllocation = HwdDescribeAllocation;
    init->DxgkDdiOpenAllocation = HwdOpenAllocation;
    init->DxgkDdiPresent = HwdPresent;   /* a shared surface's present: a fenced no-op, the phone shows it */
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
