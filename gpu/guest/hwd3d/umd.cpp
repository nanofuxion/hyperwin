/* hwd3d_umd.dll: the Direct3D 10/11 user-mode driver of the HyperWin GPU adapter (wddm/README.md,
 * phase 4). Windows' own d3d11.dll loads it for any program, with nothing beside the program. Each
 * driver entry turns into the same HwD3D call (include/hw_d3d.h) the per-program d3d11.dll sends, so
 * the phone side is DXVK as before. Resources carry no kernel allocation: the bytes live on the phone.
 *
 * The table of entries comes from the driver header, read at build time by gen_umd.py into
 * gpu/work/umd-gen. Every entry exists from the first build: the ones not written here log their name
 * once and return an error. Nothing from that header is copied into this file. */
#include "wire.h"

#include <initializer_list>
#include <new>

#include <d3d11.h>
#include <d3d10umddi.h>

#include "hwd3d_methods.h"

/* ---- logging ---- */

static void ulog(const char *fmt, ...)
{
    char s[512];
    va_list a;
    va_start(a, fmt);
    vsnprintf(s, sizeof s, fmt, a);
    va_end(a);
    OutputDebugStringA(s);
    if (getenv("HWD3D_LOG")) fputs(s, stderr);
    /* HWD3D_UMDLOG=<file>: the same lines in a file, for programs whose stderr goes nowhere. */
    char path[MAX_PATH];
    if (GetEnvironmentVariableA("HWD3D_UMDLOG", path, sizeof path) && path[0]) {
        HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD n;
            WriteFile(f, s, (DWORD)strlen(s), &n, NULL);
            CloseHandle(f);
        }
    }
}

/* One line per call of an entry written here, when HWD3D_UMDLOG asks for the log. */
static void ulog_call(const char *name)
{
    char v[8];
    if (GetEnvironmentVariableA("HWD3D_UMDCALLS", v, sizeof v)) ulog("hwd3d_umd: %s\n", name);
}

/* ---- stubs: every table entry has one ---- */

static const char *g_names[2048];
static LONG g_stub_seen[2048];

static void stub_note(int n)
{
    if (n >= 0 && n < 2048 && !InterlockedExchange(&g_stub_seen[n], 1)) ulog("hwd3d_umd: %s is not done yet\n", g_names[n]);
}

template <class R> static R stub_ret() { return R(); }
template <> long stub_ret<long>() { return E_NOTIMPL; }
template <> unsigned long long stub_ret<unsigned long long>() { return 128; }

template <int N, class F> struct Stub;
template <int N, class R, class... A> struct Stub<N, R(APIENTRY *)(A...)> {
    static R APIENTRY fn(A...)
    {
        stub_note(N);
        return stub_ret<R>();
    }
};

/* ---- objects ---- */

struct Dev {
    D3D10DDI_HRTDEVICE rt;
    D3D10DDI_HRTCORELAYER core;
    const D3D11DDI_CORELAYER_DEVICECALLBACKS *ucb;
    const D3DDDI_DEVICECALLBACKS *kcb;
    uint32_t hdev, hctx;  /* the phone's device and immediate context */
    HANDLE kctx;          /* the kernel context the runtime wants a driver to have */
    UINT flags;
};

struct Shadow {
    Shadow *next;
    UINT sub;
    UINT type;
    uint32_t size, row_pitch, depth_pitch;
    int local;
    uint8_t *mem;
};

/* One private block for every kind of object: the driver is told one size for all of them. */
struct Obj {
    uint32_t h;            /* the phone's handle */
    uint32_t dim;          /* resources: D3D11_RESOURCE_DIMENSION */
    struct hwd3d_res r;
    uint32_t subs, samples, array, usage;
    Shadow *shadows;
};

static Dev *DV(D3D10DDI_HDEVICE h) { return static_cast<Dev *>(h.pDrvPrivate); }
static Obj *OB(D3D10DDI_HRESOURCE h) { return static_cast<Obj *>(h.pDrvPrivate); }
static Obj *OB(D3D10DDI_HRENDERTARGETVIEW h) { return static_cast<Obj *>(h.pDrvPrivate); }

static void fail(Dev *d, HRESULT hr)
{
    if (d && d->ucb && d->ucb->pfnSetErrorCb) d->ucb->pfnSetErrorCb(d->core, hr);
}

static void release_obj(Obj *o)
{
    if (!o || !o->h) return;
    AcquireSRWLockExclusive(&g_lock);
    struct hwd3d_call c = {{sizeof c, HWD3D_MAGIC, HWD3D_OP_RELEASE, HWD3D_NO_REPLY, ++g_seq}, o->h, 0};
    raw_send(&c, sizeof c);
    ReleaseSRWLockExclusive(&g_lock);
    while (o->shadows) {
        Shadow *m = o->shadows;
        o->shadows = m->next;
        free(m->mem);
        free(m);
    }
    o->h = 0;
}

