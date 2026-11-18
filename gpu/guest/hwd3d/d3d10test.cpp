// d3d10test: Direct3D 10 through whatever d3d10 runtime and driver Windows has, on the adapter whose name has
// HWD3D_ADAPTER in it (the first one when unset): clears a target, draws nothing, copies and reads a pixel back.
// Checks the user-mode driver's Direct3D 10.1 table (wddm/README.md, phase 4).
//   d3d10test.exe
#include <windows.h>
#include <d3d10.h>
#include <dxgi.h>
#include <stdio.h>
#include <string.h>

int main() {
    IDXGIFactory* fac = nullptr;
    CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&fac);
    IDXGIAdapter *ad = nullptr, *pick = nullptr;
    char want[64] = "";
    GetEnvironmentVariableA("HWD3D_ADAPTER", want, sizeof want);
    for (UINT i = 0; fac && fac->EnumAdapters(i, &ad) == S_OK; i++) {
        DXGI_ADAPTER_DESC d;
        ad->GetDesc(&d);
        char name[128];
        WideCharToMultiByte(CP_ACP, 0, d.Description, -1, name, sizeof name, nullptr, nullptr);
        if (!pick && (!want[0] || strstr(name, want))) { pick = ad; printf("adapter: %s\n", name); } else ad->Release();
    }
    ID3D10Device* dev = nullptr;
    HRESULT hr = D3D10CreateDevice(pick, pick ? D3D10_DRIVER_TYPE_HARDWARE : D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, D3D10_SDK_VERSION, &dev);
    printf("D3D10CreateDevice 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr)) return 2;
    D3D10_TEXTURE2D_DESC td{64, 64, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D10_USAGE_DEFAULT, D3D10_BIND_RENDER_TARGET, 0, 0};
    ID3D10Texture2D *rt = nullptr, *st = nullptr;
    ID3D10RenderTargetView* rtv = nullptr;
    dev->CreateTexture2D(&td, nullptr, &rt);
    dev->CreateRenderTargetView(rt, nullptr, &rtv);
    td.Usage = D3D10_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
    dev->CreateTexture2D(&td, nullptr, &st);
    if (!rt || !rtv || !st) { printf("FAIL resources\n"); return 3; }
    const float c[4] = {0, 1, 0, 1};
    dev->ClearRenderTargetView(rtv, c);
    dev->CopyResource(st, rt);
    D3D10_MAPPED_TEXTURE2D m{};
    hr = st->Map(0, D3D10_MAP_READ, 0, &m);
    if (FAILED(hr)) { printf("FAIL Map 0x%08lx\n", (unsigned long)hr); return 4; }
    unsigned char* px = (unsigned char*)m.pData + 32 * m.RowPitch + 32 * 4;
    printf("pixel %02x %02x %02x %02x\n", px[0], px[1], px[2], px[3]);
    bool ok = px[0] == 0 && px[1] == 0xff && px[2] == 0 && px[3] == 0xff;
    st->Unmap(0);
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
