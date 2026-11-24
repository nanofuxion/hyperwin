/* HwD3D's Windows side of the channel (include/hw_d3d.h): opens the phone's channel through the WDDM
 * adapter, sends and reads messages, stages big arrays. Header only, static, valid as C and C++. Included
 * by the per-program DLLs (d3d11.c) and by the Direct3D driver (umd.cpp). */
#ifndef HWD3D_WIRE_H
#define HWD3D_WIRE_H
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef DXGI_ERROR_DEVICE_REMOVED
#define DXGI_ERROR_DEVICE_REMOVED ((HRESULT)0x887A0005L)
#endif
#include <hw_d3d.h>
#include <hwgpu_ioctl.h>
#include <hwgpu_ring.h>
#include <hww_user.h>

/* ---- the channel ---- */

static void *g_chan;
static uint32_t g_gen, g_seq;
static struct hww *g_adapter;          /* the WDDM adapter, when the channel came from it */
static uint8_t *g_stage;               /* the staging area (hw_d3d.h), or null */
static uint8_t *g_readback;            /* phone->guest readback window in g_stage (HWD3D_READBACK_SIZE) */
static uint64_t g_stage_cap, g_stage_pos;
static volatile struct hwd3d_query_slot *g_qpage;  /* query results (hw_d3d.h), or null */
static uint8_t g_qused[HWD3D_QUERY_SLOTS];
static SRWLOCK g_lock = SRWLOCK_INIT;  /* one call at a time on the channel */
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static int g_ok;                       /* the phone speaks HwD3D */
static HANDLE g_dev = INVALID_HANDLE_VALUE;  /* the legacy device, when the channel did not come from the adapter */
static uint32_t g_slot;
static int g_stage_tried;              /* the staging area has been asked for */

static void hlog(const char *fmt, ...)
{
    char s[512];
    va_list a;
    va_start(a, fmt);
    vsnprintf(s, sizeof s, fmt, a);
    va_end(a);
    OutputDebugStringA(s);
    if (getenv("HWD3D_LOG")) fputs(s, stderr);
}

/* ---- the frame profile (speed plan, phase 0) ----
 * HWD3D_PROFILE=1 (environment, or HKLM\SOFTWARE\HyperWin\Gpu): every 2 s, and at each present, the program says
 * where a frame went on this side: calls sent without waiting (and the ring time they took, which includes waiting
 * for room), calls that waited for the phone's answer, by method, and the time the lock waited for other threads.
 * It writes to C:\Users\Public\hwd3d_prof.log, or HWD3D_PROFILE_LOG. */
static int g_prof = -1;
static LARGE_INTEGER g_pq_freq;
#define PROF_KEYS 8192
static struct { uint64_t n, ns; } g_prof_wait[PROF_KEYS];
static uint64_t g_prof_async_n, g_prof_async_ns, g_prof_async_bytes, g_prof_wait_n, g_prof_wait_ns, g_prof_lock_ns;
static uint64_t g_prof_frames, g_prof_t0, g_prof_last;

static int prof_on(void)
{
    if (g_prof >= 0) return g_prof;
    char v[8] = "";
    g_prof = GetEnvironmentVariableA("HWD3D_PROFILE", v, sizeof v) && v[0] == '1';
    if (!g_prof) {
        HKEY k;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\HyperWin\\Gpu", 0, KEY_READ | KEY_WOW64_64KEY, &k) == ERROR_SUCCESS) {
            DWORD type = 0, len = sizeof v;
            if (RegQueryValueExA(k, "HWD3D_PROFILE", NULL, &type, (BYTE *)v, &len) == ERROR_SUCCESS)
                g_prof = (type == REG_DWORD && *(DWORD *)v) || (type == REG_SZ && v[0] == '1');
            RegCloseKey(k);
        }
    }
    QueryPerformanceFrequency(&g_pq_freq);
    return g_prof;
}

static uint64_t prof_now(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)((double)t.QuadPart * 1e9 / (double)g_pq_freq.QuadPart);
}