/* A call on the phone's context. Under begin/call_*: the lock is held until the call ends. */
static struct wbuf *cx(Dev *d, uint32_t method) { return begin(d->hctx, method, HWD3D_OP_CALL); }
static struct wbuf *dc(Dev *d, uint32_t method) { return begin(d->hdev, method, HWD3D_OP_CALL); }

/* ---- the DDI to the phone's flags ---- */

static UINT bind11(UINT b)
{
    UINT r = b & 0x7Fu;
    if (b & D3D11_DDI_BIND_UNORDERED_ACCESS) r |= D3D11_BIND_UNORDERED_ACCESS;
    return r;
}

static UINT cpu11(UINT c)
{
    return ((c & D3D10_DDI_CPU_ACCESS_WRITE) ? D3D11_CPU_ACCESS_WRITE : 0) | ((c & D3D10_DDI_CPU_ACCESS_READ) ? D3D11_CPU_ACCESS_READ : 0);
}

static UINT misc11(UINT m) { return m & 0xF5u; }

/* ---- resources ---- */

static SIZE_T APIENTRY u_CalcPrivateResourceSize(D3D10DDI_HDEVICE, const D3D11DDIARG_CREATERESOURCE *) { return sizeof(Obj); }

static void APIENTRY u_CreateResource(D3D10DDI_HDEVICE hd, const D3D11DDIARG_CREATERESOURCE *a, D3D10DDI_HRESOURCE hr, D3D10DDI_HRTRESOURCE)
{
    ulog_call("CreateResource");
    Dev *d = DV(hd);
    Obj *o = new (hr.pDrvPrivate) Obj();
    uint32_t layers = a->ArraySize ? a->ArraySize : 1, mips = a->MipLevels ? a->MipLevels : 1;
    const D3D10DDI_MIPINFO *mi = a->pMipInfoList;
    uint32_t method;
    union {
        D3D11_BUFFER_DESC b;
        D3D11_TEXTURE1D_DESC t1;
        D3D11_TEXTURE2D_DESC t2;
        D3D11_TEXTURE3D_DESC t3;
    } desc;
    uint32_t desc_size;
    memset(&desc, 0, sizeof desc);
    o->samples = a->SampleDesc.Count ? a->SampleDesc.Count : 1;
    switch (a->ResourceDimension) {
    case D3D10DDIRESOURCE_BUFFER:
        desc.b.ByteWidth = mi ? mi[0].TexelWidth : 0;
        desc.b.Usage = (D3D11_USAGE)a->Usage;
        desc.b.BindFlags = bind11(a->BindFlags);
        desc.b.CPUAccessFlags = cpu11(a->MapFlags);
        desc.b.MiscFlags = misc11(a->MiscFlags);
        desc.b.StructureByteStride = a->ByteStride;
        method = HMX_ID3D11Device_CreateBuffer;
        desc_size = sizeof desc.b;
        o->dim = 1;
        o->r = {1, 0, desc.b.ByteWidth, 1, 1, 1};
        layers = 1;
        mips = 1;
        break;
    case D3D10DDIRESOURCE_TEXTURE1D:
        desc.t1 = {mi ? mi[0].TexelWidth : 0, mips, layers, a->Format, (D3D11_USAGE)a->Usage, bind11(a->BindFlags),
                   cpu11(a->MapFlags), misc11(a->MiscFlags)};
        method = HMX_ID3D11Device_CreateTexture1D;
        desc_size = sizeof desc.t1;
        o->dim = 2;
        o->r = {2, (uint32_t)a->Format, desc.t1.Width, 1, 1, mips};
        break;
    case D3D10DDIRESOURCE_TEXTURE3D:
        desc.t3 = {mi ? mi[0].TexelWidth : 0, mi ? mi[0].TexelHeight : 0, mi ? mi[0].TexelDepth : 0, mips, a->Format,
                   (D3D11_USAGE)a->Usage, bind11(a->BindFlags), cpu11(a->MapFlags), misc11(a->MiscFlags)};
        method = HMX_ID3D11Device_CreateTexture3D;
        desc_size = sizeof desc.t3;
        o->dim = 4;
        o->r = {4, (uint32_t)a->Format, desc.t3.Width, desc.t3.Height, desc.t3.Depth, mips};
        layers = 1;
        break;
    default: {  /* 2D and cube */
        UINT misc = misc11(a->MiscFlags);
        if (a->ResourceDimension == D3D10DDIRESOURCE_TEXTURECUBE) misc |= D3D11_RESOURCE_MISC_TEXTURECUBE;
        desc.t2 = {mi ? mi[0].TexelWidth : 0, mi ? mi[0].TexelHeight : 0, mips, layers, a->Format,
                   {o->samples, a->SampleDesc.Quality}, (D3D11_USAGE)a->Usage, bind11(a->BindFlags), cpu11(a->MapFlags), misc};
        method = HMX_ID3D11Device_CreateTexture2D;
        desc_size = sizeof desc.t2;
        o->dim = 3;
        o->r = {3, (uint32_t)a->Format, desc.t2.Width, desc.t2.Height, 1, mips};
        break;
    }
    }
    o->array = layers;
    o->usage = a->Usage;
    o->subs = mips * layers;
    struct wbuf *b = dc(d, method);
    put_arr(b, &desc, 1, desc_size);
    const D3D10_DDIARG_SUBRESOURCE_UP *init = a->pInitialDataUP;
    put32(b, init ? o->subs : 0);
    for (uint32_t i = 0; init && i < o->subs; i++) {
        uint32_t row, rows, slices;
        if (!init[i].pSysMem || !hwd3d_region(&o->r, i, NULL, &row, &rows, &slices)) {
            call_abort(b);
            fail(d, E_INVALIDARG);
            return;
        }
        put_rows(b, (const uint8_t *)init[i].pSysMem, row, rows, slices, o->dim == 2 ? row : init[i].SysMemPitch, init[i].SysMemSlicePitch);
    }
    put_want(b, &o);
    struct rd r;
    HRESULT res = (HRESULT)call_wait(b, &r);
    o->h = take32(&r);
    call_end(&r);
    if (FAILED(res) || !o->h) fail(d, FAILED(res) ? res : E_FAIL);
}

