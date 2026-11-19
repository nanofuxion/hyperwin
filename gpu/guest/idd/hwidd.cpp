// HyperWin's indirect display driver: one adapter with one monitor whose frames Windows hands to this
// driver as Direct3D surfaces (IddCx). It runs as a program in WUDFHost.exe, so a fault here ends that
// process, not Windows. wddm/README.md phase 6 has the plan; for now it takes frames, counts them, and
// can save one for a look, so the screen path to the phone comes next.
#include "hwidd.h"
#include <dxgi1_4.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "hw_abi.h"
#include "hwgpu_ioctl.h"
#include "hw_d3d.h"

// ---- a small log, readable by an administrator: C:\ProgramData\HyperWin\idd.log ----
static CRITICAL_SECTION g_log_lock;
static bool g_log_ready;

void hwidd_log(const char *fmt, ...)
{
    if (!g_log_ready) {
        InitializeCriticalSection(&g_log_lock);
        CreateDirectoryW(L"C:\\ProgramData\\HyperWin", nullptr);
        g_log_ready = true;
    }
    char line[512];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = snprintf(line, sizeof line, "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a;
    va_start(a, fmt);
    n += vsnprintf(line + n, sizeof line - n - 2, fmt, a);
    va_end(a);
    if (n > (int)sizeof line - 3) n = sizeof line - 3;
    line[n++] = '\r';
    line[n++] = '\n';
    EnterCriticalSection(&g_log_lock);
    HANDLE f = CreateFileW(L"C:\\ProgramData\\HyperWin\\idd.log", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteFile(f, line, (DWORD)n, &w, nullptr);
        CloseHandle(f);
    }
    LeaveCriticalSection(&g_log_lock);
}

// ---- the shared window: header and frames, mapped by hwgpu.sys (IOCTL_HWGPU_SCREEN) ----
static volatile hw_header *g_win;

static void open_screen()
{
    HANDLE f = CreateFileW(HWGPU_USER_PATH, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        static DWORD told;
        if (GetLastError() != told) hwidd_log("no \\.\HwGpu: %lu", told = GetLastError());
        return;
    }
    hwgpu_map_out out = {};
    DWORD got = 0;
    if (!DeviceIoControl(f, IOCTL_HWGPU_SCREEN, nullptr, 0, &out, sizeof out, &got, nullptr) || !out.user) {
        hwidd_log("screen mapping refused: %lu", GetLastError());
        CloseHandle(f);
        return;
    }
    g_win = (volatile hw_header *)(ULONG_PTR)out.user;   // the handle stays open: closing it unmaps
    hwidd_log("screen mapped, app size %ux%u, phone %ux%u", g_win->screen_want_width, g_win->screen_want_height,
              g_win->screen_native_width, g_win->screen_native_height);
}

// ---- modes: the size chosen in the app first, then the phone's own screen, then common ones ----
struct mode { UINT w, h, hz; };
static mode g_modes[10];
static UINT g_mode_count;

static bool size_ok(UINT w, UINT h)
{
    return w >= HW_MIN_WIDTH && w <= HW_MAX_WIDTH && h >= HW_MIN_HEIGHT && h <= HW_MAX_HEIGHT;
}

static void add_mode(UINT w, UINT h)
{
    if (!size_ok(w, h) || g_mode_count >= sizeof g_modes / sizeof g_modes[0]) return;
    for (UINT i = 0; i < g_mode_count; i++)
        if (g_modes[i].w == w && g_modes[i].h == h) return;
    g_modes[g_mode_count++] = { w, h, 60 };
}

static void build_modes()
{
    g_mode_count = 0;
    if (g_win) {
        add_mode(g_win->screen_want_width, g_win->screen_want_height);
        add_mode(g_win->screen_native_width, g_win->screen_native_height);
    }
    static const UINT common[][2] = { { 1280, 720 }, { 1920, 1080 }, { 1600, 900 }, { 1366, 768 }, { 1024, 768 } };
    for (auto &c : common) add_mode(c[0], c[1]);
}

// IddCx wants the vsync divider 0 in a monitor mode and non-zero in a target mode.
static void signal_info(DISPLAYCONFIG_VIDEO_SIGNAL_INFO *s, const mode &m, UINT divider)
{
    ZeroMemory(s, sizeof *s);
    s->totalSize.cx = s->activeSize.cx = m.w;
    s->totalSize.cy = s->activeSize.cy = m.h;
    s->vSyncFreq.Numerator = m.hz;
    s->vSyncFreq.Denominator = 1;
    s->hSyncFreq.Numerator = m.hz * m.h;
    s->hSyncFreq.Denominator = 1;
    s->pixelRate = (UINT64)m.w * m.h * m.hz;
    s->scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    s->videoStandard = 255 | (divider << 16);   // other standard
}

// The screen's size is the one chosen in the app, so that is the only mode the screen offers Windows.
static void fill_target(IDDCX_TARGET_MODE *t, const mode &m)
{
    ZeroMemory(t, sizeof *t);
    t->Size = sizeof *t;
    signal_info(&t->TargetVideoSignalInfo.targetVideoSignalInfo, m, 1);
    t->RequiredBandwidth = (UINT64)m.w * m.h * m.hz;
}

// ---- the frame thread: one per swap chain Windows gives the monitor ----
struct frame_thread {
    IDDCX_SWAPCHAIN swapchain;
    volatile LONG abandoned;   // unassign gave up waiting: the swap chain is no longer ours to delete
    HANDLE available;   // set by IddCx when a new frame is there
    HANDLE stop;
    HANDLE thread;
    LUID luid;
};
static frame_thread *g_ft;

// The frame goes into the shared window's back buffer and becomes the front one: the app shows it
// on the phone's screen as it does the boot screen. The frame was drawn by Windows' software
// renderer, so this is a CPU copy; on the HyperWin GPU it will become a hand-over instead.
static ID3D11Texture2D *g_stage;
static D3D11_TEXTURE2D_DESC g_stage_desc;

static void publish_frame(ID3D11Device *dev, ID3D11DeviceContext *ctx, IDXGIResource *res)
{
    volatile hw_header *h = g_win;
    if (!h) return;
    ID3D11Texture2D *tex = nullptr;
    if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&tex))) return;
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    if (!g_stage || g_stage_desc.Width != d.Width || g_stage_desc.Height != d.Height || g_stage_desc.Format != d.Format) {
        if (g_stage) g_stage->Release();
        g_stage = nullptr;
        g_stage_desc = d;
        g_stage_desc.Usage = D3D11_USAGE_STAGING;
        g_stage_desc.BindFlags = 0;
        g_stage_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        g_stage_desc.MiscFlags = 0;
        if (FAILED(dev->CreateTexture2D(&g_stage_desc, nullptr, &g_stage))) g_stage = nullptr;
    }
    if (g_stage && size_ok(d.Width, d.Height)) {
        ctx->CopyResource(g_stage, tex);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(g_stage, 0, D3D11_MAP_READ, 0, &m))) {
            hw_header *hh = (hw_header *)h;
            if (hh->width != d.Width || hh->height != d.Height || hh->stride != d.Width * 4 || hh->format != HW_FORMAT_BGRA) {
                hh->width = d.Width;
                hh->height = d.Height;
                hh->stride = d.Width * 4;
                hh->format = HW_FORMAT_BGRA;
            }
            hh->screen_guest = 1;
            if (hw_mode_ok(hh)) {
                uint32_t back = __atomic_load_n(&hh->front, __ATOMIC_ACQUIRE) == 0 ? 1 : 0;
                BYTE *dst = (BYTE *)hh + hw_frame_off(hh, back);
                for (UINT y = 0; y < d.Height; y++)
                    memcpy(dst + (size_t)y * hh->stride, (const BYTE *)m.pData + (size_t)y * m.RowPitch, (size_t)d.Width * 4);
                __atomic_store_n(&hh->front, back, __ATOMIC_RELEASE);
                __atomic_add_fetch(&hh->seq, 1, __ATOMIC_RELEASE);
            }
            ctx->Unmap(g_stage, 0);
        }
    }
    tex->Release();
}

