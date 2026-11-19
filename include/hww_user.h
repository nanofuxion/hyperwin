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

/* A driver update stops the adapter while programs still hold its mappings, and the kernel driver takes
 * them away. A thread reading one then would fault. Each mapping is remembered, and a fault inside one
 * gets fresh zeroed memory there instead: a zeroed channel reads as closed (generation 0), so the
 * program sees its device lost and opens a new channel. */
#define HWW_GUARDED 64
static struct { uintptr_t base, end; } hww_guarded[HWW_GUARDED];
static LONG hww_nguarded;

static LONG CALLBACK hww_fault(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || ep->ExceptionRecord->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t at = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
    LONG n = hww_nguarded < HWW_GUARDED ? hww_nguarded : HWW_GUARDED;
    for (LONG i = 0; i < n; i++) {
        if (at < hww_guarded[i].base || at >= hww_guarded[i].end) continue;
        void *page = (void *)(at & ~(uintptr_t)0xfff);
        MEMORY_BASIC_INFORMATION mi;
        if (VirtualAlloc(page, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) ||
            (VirtualQuery(page, &mi, sizeof mi) && mi.State == MEM_COMMIT && mi.Protect == PAGE_READWRITE))
            return EXCEPTION_CONTINUE_EXECUTION;
        return EXCEPTION_CONTINUE_SEARCH;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static PVOID hww_veh;

/* The handler lives in this module: a DLL removes it before it unloads (DllMain, or the destructor for
 * DLLs started by the C runtime). */
__attribute__((destructor, unused)) static void hww_unguard(void)
{
    PVOID h = InterlockedExchangePointer(&hww_veh, NULL);
    if (h) RemoveVectoredExceptionHandler(h);
}

static inline void hww_guard(uint64_t user)
{
    MEMORY_BASIC_INFORMATION mi;
    if (!user || !VirtualQuery((void *)(uintptr_t)user, &mi, sizeof mi)) return;
    if (!hww_veh) {
        PVOID h = AddVectoredExceptionHandler(1, hww_fault);
        if (InterlockedCompareExchangePointer(&hww_veh, h, NULL)) RemoveVectoredExceptionHandler(h);
    }
    LONG i = InterlockedIncrement(&hww_nguarded) - 1;
    if (i >= HWW_GUARDED) return;
    hww_guarded[i].base = (uintptr_t)mi.AllocationBase;
    hww_guarded[i].end = (uintptr_t)mi.BaseAddress + mi.RegionSize;
}

/* A channel for this process, as IOCTL_HWGPU_OPEN gives on the legacy driver. */
static inline LONG hww_open_channel(struct hww *w, struct hww_open *out)
{
    struct hww_escape e;
    memset(&e, 0, sizeof e);
    e.op = HWW_ESC_OPEN;
    LONG st = hww_call(w, &e);
    if (st >= 0) {
        *out = e.u.open;
        hww_guard(e.u.open.chan);
    }
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
    if (st >= 0) {
        *user = e.u.map.user;
        hww_guard(e.u.map.user);
    }
    return st;
}

#endif