static void APIENTRY u_DestroyResource(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE h) { release_obj(OB(h)); }

/* ---- views ---- */

static SIZE_T APIENTRY u_CalcPrivateRenderTargetViewSize(D3D10DDI_HDEVICE, const D3D10DDIARG_CREATERENDERTARGETVIEW *) { return sizeof(Obj); }

static void APIENTRY u_CreateRenderTargetView(D3D10DDI_HDEVICE hd, const D3D10DDIARG_CREATERENDERTARGETVIEW *a,
                                              D3D10DDI_HRENDERTARGETVIEW hv, D3D10DDI_HRTRENDERTARGETVIEW)
{
    Dev *d = DV(hd);
    Obj *v = new (hv.pDrvPrivate) Obj();
    Obj *res = OB(a->hDrvResource);
    D3D11_RENDER_TARGET_VIEW_DESC vd;
    memset(&vd, 0, sizeof vd);
    vd.Format = a->Format;
    switch (a->ResourceDimension) {
    case D3D10DDIRESOURCE_BUFFER:
        vd.ViewDimension = D3D11_RTV_DIMENSION_BUFFER;
        vd.Buffer.FirstElement = a->Buffer.FirstElement;
        vd.Buffer.NumElements = a->Buffer.NumElements;
        break;
    case D3D10DDIRESOURCE_TEXTURE1D:
        if (res->array > 1) {
            vd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE1DARRAY;
            vd.Texture1DArray = {a->Tex1D.MipSlice, a->Tex1D.FirstArraySlice, a->Tex1D.ArraySize};
        } else {
            vd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE1D;
            vd.Texture1D.MipSlice = a->Tex1D.MipSlice;
        }
        break;
    case D3D10DDIRESOURCE_TEXTURE3D:
        vd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
        vd.Texture3D = {a->Tex3D.MipSlice, a->Tex3D.FirstW, a->Tex3D.WSize};
        break;
    default:
        if (res->samples > 1) {
            if (res->array > 1) {
                vd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
                vd.Texture2DMSArray = {a->Tex2D.FirstArraySlice, a->Tex2D.ArraySize};
            } else {
                vd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
            }
        } else if (res->array > 1) {
            vd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            vd.Texture2DArray = {a->Tex2D.MipSlice, a->Tex2D.FirstArraySlice, a->Tex2D.ArraySize};
        } else {
            vd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            vd.Texture2D.MipSlice = a->Tex2D.MipSlice;
        }
        break;
    }
    struct wbuf *b = dc(d, HMX_ID3D11Device_CreateRenderTargetView);
    put32(b, res->h);
    put_arr(b, &vd, 1, sizeof vd);
    put32(b, 1);
    struct rd r;
    HRESULT hr = (HRESULT)call_wait(b, &r);
    v->h = take32(&r);
    call_end(&r);
    if (FAILED(hr) || !v->h) fail(d, FAILED(hr) ? hr : E_FAIL);
}