// One Direct3D device per render GPU, kept for the driver's life: making one on the HyperWin GPU takes a
// second or more (every format question crosses to the phone), and Windows replaces swap chains often.
struct render_dev {
    LUID luid;
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    IDXGIDevice *dxgi;
};
static render_dev g_rdev[4];
static CRITICAL_SECTION g_rdev_lock;

static HRESULT get_device(LUID luid, render_dev **out)
{
    EnterCriticalSection(&g_rdev_lock);
    HRESULT hr = S_OK;
    render_dev *slot = nullptr;
    for (auto &r : g_rdev)
        if (r.dev && r.luid.LowPart == luid.LowPart && r.luid.HighPart == luid.HighPart) slot = &r;
    if (slot && slot->dev->GetDeviceRemovedReason() != S_OK) {
        /* The phone's GPU process restarted (a driver swap): this device is gone for good. A new one is
         * made. The old one is left alone, since a frame thread that gave up on it may still hold it. */
        hwidd_log("device for the render GPU was removed: %08lx", (unsigned long)slot->dev->GetDeviceRemovedReason());
        slot->dev = nullptr;
        slot = nullptr;
    }
    if (!slot) {
        for (auto &r : g_rdev)
            if (!r.dev) { slot = &r; break; }
        if (!slot) slot = &g_rdev[0];
        IDXGIFactory4 *factory = nullptr;
        IDXGIAdapter1 *adapter = nullptr;
        hr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), (void **)&factory);
        if (SUCCEEDED(hr)) hr = factory->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter1), (void **)&adapter);
        ID3D11Device *dev = nullptr;
        ID3D11DeviceContext *ctx = nullptr;
        IDXGIDevice *dxgi = nullptr;
        if (SUCCEEDED(hr)) {
            DXGI_ADAPTER_DESC1 ad;
            adapter->GetDesc1(&ad);
            hwidd_log("render adapter: %ls (%08lx:%08lx)", ad.Description, (unsigned long)luid.HighPart, (unsigned long)luid.LowPart);
            hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, &dev, nullptr, &ctx);
        }
        if (SUCCEEDED(hr)) hr = dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi);
        if (adapter) adapter->Release();
        if (factory) factory->Release();
        if (SUCCEEDED(hr)) {
            if (slot->dev) {   // all four slots in use: the oldest goes
                slot->dxgi->Release();
                slot->ctx->Release();
                slot->dev->Release();
            }
            *slot = { luid, dev, ctx, dxgi };
            hwidd_log("device for the render GPU made");
        } else {
            if (dxgi) dxgi->Release();
            if (ctx) ctx->Release();
            if (dev) dev->Release();
            slot = nullptr;
        }
    }
    LeaveCriticalSection(&g_rdev_lock);
    *out = slot;
    return slot ? S_OK : (FAILED(hr) ? hr : E_FAIL);
}