static void prof_out(const char *fmt, ...)
{
    char s[600];
    va_list a;
    va_start(a, fmt);
    int n = vsnprintf(s, sizeof s - 2, fmt, a);
    va_end(a);
    if (n <= 0) return;
    s[n++] = '\n';
    char path[MAX_PATH];
    if (!GetEnvironmentVariableA("HWD3D_PROFILE_LOG", path, sizeof path)) lstrcpyA(path, "C:\\Users\\Public\\hwd3d_prof.log");
    HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteFile(f, s, (DWORD)n, &w, NULL);
        CloseHandle(f);
    }
}

/* One frame ended (a present). Under no lock: it only reads and clears the counters, so another thread's call in
 * the same instant may be counted in the next window, which is fine for a profile. */
static void prof_frame(void)
{
    if (!prof_on()) return;
    uint64_t now = prof_now();
    if (!g_prof_t0) g_prof_t0 = g_prof_last = now;
    g_prof_frames++;
    if (now - g_prof_last < 2000000000ull) return;
    double secs = (double)(now - g_prof_t0) / 1e9, f = (double)g_prof_frames;
    char exe[MAX_PATH] = "";
    GetModuleFileNameA(NULL, exe, sizeof exe);
    const char *base = exe;
    for (const char *q = exe; *q; q++) if (*q == '\\') base = q + 1;
    prof_out("%s pid %lu: %.1f fps (%llu frames, %.1f s); per frame: %.1f calls sent without waiting (%.0f bytes, %.3f ms in the ring), "
             "%.1f calls waited for an answer (%.3f ms), %.3f ms waiting for the lock; the rest of the %.3f ms is the program and the driver",
             base, (unsigned long)GetCurrentProcessId(), f / secs, (unsigned long long)g_prof_frames, secs, g_prof_async_n / f,
             g_prof_async_bytes / f, g_prof_async_ns / f / 1e6, g_prof_wait_n / f, g_prof_wait_ns / f / 1e6, g_prof_lock_ns / f / 1e6,
             secs * 1000.0 / f - (g_prof_async_ns + g_prof_wait_ns + g_prof_lock_ns) / f / 1e6);
    for (int round = 0; round < 12; round++) {
        int best = -1;
        for (int i = 0; i < PROF_KEYS; i++)
            if (g_prof_wait[i].ns && (best < 0 || g_prof_wait[i].ns > g_prof_wait[best].ns)) best = i;
        if (best < 0) break;
        prof_out("    waited: op %d method %d: %.2f calls and %.3f ms per frame (%.1f us each)", best >> 9, best & 511,
                 g_prof_wait[best].n / f, g_prof_wait[best].ns / f / 1e6, g_prof_wait[best].ns / (double)g_prof_wait[best].n / 1e3);
        g_prof_wait[best].ns = 0;
    }
    memset(g_prof_wait, 0, sizeof g_prof_wait);
    g_prof_async_n = g_prof_async_ns = g_prof_async_bytes = g_prof_wait_n = g_prof_wait_ns = g_prof_lock_ns = 0;
    g_prof_frames = 0;
    g_prof_t0 = g_prof_last = now;
}

static int raw_send(const void *m, uint32_t n) { return hwgpu_write_all(g_chan, g_gen, 1, m, n); }
static int raw_recv(void *m, uint32_t n) { return hwgpu_read_all(g_chan, g_gen, 1, m, n); }

/* Tells the phone which program this is and what its dxvk.conf says, so DXVK's per-game settings apply. */
static void send_program(void)
{
    WCHAR path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    char utf8[4 * MAX_PATH];
    int pb = WideCharToMultiByte(CP_UTF8, 0, path, (int)n, utf8, sizeof utf8, NULL, NULL);
    if (pb <= 0) return;
    /* dxvk.conf beside the program: up to 64 KiB of it. */
    WCHAR conf[MAX_PATH + 16];
    lstrcpyW(conf, path);
    WCHAR *slash = conf + n;
    while (slash > conf && slash[-1] != L'\\') slash--;
    lstrcpyW(slash, L"dxvk.conf");
    static char text[64 << 10];
    DWORD got = 0;
    HANDLE f = CreateFileW(conf, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        if (!ReadFile(f, text, sizeof text, &got, NULL)) got = 0;
        CloseHandle(f);
    }
    uint32_t pp = ((uint32_t)pb + 3) & ~3u, cp = (got + 3) & ~3u;
    uint32_t total = (uint32_t)sizeof(struct hwd3d_program) + pp + cp;
    uint8_t *m = (uint8_t *)calloc(1, total);
    if (!m) return;
    struct hwd3d_program *p = (struct hwd3d_program *)m;
    p->hdr = (struct hwd3d_msg){total, HWD3D_MAGIC, HWD3D_OP_PROGRAM, HWD3D_NO_REPLY, ++g_seq};
    p->path_bytes = (uint32_t)pb;
    p->conf_bytes = got;
    memcpy(m + sizeof *p, utf8, (size_t)pb);
    memcpy(m + sizeof *p + pp, text, got);
    raw_send(m, total);
    free(m);
}

