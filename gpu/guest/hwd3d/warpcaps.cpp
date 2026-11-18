// warpcaps: loads a Direct3D 10/11 user-mode driver by file name, opens its adapter with no runtime behind
// it, and prints what it answers to the supported-versions and caps queries. Black-box reading of how a
// working driver answers, for comparing with ours.
//   warpcaps.exe C:\Windows\System32\d3d10warp.dll
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <d3d11.h>
#include <d3d10umddi.h>

static HRESULT __stdcall nope(...) { printf("(callback called)\n"); return E_FAIL; }

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    HMODULE m = LoadLibraryA(argc > 1 ? argv[1] : "d3d10warp.dll");
    if (!m) { printf("load failed %lu\n", GetLastError()); return 1; }
    auto open = (PFND3D10DDI_OPENADAPTER)GetProcAddress(m, "OpenAdapter10_2");
    if (!open) { printf("no OpenAdapter10_2\n"); return 1; }
    D3D10_2DDI_ADAPTERFUNCS funcs{};
    D3DDDI_ADAPTERCALLBACKS cbs{};
    for (size_t i = 0; i < sizeof cbs / sizeof(void*); i++) ((void**)&cbs)[i] = (void*)nope;
    printf("loaded\n");
    D3D10DDIARG_OPENADAPTER a{};
    a.hRTAdapter.handle = (void*)1;
    a.Interface = 0xb0011;
    a.Version = 393216;
    a.pAdapterCallbacks = &cbs;
    a.pAdapterFuncs_2 = &funcs;
    HRESULT hr = open(&a);
    printf("OpenAdapter10_2 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr)) return 1;
    UINT32 n = 0;
    funcs.pfnGetSupportedVersions(a.hAdapter, &n, nullptr);
    UINT64 v[32] = {};
    UINT32 n2 = n < 32 ? n : 32;
    funcs.pfnGetSupportedVersions(a.hAdapter, &n2, v);
    for (UINT32 i = 0; i < n2; i++) printf("version %llx\n", (unsigned long long)v[i]);
    for (int t = 128; t <= 160; t++) {
        UINT64 data[16] = {};
        D3D10_2DDIARG_GETCAPS c{(D3D10_2DDICAPS_TYPE)t, nullptr, data, 16};
        hr = funcs.pfnGetCaps(a.hAdapter, &c);
        printf("caps %d: 0x%08lx  %08x %08x %08x %08x\n", t, (unsigned long)hr, (unsigned)(data[0] & 0xffffffff), (unsigned)(data[0] >> 32), (unsigned)(data[1] & 0xffffffff), (unsigned)(data[1] >> 32));
    }
    return 0;
}
