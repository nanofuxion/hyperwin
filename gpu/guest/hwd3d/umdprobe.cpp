// umdprobe: lists the DXGI adapters, then makes a Direct3D 11 device on each, the way a program that
// picks its adapter does. With no argument it also tries the default adapter (null), where Windows picks.
//   umdprobe.exe
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>

static void try_device(IDXGIAdapter* ad, const char* what) {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl{};
    HRESULT hr = D3D11CreateDevice(ad, ad ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
    printf("  %s: D3D11CreateDevice 0x%08lx, feature level 0x%x\n", what, (unsigned long)hr, fl);
    if (ctx) ctx->Release();
    if (dev) dev->Release();
}

int main() {
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f))) { printf("no factory\n"); return 1; }
    IDXGIAdapter* ad = nullptr;
    for (UINT i = 0; f->EnumAdapters(i, &ad) == S_OK; i++) {
        DXGI_ADAPTER_DESC d;
        ad->GetDesc(&d);
        UINT outputs = 0;
        IDXGIOutput* o = nullptr;
        while (ad->EnumOutputs(outputs, &o) == S_OK) { o->Release(); outputs++; }
        printf("adapter %u: %ls, vendor 0x%x, %u outputs\n", i, d.Description, d.VendorId, outputs);
        try_device(ad, "explicit");
        ad->Release();
    }
    try_device(nullptr, "default");
    f->Release();
    return 0;
}