static void APIENTRY u_DestroyRenderTargetView(D3D10DDI_HDEVICE, D3D10DDI_HRENDERTARGETVIEW h) { release_obj(OB(h)); }

/* ---- drawing and copying ---- */

static void APIENTRY u_ClearRenderTargetView(D3D10DDI_HDEVICE hd, D3D10DDI_HRENDERTARGETVIEW hv, FLOAT color[4])
{
    ulog_call("ClearRenderTargetView");
    struct wbuf *b = cx(DV(hd), HMX_ID3D11DeviceContext_ClearRenderTargetView);
    put32(b, OB(hv)->h);
    put_arr(b, color, 4, sizeof(FLOAT));
    call_async(b);
}

static void APIENTRY u_ResourceCopy(D3D10DDI_HDEVICE hd, D3D10DDI_HRESOURCE dst, D3D10DDI_HRESOURCE src)
{
    ulog_call("ResourceCopy");
    struct wbuf *b = cx(DV(hd), HMX_ID3D11DeviceContext_CopyResource);
    put32(b, OB(dst)->h);
    put32(b, OB(src)->h);
    call_async(b);
}

static void APIENTRY u_Flush(D3D10DDI_HDEVICE hd) { call_async(cx(DV(hd), HMX_ID3D11DeviceContext_Flush)); }

/* 11.1's Flush takes flags and says whether it flushed. */
static BOOL APIENTRY u_Flush1(D3D10DDI_HDEVICE hd, UINT)
{
    u_Flush(hd);
    return TRUE;
}

/* ---- map and unmap: the shadow copy (d3d11.c has the same) ---- */

static Shadow *shadow_for(Obj *o, UINT sub, uint32_t size)
{
    Shadow *m = o->shadows;
    while (m && m->sub != sub) m = m->next;
    if (!m) {
        m = static_cast<Shadow *>(calloc(1, sizeof *m));
        m->sub = sub;
        m->next = o->shadows;
        o->shadows = m;
    }
    if (m->size != size) {
        free(m->mem);
        m->mem = static_cast<uint8_t *>(calloc(1, size ? size : 1));
        m->size = size;
    }
    return m;
}

