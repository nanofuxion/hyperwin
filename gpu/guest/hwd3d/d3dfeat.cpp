// d3dfeat: Direct3D 11 features games lean on, through whatever d3d11.dll sits beside it (HwD3D's,
// from `use.cmd hwd3d`): a deferred context and its command list, 4x MSAA with a resolve, a compute
// shader writing a structured buffer through a UAV, and a read-back of each. Prints PASS or FAIL per
// part and a total. Shaders are compiled with Windows' d3dcompiler_47.dll at run time.
//   d3dfeat.exe
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>
#include <vector>

static ID3D11Device* dev;
static ID3D11DeviceContext* ctx;
static int failures;

static void check(const char* name, bool ok) {
    printf("%-34s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static ID3D11Texture2D* make_tex(UINT w, UINT h, DXGI_FORMAT f, UINT samples, UINT bind, D3D11_USAGE usage, UINT cpu) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = d.ArraySize = 1;
    d.Format = f;
    d.SampleDesc.Count = samples;
    d.Usage = usage;
    d.BindFlags = bind;
    d.CPUAccessFlags = cpu;
    ID3D11Texture2D* t = nullptr;
    dev->CreateTexture2D(&d, nullptr, &t);
    return t;
}

// The first pixel of a texture, read back through a staging copy.
static bool first_pixel(ID3D11Texture2D* src, UINT* px) {
    D3D11_TEXTURE2D_DESC d;
    src->GetDesc(&d);
    ID3D11Texture2D* st = make_tex(d.Width, d.Height, d.Format, 1, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
    if (!st) return false;
    ctx->CopyResource(st, src);
    D3D11_MAPPED_SUBRESOURCE m;
    bool ok = SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m));
    if (ok) {
        *px = *static_cast<UINT*>(m.pData);
        ctx->Unmap(st, 0);
    }
    st->Release();
    return ok;
}

static void test_deferred() {
    ID3D11Texture2D* rt = make_tex(64, 64, DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D11_BIND_RENDER_TARGET, D3D11_USAGE_DEFAULT, 0);
    ID3D11RenderTargetView* rtv = nullptr;
    dev->CreateRenderTargetView(rt, nullptr, &rtv);
    ID3D11DeviceContext* def = nullptr;
    ID3D11CommandList* list = nullptr;
    bool ok = rtv && SUCCEEDED(dev->CreateDeferredContext(0, &def));
    if (ok) {
        const float red[4] = {1, 0, 0, 1};
        def->ClearRenderTargetView(rtv, red);
        ok = SUCCEEDED(def->FinishCommandList(FALSE, &list)) && list;
    }
    UINT px = 0;
    if (ok) {
        ctx->ExecuteCommandList(list, FALSE);
        ok = first_pixel(rt, &px) && (px & 0xFFFFFF) == 0x0000FF;  // R8G8B8A8: red is 0xFF in the low byte
    }
    check("deferred context, command list", ok);
    if (list) list->Release();
    if (def) def->Release();
    if (rtv) rtv->Release();
    if (rt) rt->Release();
}

static void test_msaa() {
    UINT quality = 0;
    dev->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, 4, &quality);
    ID3D11Texture2D* ms = make_tex(64, 64, DXGI_FORMAT_R8G8B8A8_UNORM, 4, D3D11_BIND_RENDER_TARGET, D3D11_USAGE_DEFAULT, 0);
    ID3D11Texture2D* one = make_tex(64, 64, DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D11_BIND_RENDER_TARGET, D3D11_USAGE_DEFAULT, 0);
    ID3D11RenderTargetView* rtv = nullptr;
    bool ok = quality > 0 && ms && one && SUCCEEDED(dev->CreateRenderTargetView(ms, nullptr, &rtv));
    UINT px = 0;
    if (ok) {
        const float green[4] = {0, 1, 0, 1};
        ctx->ClearRenderTargetView(rtv, green);
        ctx->ResolveSubresource(one, 0, ms, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
        ok = first_pixel(one, &px) && (px & 0xFFFFFF) == 0x00FF00;
    }
    check("4x MSAA, resolve", ok);
    if (rtv) rtv->Release();
    if (ms) ms->Release();
    if (one) one->Release();
}

