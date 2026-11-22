/* GPU byte rings in the HyperWin shared window (include/hw_abi.h).
 *
 * The same SweetCider sgpu stream (sgpu_proto.h) that once went over vsock goes through RAM here.
 * Each Windows process that opens the GPU gets one channel: a control page, a guest-to-phone ring
 * and a phone-to-guest ring. Each ring has one producer and one consumer.
 *
 * Who does what:
 *   phone app  hwgpu_ring_init() once per boot, then serves every channel (gpu/host).
 *   guest      the kernel side claims a free channel for a process and frees it when the
 *              process's handle closes (gpu/guest/direct/hwgpu.c). The process itself only ever
 *              sees its own channel, mapped into it, never the window.
 *   phone app  sees CLOSING, drops what that process made, marks the channel FREE.
 *
 * The phone publishes finished fences in the control page instead of sending messages.
 *
 * Build with HWGPU_KERNEL in a Windows kernel driver: the blocking helpers are left out.
 */
#ifndef HYPERWIN_HWGPU_RING_H
#define HYPERWIN_HWGPU_RING_H

#include <stdint.h>
#include <stddef.h>

/* Above the largest frame pair (4096x2304 BGRA twice ends at 0x5800000) and below the guest
 * debug marks at HW_FRAME_END (0x7f00000). */
#define HWGPU_RING_OFF      0x6000000u
#define HWGPU_MAGIC         0x474e495255504748ull /* "HGPURING" little endian */
#define HWGPU_VERSION       3u  /* 3: twenty channels of 1.5 MiB; 2: heap grants */
#define HWGPU_CHANNELS      20u
#define HWGPU_CTL_BYTES     0x1000u
#define HWGPU_TX_BYTES      (1u << 20)  /* guest -> phone */
#define HWGPU_RX_BYTES      (512u << 10)  /* phone -> guest */
#define HWGPU_CHAN_STRIDE   (HWGPU_CTL_BYTES + HWGPU_TX_BYTES + HWGPU_RX_BYTES)
#define HWGPU_RING_BYTES    (0x1000u + HWGPU_CHANNELS * HWGPU_CHAN_STRIDE)
#define HWGPU_FENCE_RINGS   64u
#define HWGPU_GRANTS        128u  /* heap ranges one channel may map at once */
#define HWGPU_SCANOUT_BUFFERS 16u /* buffers one channel may show with SGPU_CMD_SCANOUT */

_Static_assert(HWGPU_RING_OFF + HWGPU_RING_BYTES <= 0x7f00000u, "GPU rings end below the debug marks");

/* guest_state */
#define HWGPU_FREE      0u
#define HWGPU_CONNECTED 2u
#define HWGPU_CLOSING   3u
/* host_state */
#define HWGPU_HOST_IDLE  0u
#define HWGPU_HOST_READY 1u

/* Context and resource ids belong to one channel: ctx = slot + 1, resources in
 * [(slot + 1) << 24, (slot + 2) << 24). The phone refuses ids from another channel. */
#define HWGPU_CTX(slot)      ((uint32_t)(slot) + 1u)
#define HWGPU_RES_BASE(slot) (((uint32_t)(slot) + 1u) << 24)

struct hwgpu_ring_hdr {
    uint64_t magic;
    uint32_t version;
    uint32_t channels;
    uint32_t tx_bytes;
    uint32_t rx_bytes;
    uint32_t host_tick;  /* the phone bumps it while its server runs */
    uint32_t reserved;
    /* phone: a value that differs for every renderer process. The WDDM adapter sees it change
     * when a driver swap restarts the renderer, and resets (wddm/README.md). */
    uint64_t epoch;
    char gpu_name[64];   /* phone: the GPU's name, for the adapter's name in Windows */
    /* phone: bytes it copied into / out of guest memory to keep a resource in step (the sgpu sync
     * path: memory that could not be shared). The counters of wddm/README.md. */
    uint64_t copy_to_guest, copy_from_guest;
};

