// HwD3D phase 0 (d3d/README.md): is Direct3D faster with DXVK running on the phone? This draws the
// AIO Graphics Test's D3D11 spin cube (same geometry, shaders, depth buffer, one-buffer discard swap
// chain, Present(0, 0)) with DXVK built for Android, on Mali through the driver wrapper, and prints
// the frame rate. Compare with the same cube in Windows through DXVK and gfxstream (104 FPS).
//
//   adb shell 'cd /data/local/tmp/hwd3d && LD_LIBRARY_PATH=. HWGPU_VK_LOADER=$PWD/libvulkan_passthrough.so \
//              ./hwd3d_cube [seconds] [width] [height]'
//
// The swap chain presents into an AImageReader's window, which a thread drains, so frames go through
// a real Android buffer queue without a display.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <android/native_window.h>
#include <media/NdkImageReader.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "spin_dxbc.inc"

namespace {

struct Mat4 { float m[16]; };

Mat4 mul(const Mat4& a, const Mat4& b) {   // row vectors: v * a * b
    Mat4 r{};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++) r.m[i * 4 + j] += a.m[i * 4 + k] * b.m[k * 4 + j];
    return r;
}

Mat4 rotate(float x, float y, float z, float a) {
    float c = cosf(a), s = sinf(a), t = 1 - c;
    return {{t * x * x + c, t * x * y + s * z, t * x * z - s * y, 0,
             t * x * y - s * z, t * y * y + c, t * y * z + s * x, 0,
             t * x * z + s * y, t * y * z - s * x, t * z * z + c, 0,
             0, 0, 0, 1}};
}

Mat4 translate(float x, float y, float z) { return {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1}}; }

Mat4 perspective(float fovy, float aspect, float zn, float zf) {
    float f = 1.0f / tanf(fovy / 2);
    return {{f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, zf / (zn - zf), -1, 0, 0, zn * zf / (zn - zf), 0}};
}

struct Vertex { float pos[3], col[3]; };

void build_cube(Vertex* out) {
    static const float face[6][4][3] = {
        {{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}},     {{1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}},
        {{-1, 1, -1}, {-1, 1, 1}, {1, 1, 1}, {1, 1, -1}},     {{-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1}},
        {{1, -1, -1}, {1, 1, -1}, {1, 1, 1}, {1, -1, 1}},     {{-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}},
    };
    static const float col[6][3] = {{0.90f, 0.20f, 0.20f}, {0.20f, 0.80f, 0.30f}, {0.25f, 0.45f, 0.95f},
                                    {0.95f, 0.80f, 0.20f}, {0.85f, 0.40f, 0.90f}, {0.20f, 0.85f, 0.90f}};
    static const int quad[6] = {0, 1, 2, 0, 2, 3};
    int v = 0;
    for (int f = 0; f < 6; f++)
        for (int k = 0; k < 6; k++, v++) {
            memcpy(out[v].pos, face[f][quad[k]], sizeof out[v].pos);
            memcpy(out[v].col, col[f], sizeof out[v].col);
        }
}

void drain(void*, AImageReader* reader) {
    AImage* img = nullptr;
    while (AImageReader_acquireNextImage(reader, &img) == AMEDIA_OK && img) AImage_delete(img);
}

}  // namespace