static DWORD WINAPI frame_loop(void *arg)
{
    frame_thread *ft = (frame_thread *)arg;
    render_dev *rd = nullptr;
    HRESULT hr = get_device(ft->luid, &rd);
    ID3D11Device *dev = rd ? rd->dev : nullptr;
    ID3D11DeviceContext *ctx = rd ? rd->ctx : nullptr;
    if (SUCCEEDED(hr) && WaitForSingleObject(ft->stop, 0) == WAIT_OBJECT_0) hr = E_ABORT;   // taken back meanwhile
    if (SUCCEEDED(hr)) {
        IDARG_IN_SWAPCHAINSETDEVICE in = {};
        in.pDevice = rd->dxgi;
        hr = IddCxSwapChainSetDevice(ft->swapchain, &in);
    }
    hwidd_log("swap chain device: %08lx", (unsigned long)hr);
    UINT frames = 0;
    ULONGLONG started = GetTickCount64();
    // On the HyperWin GPU the frames are phone buffers and go to the phone's screen layer; on another
    // GPU they are read back and written to the shared window.
    bool gpu_frames = false;
    if (SUCCEEDED(hr)) {
        IDXGIAdapter *a = nullptr;
        DXGI_ADAPTER_DESC ad = {};
        if (SUCCEEDED(rd->dxgi->GetAdapter(&a))) {
            a->GetDesc(&ad);
            a->Release();
        }
        gpu_frames = !wcsncmp(ad.Description, L"HyperWin GPU", 12);
    }
    while (SUCCEEDED(hr)) {
        IDARG_OUT_RELEASEANDACQUIREBUFFER buf = {};
        if (frames <= 3) hwidd_log("acquire after frame %u", frames);
        hr = IddCxSwapChainReleaseAndAcquireBuffer(ft->swapchain, &buf);
        if (frames <= 3 && hr != E_PENDING) hwidd_log("acquire: %08lx", (unsigned long)hr);
        if (hr == E_PENDING) {
            HANDLE wait[2] = { ft->available, ft->stop };
            DWORD r = WaitForMultipleObjects(2, wait, FALSE, 100);
            if (r == WAIT_OBJECT_0 + 1) break;
            hr = S_OK;
            continue;
        }
        if (FAILED(hr)) break;
        frames++;
        if (frames == 1) hwidd_log("first frame, %u dirty rects, %s", buf.MetaData.DirtyRectCount, gpu_frames ? "handed to the phone's screen" : "read back");
        if (gpu_frames) {
            // The frame is a shared surface on the phone: ask for it to be shown there as it is.
            ID3D11Resource *res = nullptr;
            HRESULT qr = buf.MetaData.pSurface->QueryInterface(__uuidof(ID3D11Resource), (void **)&res);
            if (SUCCEEDED(qr)) {
                static const UINT32 pixel = 0;
                D3D11_BOX one = { 0, 0, 0, 1, 1, 1 };
                ctx->UpdateSubresource(res, 0, &one, &pixel, HWD3D_SCANOUT_PITCH, HWD3D_SCANOUT_SLICE);
                ctx->Flush();
                res->Release();
            }
        } else {
            publish_frame(dev, ctx, buf.MetaData.pSurface);
        }
        if (frames % 600 == 0) {
            ULONGLONG ms = GetTickCount64() - started;
            hwidd_log("%u frames in %llu ms", frames, ms);
        }
        if (frames <= 3) hwidd_log("frame %u processed, device state %08lx", frames, (unsigned long)dev->GetDeviceRemovedReason());
        hr = IddCxSwapChainFinishedProcessingFrame(ft->swapchain);
        if (frames <= 3) hwidd_log("frame %u finished: %08lx", frames, (unsigned long)hr);
        if (WaitForSingleObject(ft->stop, 0) == WAIT_OBJECT_0) break;
    }
    hwidd_log("frame loop ends after %u frames: %08lx", frames, (unsigned long)hr);
    if (g_stage) g_stage->Release();
    g_stage = nullptr;
    if (!InterlockedCompareExchange(&ft->abandoned, 0, 0)) WdfObjectDelete((WDFOBJECT)ft->swapchain);
    // The record stays (a few bytes per swap chain): stop_frames may still signal it.
    return 0;
}

