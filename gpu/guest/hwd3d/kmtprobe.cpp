// kmtprobe: makes a kernel device and a context on the HyperWin GPU adapter through D3DKMT, the calls the
// Direct3D runtime makes for a driver, and prints each status. For finding which one refuses.
#include <windows.h>
#include <stdio.h>
#include <d3dkmthk.h>

#include "hww_user.h"

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    struct hww w;
    if (!hww_open(&w)) { printf("no adapter\n"); return 1; }
    HMODULE gdi = LoadLibraryW(L"gdi32.dll");
    char av[16];
    if (GetEnvironmentVariableA("ADAPTER", av, sizeof av)) {
        /* Another adapter, to see the same calls work on Windows' own. */
        auto enum2 = (NTSTATUS(WINAPI*)(D3DKMT_ENUMADAPTERS2*))GetProcAddress(gdi, "D3DKMTEnumAdapters2");
        D3DKMT_ADAPTERINFO list[16];
        D3DKMT_ENUMADAPTERS2 e{16, list};
        enum2(&e);
        UINT i = strtoul(av, 0, 0);
        printf("adapter %u of %u: handle %u\n", i, e.NumAdapters, list[i].hAdapter);
        w.adapter = list[i].hAdapter;
    }
    auto create = (NTSTATUS(WINAPI*)(D3DKMT_CREATEDEVICE*))GetProcAddress(gdi, "D3DKMTCreateDevice");
    auto ctx = (NTSTATUS(WINAPI*)(D3DKMT_CREATECONTEXT*))GetProcAddress(gdi, "D3DKMTCreateContext");
    auto dctx = (NTSTATUS(WINAPI*)(const D3DKMT_DESTROYCONTEXT*))GetProcAddress(gdi, "D3DKMTDestroyContext");
    D3DKMT_CREATEDEVICE cd{};
    cd.hAdapter = w.adapter;
    NTSTATUS st = create(&cd);
    printf("D3DKMTCreateDevice 0x%08lx, device %u\n", (unsigned long)st, cd.hDevice);
    if (st < 0) return 1;
    D3DKMT_CREATECONTEXT cc{};
    cc.hDevice = cd.hDevice;
    char v[16];
    if (GetEnvironmentVariableA("EA", v, sizeof v)) cc.EngineAffinity = strtoul(v, 0, 0);
    if (GetEnvironmentVariableA("HINT", v, sizeof v)) cc.ClientHint = (D3DKMT_CLIENTHINT)strtoul(v, 0, 0);
    if (GetEnvironmentVariableA("NODE", v, sizeof v)) cc.NodeOrdinal = strtoul(v, 0, 0);
    st = ctx(&cc);
    printf("D3DKMTCreateContext 0x%08lx, context %u, cmd %p size %u, alloc list %u, patch list %u\n", (unsigned long)st, cc.hContext,
           cc.pCommandBuffer, cc.CommandBufferSize, cc.AllocationListSize, cc.PatchLocationListSize);
    if (st >= 0) { D3DKMT_DESTROYCONTEXT d{cc.hContext}; dctx(&d); }
    return 0;
}