/* First page of each channel. */
struct hwgpu_ctl {
    uint32_t guest_state;
    uint32_t host_state;
    uint32_t generation;
    uint32_t slot;
    uint64_t tx_head;  /* guest publishes */
    uint64_t tx_tail;  /* phone consumes */
    uint64_t rx_head;  /* phone publishes */
    uint64_t rx_tail;  /* guest consumes */
    uint64_t fence[HWGPU_FENCE_RINGS];  /* phone: last finished fence id per ring index */
    /* phone: parts of the heap (HW_HEAP_PA) this channel's process may map, size 0 for unused.
     * hwgpu.sys maps only a range that matches one exactly. */
    struct { uint64_t offset, size; } grant[HWGPU_GRANTS];
    /* phone: per scanout buffer, the seq of its last SGPU_CMD_SCANOUT that the screen has let go
     * of. The guest draws into a buffer again only once this has reached what it last sent. */
    uint32_t scanout_free[HWGPU_SCANOUT_BUFFERS];
    /* phone: HWGPU_SCANOUT_VSYNC when every scanout frame should wait for the phone's screen, as
     * if the program had asked for FIFO (the app's "Limit to screen refresh" setting). */
    uint32_t scanout_flags;
    /* Both: one count per hwgpu_write_all, the unit of a submit across the ring (wddm/README.md,
     * "Batching submits"). Zero when a process claims the channel. */
    uint64_t submits_to_phone;
    uint64_t submits_to_guest;
    /* Guest -> phone: fullscreen direct-scanout state and hardware cursor overlay position.
     * Added at the end of hwgpu_ctl so all earlier field offsets stay binary-compatible. */
    uint32_t scanout_active;   /* 1 while this channel owns fullscreen direct scanout, 0 when hidden */
    uint32_t cursor_visible;   /* 1 when Windows' cursor is showing over the fullscreen window */
    int32_t  cursor_x;         /* cursor screen X relative to the monitor's left edge */
    int32_t  cursor_y;         /* cursor screen Y relative to the monitor's top edge */
    uint32_t cursor_mon_w;     /* monitor width in guest screen pixels */
    uint32_t cursor_mon_h;     /* monitor height in guest screen pixels */
    uint32_t cursor_seq;       /* bumped by guest whenever cursor state changes */
    uint32_t cursor_reserved;
};
#define HWGPU_SCANOUT_VSYNC 1u
/* phone: the screen shows Windows' desktop as DWM draws it on the GPU (the HyperWin screen). A
 * program's frames then stay in its window, under any window above it, and never take the layer. */
#define HWGPU_SCANOUT_DESKTOP 2u

_Static_assert(sizeof(struct hwgpu_ctl) <= HWGPU_CTL_BYTES, "control fits its page");

static inline struct hwgpu_ring_hdr *hwgpu_hdr(void *window)
{
    return (struct hwgpu_ring_hdr *)((uint8_t *)window + HWGPU_RING_OFF);
}

/* Offset of a channel from the start of the window. */
static inline uint32_t hwgpu_chan_off(uint32_t slot)
{
    return HWGPU_RING_OFF + 0x1000u + slot * HWGPU_CHAN_STRIDE;
}

/* A channel as one block: control page, tx ring, rx ring. */
static inline struct hwgpu_ctl *hwgpu_chan_ctl(void *chan) { return (struct hwgpu_ctl *)chan; }
static inline uint8_t *hwgpu_tx(void *chan) { return (uint8_t *)chan + HWGPU_CTL_BYTES; }
static inline uint8_t *hwgpu_rx(void *chan) { return (uint8_t *)chan + HWGPU_CTL_BYTES + HWGPU_TX_BYTES; }

static inline int hwgpu_ring_valid(void *window)
{
    struct hwgpu_ring_hdr *h = hwgpu_hdr(window);
    return __atomic_load_n(&h->magic, __ATOMIC_ACQUIRE) == HWGPU_MAGIC && h->version == HWGPU_VERSION &&
           h->channels == HWGPU_CHANNELS && h->tx_bytes == HWGPU_TX_BYTES && h->rx_bytes == HWGPU_RX_BYTES;
}

