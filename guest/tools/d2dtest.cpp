/* d2dtest: Direct2D drawing on the HyperWin GPU, read back. DWM draws parts of the taskbar this way
 * (gradients, geometry), so wrong output here shows up there. Prints the pixels of a few shapes and
 * PASS or FAIL. Uses the first adapter whose name holds "HyperWin", or WARP with any argument.
 * Build: aarch64-w64-mingw32-clang++ -O2 guest/tools/d2dtest.cpp -o build/d2dtest.exe -ld3d11 -ldxgi -ld2d1 */
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d2d1.h>
#include <stdio.h>
#include <wchar.h>
#include <string.h>

static int check(const char *what, UINT32 got, UINT32 want)
{
    int ok = 1;
    for (int i = 0; i < 32; i += 8) {
        int a = (got >> i) & 0xff, b = (want >> i) & 0xff;
        if (a - b > 24 || b - a > 24) ok = 0;
    }
    printf("%-28s %08x (want about %08x) %s\n", what, got, want, ok ? "ok" : "WRONG");
    return ok;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    IDXGIFactory1 *f = nullptr;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&f);
    IDXGIAdapter1 *pick = nullptr, *a = nullptr;
    for (UINT i = 0; f && f->EnumAdapters1(i, &a) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d;
        a->GetDesc1(&d);
        bool first = argc > 1 && !strcmp(argv[1], "first");   /* d2dtest first: adapter 0, as with DXVK beside it */
        if (!pick && ((argc < 2 && wcsstr(d.Description, L"HyperWin")) || first)) pick = a;
        else a->Release();
    }
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    /* d2dtest warp: the same on Windows' software renderer, to compare. */
    HRESULT hr = D3D11CreateDevice(pick, pick ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    printf("device 0x%08lx on %s\n", hr, pick ? "HyperWin GPU" : "WARP");
    if (FAILED(hr)) return 1;
    const UINT W = 64, H = 64;
    D3D11_TEXTURE2D_DESC td = {W, H, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    ID3D11Texture2D *tex = nullptr;
    hr = dev->CreateTexture2D(&td, nullptr, &tex);
    printf("texture 0x%08lx\n", hr);
    if (!tex) return 1;
    IDXGISurface *surf = nullptr;
    hr = tex->QueryInterface(__uuidof(IDXGISurface), (void **)&surf);
    printf("surface 0x%08lx\n", hr);
    if (!surf) return 1;
    ID2D1Factory *d2 = nullptr;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr, (void **)&d2);
    D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_HARDWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ID2D1RenderTarget *rt = nullptr;
    hr = d2->CreateDxgiSurfaceRenderTarget(surf, &rp, &rt);
    printf("Direct2D target 0x%08lx\n", hr);
    if (FAILED(hr)) return 1;
    ID2D1SolidColorBrush *solid = nullptr;
    rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.4f, 1.0f), &solid);
    D2D1_GRADIENT_STOP stops[2] = {{0.0f, D2D1::ColorF(1, 0, 0)}, {1.0f, D2D1::ColorF(0, 0, 1)}};
    ID2D1GradientStopCollection *sc = nullptr;
    rt->CreateGradientStopCollection(stops, 2, &sc);
    ID2D1LinearGradientBrush *lin = nullptr;
    rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(64, 0)), sc, &lin);
    ID2D1RadialGradientBrush *rad = nullptr;
    rt->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(32, 56), D2D1::Point2F(0, 0), 8, 8), sc, &rad);
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 1));
    rt->FillRectangle(D2D1::RectF(0, 0, 64, 16), solid);                                 /* rows 0-15: solid */
    rt->FillRectangle(D2D1::RectF(0, 16, 64, 32), lin);                                  /* rows 16-31: red to blue */
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(16, 40), 6, 6), solid);                  /* geometry, anti-aliased */
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(36, 34, 60, 46), 4, 4), lin);
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(32, 56), 8, 8), rad);                   /* radial: red middle */
    hr = rt->EndDraw();
    printf("EndDraw 0x%08lx\n", hr);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *st = nullptr;
    hr = dev->CreateTexture2D(&td, nullptr, &st);
    printf("staging 0x%08lx\n", hr);
    if (!st) return 1;
    ctx->CopyResource(st, tex);
    D3D11_MAPPED_SUBRESOURCE m;
    hr = ctx->Map(st, 0, D3D11_MAP_READ, 0, &m);
    printf("map 0x%08lx\n", hr);
    if (FAILED(hr)) return 1;
    auto px = [&](int x, int y) { return ((UINT32 *)((BYTE *)m.pData + y * m.RowPitch))[x]; };
    int ok = 1;
    ok &= check("solid fill (8,8)", px(8, 8), 0xff0066ff);
    ok &= check("linear gradient left (2,24)", px(2, 24), 0xfff80006);
    ok &= check("linear gradient right (61,24)", px(61, 24), 0xff0700f7);
    ok &= check("ellipse middle (16,40)", px(16, 40), 0xff0066ff);
    ok &= check("rounded rect gradient (48,40)", px(48, 40), 0xff3e00c0);
    ok &= check("radial middle (32,56)", px(32, 56), 0xffff0000);
    ok &= check("background (2,62)", px(2, 62), 0xff000000);
    ctx->Unmap(st, 0);
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
