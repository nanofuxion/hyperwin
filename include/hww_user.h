/* User-mode side of the escape in hw_wddm.h: finds the HyperWin GPU adapter and talks to it through
 * D3DKMT. Header only, Windows only. The D3DKMT structures here are our own declarations from
 * Microsoft's public documentation; gdi32.dll is called by name at run time. */
#ifndef HYPERWIN_HWW_USER_H
#define HYPERWIN_HWW_USER_H

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hw_wddm.h"

typedef UINT hww_handle;

typedef struct { hww_handle hAdapter; LUID luid; ULONG sources; BOOL moveRegions; } hww_kmt_adapterinfo;
typedef struct { ULONG num; hww_kmt_adapterinfo *adapters; } hww_kmt_enum2;
typedef struct { hww_handle hAdapter; } hww_kmt_close;
typedef struct { hww_handle hAdapter, hDevice; UINT type, flags; void *data; UINT size; hww_handle hContext; } hww_kmt_escape;
typedef struct { hww_handle hAdapter; UINT type; void *data; UINT size; } hww_kmt_query;
#define HWW_KMTQAI_UMDRIVERPRIVATE 0

struct hww {
    hww_handle adapter;
    LONG (WINAPI *escape)(hww_kmt_escape *);
    LONG (WINAPI *close)(hww_kmt_close *);
};

static inline void hww_close(struct hww *w)
{
    if (w->adapter && w->close) {
        hww_kmt_close c = {w->adapter};
        w->close(&c);
    }
    w->adapter = 0;
}

/* Opens the HyperWin GPU adapter: the one that answers an info escape.
 * Returns 1 on success. */
static inline int hww_open(struct hww *w)
{
    memset(w, 0, sizeof *w);
    HMODULE gdi = LoadLibraryW(L"gdi32.dll");
    if (!gdi) return 0;
    LONG(WINAPI * enum2)(hww_kmt_enum2 *) = (LONG(WINAPI *)(hww_kmt_enum2 *))(void *)GetProcAddress(gdi, "D3DKMTEnumAdapters2");
    w->escape = (LONG(WINAPI *)(hww_kmt_escape *))(void *)GetProcAddress(gdi, "D3DKMTEscape");
    w->close = (LONG(WINAPI *)(hww_kmt_close *))(void *)GetProcAddress(gdi, "D3DKMTCloseAdapter");
    if (!enum2 || !w->escape || !w->close) return 0;
    hww_kmt_adapterinfo list[16];
    hww_kmt_enum2 e = {16, list};
    if (enum2(&e) < 0) return 0;
    for (ULONG i = 0; i < e.num && i < 16; i++) {
        /* Ours answers an info escape with its version in the reply; any other adapter leaves the
         * buffer as it was. */
        struct hww_escape probe;
        memset(&probe, 0, sizeof probe);
        probe.magic = HWW_MAGIC;
        probe.size = sizeof probe;
        probe.op = HWW_ESC_INFO;
        probe.status = 0x7fffffff;
        hww_kmt_escape k = {list[i].hAdapter, 0, 0, 0, &probe, sizeof probe, 0};
        LONG qs = w->escape(&k);
        if (GetEnvironmentVariableA("HWW_DEBUG", NULL, 0))
            fprintf(stderr, "adapter %u: escape 0x%08lx status %d version %u\n", list[i].hAdapter, (unsigned long)qs,
                    probe.status, probe.u.info.driver_version);
        int ours = qs >= 0 && probe.status == 0 && probe.u.info.driver_version == HWW_VERSION;
        if (ours && !w->adapter) {
            w->adapter = list[i].hAdapter;
        } else {
            hww_kmt_close c = {list[i].hAdapter};
            w->close(&c);
        }
    }
    return w->adapter != 0;
}

/* One escape. Returns the call's NTSTATUS, or the driver's own when the call itself worked. */
static inline LONG hww_call(struct hww *w, struct hww_escape *e)
{
    e->magic = HWW_MAGIC;
    e->size = sizeof *e;
    hww_kmt_escape k = {w->adapter, 0, 0, 0, e, sizeof *e, 0};
    LONG st = w->escape(&k);
    if (st < 0) return st;
    return e->status;
}

/* A channel for this process, as IOCTL_HWGPU_OPEN gives on the legacy driver. */
static inline LONG hww_open_channel(struct hww *w, struct hww_open *out)
{
    struct hww_escape e;
    memset(&e, 0, sizeof e);
    e.op = HWW_ESC_OPEN;
    LONG st = hww_call(w, &e);
    if (st >= 0) *out = e.u.open;
    return st;
}

static inline LONG hww_map(struct hww *w, uint32_t slot, uint64_t offset, uint64_t size, uint64_t *user)
{
    struct hww_escape e;
    memset(&e, 0, sizeof e);
    e.op = HWW_ESC_MAP;
    e.u.map.slot = slot;
    e.u.map.offset = offset;
    e.u.map.size = size;
    LONG st = hww_call(w, &e);
    if (st >= 0) *user = e.u.map.user;
    return st;
}

#endif