/* Guest kernel: reserve a free channel for one process. Returns the slot or -1. */
static inline int hwgpu_claim(void *window, uint32_t *generation)
{
    if (!hwgpu_ring_valid(window)) return -1;
    for (uint32_t i = 0; i < HWGPU_CHANNELS; i++) {
        struct hwgpu_ctl *c = hwgpu_chan_ctl((uint8_t *)window + hwgpu_chan_off(i));
        uint32_t expected = HWGPU_FREE;
        /* Claim in a state the phone ignores, reset, then publish CONNECTED. */
        if (!__atomic_compare_exchange_n(&c->guest_state, &expected, HWGPU_CLOSING + 1u, 0,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            continue;
        c->tx_head = c->tx_tail = c->rx_head = c->rx_tail = 0;
        c->submits_to_phone = c->submits_to_guest = 0;
        c->scanout_active = c->cursor_visible = 0;
        c->cursor_x = c->cursor_y = 0;
        c->cursor_mon_w = c->cursor_mon_h = c->cursor_seq = c->cursor_reserved = 0;
        c->slot = i;
        __atomic_store_n(&c->host_state, HWGPU_HOST_IDLE, __ATOMIC_RELAXED);
        *generation = __atomic_add_fetch(&c->generation, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&c->guest_state, HWGPU_CONNECTED, __ATOMIC_RELEASE);
        return (int)i;
    }
    return -1;
}

/* Guest kernel: the process let go. The phone cleans up and frees the channel. */
static inline void hwgpu_release(void *window, uint32_t slot)
{
    struct hwgpu_ctl *c = hwgpu_chan_ctl((uint8_t *)window + hwgpu_chan_off(slot));
    __atomic_store_n(&c->guest_state, HWGPU_CLOSING, __ATOMIC_RELEASE);
}

static inline size_t hwgpu_write_some(uint8_t *data, uint32_t capacity, uint64_t *head, uint64_t *tail,
                                      const void *src, size_t bytes)
{
    const uint64_t h = __atomic_load_n(head, __ATOMIC_RELAXED);
    const uint64_t used = h - __atomic_load_n(tail, __ATOMIC_ACQUIRE);
#if defined(__i386__) || defined(__x86_64__)
    /* Under ARM64 Prism x86/x64 emulation, plain loads/stores lack hardware barriers unless
     * sequenced with a full fence (mfence / lock), which Prism lowers to ARM64 DMB ISH. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
    if (used >= capacity) return 0;
    const size_t n = bytes < capacity - used ? bytes : (size_t)(capacity - used);
    const size_t at = (size_t)(h % capacity);
    const size_t first = n < capacity - at ? n : capacity - at;
    __builtin_memcpy(data + at, src, first);
    if (n > first) __builtin_memcpy(data, (const uint8_t *)src + first, n - first);
#if defined(__i386__) || defined(__x86_64__)
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
    __atomic_store_n(head, h + n, __ATOMIC_RELEASE);
    return n;
}

static inline size_t hwgpu_read_some(const uint8_t *data, uint32_t capacity, uint64_t *head, uint64_t *tail,
                                     void *dst, size_t bytes)
{
    const uint64_t t = __atomic_load_n(tail, __ATOMIC_RELAXED);
    const uint64_t avail = __atomic_load_n(head, __ATOMIC_ACQUIRE) - t;
#if defined(__i386__) || defined(__x86_64__)
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
    if (!avail) return 0;
    const size_t n = bytes < avail ? bytes : (size_t)avail;
    const size_t at = (size_t)(t % capacity);
    const size_t first = n < capacity - at ? n : capacity - at;
    __builtin_memcpy(dst, data + at, first);
    if (n > first) __builtin_memcpy((uint8_t *)dst + first, data, n - first);
#if defined(__i386__) || defined(__x86_64__)
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
    __atomic_store_n(tail, t + n, __ATOMIC_RELEASE);
    return n;
}

#ifndef HWGPU_KERNEL
#ifdef _WIN32
#include <windows.h>
/* Sleep() rounds up to the system timer, often 15.6 ms, so waits of a millisecond or more use a
 * high-resolution timer, one per thread. */
static inline void hwgpu_pause(unsigned us)
{
    static __thread HANDLE timer;
    if (us < 1000) {
        SwitchToThread();
        return;
    }
    if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                                               TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)us * 10;
    if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, INFINITE);
    else Sleep(us / 1000);
}
#else
#include <unistd.h>
static inline void hwgpu_pause(unsigned us) { usleep(us); }
#endif

/* No doorbell crosses the VM boundary, so a blocked side polls: short waits first, then at most
 * every 300 us. *misses resets on every transfer. */
static inline void hwgpu_backoff(uint32_t *misses)
{
    if (*misses < 200) ++*misses;
    hwgpu_pause(*misses < 20 ? 50 : *misses < 200 ? 100 : 300);
}

/* Both sides: the channel is still the connection this side opened. */
static inline int hwgpu_alive(struct hwgpu_ctl *c, uint32_t generation)
{
    return __atomic_load_n(&c->generation, __ATOMIC_ACQUIRE) == generation &&
           __atomic_load_n(&c->guest_state, __ATOMIC_ACQUIRE) == HWGPU_CONNECTED &&
           __atomic_load_n(&c->host_state, __ATOMIC_ACQUIRE) == HWGPU_HOST_READY;
}

/* Moves all bytes, waiting for room or data. 0 when the connection ends first.
 * to_phone picks the ring: the guest writes tx and reads rx, the phone the other way round. */
static inline int hwgpu_write_all(void *chan, uint32_t generation, int to_phone, const void *src, size_t bytes)
{
    struct hwgpu_ctl *c = hwgpu_chan_ctl(chan);
    uint8_t *data = to_phone ? hwgpu_tx(chan) : hwgpu_rx(chan);
    uint32_t cap = to_phone ? HWGPU_TX_BYTES : HWGPU_RX_BYTES;
    uint64_t *head = to_phone ? &c->tx_head : &c->rx_head, *tail = to_phone ? &c->tx_tail : &c->rx_tail;
    const uint8_t *p = (const uint8_t *)src;
    uint32_t misses = 0;
    while (bytes) {
        if (!hwgpu_alive(c, generation)) return 0;
        size_t n = hwgpu_write_some(data, cap, head, tail, p, bytes);
        if (!n) { hwgpu_backoff(&misses); continue; }
        misses = 0;
        p += n;
        bytes -= n;
    }
    __atomic_fetch_add(to_phone ? &c->submits_to_phone : &c->submits_to_guest, 1, __ATOMIC_RELAXED);
    return 1;
}

static inline int hwgpu_read_all(void *chan, uint32_t generation, int from_phone, void *dst, size_t bytes)
{
    struct hwgpu_ctl *c = hwgpu_chan_ctl(chan);
    const uint8_t *data = from_phone ? hwgpu_rx(chan) : hwgpu_tx(chan);
    uint32_t cap = from_phone ? HWGPU_RX_BYTES : HWGPU_TX_BYTES;
    uint64_t *head = from_phone ? &c->rx_head : &c->tx_head, *tail = from_phone ? &c->rx_tail : &c->tx_tail;
    uint8_t *p = (uint8_t *)dst;
    uint32_t misses = 0;
    while (bytes) {
        if (!hwgpu_alive(c, generation)) return 0;
        size_t n = hwgpu_read_some(data, cap, head, tail, p, bytes);
        if (!n) { hwgpu_backoff(&misses); continue; }
        misses = 0;
        p += n;
        bytes -= n;
    }
    return 1;
}
#endif /* HWGPU_KERNEL */

#if defined(_WIN32) && !defined(HWGPU_KERNEL) && defined(HWGPU_WIN32_FULLSCREEN_HELPER)
#include <stdlib.h>
#include <string.h>
/* Shared guest helper for Vulkan, OpenGL (Zink), D3D12 (vkd3d), D3D10/11 and D3D9:
 *   1. Detects whether a window fills its monitor and is unoccluded by any visible non-cloaked
 *      window above it (exclusive fullscreen or borderless fullscreen).
 *   2. Tracks the Windows cursor shape and position in hwgpu_ctl so the phone can show a cursor
 *      sublayer above the fullscreen game.
 *   3. Runs a lightweight background watcher while fullscreen is active so that if a popup,
 *      Alt-Tab, touch keyboard, or dialog covers the game while its render thread is paused,
 *      scanout_active drops to 0 within ~32 ms and the phone restores the DWM desktop at once. */

static inline int hwgpu_window_fills_screen(HWND wnd)
{
    if (!wnd || !IsWindow(wnd) || !IsWindowVisible(wnd) || IsIconic(wnd)) return 0;
    HWND root = GetAncestor(wnd, GA_ROOT);
    if (!root) root = wnd;
    HWND fg = GetForegroundWindow();
    if (!fg || (fg != wnd && fg != root)) return 0;
    MONITORINFO mi;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    RECT rc;
    POINT tl = {0, 0};
    if (!GetMonitorInfoA(MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST), &mi) ||
        !GetClientRect(wnd, &rc) || !ClientToScreen(wnd, &tl))
        return 0;
    if (!(tl.x <= mi.rcMonitor.left && tl.y <= mi.rcMonitor.top &&
          tl.x + rc.right >= mi.rcMonitor.right && tl.y + rc.bottom >= mi.rcMonitor.bottom))
        return 0;
    typedef HRESULT(WINAPI * cloaked_fn)(HWND, DWORD, PVOID, DWORD);
    static cloaked_fn get_attr;
    static int looked;
    if (!looked) {
        HMODULE dwm = LoadLibraryA("dwmapi.dll");
        get_attr = dwm ? (cloaked_fn)(void *)GetProcAddress(dwm, "DwmGetWindowAttribute") : NULL;
        looked = 1;
    }
    for (HWND w = GetWindow(root, GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV)) {
        if (!IsWindowVisible(w) || IsIconic(w)) continue;
        DWORD cloaked = 0;
        if (get_attr && SUCCEEDED(get_attr(w, 14 /* DWMWA_CLOAKED */, &cloaked, sizeof(cloaked))) && cloaked)
            continue;
        RECT wr, both;
        if (GetWindowRect(w, &wr) && IntersectRect(&both, &wr, &mi.rcMonitor)) {
            /* Ignore zero-area or transparent tool helper windows owned by the game itself that
             * have no visible region; any real popup/dialog/overlay blocks fullscreen scanout. */
            LONG ex = GetWindowLongA(w, GWL_EXSTYLE);
            if ((ex & WS_EX_TRANSPARENT) && (ex & WS_EX_LAYERED)) {
                BYTE alpha = 255;
                DWORD flags = 0;
                if (GetLayeredWindowAttributes(w, NULL, &alpha, &flags) && (flags & LWA_ALPHA) && alpha == 0)
                    continue;
            }
            return 0;
        }
    }
    return 1;
}