/* A write the phone need not wait for: the program writes into the copy and Unmap sends it. */
static void map_local(D3D10DDI_HDEVICE hd, D3D10DDI_HRESOURCE hr, UINT sub, UINT type, D3D10DDI_MAPPED_SUBRESOURCE *out)
{
    Obj *o = OB(hr);
    uint32_t row, rows, slices;
    if (!hwd3d_region(&o->r, sub, NULL, &row, &rows, &slices)) {
        fail(DV(hd), E_INVALIDARG);
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    Shadow *m = shadow_for(o, sub, row * rows * slices);
    m->type = type;
    m->row_pitch = row;
    m->depth_pitch = row * rows;
    m->local = 1;
    ReleaseSRWLockExclusive(&g_lock);
    out->pData = m->mem;
    out->RowPitch = row;
    out->DepthPitch = row * rows;
}

static void map_phone(D3D10DDI_HDEVICE hd, D3D10DDI_HRESOURCE hr, UINT sub, UINT type, UINT flags, D3D10DDI_MAPPED_SUBRESOURCE *out)
{
    Dev *d = DV(hd);
    Obj *o = OB(hr);
    struct wbuf *b = cx(d, HMX_ID3D11DeviceContext_Map);
    put32(b, o->h);
    put32(b, sub);
    put32(b, type);
    put32(b, (flags & D3D10_DDI_MAP_FLAG_DONOTWAIT) ? D3D11_MAP_FLAG_DO_NOT_WAIT : 0);
    struct rd r;
    HRESULT res = (HRESULT)call_wait(b, &r);
    uint32_t row_pitch = take32(&r), depth_pitch = take32(&r), size = take32(&r), has = take32(&r);
    if (FAILED(res)) {
        call_end(&r);
        fail(d, res);
        return;
    }
    Shadow *m = shadow_for(o, sub, size);
    m->type = type;
    m->row_pitch = row_pitch;
    m->depth_pitch = depth_pitch;
    m->local = 0;
    if (has) take(&r, m->mem, size);
    call_end(&r);
    out->pData = m->mem;
    out->RowPitch = row_pitch;
    out->DepthPitch = depth_pitch;
}

static void APIENTRY u_StagingResourceMap(D3D10DDI_HDEVICE hd, D3D10DDI_HRESOURCE hr, UINT sub, D3D10_DDI_MAP type, UINT flags,
                                          D3D10DDI_MAPPED_SUBRESOURCE *out)
{
    memset(out, 0, sizeof *out);
    map_phone(hd, hr, sub, type, flags, out);
}

static void APIENTRY u_ResourceMap(D3D10DDI_HDEVICE hd, D3D10DDI_HRESOURCE hr, UINT sub, D3D10_DDI_MAP type, UINT flags,
                                   D3D10DDI_MAPPED_SUBRESOURCE *out)
{
    memset(out, 0, sizeof *out);
    if (type == D3D10_DDI_MAP_WRITE_DISCARD || type == D3D10_DDI_MAP_WRITE_NOOVERWRITE) map_local(hd, hr, sub, type, out);
    else map_phone(hd, hr, sub, type, flags, out);
}

static void APIENTRY u_Unmap(D3D10DDI_HDEVICE hd, D3D10DDI_HRESOURCE hr, UINT sub)
{
    ulog_call("Unmap");
    Dev *d = DV(hd);
    Obj *o = OB(hr);
    struct wbuf *b = cx(d, HMX_ID3D11DeviceContext_Unmap);
    Shadow *m = o->shadows;
    while (m && m->sub != sub) m = m->next;
    put32(b, o->h);
    put32(b, sub);
    put32(b, m && m->local);
    if (m && m->local) {
        put32(b, m->type);
        put32(b, m->row_pitch);
        put32(b, m->row_pitch ? m->depth_pitch / m->row_pitch : 0);
        put_arr(b, m->mem, m->size, 1);
        m->local = 0;
    } else if (m && m->type != D3D10_DDI_MAP_READ) {
        put_arr(b, m->mem, m->size, 1);
    } else {
        put32(b, 0);
    }
    call_async(b);
}

/* ---- format support ---- */

/* What the driver tells the runtime about a format: the optional features only (the runtime knows the
 * rest). Asked of the phone once per format. */
static UINT ddi_format_bits(Dev *d, DXGI_FORMAT f)
{
    static UINT cache[256];
    static bool have[256];
    if ((UINT)f < 256 && have[f]) return cache[f];
    struct wbuf *b = dc(d, HMX_ID3D11Device_CheckFormatSupport);
    put32(b, f);
    put32(b, 1);
    struct rd r;
    HRESULT hr = (HRESULT)call_wait(b, &r);
    UINT s = take32(&r);
    call_end(&r);
    UINT o = 0;
    /* The depth planes as shader views only: no resource has these formats, so nothing optional applies
     * (the runtime refuses a driver that says a plane of depth can be multisampled). */
    bool view_only = f == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS || f == DXGI_FORMAT_X32_TYPELESS_G8X24_UINT ||
                     f == DXGI_FORMAT_R24_UNORM_X8_TYPELESS || f == DXGI_FORMAT_X24_TYPELESS_G8_UINT;
    if (SUCCEEDED(hr) && !view_only) {
        if (s & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) o |= D3D10_DDI_FORMAT_SUPPORT_SHADER_SAMPLE;
        if (s & D3D11_FORMAT_SUPPORT_RENDER_TARGET) o |= D3D10_DDI_FORMAT_SUPPORT_RENDERTARGET;
        if ((s & D3D11_FORMAT_SUPPORT_BLENDABLE) && (s & D3D11_FORMAT_SUPPORT_RENDER_TARGET)) o |= D3D10_DDI_FORMAT_SUPPORT_BLENDABLE;
        /* Only a format that can be a target can be a multisampled target. Depth formats count as one. */
        bool depth = f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || f == DXGI_FORMAT_D32_FLOAT || f == DXGI_FORMAT_D24_UNORM_S8_UINT ||
                     f == DXGI_FORMAT_D16_UNORM;
        if ((s & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET) && ((s & D3D11_FORMAT_SUPPORT_RENDER_TARGET) || depth))
            o |= D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET;
        if (s & D3D11_FORMAT_SUPPORT_MULTISAMPLE_LOAD) o |= D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_LOAD;
        if (s & D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER) o |= D3D11_1DDI_FORMAT_SUPPORT_VERTEX_BUFFER;
        if (s & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) o |= D3D11_1DDI_FORMAT_SUPPORT_UAV_WRITES;
        if (s & D3D11_FORMAT_SUPPORT_BUFFER) o |= D3D11_1DDI_FORMAT_SUPPORT_BUFFER;
        if (s & D3D11_FORMAT_SUPPORT_SHADER_GATHER) o |= D3D11_1DDI_FORMAT_SUPPORT_SHADER_GATHER;
    } else if (f == DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM) {
        o = D3D10_DDI_FORMAT_SUPPORT_NOT_SUPPORTED;  /* the one format the runtime lets a driver refuse outright */
    }
    char name[24], ov[16];
    snprintf(name, sizeof name, "HWD3D_FMT_%u", (unsigned)f);
    if (GetEnvironmentVariableA(name, ov, sizeof ov)) o = (UINT)strtoul(ov, NULL, 16);  /* bring-up override */
    if ((UINT)f < 256) {
        cache[f] = o;
        have[f] = true;
    }
    return o;
}

static void APIENTRY u_CheckFormatSupport(D3D10DDI_HDEVICE hd, DXGI_FORMAT f, UINT *out)
{
    ulog_call("CheckFormatSupport");
    *out = ddi_format_bits(DV(hd), f);
    char v[8];
    if (GetEnvironmentVariableA("HWD3D_UMDFMT", v, sizeof v)) ulog("hwd3d_umd: format %u -> %08x\n", (unsigned)f, *out);
}

static void APIENTRY u_CheckMultisampleQualityLevels(D3D10DDI_HDEVICE hd, DXGI_FORMAT f, UINT samples, UINT *out)
{
    ulog_call("CheckMultisampleQualityLevels");
    Dev *d = DV(hd);
    *out = 0;
    /* The runtime holds the two answers to each other: a format with levels for some sample count must
     * say it can be loaded or targeted that way, and one that says so must have levels. */
    UINT bits = ddi_format_bits(d, f);
    if (!(bits & (D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET | D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_LOAD))) return;
    static UINT8 cache[256][33];  /* levels + 1, or 0 for not asked yet */
    if ((UINT)f < 256 && samples < 33 && cache[f][samples]) {
        *out = cache[f][samples] - 1u;
        return;
    }
    struct wbuf *b = dc(d, HMX_ID3D11Device_CheckMultisampleQualityLevels);
    put32(b, f);
    put32(b, samples);
    put32(b, 1);
    struct rd r;
    HRESULT hr = (HRESULT)call_wait(b, &r);
    UINT n = take32(&r);
    call_end(&r);
    *out = SUCCEEDED(hr) ? n : 0;
    if ((UINT)f < 256 && samples < 33 && *out < 255) cache[f][samples] = (UINT8)(*out + 1);
    char v[8];
    if (GetEnvironmentVariableA("HWD3D_UMDFMT", v, sizeof v)) ulog("hwd3d_umd:   msaa %u x%u -> %u\n", (unsigned)f, samples, *out);
}

/* WDDM 1.3 adds flags (tiled resources); the answer is the same. */
static void APIENTRY u_CheckMultisampleQualityLevels1(D3D10DDI_HDEVICE hd, DXGI_FORMAT f, UINT samples, UINT, UINT *out)
{
    u_CheckMultisampleQualityLevels(hd, f, samples, out);
}

/* The phone offers no device-specific counters. The runtime reads these, so they are zeroed. */
static void APIENTRY u_CheckCounterInfo(D3D10DDI_HDEVICE, D3D10DDI_COUNTER_INFO *info) { memset(info, 0, sizeof *info); }

static void APIENTRY u_DestroyDevice(D3D10DDI_HDEVICE hd)
{
    ulog_call("DestroyDevice");
    Dev *d = DV(hd);
    AcquireSRWLockExclusive(&g_lock);
    for (uint32_t h : {d->hctx, d->hdev}) {
        struct hwd3d_call c = {{sizeof c, HWD3D_MAGIC, HWD3D_OP_RELEASE, HWD3D_NO_REPLY, ++g_seq}, h, 0};
        if (h) raw_send(&c, sizeof c);
    }
    ReleaseSRWLockExclusive(&g_lock);
    d->hctx = d->hdev = 0;
    if (d->kctx && d->kcb && d->kcb->pfnDestroyContextCb) {
        D3DDDICB_DESTROYCONTEXT dc = {d->kctx};
        d->kcb->pfnDestroyContextCb(d->rt.handle, &dc);
        d->kctx = NULL;
    }
}

/* ---- the device's function table ---- */

template <class A, class B> struct Same { static constexpr bool v = false; };
template <class A> struct Same<A, A> { static constexpr bool v = true; };

/* Every entry of a table points at its stub; the ones this file has follow. The tables of 11.0 and 11.1
 * share their names, so one function fills either. */
#define UMD_FILL(TT, m) \
    if constexpr (Same<Tbl, TT>::v) { \
        t->m = Stub<__COUNTER__, decltype(t->m)>::fn; \
        g_names[__COUNTER__ - 1] = #m; \
    }
template <class Tbl> static void fill_device_funcs(Tbl *t)
{
#include "umd_tables.inc"
    t->pfnCalcPrivateResourceSize = u_CalcPrivateResourceSize;
    t->pfnCreateResource = u_CreateResource;
    t->pfnDestroyResource = u_DestroyResource;
    t->pfnCalcPrivateRenderTargetViewSize = u_CalcPrivateRenderTargetViewSize;
    t->pfnCreateRenderTargetView = u_CreateRenderTargetView;
    t->pfnDestroyRenderTargetView = u_DestroyRenderTargetView;
    t->pfnClearRenderTargetView = u_ClearRenderTargetView;
    t->pfnResourceCopy = u_ResourceCopy;
    if constexpr (Same<Tbl, D3D11_1DDI_DEVICEFUNCS>::v || Same<Tbl, D3DWDDM1_3DDI_DEVICEFUNCS>::v) t->pfnFlush = u_Flush1;
    else t->pfnFlush = u_Flush;
    t->pfnStagingResourceMap = u_StagingResourceMap;
    t->pfnStagingResourceUnmap = u_Unmap;
    t->pfnResourceMap = u_ResourceMap;
    t->pfnResourceUnmap = u_Unmap;
    t->pfnCheckFormatSupport = u_CheckFormatSupport;
    if constexpr (Same<Tbl, D3DWDDM1_3DDI_DEVICEFUNCS>::v) t->pfnCheckMultisampleQualityLevels = u_CheckMultisampleQualityLevels1;
    else t->pfnCheckMultisampleQualityLevels = u_CheckMultisampleQualityLevels;
    t->pfnCheckCounterInfo = u_CheckCounterInfo;
    t->pfnDestroyDevice = u_DestroyDevice;
}
#undef UMD_FILL

/* ---- the DXGI entries the runtime asks of the driver ---- */

/* The DXGI entries are all one shape, an answer and one pointer to arguments. Each table is the entries of
 * the one before it and more, so the table the runtime gave is filled to its size. */
static HRESULT __stdcall dxgi_stub(void *)
{
    ulog("hwd3d_umd: a DXGI entry is not done yet\n");
    return E_NOTIMPL;
}

static void fill_dxgi(DXGI_DDI_BASE_ARGS *a, UINT interface_version)
{
    size_t n = sizeof(DXGI1_1_DDI_BASE_FUNCTIONS);
    if (interface_version == D3D11_1_DDI_INTERFACE_VERSION) n = sizeof(DXGI1_2_DDI_BASE_FUNCTIONS);
    else if (interface_version >= D3DWDDM1_3_DDI_INTERFACE_VERSION) n = sizeof(DXGI1_3_DDI_BASE_FUNCTIONS);
    void **f = reinterpret_cast<void **>(a->pDXGIDDIBaseFunctions2);
    for (size_t i = 0; i < n / sizeof(void *); i++) f[i] = reinterpret_cast<void *>(dxgi_stub);
}

/* ---- the adapter ---- */

static SIZE_T APIENTRY a_CalcPrivateDeviceSize(D3D10DDI_HADAPTER, const D3D10DDIARG_CALCPRIVATEDEVICESIZE *a)
{
    ulog("hwd3d_umd: CalcPrivateDeviceSize %p\n", (const void *)a);
    ulog("hwd3d_umd: interface 0x%x, version %u, flags 0x%x\n", a->Interface, a->Version, a->Flags);
    return sizeof(Dev);
}

static HRESULT APIENTRY a_CreateDevice(D3D10DDI_HADAPTER, D3D10DDIARG_CREATEDEVICE *a)
{
    ulog("hwd3d_umd: CreateDevice, interface 0x%x, version %u, flags 0x%x\n", a->Interface, a->Version, a->Flags);
    if (a->Interface != D3D11_0_DDI_INTERFACE_VERSION && a->Interface != D3D11_1_DDI_INTERFACE_VERSION &&
        a->Interface != D3DWDDM1_3_DDI_INTERFACE_VERSION)
        return E_FAIL;
    if (!ready()) return E_FAIL;
    Dev *d = new (a->hDrvDevice.pDrvPrivate) Dev();
    d->rt = a->hRTDevice;
    d->core = a->hRTCoreLayer;
    d->ucb = a->p11UMCallbacks;
    d->kcb = a->pKTCallbacks;
    d->flags = a->Flags;
    /* The phone's device and context, at the feature levels DXVK gives. */
    struct wbuf *b = begin(0, 0, HWD3D_OP_CREATE_DEVICE);
    put32(b, 0);
    put32(b, 0);
    struct rd r;
    HRESULT hr = (HRESULT)call_wait(b, &r);
    d->hdev = take32(&r);
    (void)take32(&r);
    d->hctx = take32(&r);
    call_end(&r);
    if (FAILED(hr) || !d->hdev || !d->hctx) {
        ulog("hwd3d_umd: the phone made no device (0x%08lx)\n", (unsigned long)hr);
        return FAILED(hr) ? hr : E_FAIL;
    }
    /* The runtime wants a kernel context from every driver, though no command buffer goes through it. */
    if (d->kcb && d->kcb->pfnCreateContextCb) {
        D3DDDICB_CREATECONTEXT cc;
        memset(&cc, 0, sizeof cc);
        HRESULT kh = d->kcb->pfnCreateContextCb(d->rt.handle, &cc);
        ulog("hwd3d_umd: kernel context 0x%08lx, handle %p\n", (unsigned long)kh, cc.hContext);
        if (SUCCEEDED(kh)) d->kctx = cc.hContext;
    }
    if (a->Interface == D3DWDDM1_3_DDI_INTERFACE_VERSION) fill_device_funcs(a->pWDDM1_3DeviceFuncs);
    else if (a->Interface == D3D11_1_DDI_INTERFACE_VERSION) fill_device_funcs(a->p11_1DeviceFuncs);
    else fill_device_funcs(a->p11DeviceFuncs);
    fill_dxgi(&a->DXGIBaseDDI, a->Interface);
    ulog("hwd3d_umd: device %u, context %u\n", d->hdev, d->hctx);
    return S_OK;
}

static HRESULT APIENTRY a_CloseAdapter(D3D10DDI_HADAPTER)
{
    ulog("hwd3d_umd: CloseAdapter\n");
    return S_OK;
}

static HRESULT APIENTRY a_GetSupportedVersions(D3D10DDI_HADAPTER, UINT32 *n, UINT64 *v)
{
    if (!n) return E_INVALIDARG;
    ulog("hwd3d_umd: GetSupportedVersions (room for %u)\n", *n);
    if (v && *n >= 3) {
        v[0] = D3DWDDM1_3_DDI_SUPPORTED;
        v[1] = D3D11_1_DDI_SUPPORTED;
        v[2] = D3D11_0_DDI_SUPPORTED;
    }
    *n = 3;
    return S_OK;
}

static HRESULT APIENTRY a_GetCaps(D3D10DDI_HADAPTER, const D3D10_2DDIARG_GETCAPS *c)
{
    ulog("hwd3d_umd: GetCaps type %d, %u bytes\n", c ? (int)c->Type : -1, c ? c->DataSize : 0);
    if (!c || !c->pData) return E_INVALIDARG;
    memset(c->pData, 0, c->DataSize);
    /* HWD3D_CAP_<type>=<hex> overrides a caps word while the driver is being brought up. */
    char name[32], e[16];
    snprintf(name, sizeof name, "HWD3D_CAP_%d", (int)c->Type);
    if (c->DataSize >= 4 && GetEnvironmentVariableA(name, e, sizeof e)) {
        *static_cast<UINT *>(c->pData) = (UINT)strtoul(e, NULL, 16);
        return S_OK;
    }
    switch (c->Type) {    case D3D11DDICAPS_THREADING:
        /* Calls from several threads are safe: the channel takes one at a time. No command lists: the
         * runtime records deferred contexts itself and replays them on this one. */
        if (c->DataSize >= 4) *static_cast<UINT *>(c->pData) = D3D11DDICAPS_FREETHREADED;
        break;
    case D3D11DDICAPS_3DPIPELINESUPPORT:
        /* Levels 10.0 and 10.1. Claiming 11.0 as well makes the runtime refuse the adapter before it asks for
         * a device; what it wants for that is not found yet (wddm/README.md, phase 4). */
        if (c->DataSize >= 4) *static_cast<UINT *>(c->pData) = D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(D3D11DDI_3DPIPELINELEVEL_10_0) |
                                                                 D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(D3D11DDI_3DPIPELINELEVEL_10_1);
        break;
    default:
        break;  /* zeroes: no optional feature */
    }
    return S_OK;
}

extern "C" HRESULT APIENTRY OpenAdapter10_2(D3D10DDIARG_OPENADAPTER *a)
{
    ulog("hwd3d_umd: OpenAdapter10_2, interface 0x%x, version %u\n", a ? a->Interface : 0, a ? a->Version : 0);
    if (!a || !a->pAdapterFuncs_2) return E_INVALIDARG;
    static int token;
    a->hAdapter.pDrvPrivate = &token;
    D3D10_2DDI_ADAPTERFUNCS *f = a->pAdapterFuncs_2;
    f->pfnCalcPrivateDeviceSize = a_CalcPrivateDeviceSize;
    f->pfnCreateDevice = a_CreateDevice;
    f->pfnCloseAdapter = a_CloseAdapter;
    f->pfnGetSupportedVersions = a_GetSupportedVersions;
    f->pfnGetCaps = a_GetCaps;
    return S_OK;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD, void *) { return TRUE; }