static void connect_channel(void)
{
    /* The WDDM adapter first (include/hw_wddm.h), the legacy \\.\HwGpu device without it. */
    struct hwgpu_open_out o;
    DWORD got = 0;
    HANDLE dev = INVALID_HANDLE_VALUE;
    static struct hww adapter;
    struct hww_open wo;
    int opened = g_adapter && hww_open_channel(&adapter, &wo) >= 0;
    if (!opened && g_adapter) {   /* the adapter was replaced under us (a driver update): open the new one */
        hww_close(&adapter);
        g_adapter = NULL;
    }
    if (!opened && hww_open(&adapter)) {
        if (hww_open_channel(&adapter, &wo) < 0) {
            hww_close(&adapter);
            return;
        }
        opened = 1;
    }
    if (opened) {
        o.chan = wo.chan;
        o.slot = wo.slot;
        o.generation = wo.generation;
        g_adapter = &adapter;
    } else {
        dev = CreateFileW(HWGPU_USER_PATH, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (dev == INVALID_HANDLE_VALUE) return;
        if (!DeviceIoControl(dev, IOCTL_HWGPU_OPEN, NULL, 0, &o, sizeof o, &got, NULL) || got != sizeof o) {
            CloseHandle(dev);
            return;
        }
    }
    /* The handle stays open for the life of the process: closing it frees the channel. */
    g_chan = (void *)(uintptr_t)o.chan;
    g_gen = o.generation;
    struct hwgpu_ctl *c = hwgpu_chan_ctl(g_chan);
    for (int i = 0; i < 2000 && __atomic_load_n(&c->host_state, __ATOMIC_ACQUIRE) != HWGPU_HOST_READY; i++) Sleep(1);
    struct hwd3d_hello h = {{sizeof h, HWD3D_MAGIC, HWD3D_OP_HELLO, 0, ++g_seq}, HWD3D_VERSION, GetCurrentProcessId(),
#ifdef __arm64ec__
                            2,
#else
                            1,
#endif
                            0};
    struct hwd3d_hello_reply r = {0};
    if (raw_send(&h, sizeof h) && raw_recv(&r, sizeof r) && r.r.hdr.magic == HWD3D_MAGIC && r.r.status == 0 &&
        r.version == HWD3D_VERSION) {
        g_ok = 1;
        g_dev = dev;
        g_slot = o.slot;
        send_program();
    } else {
        hlog("hwd3d: the phone has no HwD3D %u (status 0x%lx), using Windows' Direct3D\n", HWD3D_VERSION,
             (unsigned long)r.r.status);
        if (opened && g_adapter) {
            struct hww_escape cl;
            memset(&cl, 0, sizeof cl);
            cl.op = HWW_ESC_CLOSE;
            cl.u.slot = o.slot;
            hww_call(g_adapter, &cl);
        } else if (dev != INVALID_HANDLE_VALUE) {
            CloseHandle(dev);
        }
        g_chan = NULL;
        g_gen = 0;
    }
}

static ULONGLONG g_last_connect_try;

static void close_channel_locked(int drop_adapter)
{
    if (g_adapter && g_chan) {
        struct hww_escape cl;
        memset(&cl, 0, sizeof cl);
        cl.op = HWW_ESC_CLOSE;
        cl.u.slot = g_slot;
        hww_call(g_adapter, &cl);
    }
    if (g_dev != INVALID_HANDLE_VALUE) {
        CloseHandle(g_dev);
        g_dev = INVALID_HANDLE_VALUE;
    }
    if (drop_adapter && g_adapter) {
        hww_close(g_adapter);
        g_adapter = NULL;
    }
    g_chan = NULL;
    g_ok = 0;
    g_gen = 0;
    g_slot = 0;
    g_stage = NULL;
    g_readback = NULL;
    g_stage_cap = g_stage_pos = 0;
    g_qpage = NULL;
    memset(g_qused, 0, sizeof g_qused);
    g_stage_tried = 0;
    g_last_connect_try = 0;
}

static void close_channel(void)
{
    AcquireSRWLockExclusive(&g_lock);
    close_channel_locked(1);
    ReleaseSRWLockExclusive(&g_lock);
}

static BOOL CALLBACK open_channel(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    if (!getenv("HWD3D_OFF")) connect_channel();
    return TRUE;
}

/* The phone's GPU process restarted (a driver swap, or it was stopped): every object on the old
 * channel is gone and the devices on it are removed. A program that makes a new device, such as the
 * screen driver or DWM rebuilding, gets a new channel. The old one is closed so long-lived processes
 * do not exhaust HWGPU_CHANNELS. */
/* A driver update takes the channel's memory away from the process: reading it then would fault. */
static int chan_alive(void)
{
    if (!g_chan) return 0;
    struct hwgpu_ctl *c = hwgpu_chan_ctl(g_chan);
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(c, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    return hwgpu_alive(c, g_gen);
}

static void reconnect_if_lost(void)
{
    if (g_ok && chan_alive()) return;
    if (getenv("HWD3D_OFF")) return;
    ULONGLONG now = GetTickCount64();
    if (!g_ok && g_last_connect_try && now - g_last_connect_try < 250) return;
    AcquireSRWLockExclusive(&g_lock);
    if (!g_ok || !chan_alive()) {
        now = GetTickCount64();
        if (g_ok || !g_last_connect_try || now - g_last_connect_try >= 250) {
            if (g_ok) hlog("hwd3d: the phone's GPU restarted, opening a new channel\n");
            close_channel_locked(0);
            g_last_connect_try = now;
            connect_channel();
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

static int ready(void)
{
    InitOnceExecuteOnce(&g_once, open_channel, NULL, NULL);
    if (g_prof < 0) prof_on();
    reconnect_if_lost();
    return g_ok;
}

/* A request being written, and an answer being read. Both under g_lock. */
struct wbuf { uint8_t *p; uint32_t n, cap; uint16_t op; };
struct rd { const uint8_t *p, *end; };
static struct wbuf g_w;
static uint8_t *g_rbuf;
static uint32_t g_rcap;

static void reserve(struct wbuf *b, uint32_t n)
{
    if (b->n + n <= b->cap) return;
    uint32_t cap = b->cap ? b->cap : 4096;
    while (cap < b->n + n) cap *= 2;
    b->p = (uint8_t *)realloc(b->p, cap);
    b->cap = cap;
}

static void put(struct wbuf *b, const void *v, uint32_t n)
{
    uint32_t padded = (n + 3) & ~3u;
    reserve(b, padded);
    memcpy(b->p + b->n, v, n);
    memset(b->p + b->n + n, 0, padded - n);
    b->n += padded;
}

static void put32(struct wbuf *b, uint32_t v) { put(b, &v, 4); }
static void put_want(struct wbuf *b, const void *p) { put32(b, p != NULL); }

/* The staging area, where big uploads go instead of the ring, is asked of the phone when a program first has
 * one. Many programs never do, and each area is a share of a heap that holds frames and Vulkan memory too.
 * Without it, data rides inline. Under g_lock; nothing of the request being built has been sent. */
#define HWD3D_STAGING_MIN (32u << 10)
static void ensure_staging(void)
{
    if (g_stage || g_stage_tried || !g_ok) return;
    g_stage_tried = 1;
    struct hwd3d_msg m = {sizeof m, HWD3D_MAGIC, HWD3D_OP_STAGING, 0, ++g_seq};
    struct hwd3d_hello_reply r;
    memset(&r, 0, sizeof r);
    if (!raw_send(&m, sizeof m) || !raw_recv(&r, sizeof r) || r.r.hdr.magic != HWD3D_MAGIC || r.r.status != 0) return;
    struct hwgpu_map_in in = {r.staging_offset, r.staging_size};
    struct hwgpu_map_out out = {0};
    DWORD got = 0;
    int mapped = 0;
    if (r.staging_size > HWD3D_STAGING_DATA) {
        if (g_adapter) mapped = hww_map(g_adapter, g_slot, r.staging_offset, r.staging_size, &out.user) >= 0 && out.user;
        else mapped = DeviceIoControl(g_dev, IOCTL_HWGPU_MAP, &in, sizeof in, &out, sizeof out, &got, NULL) && out.user;
    }
    if (mapped && r.staging_size > HWD3D_STAGING_DATA + HWD3D_QUERY_PAGE + HWD3D_READBACK_SIZE) {
        g_stage = (uint8_t *)(uintptr_t)out.user;
        g_stage_cap = r.staging_size - HWD3D_STAGING_DATA - HWD3D_QUERY_PAGE - HWD3D_READBACK_SIZE;
        g_readback = g_stage + r.staging_size - HWD3D_QUERY_PAGE - HWD3D_READBACK_SIZE;
        g_qpage = (volatile struct hwd3d_query_slot *)(g_stage + r.staging_size - HWD3D_QUERY_PAGE);
    }
}

/* Room for n bytes in the staging area, or null when it is missing or the data is too big for it.
 * Waits while the phone still needs what is there. Under g_lock, before the request is sent: every
 * request the phone is waiting on to free room has gone already. */
static uint8_t *stage_alloc(uint64_t n, uint64_t *pos)
{
    if (!g_stage && n >= HWD3D_STAGING_MIN) ensure_staging();
    if (!g_stage || n > g_stage_cap / 2) return NULL;
    uint64_t at = g_stage_pos % g_stage_cap;
    if (at + n > g_stage_cap) g_stage_pos += g_stage_cap - at;  /* one array never wraps */
    const struct hwd3d_staging *sc = (const struct hwd3d_staging *)g_stage;
    struct hwgpu_ctl *c = hwgpu_chan_ctl(g_chan);
    while (g_stage_pos + n > __atomic_load_n(&sc->done, __ATOMIC_ACQUIRE) + g_stage_cap) {
        if (!hwgpu_alive(c, g_gen)) return NULL;
        SwitchToThread();
    }
    *pos = g_stage_pos;
    g_stage_pos += (n + 15) & ~15ull;
    return g_stage + HWD3D_STAGING_DATA + *pos % g_stage_cap;
}

static void put_staged(struct wbuf *b, uint64_t count, uint64_t pos)
{
    put32(b, HWD3D_STAGED);
    put32(b, (uint32_t)count);
    put(b, &pos, 8);
}

/* An array: present, count, the elements, or where they are staged. A count past any real use
 * (D3D11_KEEP_*) sends none. */
static void put_arr(struct wbuf *b, const void *p, uint64_t n, uint32_t el)
{
    if (!p || n > (HWD3D_MAX_MSG / (el ? el : 1))) {
        put32(b, 0);
        return;
    }
    uint64_t pos;
    uint8_t *d = n * el >= 256 ? stage_alloc(n * el, &pos) : NULL;
    if (d) {
        memcpy(d, p, (size_t)(n * el));
        put_staged(b, n, pos);
        return;
    }
    put32(b, HWD3D_INLINE);
    put32(b, (uint32_t)n);
    put(b, p, (uint32_t)(n * el));
}

static struct wbuf *begin(uint32_t object, uint32_t method, uint16_t op)
{
    if (g_prof > 0) {
        uint64_t t = prof_now();
        AcquireSRWLockExclusive(&g_lock);
        g_prof_lock_ns += prof_now() - t;
    } else
        AcquireSRWLockExclusive(&g_lock);
    struct wbuf *b = &g_w;
    b->n = 0;
    b->op = op;
    reserve(b, sizeof(struct hwd3d_call));
    struct hwd3d_call *c = (struct hwd3d_call *)b->p;
    memset(c, 0, sizeof *c);
    c->object = object;
    c->method = method;
    b->n = sizeof *c;
    return b;
}

static void seal(struct wbuf *b, uint16_t flags)
{
    struct hwd3d_msg *h = (struct hwd3d_msg *)b->p;
    h->size = b->n;
    h->magic = HWD3D_MAGIC;
    h->op = b->op;
    h->flags = flags;
    h->seq = ++g_seq;
}

static void call_abort(struct wbuf *b)
{
    (void)b;
    ReleaseSRWLockExclusive(&g_lock);
}

static void call_async(struct wbuf *b)
{
    seal(b, HWD3D_NO_REPLY);
    if (g_prof > 0) {
        uint64_t t = prof_now();
        raw_send(b->p, b->n);
        g_prof_async_ns += prof_now() - t;
        g_prof_async_n++;
        g_prof_async_bytes += b->n;
    } else
        raw_send(b->p, b->n);
    ReleaseSRWLockExclusive(&g_lock);
}

/* Sends and reads the answer. The lock stays held until call_end, while the outputs are read. */
static int32_t call_wait_(struct wbuf *b, struct rd *r);
static int32_t call_wait(struct wbuf *b, struct rd *r)
{
    if (g_prof <= 0) return call_wait_(b, r);
    uint64_t t = prof_now();
    int key = (int)((b->op & 15) << 9 | (((struct hwd3d_call *)b->p)->method & 511));
    int32_t st = call_wait_(b, r);
    uint64_t d = prof_now() - t;
    g_prof_wait[key].n++;
    g_prof_wait[key].ns += d;
    g_prof_wait_n++;
    g_prof_wait_ns += d;
    return st;
}

static int32_t call_wait_(struct wbuf *b, struct rd *r)
{
    seal(b, 0);
    struct hwd3d_reply rep;
    r->p = r->end = g_rbuf;
    if (!raw_send(b->p, b->n) || !raw_recv(&rep, sizeof rep) || rep.hdr.magic != HWD3D_MAGIC ||
        rep.hdr.size < sizeof rep || rep.hdr.size > HWD3D_MAX_MSG)
        return (int32_t)DXGI_ERROR_DEVICE_REMOVED;
    uint32_t n = rep.hdr.size - sizeof rep;
    if (n > g_rcap) {
        g_rbuf = (uint8_t *)realloc(g_rbuf, n);
        g_rcap = n;
    }
    if (n && !raw_recv(g_rbuf, n)) return (int32_t)DXGI_ERROR_DEVICE_REMOVED;
    r->p = g_rbuf;
    r->end = g_rbuf + n;
    return rep.status;
}

static void call_end(struct rd *r)
{
    (void)r;
    ReleaseSRWLockExclusive(&g_lock);
}

/* Reads n bytes of the answer, padded to 4. Past its end reads zeroes. */
static void take(struct rd *r, void *dst, uint32_t n)
{
    uint32_t have = (uint32_t)(r->end - r->p), padded = (n + 3) & ~3u;
    if (dst) {
        memcpy(dst, r->p, n < have ? n : have);
        if (n > have) memset((uint8_t *)dst + have, 0, n - have);
    }
    r->p += padded < have ? padded : have;
}

static uint32_t take32(struct rd *r)
{
    uint32_t v;
    take(r, &v, 4);
    return v;
}

static void take_arr(struct rd *r, void *p, uint64_t n, uint32_t el)
{
    if (p) take(r, p, (uint32_t)(n * el));
}


/* A region's rows into the request, packed tightly: row bytes, rows, then the bytes as an array.
 * src is laid out with the given pitches. */
static void put_rows(struct wbuf *b, const uint8_t *src, uint32_t row, uint32_t rows, uint32_t slices, uint32_t pitch,
                     uint32_t slice_pitch)
{
    uint64_t total = (uint64_t)row * rows * slices, padded = (total + 3) & ~3ull, pos;
    put32(b, row);
    put32(b, rows);
    uint8_t *st = stage_alloc(total, &pos);
    if (st) {
        for (uint32_t z = 0; z < slices; z++)
            for (uint32_t y = 0; y < rows; y++, st += row) memcpy(st, src + (size_t)z * slice_pitch + (size_t)y * pitch, row);
        put_staged(b, total, pos);
        return;
    }
    put32(b, HWD3D_INLINE);
    put32(b, (uint32_t)total);
    reserve(b, (uint32_t)padded);
    uint8_t *d = b->p + b->n;
    for (uint32_t z = 0; z < slices; z++)
        for (uint32_t y = 0; y < rows; y++, d += row) memcpy(d, src + (size_t)z * slice_pitch + (size_t)y * pitch, row);
    memset(d, 0, (size_t)(padded - total));
    b->n += (uint32_t)padded;
}

#endif
