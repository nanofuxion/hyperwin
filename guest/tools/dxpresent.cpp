// Measures how fast a fullscreen Direct3D 11 program presents, for DXVK: a borderless window over
// the whole screen, a flip swapchain, and a clear per frame (a slow colour fade).
//   dxpresent.exe [seconds] [sync interval 0|1]          default: 5, 0
// Put DXVK's d3d11.dll and dxgi.dll beside it (gpu\dx\use.cmd dxvk). Writes the result to stdout
// and to dxpresent.txt beside the program.
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static FILE* out_file;

static void report(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    if (out_file) {
        va_start(a, fmt);
        vfprintf(out_file, fmt, a);
        va_end(a);
        fflush(out_file);
    }
}

static LRESULT CALLBACK wnd_proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(w, m, wp, lp);
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    strcpy(strrchr(path, '\\') + 1, "dxpresent.txt");
    out_file = fopen(path, "w");
    int seconds = argc > 1 ? atoi(argv[1]) : 5;
    UINT sync = argc > 2 ? (UINT)atoi(argv[2]) : 0;

    HINSTANCE hi = GetModuleHandleW(NULL);
    WNDCLASSW wc = {0, wnd_proc, 0, 0, hi, NULL, LoadCursor(NULL, IDC_ARROW), NULL, NULL, L"dxpresent"};
    RegisterClassW(&wc);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, L"dxpresent", L"dxpresent", WS_POPUP | WS_VISIBLE, 0, 0, sw, sh,
                                NULL, NULL, hi, NULL);
    SetForegroundWindow(hwnd);
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferDesc.Width = sw;
    sd.BufferDesc.Height = sh;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 3;
    sd.OutputWindow = hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ID3D11Device* dev = NULL;
    ID3D11DeviceContext* ctx = NULL;
    IDXGISwapChain* chain = NULL;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION,
                                               &sd, &chain, &dev, NULL, &ctx);
    if (FAILED(hr)) {
        report("FAIL D3D11CreateDeviceAndSwapChain 0x%08lx\n", hr);
        return 1;
    }
    ID3D11Texture2D* back = NULL;
    chain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);
    ID3D11RenderTargetView* rtv = NULL;
    dev->CreateRenderTargetView(back, NULL, &rtv);
    back->Release();

    IDXGIDevice* dxgi = NULL;
    IDXGIAdapter* adapter = NULL;
    DXGI_ADAPTER_DESC ad = {};
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)))
        adapter->GetDesc(&ad);
    report("%ls, %dx%d, sync %u\n", ad.Description, sw, sh, sync);

    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    unsigned long long frames = 0;
    for (;;) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        QueryPerformanceCounter(&now);
        double t = (double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart;
        if (t >= seconds) {
            report("%llu frames in %.2f s: %.1f FPS\n", frames, t, frames / t);
            break;
        }
        float c = (float)(0.5 + 0.5 * sin(t * 1.5));
        float color[4] = {0.9f - c * 0.6f, c * 0.6f, 0.1f, 1.0f};
        ctx->OMSetRenderTargets(1, &rtv, NULL);
        ctx->ClearRenderTargetView(rtv, color);
        hr = chain->Present(sync, 0);
        if (FAILED(hr)) {
            report("FAIL Present 0x%08lx after %llu frames\n", hr, frames);
            break;
        }
        frames++;
    }
    DestroyWindow(hwnd);
    return 0;
}