static void test_compute() {
    static const char* src =
        "RWStructuredBuffer<uint> dst : register(u0);\n"
        "[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID) { dst[id.x] = id.x * 2u + 1u; }\n";
    HMODULE comp = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = comp ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(comp, "D3DCompile")) : nullptr;
    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hc = compile ? compile(src, strlen(src), "cs.hlsl", nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &err) : E_FAIL;
    bool ok = SUCCEEDED(hc);
    if (!ok) printf("  compile 0x%08lx %s\n", (unsigned long)hc, err ? (const char*)err->GetBufferPointer() : "");
    ID3D11ComputeShader* cs = nullptr;
    HRESULT h1 = ok ? dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs) : E_FAIL;
    if (ok && FAILED(h1)) printf("  CreateComputeShader 0x%08lx\n", (unsigned long)h1);
    ok = ok && SUCCEEDED(h1);
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 256 * 4;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = 4;
    ID3D11Buffer *buf = nullptr, *stage = nullptr;
    HRESULT h2 = ok ? dev->CreateBuffer(&bd, nullptr, &buf) : E_FAIL;
    if (ok && FAILED(h2)) printf("  CreateBuffer(UAV) 0x%08lx\n", (unsigned long)h2);
    ok = ok && SUCCEEDED(h2);
    bd.Usage = D3D11_USAGE_STAGING;
    bd.BindFlags = 0;
    bd.MiscFlags = 0;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ok = ok && SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &stage));
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_UNKNOWN;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = 256;
    ID3D11UnorderedAccessView* uav = nullptr;
    HRESULT h3 = ok ? dev->CreateUnorderedAccessView(buf, &ud, &uav) : E_FAIL;
    if (ok && FAILED(h3)) printf("  CreateUnorderedAccessView 0x%08lx\n", (unsigned long)h3);
    ok = ok && SUCCEEDED(h3);
    if (ok) {
        ctx->CSSetShader(cs, nullptr, 0);
        ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        ctx->Dispatch(4, 1, 1);
        ctx->CopyResource(stage, buf);
        D3D11_MAPPED_SUBRESOURCE m;
        ok = SUCCEEDED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m));
        if (ok) {
            const UINT* v = static_cast<const UINT*>(m.pData);
            UINT bad = 0;
            for (UINT i = 0; i < 256; i++) bad += v[i] != i * 2 + 1;
            if (bad) printf("  %u of 256 wrong: %u %u %u %u\n", bad, v[0], v[1], v[2], v[3]);
            ok = bad == 0;
            ctx->Unmap(stage, 0);
        } else {
            printf("  Map failed\n");
        }
    }
    check("compute shader, UAV, read-back", ok);
    if (err) err->Release();
    if (code) code->Release();
    if (uav) uav->Release();
    if (buf) buf->Release();
    if (stage) stage->Release();
    if (cs) cs->Release();
}

static void test_query() {
    D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION, 0};
    ID3D11Query* q = nullptr;
    bool ok = SUCCEEDED(dev->CreateQuery(&qd, &q));
    UINT64 samples = 0;
    if (ok) {
        ctx->Begin(q);
        ctx->End(q);
        int spins = 0;
        while (ctx->GetData(q, &samples, sizeof samples, 0) != S_OK && spins++ < 5000000) {}
        ok = spins < 5000000;
    }
    check("occlusion query result", ok);
    if (q) q->Release();
}

int main() {
    D3D_FEATURE_LEVEL level{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &level, &ctx);
    printf("D3D11CreateDevice 0x%08lx, feature level 0x%x\n", hr, level);
    if (FAILED(hr)) return 2;
    test_deferred();
    test_msaa();
    test_compute();
    test_query();
    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