// Ends the frame thread and waits for it: IddCx expects the swap chain deleted when the unassign callback
// returns. Only the first swap chain on a GPU waits for its device (get_device keeps it); the wait has a
// bound all the same, as a hang here makes WUDFHost restart.
static void stop_frames()
{
    frame_thread *ft = g_ft;
    g_ft = nullptr;
    if (!ft) return;
    SetEvent(ft->stop);
    if (WaitForSingleObject(ft->thread, 4000) == WAIT_TIMEOUT) {
        InterlockedExchange(&ft->abandoned, 1);
        hwidd_log("frame thread still busy after 4 s (making the device): left to finish alone");
    }
}

// An EDID for the monitor: Windows refuses an arrival without one. Manufacturer "HWN", named "HyperWin",
// preferring the size chosen in the app at 60 Hz. The serial stays the same, so Windows keeps one
// monitor and its place in the desktop layout whatever the size.
static void make_edid(BYTE e[128], UINT ha, UINT va)
{
    ZeroMemory(e, 128);
    static const BYTE header[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
    memcpy(e, header, 8);
    UINT16 mfg = (UINT16)((('H' - 64) << 10) | (('W' - 64) << 5) | ('N' - 64));
    e[8] = (BYTE)(mfg >> 8);
    e[9] = (BYTE)mfg;
    e[10] = 0x01;            // product code
    e[12] = 0x01;            // serial
    e[16] = 1;               // week
    e[17] = 36;              // year 2026 - 1990
    e[18] = 1;               // EDID 1.4
    e[19] = 4;
    e[20] = 0xa5;            // digital, 8 bits per colour, DisplayPort-like
    e[21] = 34;              // 34 x 19 cm
    e[22] = 19;
    e[23] = 120;             // gamma 2.2
    e[24] = 0x06;            // RGB, preferred timing is native
    static const BYTE chroma[10] = { 0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26, 0x0f, 0x50, 0x54 };   // sRGB
    memcpy(e + 25, chroma, 10);
    for (int i = 38; i < 54; i += 2) { e[i] = 0x01; e[i + 1] = 0x01; }   // no standard timings
    // Detailed timing at 60 Hz with reduced blanking; the pixel clock follows from it.
    BYTE *d = e + 54;
    UINT hb = 160, vb = 30, hso = 48, hsw = 32, vso = 3, vsw = 5;
    UINT clock = (UINT)(((UINT64)(ha + hb) * (va + vb) * 60 + 5000) / 10000);   // 10 kHz units
    d[0] = (BYTE)clock;
    d[1] = (BYTE)(clock >> 8);
    d[2] = (BYTE)ha;
    d[3] = (BYTE)hb;
    d[4] = (BYTE)(((ha >> 8) << 4) | (hb >> 8));
    d[5] = (BYTE)va;
    d[6] = (BYTE)vb;
    d[7] = (BYTE)(((va >> 8) << 4) | (vb >> 8));
    d[8] = (BYTE)hso;
    d[9] = (BYTE)hsw;
    d[10] = (BYTE)(((vso & 15) << 4) | (vsw & 15));
    d[11] = (BYTE)(((hso >> 8) << 6) | ((hsw >> 8) << 4) | ((vso >> 4) << 2) | (vsw >> 4));
    d[12] = 0x54;            // 340 x 190 mm
    d[13] = 0xbe;
    d[14] = 0x10;
    d[17] = 0x1e;            // digital separate sync, both positive
    // Monitor name.
    BYTE *n = e + 72;
    n[3] = 0xfc;
    memcpy(n + 5, "HyperWin\n   ", 13);
    // Range limits: 24-75 Hz vertical, 15-255 kHz horizontal, 660 MHz.
    BYTE *r = e + 90;
    r[3] = 0xfd;
    r[5] = 24; r[6] = 75; r[7] = 15; r[8] = 255; r[9] = 66; r[10] = 0x00; r[11] = 0x0a;
    for (int i = 12; i < 18; i++) r[i] = 0x20;
    // Unused descriptor.
    e[108 + 3] = 0x10;
    BYTE sum = 0;
    for (int i = 0; i < 127; i++) sum += e[i];
    e[127] = (BYTE)(256 - sum);
}

// ---- IddCx callbacks ----
static IDDCX_ADAPTER g_adapter;
static IDDCX_MONITOR g_monitor;

// Which GPU Windows draws this screen's desktop with. Until the HyperWin GPU's Direct3D driver serves DWM
// and shares surfaces across programs, that is Windows' own software renderer; DWM on the HyperWin GPU
// fails today and leaves the screen without frames.
static void choose_render_adapter(IDDCX_ADAPTER adapter)
{
    // HKLM\SOFTWARE\HyperWin\Gpu\IddRender = 1 asks for the HyperWin GPU instead, for bringing that up.
    DWORD gpu = 0, len = sizeof gpu;
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\HyperWin\\Gpu", L"IddRender", RRF_RT_REG_DWORD, nullptr, &gpu, &len);
    IDXGIFactory1 *f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&f))) return;
    IDXGIAdapter1 *a = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d;
        a->GetDesc1(&d);
        a->Release();
        bool ours = !wcsncmp(d.Description, L"HyperWin GPU", 12);
        if (gpu ? ours : (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
            IDARG_IN_ADAPTERSETRENDERADAPTER in = {};
            in.PreferredRenderAdapter = d.AdapterLuid;
            IddCxAdapterSetRenderAdapter(adapter, &in);
            hwidd_log("render adapter set to %ls", d.Description);

            break;
        }
    }
    f->Release();
}