/* A windowed program whose client area is showing: the phone may show its frames on a layer of their
 * own, over the desktop, at the client rect, and Windows never sees the pixels. Walks the windows above
 * it; a window that covers a whole edge strip of what is left (the taskbar, the screen's edge) only trims
 * the visible part, anything else refuses. client_out: the client rect in screen pixels (it may run past
 * the screen). vis_out: the part to show, in client pixels. */
static inline int hwgpu_window_overlay_ok(HWND wnd, RECT *client_out, RECT *vis_out)
{
    if (!wnd || !IsWindow(wnd) || !IsWindowVisible(wnd) || IsIconic(wnd)) return 0;
    HWND root = GetAncestor(wnd, GA_ROOT);
    if (!root) root = wnd;
    MONITORINFO mi;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    RECT rc, cr, vis;
    POINT tl = {0, 0};
    if (!GetMonitorInfoA(MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST), &mi) ||
        !GetClientRect(wnd, &rc) || !ClientToScreen(wnd, &tl))
        return 0;
    cr.left = tl.x;
    cr.top = tl.y;
    cr.right = tl.x + rc.right;
    cr.bottom = tl.y + rc.bottom;
    if (cr.right <= cr.left || cr.bottom <= cr.top || !IntersectRect(&vis, &cr, &mi.rcMonitor))
        return 0;
    typedef HRESULT(WINAPI * cloaked_fn)(HWND, DWORD, PVOID, DWORD);
    static cloaked_fn get_attr;
    static int looked;
    if (!looked) {
        HMODULE dwm = LoadLibraryA("dwmapi.dll");
        get_attr = dwm ? (cloaked_fn)(void *)GetProcAddress(dwm, "DwmGetWindowAttribute") : NULL;
        looked = 1;
    }
    for (HWND w = GetWindow(root, GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV)) {
        if (!IsWindowVisible(w) || IsIconic(w)) continue;
        DWORD cloaked = 0;
        if (get_attr && SUCCEEDED(get_attr(w, 14 /* DWMWA_CLOAKED */, &cloaked, sizeof(cloaked))) && cloaked)
            continue;
        RECT o, both;
        if (!GetWindowRect(w, &o) || !IntersectRect(&both, &o, &vis)) continue;
        LONG ex = GetWindowLongA(w, GWL_EXSTYLE);
        if ((ex & WS_EX_TRANSPARENT) && (ex & WS_EX_LAYERED)) {
            BYTE alpha = 255;
            DWORD flags = 0;
            if (GetLayeredWindowAttributes(w, NULL, &alpha, &flags) && (flags & LWA_ALPHA) && alpha == 0)
                continue;
        }
        if (o.left <= vis.left && o.right >= vis.right) {
            if (o.top <= vis.top) vis.top = o.bottom < vis.bottom ? o.bottom : vis.bottom;
            else if (o.bottom >= vis.bottom) vis.bottom = o.top > vis.top ? o.top : vis.top;
            else return 0;
        } else if (o.top <= vis.top && o.bottom >= vis.bottom) {
            if (o.left <= vis.left) vis.left = o.right < vis.right ? o.right : vis.right;
            else if (o.right >= vis.right) vis.right = o.left > vis.left ? o.left : vis.left;
            else return 0;
        } else {
            return 0;
        }
        if (vis.right <= vis.left || vis.bottom <= vis.top) return 0;
    }
    if (client_out) *client_out = cr;
    if (vis_out) {
        vis_out->left = vis.left - cr.left;
        vis_out->top = vis.top - cr.top;
        vis_out->right = vis.right - cr.left;
        vis_out->bottom = vis.bottom - cr.top;
    }
    return 1;
}

