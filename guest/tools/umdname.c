/* umdname: asks the kernel which user-mode driver file each Direct3D version would load for the HyperWin
 * GPU adapter, then loads that file and prints how it went.
 * Build: aarch64-w64-mingw32-clang -O2 -Iinclude -Igpu/include guest/tools/umdname.c */
#include <stdio.h>
#include <wchar.h>

#include "hww_user.h"

struct umdinfo { UINT version; WCHAR name[MAX_PATH]; };

int main(void)
{
    struct hww w;
    if (!hww_open(&w)) {
        printf("no adapter\n");
        return 1;
    }
    HMODULE gdi = LoadLibraryW(L"gdi32.dll");
    LONG(WINAPI * query)(hww_kmt_query *) = (LONG(WINAPI *)(hww_kmt_query *))(void *)GetProcAddress(gdi, "D3DKMTQueryAdapterInfo");
    const char *names[] = {"DX9", "DX10", "DX11", "DX12"};
    for (UINT v = 0; v < 4; v++) {
        struct umdinfo u;
        memset(&u, 0, sizeof u);
        u.version = v;
        hww_kmt_query q = {w.adapter, 1 /* UMDRIVERNAME */, &u, sizeof u};
        LONG st = query(&q);
        printf("%s: status 0x%08lx, \"%ls\"\n", names[v], (unsigned long)st, st >= 0 ? u.name : L"");
        if (st >= 0 && u.name[0]) {
            HMODULE m = LoadLibraryW(u.name);
            printf("  LoadLibrary %s (error %lu)\n", m ? "ok" : "FAILED", m ? 0 : GetLastError());
            if (m) printf("  OpenAdapter10_2 %s\n", GetProcAddress(m, "OpenAdapter10_2") ? "found" : "missing");
        }
    }
    /* The kernel half of what the runtime does next: a device on the adapter, then a context. */
    struct { UINT hAdapter; UINT flags; UINT hDevice; void *cb; UINT cbs; void *al; UINT als; void *pl; UINT pls; } cd;
    memset(&cd, 0, sizeof cd);
    cd.hAdapter = w.adapter;
    LONG(WINAPI * create)(void *) = (LONG(WINAPI *)(void *))(void *)GetProcAddress(gdi, "D3DKMTCreateDevice");
    LONG st = create ? create(&cd) : -1;
    printf("D3DKMTCreateDevice: 0x%08lx, device %u\n", (unsigned long)st, cd.hDevice);
    if (st >= 0) {
        struct { UINT hDevice; UINT node; UINT engine; UINT pad; void *priv; UINT privsize; UINT flags; UINT hContext; void *cb; UINT cbs; void *al; UINT als; void *pl; UINT pls; UINT pad2[4]; } cc;
        memset(&cc, 0, sizeof cc);
        cc.hDevice = cd.hDevice;
        LONG(WINAPI * ctx)(void *) = (LONG(WINAPI *)(void *))(void *)GetProcAddress(gdi, "D3DKMTCreateContext");
        st = ctx ? ctx(&cc) : -1;
        printf("D3DKMTCreateContext: 0x%08lx\n", (unsigned long)st);
    }
    hww_close(&w);
    return 0;
}