// Connects the monitor with the modes and EDID for the current app size.
static NTSTATUS connect_monitor(IDDCX_ADAPTER adapter)
{
    build_modes();
    IDDCX_MONITOR_INFO info = {};
    info.Size = sizeof info;
    info.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;   // a connector type: the indirect ones are refused here
    info.ConnectorIndex = 0;
    info.MonitorDescription.Size = sizeof info.MonitorDescription;
    info.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    static BYTE edid[128];
    make_edid(edid, g_modes[0].w, g_modes[0].h);
    info.MonitorDescription.DataSize = sizeof edid;
    info.MonitorDescription.pData = edid;
    // {7b4a1d52-9f3e-4c51-a8a2-6f1e0c3b5d21}: the same container on every boot, so Windows keeps its settings.
    info.MonitorContainerId = { 0x7b4a1d52, 0x9f3e, 0x4c51, { 0xa8, 0xa2, 0x6f, 0x1e, 0x0c, 0x3b, 0x5d, 0x21 } };
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    IDARG_IN_MONITORCREATE mc = {};
    mc.ObjectAttributes = &attr;
    mc.pMonitorInfo = &info;
    IDARG_OUT_MONITORCREATE mo = {};
    NTSTATUS ns = IddCxMonitorCreate(adapter, &mc, &mo);
    hwidd_log("monitor create %ux%u: %08lx", g_modes[0].w, g_modes[0].h, (unsigned long)ns);
    if (!NT_SUCCESS(ns)) return ns;
    g_monitor = mo.MonitorObject;
    IDARG_OUT_MONITORARRIVAL ma = {};
    ns = IddCxMonitorArrival(g_monitor, &ma);
    hwidd_log("monitor arrival: %08lx, adapter %08lx:%08lx target %u", (unsigned long)ns, (unsigned long)ma.OsAdapterLuid.HighPart,
              (unsigned long)ma.OsAdapterLuid.LowPart, ma.OsTargetId);
    return ns;
}