static int g_hwgpu_watch_overlay;

static inline int hwgpu_extract_cursor_rgba(HCURSOR hcur, uint32_t *out_w, uint32_t *out_h,
                                            int32_t *out_hx, int32_t *out_hy, uint32_t rgba[64 * 64])
{
    if (!hcur) return 0;
    ICONINFO ii;
    memset(&ii, 0, sizeof(ii));
    if (!GetIconInfo(hcur, &ii)) return 0;
    BITMAP bm;
    memset(&bm, 0, sizeof(bm));
    HBITMAP hbm = ii.hbmColor ? ii.hbmColor : ii.hbmMask;
    if (!hbm || !GetObjectA(hbm, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) {
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        return 0;
    }
    uint32_t src_w = (uint32_t)bm.bmWidth;
    uint32_t src_h = (uint32_t)(ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2);
    if (!src_w || !src_h) {
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        return 0;
    }
    uint32_t cw = src_w > 64 ? 64 : src_w;
    uint32_t ch = src_h > 64 ? 64 : src_h;
    *out_w = cw;
    *out_h = ch;
    *out_hx = (int32_t)ii.xHotspot;
    *out_hy = (int32_t)ii.yHotspot;
    memset(rgba, 0, 64 * 64 * sizeof(uint32_t));

    HDC hdc = GetDC(NULL);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = (LONG)src_w;
    bi.bmiHeader.biHeight = -(LONG)(ii.hbmColor ? src_h : src_h * 2);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    uint32_t total_rows = ii.hbmColor ? src_h : src_h * 2;
    uint32_t *cbuf = (uint32_t *)calloc((size_t)src_w * total_rows, sizeof(uint32_t));
    uint32_t *mbuf = (uint32_t *)calloc((size_t)src_w * total_rows, sizeof(uint32_t));
    if (cbuf && mbuf && hdc) {
        if (ii.hbmColor) {
            GetDIBits(hdc, ii.hbmColor, 0, src_h, cbuf, &bi, DIB_RGB_COLORS);
            if (ii.hbmMask) GetDIBits(hdc, ii.hbmMask, 0, src_h, mbuf, &bi, DIB_RGB_COLORS);
            int has_alpha = 0;
            for (uint32_t i = 0; i < src_w * src_h; i++) {
                if (cbuf[i] & 0xFF000000u) { has_alpha = 1; break; }
            }
            for (uint32_t y = 0; y < ch; y++) {
                for (uint32_t x = 0; x < cw; x++) {
                    uint32_t c = cbuf[y * src_w + x];
                    uint32_t m = ii.hbmMask ? mbuf[y * src_w + x] : 0;
                    uint32_t a = has_alpha ? (c >> 24) : ((m & 0xFFFFFFu) == 0 ? 0xFFu : ((c & 0xFFFFFFu) ? 0xFFu : 0x00u));
                    uint32_t r = (c >> 16) & 0xFFu, g = (c >> 8) & 0xFFu, b = c & 0xFFu;
                    rgba[y * cw + x] = r | (g << 8) | (b << 16) | (a << 24);
                }
            }
        } else if (ii.hbmMask) {
            GetDIBits(hdc, ii.hbmMask, 0, src_h * 2, mbuf, &bi, DIB_RGB_COLORS);
            for (uint32_t y = 0; y < ch; y++) {
                for (uint32_t x = 0; x < cw; x++) {
                    uint32_t and_m = mbuf[y * src_w + x] & 0xFFFFFFu;
                    uint32_t xor_m = mbuf[(y + src_h) * src_w + x] & 0xFFFFFFu;
                    uint32_t px = 0;
                    if (and_m == 0 && xor_m == 0) px = 0xFF000000u;
                    else if (and_m == 0 && xor_m != 0) px = 0xFFFFFFFFu;
                    else if (and_m != 0 && xor_m != 0) px = 0xFFFFFFFFu;
                    rgba[y * cw + x] = px;
                }
            }
        }
    }
    free(cbuf);
    free(mbuf);
    if (hdc) ReleaseDC(NULL, hdc);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return 1;
}

static struct hwgpu_ctl * volatile g_hwgpu_watch_ctl;
static HWND volatile g_hwgpu_watch_wnd;
static HANDLE g_hwgpu_watch_thread;
static HCURSOR g_hwgpu_last_hcursor;

static inline void hwgpu_publish_cursor_pos(struct hwgpu_ctl *ctl, HWND wnd, CURSORINFO *out_ci)
{
    CURSORINFO ci;
    memset(&ci, 0, sizeof(ci));
    ci.cbSize = sizeof(ci);
    int vis = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor != NULL;
    if (out_ci) *out_ci = ci;
    MONITORINFO mi;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    int32_t cx = 0, cy = 0;
    uint32_t mw = 0, mh = 0;
    if (wnd && GetMonitorInfoA(MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST), &mi)) {
        cx = (int32_t)(ci.ptScreenPos.x - mi.rcMonitor.left);
        cy = (int32_t)(ci.ptScreenPos.y - mi.rcMonitor.top);
        mw = (uint32_t)(mi.rcMonitor.right - mi.rcMonitor.left);
        mh = (uint32_t)(mi.rcMonitor.bottom - mi.rcMonitor.top);
    }
    if ((int)ctl->cursor_visible != vis || ctl->cursor_x != cx || ctl->cursor_y != cy ||
        ctl->cursor_mon_w != mw || ctl->cursor_mon_h != mh) {
        ctl->cursor_x = cx;
        ctl->cursor_y = cy;
        ctl->cursor_mon_w = mw;
        ctl->cursor_mon_h = mh;
        __atomic_store_n(&ctl->cursor_visible, (uint32_t)vis, __ATOMIC_RELEASE);
        __atomic_add_fetch(&ctl->cursor_seq, 1u, __ATOMIC_RELEASE);
    }
}

