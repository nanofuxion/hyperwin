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
static uint64_t g_stage_cap, g_stage_pos;
static volatile struct hwd3d_query_slot *g_qpage;  /* query results (hw_d3d.h), or null */
static uint8_t g_qused[HWD3D_QUERY_SLOTS];
static SRWLOCK g_lock = SRWLOCK_INIT;  /* one call at a time on the channel */
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static int g_ok;                       /* the phone speaks HwD3D */

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

static BOOL CALLBACK open_channel(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    if (getenv("HWD3D_OFF")) return TRUE;
    /* The WDDM adapter first (include/hw_wddm.h), the legacy \\.\HwGpu device without it. */
    struct hwgpu_open_out o;
    DWORD got = 0;
    HANDLE dev = INVALID_HANDLE_VALUE;
    static struct hww adapter;
    if (hww_open(&adapter)) {
        struct hww_open wo;
        if (hww_open_channel(&adapter, &wo) < 0) {
            hww_close(&adapter);
            return TRUE;
        }
        o.chan = wo.chan;
        o.slot = wo.slot;
        o.generation = wo.generation;
        g_adapter = &adapter;
    } else {
        dev = CreateFileW(HWGPU_USER_PATH, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (dev == INVALID_HANDLE_VALUE) return TRUE;
        if (!DeviceIoControl(dev, IOCTL_HWGPU_OPEN, NULL, 0, &o, sizeof o, &got, NULL) || got != sizeof o) {
            CloseHandle(dev);
            return TRUE;
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
        /* The staging area, where uploads go instead of the ring. Without it they ride inline. */
        struct hwgpu_map_in in = {r.staging_offset, r.staging_size};
        struct hwgpu_map_out out = {0};
        int mapped = 0;
        if (r.staging_size > HWD3D_STAGING_DATA) {
            if (g_adapter) mapped = hww_map(g_adapter, o.slot, r.staging_offset, r.staging_size, &out.user) >= 0 && out.user;
            else mapped = DeviceIoControl(dev, IOCTL_HWGPU_MAP, &in, sizeof in, &out, sizeof out, &got, NULL) && out.user;
        }
        if (mapped) {
            g_stage = (uint8_t *)(uintptr_t)out.user;
            g_stage_cap = r.staging_size - HWD3D_STAGING_DATA - HWD3D_QUERY_PAGE;
            g_qpage = (volatile struct hwd3d_query_slot *)(g_stage + r.staging_size - HWD3D_QUERY_PAGE);
        }
        send_program();
    } else
        hlog("hwd3d: the phone has no HwD3D %u (status 0x%lx), using Windows' Direct3D\n", HWD3D_VERSION,
             (unsigned long)r.r.status);
    return TRUE;
}

static int ready(void)
{
    InitOnceExecuteOnce(&g_once, open_channel, NULL, NULL);
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

/* Room for n bytes in the staging area, or null when it is missing or the data is too big for it.
 * Waits while the phone still needs what is there. Under g_lock, before the request is sent: every
 * request the phone is waiting on to free room has gone already. */
static uint8_t *stage_alloc(uint64_t n, uint64_t *pos)
{
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
    raw_send(b->p, b->n);
    ReleaseSRWLockExclusive(&g_lock);
}

/* Sends and reads the answer. The lock stays held until call_end, while the outputs are read. */
static int32_t call_wait(struct wbuf *b, struct rd *r)
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