int main(int argc, char** argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 10;
    int w = argc > 2 ? atoi(argv[2]) : 1280, h = argc > 3 ? atoi(argv[3]) : 720;
    setenv("DXVK_WSI_DRIVER", "Android", 0);

    AImageReader* reader = nullptr;
    if (AImageReader_newWithUsage(w, h, AIMAGE_FORMAT_RGBA_8888, AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                                  AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 4, &reader) != AMEDIA_OK) {
        printf("FAIL AImageReader\n");
        return 1;
    }
    AImageReader_ImageListener listener{nullptr, drain};
    AImageReader_setImageListener(reader, &listener);
    ANativeWindow* window = nullptr;
    AImageReader_getWindow(reader, &window);

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount = 1;
    scd.BufferDesc.Width = w;
    scd.BufferDesc.Height = h;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = reinterpret_cast<HWND>(window);
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* swap = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, want, 3,
                                               D3D11_SDK_VERSION, &scd, &swap, &dev, nullptr, &ctx);
    if (FAILED(hr)) {
        printf("FAIL D3D11CreateDeviceAndSwapChain 0x%08x\n", unsigned(hr));
        return 1;
    }

    ID3D11Texture2D* back = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back));
    dev->CreateRenderTargetView(back, nullptr, &rtv);
    D3D11_TEXTURE2D_DESC dd{};
    dd.Width = w;
    dd.Height = h;
    dd.MipLevels = dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.SampleDesc.Count = 1;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ID3D11Texture2D* depth = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    dev->CreateTexture2D(&dd, nullptr, &depth);
    dev->CreateDepthStencilView(depth, nullptr, &dsv);
    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_LESS;
    ID3D11DepthStencilState* dss = nullptr;
    dev->CreateDepthStencilState(&dsd, &dss);
    D3D11_RASTERIZER_DESC rsd{};
    rsd.FillMode = D3D11_FILL_SOLID;
    rsd.CullMode = D3D11_CULL_NONE;
    rsd.DepthClipEnable = TRUE;
    ID3D11RasterizerState* rs = nullptr;
    dev->CreateRasterizerState(&rsd, &rs);
    D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};

    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11InputLayout* layout = nullptr;
    dev->CreateVertexShader(kSpinVS, sizeof kSpinVS, nullptr, &vs);
    dev->CreatePixelShader(kSpinPS, sizeof kSpinPS, nullptr, &ps);
    D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    dev->CreateInputLayout(il, 2, kSpinVS, sizeof kSpinVS, &layout);
    Vertex verts[36];
    build_cube(verts);
    D3D11_BUFFER_DESC vbd{sizeof verts, D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA sr{verts, 0, 0};
    ID3D11Buffer *vbo = nullptr, *cbo = nullptr;
    dev->CreateBuffer(&vbd, &sr, &vbo);
    D3D11_BUFFER_DESC cbd{sizeof(Mat4), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    dev->CreateBuffer(&cbd, nullptr, &cbo);
    if (!vs || !ps || !layout || !vbo || !cbo) {
        printf("FAIL resources\n");
        return 1;
    }

    auto start = std::chrono::steady_clock::now(), last = start;
    unsigned long long frames = 0, window_frames = 0;
    for (;;) {
        auto now = std::chrono::steady_clock::now();
        double t = std::chrono::duration<double>(now - start).count();
        if (t >= seconds) break;
        if (std::chrono::duration<double>(now - last).count() >= 1.0) {
            printf("%.0f FPS\n", window_frames / std::chrono::duration<double>(now - last).count());
            fflush(stdout);
            last = now;
            window_frames = 0;
        }
        const float clear[4] = {0.1f, 0.1f, 0.12f, 1};
        ctx->OMSetRenderTargets(1, &rtv, dsv);
        ctx->OMSetDepthStencilState(dss, 1);
        ctx->RSSetState(rs);
        ctx->RSSetViewports(1, &vp);
        ctx->ClearRenderTargetView(rtv, clear);
        ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1, 0);
        float a = float(t) * 0.6f;
        Mat4 mvp = mul(mul(mul(rotate(0, 1, 0, a), rotate(1, 0, 0, 0.5f)), translate(0, 0, -6.5f)),
                       perspective(0.6f, float(w) / float(h), 0.1f, 100.0f));
        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(ctx->Map(cbo, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
            memcpy(map.pData, mvp.m, sizeof mvp.m);
            ctx->Unmap(cbo, 0);
        }
        UINT stride = sizeof(Vertex), offset = 0;
        ctx->IASetInputLayout(layout);
        ctx->IASetVertexBuffers(0, 1, &vbo, &stride, &offset);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &cbo);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->Draw(36, 0);
        hr = swap->Present(0, 0);
        if (FAILED(hr)) {
            printf("FAIL Present 0x%08x after %llu frames\n", unsigned(hr), frames);
            break;
        }
        frames++;
        window_frames++;
    }
    double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    printf("%llu frames in %.1f s: %.1f FPS at %dx%d\n", frames, total, frames / total, w, h);
    return 0;
}