static DWORD WINAPI hwgpu_fullscreen_watch_proc(LPVOID param)
{
    (void)param;
    int unfocused = 0;
    for (;;) {
        hwgpu_pause(16000);
        struct hwgpu_ctl *ctl = g_hwgpu_watch_ctl;
        HWND wnd = g_hwgpu_watch_wnd;
        if (!ctl || !wnd) {
            unfocused = 0;
            continue;
        }
        if (g_hwgpu_watch_overlay ? hwgpu_window_overlay_ok(wnd, NULL, NULL) : hwgpu_window_fills_screen(wnd)) {
            unfocused = 0;
            hwgpu_publish_cursor_pos(ctl, wnd, NULL);
        } else if (++unfocused >= 2) {
            /* Covered by another window or lost focus while not presenting: drop scanout at once. */
            g_hwgpu_watch_wnd = NULL;
            __atomic_store_n(&ctl->cursor_visible, 0u, __ATOMIC_RELEASE);
            __atomic_store_n(&ctl->scanout_active, 0u, __ATOMIC_RELEASE);
            __atomic_add_fetch(&ctl->cursor_seq, 1u, __ATOMIC_RELEASE);
            InvalidateRect(wnd, NULL, FALSE);
            unfocused = 0;
        }
    }
    return 0;
}