// The app's screen size setting, while Windows runs: the screen then offers only the new size, and
// Windows switches to it without the desktop layout changing.
static HANDLE g_watch;
static DWORD WINAPI watch_size(void *)
{
    UINT32 seen = g_win ? g_win->screen_seq : 0;
    for (unsigned n = 1;; n++) {
        Sleep(250);
        if (!g_win && n % 8 == 0) {
            open_screen();
            if (g_win) seen = g_win->screen_seq - 1;
        }
        if (!g_win || g_win->screen_seq == seen) continue;
        seen = g_win->screen_seq;
        UINT w = g_win->screen_want_width, h = g_win->screen_want_height;
        hwidd_log("app asks for %ux%u", w, h);
        if (!size_ok(w, h) || !g_monitor || (g_mode_count && g_modes[0].w == w && g_modes[0].h == h)) continue;
        build_modes();
        IDDCX_TARGET_MODE t;
        fill_target(&t, g_modes[0]);
        IDARG_IN_UPDATEMODES in = {};
        in.Reason = IDDCX_UPDATE_REASON_CONFIGURATION_CONSTRAINTS;
        in.TargetModeCount = 1;
        in.pTargetModes = &t;
        NTSTATUS ns = IddCxMonitorUpdateModes(g_monitor, &in);
        hwidd_log("modes now %ux%u: %08lx", g_modes[0].w, g_modes[0].h, (unsigned long)ns);
    }
    return 0;
}

