// Checks Direct3D 11 and 12 through whatever d3d11.dll / d3d12.dll sit beside it (use.cmd puts
// DXVK and vkd3d-proton there): makes a device, clears a 64x64 target to a known colour on the GPU,
// reads it back. Prints the adapter, the pixel, and PASS or FAIL.
//   dxtest.exe [11|12]
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include <string.h>

static const float kColour[4] = {0.0f, 1.0f, 0.0f, 1.0f};  // green: 0xff00ff00 as BGRA bytes 00 ff 00 ff
static const UINT kSize = 64;

static int check(const unsigned char* px, const char* api) {
    printf("%s: pixel %02x %02x %02x %02x\n", api, px[0], px[1], px[2], px[3]);
    bool ok = px[0] == 0 && px[1] == 0xff && px[2] == 0 && px[3] == 0xff;
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}


// dxtest.exe <11|12> [adapter text] (or HWTEST_ADAPTER=<text>) picks the adapter whose name has that text; without it the default adapter is used.
#include <dxgi.h>
static IDXGIAdapter* pickAdapter() {
    char want[64];
    if (!GetEnvironmentVariableA("HWTEST_ADAPTER", want, sizeof want)) return nullptr;
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f))) return nullptr;
    IDXGIAdapter* found = nullptr;
    IDXGIAdapter* a = nullptr;
    for (UINT i = 0; !found && f->EnumAdapters(i, &a) == S_OK; i++) {
        DXGI_ADAPTER_DESC d{};
        a->GetDesc(&d);
        char name[256];
        WideCharToMultiByte(CP_ACP, 0, d.Description, -1, name, sizeof name, nullptr, nullptr);
        printf("adapter %u: %s\n", i, name);
        if (strstr(name, want)) found = a; else a->Release();
    }
    f->Release();
    return found;
}

static int test11() {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl;
    IDXGIAdapter* pick = pickAdapter();
    HRESULT hr = D3D11CreateDevice(pick, pick ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
    printf("D3D11CreateDevice 0x%08lx, feature level 0x%x\n", hr, fl);
    if (FAILED(hr)) return 2;
    IDXGIDevice* xd = nullptr;
    IDXGIAdapter* ad = nullptr;
    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&xd)) && SUCCEEDED(xd->GetAdapter(&ad)) && SUCCEEDED(ad->GetDesc(&desc)))
        printf("adapter: %ls\n", desc.Description);
    D3D11_TEXTURE2D_DESC td{kSize, kSize, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
    ID3D11Texture2D *rt = nullptr, *st = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    dev->CreateTexture2D(&td, nullptr, &rt);
    dev->CreateRenderTargetView(rt, nullptr, &rtv);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    dev->CreateTexture2D(&td, nullptr, &st);
    if (!rt || !rtv || !st) { printf("resource creation failed\n"); return 3; }
    ctx->ClearRenderTargetView(rtv, kColour);
    ctx->CopyResource(st, rt);
    D3D11_MAPPED_SUBRESOURCE m{};
    hr = ctx->Map(st, 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) { printf("Map 0x%08lx\n", hr); return 4; }
    unsigned char px[4];
    memcpy(px, (unsigned char*)m.pData + (kSize / 2) * m.RowPitch + (kSize / 2) * 4, 4);
    ctx->Unmap(st, 0);
    return check(px, "d3d11");
}

static int test12() {
    ID3D12Device* dev = nullptr;
    HRESULT hr = D3D12CreateDevice(pickAdapter(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&dev);
    printf("D3D12CreateDevice 0x%08lx\n", hr);
    if (FAILED(hr)) return 2;
    LUID luid = dev->GetAdapterLuid();
    IDXGIFactory4* f = nullptr;
    IDXGIAdapter* ad = nullptr;
    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&f)) &&
        SUCCEEDED(f->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter), (void**)&ad)) && SUCCEEDED(ad->GetDesc(&desc)))
        printf("adapter: %ls\n", desc.Description);
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    ID3D12CommandQueue* q = nullptr;
    ID3D12CommandAllocator* al = nullptr;
    ID3D12GraphicsCommandList* cl = nullptr;
    dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&q);
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&al);
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&cl);
    D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT}, rb{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC td{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, kSize, kSize, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
                           D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    const UINT pitch = kSize * 4;  // 256, already D3D12_TEXTURE_DATA_PITCH_ALIGNMENT
    D3D12_RESOURCE_DESC bd{D3D12_RESOURCE_DIMENSION_BUFFER, 0, pitch * kSize, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                           D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    ID3D12Resource *rt = nullptr, *buf = nullptr;
    dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, __uuidof(ID3D12Resource), (void**)&rt);
    dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)&buf);
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    ID3D12DescriptorHeap* heap = nullptr;
    dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&heap);
    if (!q || !al || !cl || !rt || !buf || !heap) { printf("object creation failed\n"); return 3; }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = heap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(rt, nullptr, rtv);
    cl->ClearRenderTargetView(rtv, kColour, 0, nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = rt;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION src{rt, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}, dst{buf, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.SubresourceIndex = 0;
    dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, 1, pitch};
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    cl->Close();
    ID3D12CommandList* lists[] = {cl};
    q->ExecuteCommandLists(1, lists);
    ID3D12Fence* fence = nullptr;
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&fence);
    HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    q->Signal(fence, 1);
    fence->SetEventOnCompletion(1, ev);
    if (WaitForSingleObject(ev, 10000) != WAIT_OBJECT_0) { printf("GPU did not finish\n"); return 5; }
    unsigned char* p = nullptr;
    D3D12_RANGE r{0, pitch * kSize};
    if (FAILED(buf->Map(0, &r, (void**)&p))) { printf("Map failed\n"); return 4; }
    unsigned char px[4];
    memcpy(px, p + (kSize / 2) * pitch + (kSize / 2) * 4, 4);
    return check(px, "d3d12");
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    int which = argc > 1 ? atoi(argv[1]) : 11;
    if (argc > 2) SetEnvironmentVariableA("HWTEST_ADAPTER", argv[2]);
    char path[MAX_PATH];
    HMODULE m = LoadLibraryA(which == 12 ? "d3d12.dll" : "d3d11.dll");
    GetModuleFileNameA(m, path, sizeof(path));
    printf("using %s\n", path);
    return which == 12 ? test12() : test11();
}