static inline void hwgpu_update_cursor_ex(
    struct hwgpu_ctl *ctl, HWND wnd,
    void (*send_shape)(uint32_t w, uint32_t h, int32_t hx, int32_t hy, const uint32_t *rgba), int overlay)
{
    if (!ctl || !wnd) return;
    g_hwgpu_watch_overlay = overlay;
    __atomic_store_n(&ctl->scanout_active, 1u, __ATOMIC_RELEASE);
    g_hwgpu_watch_ctl = ctl;
    g_hwgpu_watch_wnd = wnd;
    if (!g_hwgpu_watch_thread) {
        HANDLE th = CreateThread(NULL, 0, hwgpu_fullscreen_watch_proc, NULL, 0, NULL);
        if (th) g_hwgpu_watch_thread = th;
    }
    CURSORINFO ci;
    hwgpu_publish_cursor_pos(ctl, wnd, &ci);
    if ((ci.flags & CURSOR_SHOWING) && ci.hCursor && ci.hCursor != g_hwgpu_last_hcursor && send_shape) {
        uint32_t cw = 0, ch = 0, rgba[64 * 64];
        int32_t hx = 0, hy = 0;
        if (hwgpu_extract_cursor_rgba(ci.hCursor, &cw, &ch, &hx, &hy, rgba)) {
            g_hwgpu_last_hcursor = ci.hCursor;
            send_shape(cw, ch, hx, hy, rgba);
        }
    }
}

static inline void hwgpu_update_fullscreen_cursor(
    struct hwgpu_ctl *ctl, HWND wnd,
    void (*send_shape)(uint32_t w, uint32_t h, int32_t hx, int32_t hy, const uint32_t *rgba))
{
    hwgpu_update_cursor_ex(ctl, wnd, send_shape, 0);
}

static inline void hwgpu_stop_fullscreen_watch(struct hwgpu_ctl *ctl)
{
    g_hwgpu_watch_wnd = NULL;
    if (ctl) {
        __atomic_store_n(&ctl->cursor_visible, 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&ctl->scanout_active, 0u, __ATOMIC_RELEASE);
        __atomic_add_fetch(&ctl->cursor_seq, 1u, __ATOMIC_RELEASE);
    }
}
#endif /* _WIN32 && !HWGPU_KERNEL && HWGPU_WIN32_FULLSCREEN_HELPER */

#endif