static NTSTATUS adapter_init_finished(IDDCX_ADAPTER adapter, const IDARG_IN_ADAPTER_INIT_FINISHED *in)
{
    hwidd_log("adapter init finished: %08lx", (unsigned long)in->AdapterInitStatus);
    if (!NT_SUCCESS(in->AdapterInitStatus)) return STATUS_SUCCESS;
    choose_render_adapter(adapter);
    /* hwgpu.sys can start a second or two after the screen driver. Waiting for it here lets the monitor
     * arrive at the app's size. The monitor must arrive in this callback: arriving later from another
     * thread, Windows attached it but never gave it a swap chain. */
    for (int i = 0; !g_win && i < 20; i++) {
        Sleep(250);
        open_screen();
    }
    NTSTATUS ns = connect_monitor(adapter);
    if (!g_watch) g_watch = CreateThread(nullptr, 0, watch_size, nullptr, 0, nullptr);
    return ns;
}

static NTSTATUS parse_monitor_description(const IDARG_IN_PARSEMONITORDESCRIPTION *in, IDARG_OUT_PARSEMONITORDESCRIPTION *out)
{
    hwidd_log("parse description, room for %u", in->MonitorModeBufferInputCount);
    out->MonitorModeBufferOutputCount = g_mode_count;
    out->PreferredMonitorModeIdx = 0;
    if (in->MonitorModeBufferInputCount < g_mode_count) return in->MonitorModeBufferInputCount ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
    for (UINT i = 0; i < g_mode_count; i++) {
        in->pMonitorModes[i].Size = sizeof(IDDCX_MONITOR_MODE);
        in->pMonitorModes[i].Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER;
        signal_info(&in->pMonitorModes[i].MonitorVideoSignalInfo, g_modes[i], 0);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS default_modes(IDDCX_MONITOR, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES *in, IDARG_OUT_GETDEFAULTDESCRIPTIONMODES *out)
{
    hwidd_log("default modes, room for %u", in->DefaultMonitorModeBufferInputCount);
    out->DefaultMonitorModeBufferOutputCount = g_mode_count;
    out->PreferredMonitorModeIdx = 0;
    if (in->DefaultMonitorModeBufferInputCount == 0) return STATUS_SUCCESS;
    if (in->DefaultMonitorModeBufferInputCount < g_mode_count) return STATUS_BUFFER_TOO_SMALL;
    for (UINT i = 0; i < g_mode_count; i++) {
        in->pDefaultMonitorModes[i].Size = sizeof(IDDCX_MONITOR_MODE);
        in->pDefaultMonitorModes[i].Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER;
        signal_info(&in->pDefaultMonitorModes[i].MonitorVideoSignalInfo, g_modes[i], 0);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS query_target_modes(IDDCX_MONITOR, const IDARG_IN_QUERYTARGETMODES *in, IDARG_OUT_QUERYTARGETMODES *out)
{
    hwidd_log("target modes, room for %u", in->TargetModeBufferInputCount);
    out->TargetModeBufferOutputCount = 1;
    if (in->TargetModeBufferInputCount == 0) return STATUS_SUCCESS;
    fill_target(&in->pTargetModes[0], g_modes[0]);
    return STATUS_SUCCESS;
}

static NTSTATUS commit_modes(IDDCX_ADAPTER, const IDARG_IN_COMMITMODES *in)
{
    hwidd_log("commit modes: %u paths", in->PathCount);
    return STATUS_SUCCESS;
}

static NTSTATUS assign_swapchain(IDDCX_MONITOR, const IDARG_IN_SETSWAPCHAIN *in)
{
    stop_frames();
    frame_thread *ft = new frame_thread();
    ft->swapchain = in->hSwapChain;
    ft->available = in->hNextSurfaceAvailable;
    ft->luid = in->RenderAdapterLuid;
    ft->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ft->thread = CreateThread(nullptr, 0, frame_loop, ft, 0, nullptr);
    g_ft = ft;
    hwidd_log("swap chain assigned, render adapter %08lx:%08lx", (unsigned long)in->RenderAdapterLuid.HighPart,
              (unsigned long)in->RenderAdapterLuid.LowPart);
    return STATUS_SUCCESS;
}

static NTSTATUS unassign_swapchain(IDDCX_MONITOR)
{
    hwidd_log("swap chain unassigned");
    stop_frames();
    return STATUS_SUCCESS;
}

static NTSTATUS d0_entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE)
{
    IDDCX_ENDPOINT_VERSION version = {};
    version.Size = sizeof version;
    version.MajorVer = 1;
    IDDCX_ADAPTER_CAPS caps = {};
    caps.Size = sizeof caps;
    caps.MaxMonitorsSupported = 1;
    caps.MaxDisplayPipelineRate = 3840ull * 2160 * 60;   // 0 would rule out every mode
    caps.EndPointDiagnostics.Size = sizeof caps.EndPointDiagnostics;
    caps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    caps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;
    caps.EndPointDiagnostics.pEndPointFriendlyName = L"HyperWin screen";
    caps.EndPointDiagnostics.pEndPointModelName = L"HyperWin";
    caps.EndPointDiagnostics.pEndPointManufacturerName = L"HyperWin";
    caps.EndPointDiagnostics.pHardwareVersion = &version;
    caps.EndPointDiagnostics.pFirmwareVersion = &version;
    IDARG_IN_ADAPTER_INIT in = {};
    in.WdfDevice = device;
    in.pCaps = &caps;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    in.ObjectAttributes = &attr;
    IDARG_OUT_ADAPTER_INIT out = {};
    NTSTATUS ns = IddCxAdapterInitAsync(&in, &out);
    hwidd_log("adapter init: %08lx", (unsigned long)ns);
    if (NT_SUCCESS(ns)) g_adapter = out.AdapterObject;
    return ns;
}

static NTSTATUS device_add(WDFDRIVER, PWDFDEVICE_INIT init)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDeviceD0Entry = d0_entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &pnp);

    IDD_CX_CLIENT_CONFIG cfg;
    IDD_CX_CLIENT_CONFIG_INIT(&cfg);
    cfg.EvtIddCxAdapterInitFinished = adapter_init_finished;
    cfg.EvtIddCxParseMonitorDescription = parse_monitor_description;
    cfg.EvtIddCxMonitorGetDefaultDescriptionModes = default_modes;
    cfg.EvtIddCxMonitorQueryTargetModes = query_target_modes;
    cfg.EvtIddCxAdapterCommitModes = commit_modes;
    cfg.EvtIddCxMonitorAssignSwapChain = assign_swapchain;
    cfg.EvtIddCxMonitorUnassignSwapChain = unassign_swapchain;
    NTSTATUS ns = IddCxDeviceInitConfig(init, &cfg);
    hwidd_log("device init config: %08lx", (unsigned long)ns);
    if (!NT_SUCCESS(ns)) return ns;
    WDFDEVICE device = nullptr;
    ns = WdfDeviceCreate(&init, WDF_NO_OBJECT_ATTRIBUTES, &device);
    hwidd_log("device create: %08lx", (unsigned long)ns);
    if (!NT_SUCCESS(ns)) return ns;
    ns = IddCxDeviceInitialize(device);
    hwidd_log("device initialize: %08lx", (unsigned long)ns);
    return ns;
}

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry)
{
    hwidd_log("driver entry");
    InitializeCriticalSection(&g_rdev_lock);
    open_screen();
    build_modes();
    WDF_DRIVER_CONFIG cfg;
    WDF_DRIVER_CONFIG_INIT(&cfg, device_add);
    NTSTATUS ns = WdfDriverCreate(driver, registry, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
    hwidd_log("driver create: %08lx", (unsigned long)ns);
    return ns;
}
