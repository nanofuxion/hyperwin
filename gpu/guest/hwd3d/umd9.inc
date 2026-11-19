/* The Direct3D 9 driver part (wddm/README.md phase 7), included at the end of umd.cpp so it shares the
 * channel. Windows' d3d9.dll (and, through it, Direct3D 8, 7 and DirectDraw) calls OpenAdapter here; the
 * device functions send HWD3D_OP_D9 requests (include/hw_d3d.h) that DXVK's Direct3D 9 runs on the phone.
 * The driver interface types come from the WDK's d3dumddi.h at build time. */
#include <d3d9.h>
#include <d3dumddi.h>
#include <algorithm>

namespace d9 {

static int g_logged = -1;
static bool logging()
{
    if (g_logged < 0) g_logged = calls_logged() ? 1 : 0;
    return g_logged;
}
#define D9LOG(...) do { if (logging()) ulog(__VA_ARGS__); } while (0)

/* ---- formats ---- */

static const D3DDDIFORMAT kDXT1 = (D3DDDIFORMAT)MAKEFOURCC('D', 'X', 'T', '1');
static bool is_bc(D3DDDIFORMAT f)
{
    return f == kDXT1 || f == (D3DDDIFORMAT)MAKEFOURCC('D', 'X', 'T', '2') || f == (D3DDDIFORMAT)MAKEFOURCC('D', 'X', 'T', '3') ||
           f == (D3DDDIFORMAT)MAKEFOURCC('D', 'X', 'T', '4') || f == (D3DDDIFORMAT)MAKEFOURCC('D', 'X', 'T', '5');
}

/* Bits per pixel, or for block formats bytes per 4x4 block times 8 / 16 so rows work out (see row_bytes). */
static uint32_t bits_of(D3DDDIFORMAT f)
{
    switch ((uint32_t)f) {
    case 21: case 22: case 31: case 32: case 33: case 34: case 35: case 62: case 63: case 64: case 67: case 71: case 75: case 77:
    case 79: case 82: case 83: case 102: case 112: case 114:
        return 32;
    case 23: case 24: case 25: case 26: case 29: case 30: case 40: case 51: case 60: case 61: case 70: case 73: case 80: case 81:
    case 101: case 111: case 117:
        return 16;
    case 27: case 28: case 41: case 50: case 52:
        return 8;
    case 36: case 110: case 113: case 115:
        return 64;
    case 116:
        return 128;
    case 20:  /* R8G8B8 */
        return 24;
    default:
        if (f == (D3DDDIFORMAT)MAKEFOURCC('U', 'Y', 'V', 'Y') || f == (D3DDDIFORMAT)MAKEFOURCC('Y', 'U', 'Y', '2')) return 16;
        return 32;
    }
}

static uint32_t row_bytes(D3DDDIFORMAT f, uint32_t w)
{
    if (is_bc(f)) return ((w + 3) / 4) * (f == kDXT1 ? 8 : 16);
    return (w * bits_of(f) + 7) / 8;
}
static uint32_t rows_of(D3DDDIFORMAT f, uint32_t h) { return is_bc(f) ? (h + 3) / 4 : h; }

/* ---- objects ---- */

struct Sub {
    uint32_t w, h, d, pitch, slice;
    std::vector<uint8_t> mem;  /* the CPU copy a lock hands out */
    bool cpu_ok;               /* mem holds what the phone has (or nothing was ever written there) */
    const uint8_t *sys;        /* system-memory resources: the runtime's own memory */
    uint32_t sys_pitch, sys_slice;
};

struct Res {
    uint32_t h;                /* the phone's handle (0 for system-memory resources) */
    uint32_t kind;             /* HWD3D9_* kind */
    D3DDDIFORMAT format;
    uint32_t levels;
    bool sysmem, gpu_writes, buffer;
    std::vector<Sub> subs;
};

struct Dev {
    HANDLE rt;
    D3DDDI_DEVICECALLBACKS cb;
    uint32_t h;                       /* the phone's device */
    const uint8_t *um_vb;             /* a user-memory vertex stream (SetStreamSourceUm) */
    uint32_t um_stride, um_stream;
    const uint8_t *um_ib;             /* user-memory indices (SetIndicesUm) */
    uint32_t um_isize;
    Res *rts[4];
};

struct Adapter {
    HANDLE rt;
    D3DDDI_ADAPTERCALLBACKS cb;
    bool caps_ok;
    D3DCAPS9 caps;
    std::vector<FORMATOP> formats;
};

static Dev *DV(HANDLE h) { return static_cast<Dev *>(h); }
static Res *RS(HANDLE h) { return static_cast<Res *>(h); }

/* A request to the phone for device d. */
static struct wbuf *req(uint32_t object, uint32_t method) { return begin(object, method, HWD3D_OP_D9); }

static void send(struct wbuf *b) { call_async(b); }

static HRESULT wait_status(struct wbuf *b)
{
    struct rd r;
    HRESULT hr = (HRESULT)call_wait(b, &r);
    call_end(&r);
    return hr;
}

static void release_handle(uint32_t h)
{
    if (!h) return;
    send(req(h, HWD3D9_RELEASE));
}

/* ---- the adapter ---- */

/* The formats Windows may ask about, and what the phone says of each. */
static void load_formats(Adapter *a)
{
    static const uint32_t cand[] = {21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 50, 51, 52, 60, 61, 62, 63, 64, 67,
                                    70, 71, 73, 75, 77, 79, 80, 81, 82, 83, 110, 111, 112, 113, 114, 115, 116, 117,
                                    MAKEFOURCC('D', 'X', 'T', '1'), MAKEFOURCC('D', 'X', 'T', '2'), MAKEFOURCC('D', 'X', 'T', '3'),
                                    MAKEFOURCC('D', 'X', 'T', '4'), MAKEFOURCC('D', 'X', 'T', '5'), MAKEFOURCC('I', 'N', 'T', 'Z'),
                                    MAKEFOURCC('D', 'F', '2', '4'), MAKEFOURCC('D', 'F', '1', '6'), MAKEFOURCC('N', 'U', 'L', 'L')};
    const uint32_t n = sizeof cand / sizeof *cand;
    struct wbuf *b = req(0, HWD3D9_FORMATS);
    put_arr(b, cand, n, 4);
    struct rd r;
    HRESULT hr = (HRESULT)call_wait(b, &r);
    uint32_t bits[sizeof cand / sizeof *cand] = {};
    for (uint32_t i = 0; i < n; i++) bits[i] = take32(&r);
    call_end(&r);
    if (FAILED(hr)) return;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t x = bits[i], ops = 0;
        if (x & HWD3D9_FMT_TEXTURE) ops |= FORMATOP_TEXTURE | FORMATOP_OFFSCREENPLAIN;
        if (x & HWD3D9_FMT_CUBE) ops |= FORMATOP_CUBETEXTURE;
        if (x & HWD3D9_FMT_VOLUME) ops |= FORMATOP_VOLUMETEXTURE;
        if (x & HWD3D9_FMT_RT) ops |= FORMATOP_OFFSCREEN_RENDERTARGET | FORMATOP_SAME_FORMAT_RENDERTARGET;
        if (x & HWD3D9_FMT_DS) ops |= FORMATOP_ZSTENCIL | FORMATOP_ZSTENCIL_WITH_ARBITRARY_COLOR_DEPTH;
        if (x & HWD3D9_FMT_SRGBREAD) ops |= FORMATOP_SRGBREAD;
        if (x & HWD3D9_FMT_SRGBWRITE) ops |= FORMATOP_SRGBWRITE;
        if (x & HWD3D9_FMT_VTX_TEX) ops |= FORMATOP_VERTEXTEXTURE;
        /* The screen can show these whatever the phone's mode list says (it has no display of its own). */
        bool display = cand[i] == 22 || cand[i] == 23 || cand[i] == 35;   /* not X1R5G5B5 too: the runtime disables a driver that offers both 16-bit display modes */
        if (cand[i] != 24 && ((x & HWD3D9_FMT_DISPLAY) || (display && (x & HWD3D9_FMT_RT)))) ops |= FORMATOP_DISPLAYMODE | FORMATOP_3DACCELERATION;
        if (x & HWD3D9_FMT_AUTOGEN) ops |= FORMATOP_AUTOGENMIPMAP;
        if (!(x & HWD3D9_FMT_BLEND) && (x & HWD3D9_FMT_RT)) ops |= FORMATOP_NOALPHABLEND;
        if (!(x & HWD3D9_FMT_FILTER) && (x & HWD3D9_FMT_TEXTURE)) ops |= FORMATOP_NOFILTER;
        if (cand[i] == 21 && (bits[1] & (HWD3D9_FMT_DISPLAY | HWD3D9_FMT_RT))) ops |= FORMATOP_SAME_FORMAT_UP_TO_ALPHA_RENDERTARGET;
        D9LOG("hwd3d_umd:   format %u phone 0x%x ops 0x%x\n", cand[i], x, ops);
        if (!ops) continue;
        FORMATOP f = {};
        f.Format = (D3DDDIFORMAT)cand[i];
        f.Operations = ops;
        if (ops & (FORMATOP_OFFSCREEN_RENDERTARGET | FORMATOP_DISPLAYMODE)) {
            f.FlipMsTypes = 1u << D3DDDIMULTISAMPLE_NONE;   /* the multisample types each can flip and blt with */
            f.BltMsTypes = 1u << D3DDDIMULTISAMPLE_NONE;
        }
        if (cand[i] == 21 || cand[i] == 22 || cand[i] == 35) f.PrivateFormatBitCount = 32;
        else if (cand[i] == 23 || cand[i] == 25 || cand[i] == 80) f.PrivateFormatBitCount = 16;
        a->formats.push_back(f);
    }
}

static void load_caps(Adapter *a)
{
    if (a->caps_ok) return;
    struct wbuf *b = req(0, HWD3D9_CREATE_DEVICE);
    struct rd r;
    HRESULT hr = (HRESULT)call_wait(b, &r);
    uint32_t dev = take32(&r);
    take(&r, &a->caps, sizeof a->caps);
    call_end(&r);
    if (SUCCEEDED(hr)) {
        a->caps_ok = true;
        release_handle(dev);
    }
    D9LOG("hwd3d_umd: Direct3D 9 caps 0x%08lx, vs 0x%lx ps 0x%lx, type %u caps 0x%lx caps2 0x%lx caps3 0x%lx devcaps 0x%lx devcaps2 0x%lx streams %lu\n",
          (unsigned long)hr, (unsigned long)a->caps.VertexShaderVersion, (unsigned long)a->caps.PixelShaderVersion, (unsigned)a->caps.DeviceType,
          (unsigned long)a->caps.Caps, (unsigned long)a->caps.Caps2, (unsigned long)a->caps.Caps3, (unsigned long)a->caps.DevCaps,
          (unsigned long)a->caps.DevCaps2, (unsigned long)a->caps.MaxStreams);
    D9LOG("hwd3d_umd: Direct3D 9 caps: tex %lux%lu, textures %lu, primcount %lu, vertexindex %lu, texcaps 0x%lx, rast 0x%lx, zcmp 0x%lx, blend 0x%lx/0x%lx, adapters %lu, ordinal %lu, driver ver %lu\n",
          (unsigned long)a->caps.MaxTextureWidth, (unsigned long)a->caps.MaxTextureHeight, (unsigned long)a->caps.MaxSimultaneousTextures,
          (unsigned long)a->caps.MaxPrimitiveCount, (unsigned long)a->caps.MaxVertexIndex, (unsigned long)a->caps.TextureCaps,
          (unsigned long)a->caps.RasterCaps, (unsigned long)a->caps.ZCmpCaps, (unsigned long)a->caps.SrcBlendCaps, (unsigned long)a->caps.DestBlendCaps,
          (unsigned long)a->caps.NumberOfAdaptersInGroup, (unsigned long)a->caps.AdapterOrdinal, (unsigned long)a->caps.MasterAdapterOrdinal);
    load_formats(a);
}

static HRESULT APIENTRY GetCaps(HANDLE ha, const D3DDDIARG_GETCAPS *g)
{
    Adapter *a = static_cast<Adapter *>(ha);
    D9LOG("hwd3d_umd: Direct3D 9 GetCaps %u, %u bytes, adapter %p, info %p\n", (unsigned)g->Type, g->DataSize, (void *)a, (const void *)g->pInfo);
    if (g->pData && g->DataSize) memset(g->pData, 0, g->DataSize);
    switch (g->Type) {
    case D3DDDICAPS_GETD3D9CAPS:
    case D3DDDICAPS_GETD3D8CAPS:
        load_caps(a);
        if (!a->caps_ok) return E_FAIL;
        memcpy(g->pData, &a->caps, g->DataSize < sizeof a->caps ? g->DataSize : sizeof a->caps);
        /* Direct3D 8 refuses windowed devices without D3DCAPS2_CANRENDERWINDOWED, a bit Direct3D 9 dropped. */
        if (g->Type == D3DDDICAPS_GETD3D8CAPS && g->DataSize >= 16) static_cast<DWORD *>(g->pData)[3] |= 0x00080000;
        return S_OK;
    case D3DDDICAPS_GETFORMATCOUNT:
        load_caps(a);
        *static_cast<UINT *>(g->pData) = (UINT)a->formats.size();
        D9LOG("hwd3d_umd: Direct3D 9 format count %u\n", (unsigned)a->formats.size());
        return S_OK;
    case D3DDDICAPS_GETFORMATDATA: {
        load_caps(a);
        size_t n = g->DataSize / sizeof(FORMATOP);
        if (n > a->formats.size()) n = a->formats.size();
        memcpy(g->pData, a->formats.data(), n * sizeof(FORMATOP));
        return S_OK;
    }
    case D3DDDICAPS_GETMULTISAMPLEQUALITYLEVELS: {
        const DDIMULTISAMPLEQUALITYLEVELSDATA *in = static_cast<const DDIMULTISAMPLEQUALITYLEVELSDATA *>(g->pInfo);
        DDIMULTISAMPLEQUALITYLEVELSDATA *out = static_cast<DDIMULTISAMPLEQUALITYLEVELSDATA *>(g->pData);
        if (in && out && g->DataSize >= sizeof *out) {
            *out = *in;
            out->QualityLevels = in->MsType == D3DDDIMULTISAMPLE_NONE ? 1 : 0;
        }
        return S_OK;
    }
    case D3DDDICAPS_GETD3DQUERYCOUNT:
        *static_cast<UINT *>(g->pData) = 2;
        return S_OK;
    case D3DDDICAPS_GETD3DQUERYDATA: {
        D3DDDIQUERYTYPE *q = static_cast<D3DDDIQUERYTYPE *>(g->pData);
        if (g->DataSize >= 2 * sizeof *q) {
            q[0] = D3DDDIQUERYTYPE_EVENT;
            q[1] = D3DDDIQUERYTYPE_OCCLUSION;
        }
        return S_OK;
    }
    case D3DDDICAPS_GETD3D3CAPS: {
        /* DirectDraw asks this first. A zero size here means "no 3D part" and DirectDraw then crashes
           on its own null pointer, so the Direct3D 3 device record is always filled:
           size, device description (172 bytes), two vertex counts, then DirectDraw's own format list. */
        load_caps(a);
        if (!a->caps_ok || g->DataSize < 0xc0) return E_FAIL;
        const D3DCAPS9 &c = a->caps;
        DWORD prim[14] = { 56, c.PrimitiveMiscCaps, c.RasterCaps, c.ZCmpCaps, c.SrcBlendCaps, c.DestBlendCaps, c.AlphaCmpCaps, c.ShadeCaps,
                           c.TextureCaps, c.TextureFilterCaps | 0x3f, 0xff, c.TextureAddressCaps, 0, 0 };
        DWORD *o = static_cast<DWORD *>(g->pData);
        o[0] = 0xc0;
        o[1] = 172;                      /* description size */
        o[2] = 0x7ff;                    /* every field below is valid */
        o[3] = 2;                        /* RGB colour model */
        o[4] = c.DevCaps | 0x1 | 0x400 | 0x2000 | 0x8000;   /* float TL vertices, DrawPrimitive, DrawPrimitives2(Ex) */
        o[5] = 8; o[6] = 1;              /* transform: clips */
        o[7] = 1;                        /* clipping */
        o[8] = 16; o[9] = 7; o[10] = 1; o[11] = c.MaxActiveLights;   /* point, spot and directional lights, RGB model */
        memcpy(o + 12, prim, sizeof prim);   /* lines */
        memcpy(o + 26, prim, sizeof prim);   /* triangles */
        o[40] = 0x500;                   /* draws to 16 and 32 bit */
        o[41] = 0x700;                   /* depth of 16, 24 and 32 bit */
        o[42] = 0;
        o[43] = 0xffff;                  /* most vertices in one buffer */
        return S_OK;
    }
    case D3DDDICAPS_GETD3D7CAPS: {
        /* The Direct3D 7 extended record, 116 bytes. A zero size makes DirectDraw offer only its software devices. */
        load_caps(a);
        if (!a->caps_ok || g->DataSize < 116) return E_FAIL;
        const D3DCAPS9 &c = a->caps;
        DWORD *o = static_cast<DWORD *>(g->pData);
        float *f = static_cast<float *>(g->pData);
        WORD *w = static_cast<WORD *>(g->pData);
        o[0] = 116;
        o[1] = 1; o[2] = c.MaxTextureWidth; o[3] = 1; o[4] = c.MaxTextureHeight;
        o[9] = c.MaxTextureRepeat; o[10] = c.MaxTextureAspectRatio; o[11] = c.MaxAnisotropy;
        f[12] = c.GuardBandLeft; f[13] = c.GuardBandTop; f[14] = c.GuardBandRight; f[15] = c.GuardBandBottom;
        f[16] = c.ExtentsAdjust;
        o[17] = c.StencilCaps; o[18] = c.FVFCaps; o[19] = c.TextureOpCaps;
        w[40] = (WORD)(c.MaxTextureBlendStages > 8 ? 8 : c.MaxTextureBlendStages);
        w[41] = (WORD)(c.MaxSimultaneousTextures > 8 ? 8 : c.MaxSimultaneousTextures);
        o[21] = c.MaxActiveLights;
        f[22] = c.MaxVertexW;
        w[46] = (WORD)c.MaxUserClipPlanes; w[47] = (WORD)c.MaxVertexBlendMatrices;
        o[24] = c.VertexProcessingCaps;
        return S_OK;
    }
    default:
        /* The rest (other DirectDraw records, video) answer with nothing for now. */
        return S_OK;
    }
}

/* ---- resources ---- */

static HRESULT APIENTRY CreateResource(HANDLE hd, D3DDDIARG_CREATERESOURCE *a)
{
    Dev *d = DV(hd);
    Res *o = new Res();
    o->format = a->Format;
    o->levels = a->MipLevels ? a->MipLevels : 1;
    o->sysmem = a->Pool == D3DDDIPOOL_SYSTEMMEM;
    o->gpu_writes = a->Flags.RenderTarget || a->Flags.ZBuffer || a->Flags.Primary;
    o->buffer = a->Flags.VertexBuffer || a->Flags.IndexBuffer;
    hwd3d9_res q = {};
    q.format = (uint32_t)a->Format;
    q.pool = D3DPOOL_DEFAULT;
    q.width = a->pSurfList[0].Width;
    q.height = a->pSurfList[0].Height ? a->pSurfList[0].Height : 1;
    q.depth = a->pSurfList[0].Depth ? a->pSurfList[0].Depth : 1;
    q.levels = o->levels;
    q.ms_type = (uint32_t)a->MultisampleType;
    q.ms_quality = a->MultisampleQuality;
    q.fvf = a->Fvf;
    q.lockable = !a->Flags.NotLockable;
    if (a->Flags.RenderTarget) q.usage |= D3DUSAGE_RENDERTARGET;
    if (a->Flags.ZBuffer) q.usage |= D3DUSAGE_DEPTHSTENCIL;
    if (a->Flags.Dynamic) q.usage |= D3DUSAGE_DYNAMIC;
    if (a->Flags.WriteOnly) q.usage |= D3DUSAGE_WRITEONLY;
    if (a->Flags.AutogenMipmap) q.usage |= D3DUSAGE_AUTOGENMIPMAP;
    if (a->Flags.Points) q.usage |= D3DUSAGE_POINTS;
    if (a->Flags.VertexBuffer) {
        q.kind = HWD3D9_VB;
    } else if (a->Flags.IndexBuffer) {
        q.kind = HWD3D9_IB;
        q.format = (uint32_t)a->Format == 102 ? D3DFMT_INDEX32 : D3DFMT_INDEX16;
    } else if (a->Flags.CubeMap) {
        q.kind = HWD3D9_CUBE;
        o->levels = a->SurfCount / 6 ? a->SurfCount / 6 : 1;
        q.levels = o->levels;
    } else if (a->Flags.Volume) {
        q.kind = HWD3D9_VOL;
    } else if (a->Flags.Texture) {
        q.kind = HWD3D9_TEX;
    } else if (a->Flags.ZBuffer) {
        q.kind = HWD3D9_DSSURF;
    } else if (a->Flags.RenderTarget || a->Flags.Primary) {
        q.kind = HWD3D9_RTSURF;
    } else {
        q.kind = HWD3D9_PLAIN;   /* stays in the default pool: ColorFill and StretchRect only take those */
    }
    o->kind = q.kind;
    o->subs.resize(a->SurfCount);
    for (UINT i = 0; i < a->SurfCount; i++) {
        const D3DDDI_SURFACEINFO &si = a->pSurfList[i];
        Sub &s = o->subs[i];
        s.w = si.Width;
        s.h = si.Height ? si.Height : 1;
        s.d = si.Depth ? si.Depth : 1;
        s.pitch = o->buffer ? si.Width : row_bytes(a->Format, s.w);
        s.slice = o->buffer ? si.Width : s.pitch * rows_of(a->Format, s.h);
        s.cpu_ok = !o->gpu_writes;
        s.sys = static_cast<const uint8_t *>(si.pSysMem);
        s.sys_pitch = si.SysMemPitch;
        s.sys_slice = si.SysMemSlicePitch;
    }
    if (!o->sysmem) {
        struct wbuf *b = req(d->h, HWD3D9_CREATE_RES);
        put(b, &q, sizeof q);
        struct rd r;
        HRESULT hr = (HRESULT)call_wait(b, &r);
        o->h = take32(&r);
        call_end(&r);
        D9LOG("hwd3d_umd: Direct3D 9 resource kind %u format %u %ux%ux%u levels %u subs %u flags 0x%x pool %u: 0x%08lx\n", q.kind, q.format,
              q.width, q.height, q.depth, q.levels, a->SurfCount, a->Flags.Value, (unsigned)a->Pool, (unsigned long)hr);
        if (FAILED(hr) || !o->h) {
            delete o;
            return FAILED(hr) ? hr : E_FAIL;
        }
        /* First data the runtime gives with a default-pool resource. */
        for (UINT i = 0; i < a->SurfCount; i++) {
            const Sub &s = o->subs[i];
            if (!s.sys) continue;
            struct wbuf *w = req(d->h, HWD3D9_WRITE);
            put32(w, o->h);
            put32(w, i);
            hwd3d9_box box = {};
            put(w, &box, sizeof box);
            uint32_t pitch = s.sys_pitch ? s.sys_pitch : s.pitch;
            put32(w, pitch);
            put32(w, s.sys_slice ? s.sys_slice : pitch * rows_of(a->Format, s.h));
            put_arr(w, s.sys, o->buffer ? s.w : (size_t)pitch * rows_of(a->Format, s.h) * s.d, 1);
            send(w);
        }
    } else {
        D9LOG("hwd3d_umd: Direct3D 9 system-memory resource format %u %ux%u subs %u\n", q.format, q.width, q.height, a->SurfCount);
    }
    a->hResource = o;
    return S_OK;
}

static HRESULT APIENTRY DestroyResource(HANDLE hd, HANDLE hr)
{
    (void)hd;
    Res *o = RS(hr);
    if (!o) return S_OK;
    release_handle(o->h);
    delete o;
    return S_OK;
}

/* Fills a subresource's CPU copy from the phone. */
static void fetch(Dev *d, Res *o, UINT sub)
{
    Sub &s = o->subs[sub];
    if (s.cpu_ok || !o->h) return;
    struct wbuf *b = req(d->h, HWD3D9_READ);
    put32(b, o->h);
    put32(b, sub);
    struct rd r;
    HRESULT hr = (HRESULT)call_wait(b, &r);
    uint32_t pitch = take32(&r), rows = take32(&r), n = take32(&r);
    if (SUCCEEDED(hr)) {
        s.mem.resize(s.slice * s.d);
        std::vector<uint8_t> tmp(n);
        take(&r, tmp.data(), n);
        uint32_t rb = std::min(pitch, s.pitch);
        for (uint32_t y = 0; y < rows && (size_t)(y + 1) * s.pitch <= s.mem.size(); y++)
            memcpy(s.mem.data() + (size_t)y * s.pitch, tmp.data() + (size_t)y * pitch, rb);
        s.cpu_ok = true;
    }
    call_end(&r);
}

static HRESULT APIENTRY Lock(HANDLE hd, D3DDDIARG_LOCK *a)
{
    D9LOG("hwd3d_umd: d9 Lock\n");
    Dev *d = DV(hd);
    Res *o = RS(a->hResource);
    if (!o || a->SubResourceIndex >= o->subs.size()) return E_INVALIDARG;
    Sub &s = o->subs[a->SubResourceIndex];
    if (o->sysmem && s.sys) {
        uint8_t *p = const_cast<uint8_t *>(s.sys);
        uint32_t pitch = s.sys_pitch ? s.sys_pitch : s.pitch;
        if (o->buffer && a->Flags.RangeValid) p += a->Range.Offset;
        else if (a->Flags.AreaValid) p += (size_t)rows_of(o->format, a->Area.top) * pitch + row_bytes(o->format, a->Area.left);
        a->pSurfData = p;
        a->Pitch = pitch;
        a->SlicePitch = s.sys_slice ? s.sys_slice : pitch * rows_of(o->format, s.h);
        return S_OK;
    }
    if (s.mem.size() < (size_t)s.slice * s.d) s.mem.resize((size_t)s.slice * s.d);
    if (!a->Flags.Discard && !a->Flags.NoOverwrite && !a->Flags.WriteOnly) fetch(d, o, a->SubResourceIndex);
    uint8_t *p = s.mem.data();
    if (o->buffer && a->Flags.RangeValid) p += a->Range.Offset;
    else if (a->Flags.BoxValid) p += (size_t)a->Box.Front * s.slice + (size_t)rows_of(o->format, a->Box.Top) * s.pitch + row_bytes(o->format, a->Box.Left);
    else if (a->Flags.AreaValid) p += (size_t)rows_of(o->format, a->Area.top) * s.pitch + row_bytes(o->format, a->Area.left);
    a->pSurfData = p;
    a->Pitch = s.pitch;
    a->SlicePitch = s.slice;
    return S_OK;
}

/* What the last lock covered, so Unlock sends only that. */
struct LockSpan {
    uint32_t off, size;
    hwd3d9_box box;
};
/* Made on first use: this DLL starts at DllMain without the C runtime, so no global constructor ever runs. */
static std::map<std::pair<Res *, UINT>, LockSpan> &spans()
{
    static std::map<std::pair<Res *, UINT>, LockSpan> *m = new std::map<std::pair<Res *, UINT>, LockSpan>();
    return *m;
}
#define g_spans spans()
static SRWLOCK g_span_lock = SRWLOCK_INIT;

static HRESULT APIENTRY LockSpanned(HANDLE hd, D3DDDIARG_LOCK *a)
{
    D9LOG("hwd3d_umd: d9 LockSpanned\n");
    HRESULT hr = Lock(hd, a);
    if (FAILED(hr) || a->Flags.ReadOnly) return hr;
    Res *o = RS(a->hResource);
    Sub &s = o->subs[a->SubResourceIndex];
    LockSpan sp = {};
    if (o->buffer && a->Flags.RangeValid) {
        sp.off = a->Range.Offset;
        sp.size = a->Range.Size;
    } else if (a->Flags.AreaValid && !o->buffer) {
        sp.box = {1, (uint32_t)a->Area.left, (uint32_t)a->Area.top, 0, (uint32_t)a->Area.right, (uint32_t)a->Area.bottom, 1};
    } else if (a->Flags.BoxValid && !o->buffer) {
        sp.box = {1, a->Box.Left, a->Box.Top, a->Box.Front, a->Box.Right, a->Box.Bottom, a->Box.Back};
    } else {
        sp.size = o->buffer ? s.w : 0;
    }
    AcquireSRWLockExclusive(&g_span_lock);
    g_spans[{o, a->SubResourceIndex}] = sp;
    ReleaseSRWLockExclusive(&g_span_lock);
    return hr;
}

static HRESULT APIENTRY Unlock(HANDLE hd, const D3DDDIARG_UNLOCK *a)
{
    D9LOG("hwd3d_umd: d9 Unlock\n");
    Dev *d = DV(hd);
    Res *o = RS(a->hResource);
    if (!o || o->sysmem || !o->h) return S_OK;
    LockSpan sp;
    AcquireSRWLockExclusive(&g_span_lock);
    auto it = g_spans.find({o, a->SubResourceIndex});
    bool wrote = it != g_spans.end();
    if (wrote) {
        sp = it->second;
        g_spans.erase(it);
    }
    ReleaseSRWLockExclusive(&g_span_lock);
    if (!wrote) return S_OK;   /* a read-only lock */
    Sub &s = o->subs[a->SubResourceIndex];
    struct wbuf *b = req(d->h, HWD3D9_WRITE);
    put32(b, o->h);
    put32(b, a->SubResourceIndex);
    if (o->buffer) {
        uint32_t off = sp.size ? sp.off : 0, size = sp.size ? sp.size : s.w;
        if (off > s.w) off = s.w;
        if (size > s.w - off) size = s.w - off;
        hwd3d9_box box = {1, off, 0, 0, off + size, 1, 1};
        put(b, &box, sizeof box);
        put32(b, size);
        put32(b, size);
        put_arr(b, s.mem.data() + off, size, 1);
    } else if (sp.box.valid) {
        uint32_t x0 = sp.box.left, y0 = sp.box.top, z0 = sp.box.front;
        uint32_t row = row_bytes(o->format, sp.box.right) - row_bytes(o->format, x0);
        uint32_t rows = rows_of(o->format, sp.box.bottom) - rows_of(o->format, y0), depth = sp.box.back > z0 ? sp.box.back - z0 : 1;
        std::vector<uint8_t> tmp((size_t)row * rows * depth);
        for (uint32_t z = 0; z < depth; z++)
            for (uint32_t y = 0; y < rows; y++)
                memcpy(tmp.data() + ((size_t)z * rows + y) * row,
                       s.mem.data() + (size_t)(z0 + z) * s.slice + (size_t)(rows_of(o->format, y0) + y) * s.pitch + row_bytes(o->format, x0), row);
        put(b, &sp.box, sizeof sp.box);
        put32(b, row);
        put32(b, row * rows);
        put_arr(b, tmp.data(), tmp.size(), 1);
    } else {
        hwd3d9_box box = {};
        put(b, &box, sizeof box);
        put32(b, s.pitch);
        put32(b, s.slice);
        put_arr(b, s.mem.data(), (size_t)s.slice * s.d, 1);
    }
    send(b);
    return S_OK;
}

/* Every subresource of a resource the GPU may have written: its CPU copy is stale. */
static void gpu_wrote(Res *o)
{
    if (!o) return;
    for (Sub &s : o->subs) s.cpu_ok = false;
}

/* ---- state ---- */

static HRESULT APIENTRY SetRenderState(HANDLE hd, const D3DDDIARG_RENDERSTATE *a)
{
    struct wbuf *b = req(DV(hd)->h, HWD3D9_RS);
    put32(b, (uint32_t)a->State);
    put32(b, a->Value);
    send(b);
    return S_OK;
}

/* Texture stage states carry the sampler states too, under numbers of their own. */
static HRESULT APIENTRY SetTextureStageState(HANDLE hd, const D3DDDIARG_TEXTURESTAGESTATE *a)
{
    static const uint8_t samp[35] = {/* 13 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_BORDERCOLOR,
                                     D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER, D3DSAMP_MIPMAPLODBIAS, D3DSAMP_MAXMIPLEVEL,
                                     D3DSAMP_MAXANISOTROPY, 0, 0, 0, D3DSAMP_ADDRESSW, 0, 0, 0, D3DSAMP_SRGBTEXTURE, D3DSAMP_ELEMENTINDEX,
                                     D3DSAMP_DMAPOFFSET, 0, 0, 0};
    uint32_t st = (uint32_t)a->State;
    struct wbuf *b;
    if (st < 35 && samp[st]) {
        b = req(DV(hd)->h, HWD3D9_SS);
        put32(b, a->Stage);
        put32(b, samp[st]);
    } else {
        if (st == D3DDDITSS_TEXTUREMAP) return S_OK;
        b = req(DV(hd)->h, HWD3D9_TSS);
        put32(b, a->Stage);
        put32(b, st);
    }
    put32(b, a->Value);
    send(b);
    return S_OK;
}

static HRESULT APIENTRY SetTexture(HANDLE hd, UINT stage, HANDLE t)
{
    struct wbuf *b = req(DV(hd)->h, HWD3D9_TEXTURE);
    put32(b, stage);
    put32(b, t ? RS(t)->h : 0);
    send(b);
    return S_OK;
}

static HRESULT consts(Dev *d, uint32_t m, UINT first, UINT count, const void *v, uint32_t per)
{
    struct wbuf *b = req(d->h, m);
    put32(b, first);
    put32(b, count);
    put_arr(b, v, (size_t)count * per, 4);
    send(b);
    return S_OK;
}

static HRESULT APIENTRY SetVertexShaderConst(HANDLE hd, const D3DDDIARG_SETVERTEXSHADERCONST *a, const VOID *v)
{
    return consts(DV(hd), HWD3D9_VS_F, a->Register, a->Count, v, 4);
}
static HRESULT APIENTRY SetPixelShaderConst(HANDLE hd, const D3DDDIARG_SETPIXELSHADERCONST *a, const FLOAT *v)
{
    return consts(DV(hd), HWD3D9_PS_F, a->Register, a->Count, v, 4);
}
static HRESULT APIENTRY SetVertexShaderConstI(HANDLE hd, const D3DDDIARG_SETVERTEXSHADERCONSTI *a, const INT *v)
{
    return consts(DV(hd), HWD3D9_VS_I, a->Register, a->Count, v, 4);
}
static HRESULT APIENTRY SetVertexShaderConstB(HANDLE hd, const D3DDDIARG_SETVERTEXSHADERCONSTB *a, const BOOL *v)
{
    return consts(DV(hd), HWD3D9_VS_B, a->Register, a->Count, v, 1);
}
static HRESULT APIENTRY SetPixelShaderConstI(HANDLE hd, const D3DDDIARG_SETPIXELSHADERCONSTI *a, const INT *v)
{
    return consts(DV(hd), HWD3D9_PS_I, a->Register, a->Count, v, 4);
}
static HRESULT APIENTRY SetPixelShaderConstB(HANDLE hd, const D3DDDIARG_SETPIXELSHADERCONSTB *a, const BOOL *v)
{
    return consts(DV(hd), HWD3D9_PS_B, a->Register, a->Count, v, 1);
}

static HRESULT APIENTRY SetStreamSource(HANDLE hd, const D3DDDIARG_SETSTREAMSOURCE *a)
{
    Dev *d = DV(hd);
    if (a->Stream == d->um_stream) d->um_vb = nullptr;
    /* A vertex buffer in system memory (Direct3D 8 and older keep their transformed vertices in one) has
     * nothing on the phone: it is drawn like a user-memory stream. */
    Res *vbr = a->hVertexBuffer ? RS(a->hVertexBuffer) : nullptr;
    if (vbr && !vbr->h && !vbr->subs.empty() && (vbr->subs[0].sys || !vbr->subs[0].mem.empty())) {
        const uint8_t *base = vbr->subs[0].sys ? static_cast<const uint8_t *>(vbr->subs[0].sys) : vbr->subs[0].mem.data();
        d->um_vb = base + a->Offset;
        d->um_stride = a->Stride;
        d->um_stream = a->Stream;
        return S_OK;
    }
    struct wbuf *b = req(d->h, HWD3D9_STREAM);
    put32(b, a->Stream);
    put32(b, a->hVertexBuffer ? RS(a->hVertexBuffer)->h : 0);
    put32(b, a->Offset);
    put32(b, a->Stride);
    send(b);
    return S_OK;
}

static HRESULT APIENTRY SetStreamSourceUm(HANDLE hd, const D3DDDIARG_SETSTREAMSOURCEUM *a, const VOID *p)
{
    D9LOG("hwd3d_umd: d9 SetStreamSourceUm\n");
    Dev *d = DV(hd);
    d->um_vb = static_cast<const uint8_t *>(p);
    d->um_stride = a->Stride;
    d->um_stream = a->Stream;
    return S_OK;
}

static HRESULT APIENTRY SetIndices(HANDLE hd, const D3DDDIARG_SETINDICES *a)
{
    Dev *d = DV(hd);
    d->um_ib = nullptr;
    struct wbuf *b = req(d->h, HWD3D9_INDICES);
    put32(b, a->hIndexBuffer ? RS(a->hIndexBuffer)->h : 0);
    send(b);
    return S_OK;
}

static HRESULT APIENTRY SetIndicesUm(HANDLE hd, UINT size, const VOID *p)
{
    Dev *d = DV(hd);
    d->um_ib = static_cast<const uint8_t *>(p);
    d->um_isize = size;
    return S_OK;
}

static HRESULT APIENTRY SetStreamSourceFreq(HANDLE hd, const D3DDDIARG_SETSTREAMSOURCEFREQ *a)
{
    struct wbuf *b = req(DV(hd)->h, HWD3D9_FREQ);
    put32(b, a->Stream);
    put32(b, a->Divider);
    send(b);
    return S_OK;
}

/* ---- drawing ---- */

static uint32_t vertices_of(D3DPRIMITIVETYPE t, uint32_t prims)
{
    switch (t) {
    case D3DPT_POINTLIST: return prims;
    case D3DPT_LINELIST: return prims * 2;
    case D3DPT_LINESTRIP: return prims + 1;
    case D3DPT_TRIANGLELIST: return prims * 3;
    default: return prims + 2;   /* strips and fans */
    }
}

static void rt_drawn(Dev *d)
{
    for (Res *r : d->rts) gpu_wrote(r);
}

static HRESULT draw_up(Dev *d, D3DPRIMITIVETYPE t, uint32_t first_byte, uint32_t prims)
{
    uint32_t n = vertices_of(t, prims);
    struct wbuf *b = req(d->h, HWD3D9_DRAW_UP);
    put32(b, (uint32_t)t);
    put32(b, prims);
    put32(b, d->um_stride);
    put_arr(b, d->um_vb + first_byte, (size_t)n * d->um_stride, 1);
    send(b);
    rt_drawn(d);
    return S_OK;
}

static HRESULT APIENTRY DrawPrimitive(HANDLE hd, const D3DDDIARG_DRAWPRIMITIVE *a, const UINT *)
{
    D9LOG("hwd3d_umd: d9 DrawPrimitive\n");
    Dev *d = DV(hd);
    if (d->um_vb) return draw_up(d, a->PrimitiveType, a->VStart * d->um_stride, a->PrimitiveCount);
    struct wbuf *b = req(d->h, HWD3D9_DRAW);
    put32(b, (uint32_t)a->PrimitiveType);
    put32(b, a->VStart);
    put32(b, a->PrimitiveCount);
    send(b);
    rt_drawn(d);
    return S_OK;
}

static HRESULT APIENTRY DrawPrimitive2(HANDLE hd, const D3DDDIARG_DRAWPRIMITIVE2 *a)
{
    D9LOG("hwd3d_umd: d9 DrawPrimitive2\n");
    Dev *d = DV(hd);
    if (!d->um_vb) return S_OK;
    return draw_up(d, a->PrimitiveType, a->FirstVertexOffset, a->PrimitiveCount);
}

static HRESULT APIENTRY DrawIndexedPrimitive(HANDLE hd, const D3DDDIARG_DRAWINDEXEDPRIMITIVE *a)
{
    Dev *d = DV(hd);
    struct wbuf *b = req(d->h, HWD3D9_DRAW_IDX);
    put32(b, (uint32_t)a->PrimitiveType);
    put32(b, (uint32_t)a->BaseVertexIndex);
    put32(b, a->MinIndex);
    put32(b, a->NumVertices);
    put32(b, a->StartIndex);
    put32(b, a->PrimitiveCount);
    send(b);
    rt_drawn(d);
    return S_OK;
}

static HRESULT APIENTRY DrawIndexedPrimitive2(HANDLE hd, const D3DDDIARG_DRAWINDEXEDPRIMITIVE2 *a, UINT isize, const VOID *ib, const UINT *)
{
    D9LOG("hwd3d_umd: d9 DrawIndexedPrimitive2\n");
    Dev *d = DV(hd);
    if (!d->um_vb || !ib || (isize != 2 && isize != 4)) return S_OK;
    uint32_t ni = vertices_of(a->PrimitiveType, a->PrimitiveCount);
    const uint8_t *idx = static_cast<const uint8_t *>(ib) + a->StartIndexOffset;
    const uint8_t *vb = d->um_vb + a->BaseVertexOffset;
    struct wbuf *b = req(d->h, HWD3D9_DRAW_IDX_UP);
    put32(b, (uint32_t)a->PrimitiveType);
    put32(b, a->MinIndex);
    put32(b, a->NumVertices);
    put32(b, a->PrimitiveCount);
    put32(b, isize == 4 ? D3DFMT_INDEX32 : D3DFMT_INDEX16);
    put_arr(b, idx, (size_t)ni * isize, 1);
    put32(b, d->um_stride);
    put_arr(b, vb, (size_t)(a->MinIndex + a->NumVertices) * d->um_stride, 1);
    send(b);
    rt_drawn(d);
    return S_OK;
}

static HRESULT APIENTRY Clear(HANDLE hd, const D3DDDIARG_CLEAR *a, UINT n, const RECT *rects)
{
    D9LOG("hwd3d_umd: d9 Clear\n");
    Dev *d = DV(hd);
    struct wbuf *b = req(d->h, HWD3D9_CLEAR);
    put32(b, a->Flags);
    put32(b, a->FillColor);
    uint32_t z;
    memcpy(&z, &a->FillDepth, 4);
    put32(b, z);
    put32(b, a->FillStencil);
    put_arr(b, n ? rects : nullptr, n, sizeof(RECT));
    send(b);
    rt_drawn(d);
    return S_OK;
}

static HRESULT APIENTRY SetViewport(HANDLE hd, const D3DDDIARG_VIEWPORTINFO *a)
{
    Dev *d = DV(hd);
    D3DVIEWPORT9 v = {a->X, a->Y, a->Width, a->Height, 0.0f, 1.0f};
    struct wbuf *b = req(d->h, HWD3D9_VIEWPORT);
    put(b, &v, sizeof v);
    send(b);
    return S_OK;
}

/* The depth range comes apart from the rectangle in the driver interface; the phone gets both again. */
static D3DVIEWPORT9 g_vp;
static HRESULT APIENTRY SetViewportKeep(HANDLE hd, const D3DDDIARG_VIEWPORTINFO *a)
{
    g_vp.X = a->X;
    g_vp.Y = a->Y;
    g_vp.Width = a->Width;
    g_vp.Height = a->Height;
    if (g_vp.MaxZ == 0.0f && g_vp.MinZ == 0.0f) g_vp.MaxZ = 1.0f;
    struct wbuf *b = req(DV(hd)->h, HWD3D9_VIEWPORT);
    put(b, &g_vp, sizeof g_vp);
    send(b);
    return S_OK;
}
static HRESULT APIENTRY SetZRange(HANDLE hd, const D3DDDIARG_ZRANGE *a)
{
    g_vp.MinZ = a->MinZ;
    g_vp.MaxZ = a->MaxZ;
    struct wbuf *b = req(DV(hd)->h, HWD3D9_VIEWPORT);
    put(b, &g_vp, sizeof g_vp);
    send(b);
    return S_OK;
}

static HRESULT APIENTRY SetTransform(HANDLE hd, const D3DDDIARG_SETTRANSFORM *a)
{
    struct wbuf *b = req(DV(hd)->h, HWD3D9_TRANSFORM);
    put32(b, (uint32_t)a->TransformType);
    put(b, &a->Matrix, sizeof a->Matrix);
    send(b);
    return S_OK;
}

static HRESULT APIENTRY SetMaterial(HANDLE hd, const D3DDDIARG_SETMATERIAL *a)
{
    struct wbuf *b = req(DV(hd)->h, HWD3D9_MATERIAL);
    put(b, a, sizeof(D3DMATERIAL9));
    send(b);
    return S_OK;
}

static HRESULT APIENTRY SetLight(HANDLE hd, const D3DDDIARG_SETLIGHT *a, const D3DDDI_LIGHT *l)
{
    if (a->DataType == D3DDDI_SETLIGHT_ENABLE || a->DataType == D3DDDI_SETLIGHT_DISABLE) {
        struct wbuf *b = req(DV(hd)->h, HWD3D9_LIGHT_ENABLE);
        put32(b, a->Index);
        put32(b, a->DataType == D3DDDI_SETLIGHT_ENABLE);
        send(b);
        return S_OK;
    }
    if (!l) return S_OK;
    struct wbuf *b = req(DV(hd)->h, HWD3D9_LIGHT);
    put32(b, a->Index);
    put(b, l, sizeof(D3DLIGHT9));
    send(b);
    return S_OK;
}

static HRESULT APIENTRY SetClipPlane(HANDLE hd, const D3DDDIARG_SETCLIPPLANE *a)
{
    struct wbuf *b = req(DV(hd)->h, HWD3D9_CLIP_PLANE);
    put32(b, a->Index);
    put(b, a->Plane, sizeof a->Plane);
    send(b);
    return S_OK;
}

static HRESULT APIENTRY SetScissorRect(HANDLE hd, const RECT *rc)
{
    struct wbuf *b = req(DV(hd)->h, HWD3D9_SCISSOR);
    put(b, rc, sizeof *rc);
    send(b);
    return S_OK;
}

static HRESULT APIENTRY SetRenderTarget(HANDLE hd, const D3DDDIARG_SETRENDERTARGET *a)
{
    D9LOG("hwd3d_umd: d9 SetRenderTarget\n");
    Dev *d = DV(hd);
    Res *o = RS(a->hRenderTarget);
    if (a->RenderTargetIndex < 4) d->rts[a->RenderTargetIndex] = o;
    struct wbuf *b = req(d->h, HWD3D9_RT);
    put32(b, a->RenderTargetIndex);
    put32(b, o ? o->h : 0);
    put32(b, a->SubResourceIndex);
    send(b);
    return S_OK;
}

static HRESULT APIENTRY SetDepthStencil(HANDLE hd, const D3DDDIARG_SETDEPTHSTENCIL *a)
{
    struct wbuf *b = req(DV(hd)->h, HWD3D9_DS);
    put32(b, a->hZBuffer ? RS(a->hZBuffer)->h : 0);
    send(b);
    return S_OK;
}

/* ---- shaders ---- */

static uint32_t create_shader(Dev *d, uint32_t m, const UINT *code, size_t bytes)
{
    struct wbuf *b = req(d->h, m);
    put_arr(b, code, bytes / 4, 4);
    struct rd r;
    HRESULT hr = (HRESULT)call_wait(b, &r);
    uint32_t h = take32(&r);
    call_end(&r);
    return SUCCEEDED(hr) ? h : 0;
}

static HRESULT APIENTRY CreateVertexShaderFunc(HANDLE hd, D3DDDIARG_CREATEVERTEXSHADERFUNC *a, const UINT *code)
{
    uint32_t h = create_shader(DV(hd), HWD3D9_CREATE_VS, code, a->Size);
    a->ShaderHandle = (HANDLE)(uintptr_t)h;
    return h ? S_OK : E_FAIL;
}
static HRESULT APIENTRY CreatePixelShader(HANDLE hd, D3DDDIARG_CREATEPIXELSHADER *a, const UINT *code)
{
    uint32_t h = create_shader(DV(hd), HWD3D9_CREATE_PS, code, a->CodeSize);
    a->ShaderHandle = (HANDLE)(uintptr_t)h;
    return h ? S_OK : E_FAIL;
}
static HRESULT APIENTRY CreateVertexShaderDecl(HANDLE hd, D3DDDIARG_CREATEVERTEXSHADERDECL *a, const D3DDDIVERTEXELEMENT *e)
{
    D9LOG("hwd3d_umd: d9 CreateVertexShaderDecl\n");
    struct wbuf *b = req(DV(hd)->h, HWD3D9_CREATE_DECL);
    put_arr(b, e, a->NumVertexElements, sizeof(D3DVERTEXELEMENT9));
    struct rd r;
    HRESULT hr = (HRESULT)call_wait(b, &r);
    uint32_t h = take32(&r);
    call_end(&r);
    a->ShaderHandle = (HANDLE)(uintptr_t)h;
    return SUCCEEDED(hr) && h ? S_OK : E_FAIL;
}

static HRESULT set_handle(HANDLE hd, uint32_t m, HANDLE h)
{
    struct wbuf *b = req(DV(hd)->h, m);
    put32(b, (uint32_t)(uintptr_t)h);
    send(b);
    return S_OK;
}
static HRESULT APIENTRY SetVertexShaderFunc(HANDLE hd, HANDLE h) { return set_handle(hd, HWD3D9_SET_VS, h); }
static HRESULT APIENTRY SetPixelShader(HANDLE hd, HANDLE h) { return set_handle(hd, HWD3D9_SET_PS, h); }
static HRESULT APIENTRY SetVertexShaderDecl(HANDLE hd, HANDLE h) { return set_handle(hd, HWD3D9_SET_DECL, h); }
static HRESULT APIENTRY DeleteHandle(HANDLE, HANDLE h)
{
    release_handle((uint32_t)(uintptr_t)h);
    return S_OK;
}

/* ---- copies ---- */

static HRESULT APIENTRY Blt(HANDLE hd, const D3DDDIARG_BLT *a)
{
    D9LOG("hwd3d_umd: d9 Blt\n");
    Dev *d = DV(hd);
    Res *s = RS(a->hSrcResource), *t = RS(a->hDstResource);
    if (!s || !t) return E_INVALIDARG;
    if (s->sysmem || !s->h) {
        /* From system memory: written into the target's rectangle. */
        const Sub &ss = s->subs[a->SrcSubResourceIndex];
        if (!ss.sys) return S_OK;
        uint32_t pitch = ss.sys_pitch ? ss.sys_pitch : ss.pitch;
        uint32_t row = row_bytes(s->format, a->SrcRect.right) - row_bytes(s->format, a->SrcRect.left);
        uint32_t rows = rows_of(s->format, a->SrcRect.bottom) - rows_of(s->format, a->SrcRect.top);
        std::vector<uint8_t> tmp((size_t)row * rows);
        for (uint32_t y = 0; y < rows; y++)
            memcpy(tmp.data() + (size_t)y * row, ss.sys + (size_t)(rows_of(s->format, a->SrcRect.top) + y) * pitch + row_bytes(s->format, a->SrcRect.left), row);
        struct wbuf *b = req(d->h, HWD3D9_WRITE);
        put32(b, t->h);
        put32(b, a->DstSubResourceIndex);
        hwd3d9_box box = {1, (uint32_t)a->DstRect.left, (uint32_t)a->DstRect.top, 0, (uint32_t)a->DstRect.left + (a->SrcRect.right - a->SrcRect.left),
                          (uint32_t)a->DstRect.top + (a->SrcRect.bottom - a->SrcRect.top), 1};
        put(b, &box, sizeof box);
        put32(b, row);
        put32(b, row * rows);
        put_arr(b, tmp.data(), tmp.size(), 1);
        send(b);
        gpu_wrote(t);
        return S_OK;
    }
    D9LOG("hwd3d_umd: d9 Blt src %p sys %d h %u -> dst %p sys %d h %u\n", (void *)s, s->sysmem, s->h, (void *)t, t->sysmem, t->h);
    if (t->sysmem || !t->h) {
        /* To system memory (GetRenderTargetData, GetFrontBufferData): a read the program asked for. */
        Sub &ts = t->subs[a->DstSubResourceIndex];
        if (!ts.sys) return S_OK;
        struct wbuf *rb = req(d->h, HWD3D9_READ);
        put32(rb, s->h);
        put32(rb, a->SrcSubResourceIndex);
        struct rd r;
        HRESULT hr = (HRESULT)call_wait(rb, &r);
        uint32_t pitch = take32(&r), rows = take32(&r), n = take32(&r);
        if (SUCCEEDED(hr) && pitch && n) {
            const uint8_t *src = r.p;   /* the answer's bytes, read in place */
            uint32_t dp = ts.sys_pitch ? ts.sys_pitch : ts.pitch;
            uint32_t x0 = row_bytes(s->format, a->SrcRect.left), w = row_bytes(s->format, a->SrcRect.right) - x0;
            uint32_t y0 = rows_of(s->format, a->SrcRect.top), h = rows_of(s->format, a->SrcRect.bottom) - y0;
            uint32_t dx = row_bytes(t->format, a->DstRect.left), dy = rows_of(t->format, a->DstRect.top);
            for (uint32_t y = 0; y < h && y0 + y < rows && (size_t)(y0 + y) * pitch + x0 + w <= n; y++)
                memcpy(const_cast<uint8_t *>(ts.sys) + (size_t)(dy + y) * dp + dx, src + (size_t)(y0 + y) * pitch + x0, w);
        }
        call_end(&r);
        return hr;
    }
    struct wbuf *b = req(d->h, HWD3D9_STRETCH);
    put32(b, s->h);
    put32(b, a->SrcSubResourceIndex);
    put(b, &a->SrcRect, sizeof a->SrcRect);
    put32(b, t->h);
    put32(b, a->DstSubResourceIndex);
    put(b, &a->DstRect, sizeof a->DstRect);
    put32(b, a->Flags.Linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    send(b);
    gpu_wrote(t);
    return S_OK;
}

static HRESULT APIENTRY ColorFill(HANDLE hd, const D3DDDIARG_COLORFILL *a)
{
    D9LOG("hwd3d_umd: d9 ColorFill\n");
    Res *t = RS(a->hResource);
    if (!t || !t->h) return S_OK;
    struct wbuf *b = req(DV(hd)->h, HWD3D9_FILL);
    put32(b, t->h);
    put32(b, a->SubResourceIndex);
    put(b, &a->DstRect, sizeof a->DstRect);
    put32(b, a->Color);
    send(b);
    gpu_wrote(t);
    return S_OK;
}

/* TexBlt copies a whole texture (or one cube face) between pools: what managed textures and
 * UpdateTexture become. From system memory each level is written; between GPU resources the phone
 * copies. */
static HRESULT APIENTRY TexBlt(HANDLE hd, const D3DDDIARG_TEXBLT *a)
{
    Dev *d = DV(hd);
    Res *s = RS(a->hSrcResource), *t = RS(a->hDstResource);
    if (!s || !t || !t->h) return S_OK;
    if (s->h) {
        struct wbuf *b = req(d->h, HWD3D9_COPY_RES);
        put32(b, s->h);
        put32(b, t->h);
        send(b);
        gpu_wrote(t);
        return S_OK;
    }
    uint32_t slevels = s->kind == HWD3D9_CUBE ? s->levels : (uint32_t)s->subs.size();
    uint32_t tlevels = t->kind == HWD3D9_CUBE ? t->levels : (uint32_t)t->subs.size();
    uint32_t face = t->kind == HWD3D9_CUBE ? a->CubeMapFace : 0;
    /* The source's top level may be bigger than the target's: match levels by size. */
    uint32_t skip = 0;
    while (skip < slevels && s->subs[face * slevels + skip].w > t->subs[face * tlevels].w) skip++;
    for (uint32_t l = 0; l < tlevels && l + skip < slevels; l++) {
        const Sub &ss = s->subs[face * slevels + l + skip];
        if (!ss.sys) continue;
        uint32_t pitch = ss.sys_pitch ? ss.sys_pitch : ss.pitch;
        struct wbuf *b = req(d->h, HWD3D9_WRITE);
        put32(b, t->h);
        put32(b, face * tlevels + l);
        hwd3d9_box box = {};
        put(b, &box, sizeof box);
        put32(b, pitch);
        uint32_t slice = ss.sys_slice ? ss.sys_slice : pitch * rows_of(s->format, ss.h);
        put32(b, slice);
        put_arr(b, ss.sys, (size_t)slice * ss.d, 1);
        send(b);
    }
    gpu_wrote(t);
    return S_OK;
}

static HRESULT APIENTRY GenerateMipSubLevels(HANDLE hd, const D3DDDIARG_GENERATEMIPSUBLEVELS *a)
{
    Res *t = RS(a->hResource);
    if (!t || !t->h) return S_OK;
    struct wbuf *b = req(DV(hd)->h, HWD3D9_GENMIPS);
    put32(b, t->h);
    send(b);
    gpu_wrote(t);
    return S_OK;
}

/* ---- queries ---- */

static HRESULT APIENTRY CreateQuery(HANDLE hd, D3DDDIARG_CREATEQUERY *a)
{
    struct wbuf *b = req(DV(hd)->h, HWD3D9_QUERY);
    put32(b, (uint32_t)a->QueryType);
    struct rd r;
    HRESULT hr = (HRESULT)call_wait(b, &r);
    uint32_t h = take32(&r);
    call_end(&r);
    a->hQuery = (HANDLE)(uintptr_t)h;
    return SUCCEEDED(hr) && h ? S_OK : E_FAIL;
}
static HRESULT APIENTRY IssueQuery(HANDLE hd, const D3DDDIARG_ISSUEQUERY *a)
{
    struct wbuf *b = req(DV(hd)->h, HWD3D9_QUERY_ISSUE);
    put32(b, (uint32_t)(uintptr_t)a->hQuery);
    put32(b, a->Flags.Begin ? D3DISSUE_BEGIN : D3DISSUE_END);
    send(b);
    return S_OK;
}
static HRESULT APIENTRY GetQueryData(HANDLE hd, const D3DDDIARG_GETQUERYDATA *a)
{
    struct wbuf *b = req(DV(hd)->h, HWD3D9_QUERY_DATA);
    put32(b, (uint32_t)(uintptr_t)a->hQuery);
    put32(b, 8);
    struct rd r;
    HRESULT hr = (HRESULT)call_wait(b, &r);
    uint32_t n = take32(&r);
    uint8_t data[8] = {};
    take(&r, data, n < 8 ? n : 8);
    call_end(&r);
    if (hr == S_FALSE) return D3DERR_WASSTILLDRAWING;
    if (a->pData) memcpy(a->pData, data, 4);   /* events: a BOOL; occlusion: a count */
    return SUCCEEDED(hr) ? S_OK : hr;
}

/* ---- present ---- */

/* The frame is read back and drawn into the program's window, the one of its size (umd.cpp's find_win).
 * As with Direct3D 11 blts, the copy goes when DXGI and Direct3D 9 can flip on this adapter. */
static HRESULT APIENTRY Present(HANDLE hd, const D3DDDIARG_PRESENT *a)
{
    D9LOG("hwd3d_umd: d9 Present\n");
    Dev *d = DV(hd);
    Res *s = RS(a->hSrcResource);
    if (!s || !s->h || s->subs.empty()) return S_OK;
    const Sub &ss = s->subs[a->SrcSubResourceIndex < s->subs.size() ? a->SrcSubResourceIndex : 0];
    FindWin f = {GetCurrentProcessId(), ss.w, ss.h, nullptr};
    EnumWindows(find_win, reinterpret_cast<LPARAM>(&f));
    if (!f.best) {
        D9LOG("hwd3d_umd: Direct3D 9 present: no window of %ux%u\n", ss.w, ss.h);
        return S_OK;
    }
    struct wbuf *b = req(d->h, HWD3D9_READ);
    put32(b, s->h);
    put32(b, a->SrcSubResourceIndex);
    struct rd r;
    HRESULT hr = (HRESULT)call_wait(b, &r);
    uint32_t pitch = take32(&r), rows = take32(&r), n = take32(&r);
    if (SUCCEEDED(hr) && pitch && rows) {
        std::vector<uint8_t> px(n);
        take(&r, px.data(), n);
        HDC dc = GetDC(f.best);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = (LONG)(pitch / 4);
        bi.bmiHeader.biHeight = -(LONG)rows;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        SetDIBitsToDevice(dc, 0, 0, ss.w, rows, 0, 0, 0, rows, px.data(), &bi, DIB_RGB_COLORS);
        ReleaseDC(f.best, dc);
    }
    call_end(&r);
    return S_OK;
}

static HRESULT APIENTRY Flush(HANDLE) { return S_OK; }

static HRESULT APIENTRY ValidateDevice(HANDLE, D3DDDIARG_VALIDATETEXTURESTAGESTATE *a)
{
    a->NumPasses = 1;
    return S_OK;
}

static HRESULT APIENTRY DestroyDevice(HANDLE hd)
{
    Dev *d = DV(hd);
    release_handle(d->h);
    delete d;
    return S_OK;
}

/* ---- the table ---- */

static const char *g_names9[192];
static LONG g_seen9[192];
template <int N, class F> struct Stub9;
template <int N, class R, class... A> struct Stub9<N, R(APIENTRY *)(A...)> {
    static R APIENTRY fn(A...)
    {
        if (N < 192 && !InterlockedExchange(&g_seen9[N], 1)) ulog("hwd3d_umd: Direct3D 9 %s is not done yet\n", g_names9[N]);
        return R();
    }
};

static HRESULT APIENTRY CreateDevice(HANDLE ha, D3DDDIARG_CREATEDEVICE *a)
{
    Adapter *ad = static_cast<Adapter *>(ha);
    ulog("hwd3d_umd: Direct3D 9 CreateDevice, interface %u version 0x%x\n", a->Interface, a->Version);
    if (!ready()) return E_FAIL;
    struct wbuf *b = req(0, HWD3D9_CREATE_DEVICE);
    struct rd r;
    HRESULT hr = (HRESULT)call_wait(b, &r);
    uint32_t h = take32(&r);
    if (!ad->caps_ok) {
        take(&r, &ad->caps, sizeof ad->caps);
        ad->caps_ok = SUCCEEDED(hr);
    }
    call_end(&r);
    /* Direct3D 8 and older create the device first and ask for the caps and formats afterwards. */
    if (ad->caps_ok && ad->formats.empty()) load_formats(ad);
    if (FAILED(hr) || !h) return FAILED(hr) ? hr : E_FAIL;
    Dev *d = new Dev();
    d->rt = a->hDevice;
    if (a->pCallbacks) d->cb = *a->pCallbacks;
    d->h = h;
    a->hDevice = d;
    D3DDDI_DEVICEFUNCS *t = a->pDeviceFuncs;
    int i = 0;
#define UMD9_FILL(m) \
    t->m = Stub9<__COUNTER__ - kBase, decltype(t->m)>::fn; \
    g_names9[i++] = #m;
    static constexpr int kBase = __COUNTER__ + 1;
#include "umd9_tables.inc"
#undef UMD9_FILL
    t->pfnSetRenderState = SetRenderState;
    t->pfnValidateDevice = ValidateDevice;
    t->pfnSetTextureStageState = SetTextureStageState;
    t->pfnSetTexture = SetTexture;
    t->pfnSetPixelShader = SetPixelShader;
    t->pfnSetPixelShaderConst = SetPixelShaderConst;
    t->pfnSetStreamSourceUm = SetStreamSourceUm;
    t->pfnSetIndices = SetIndices;
    t->pfnSetIndicesUm = SetIndicesUm;
    t->pfnDrawPrimitive = DrawPrimitive;
    t->pfnDrawIndexedPrimitive = DrawIndexedPrimitive;
    t->pfnDrawPrimitive2 = DrawPrimitive2;
    t->pfnDrawIndexedPrimitive2 = DrawIndexedPrimitive2;
    t->pfnTexBlt = TexBlt;
    t->pfnClear = Clear;
    t->pfnSetVertexShaderConst = SetVertexShaderConst;
    t->pfnSetTransform = SetTransform;
    t->pfnSetViewport = SetViewportKeep;
    t->pfnSetZRange = SetZRange;
    t->pfnSetMaterial = SetMaterial;
    t->pfnSetLight = SetLight;
    t->pfnSetClipPlane = SetClipPlane;
    t->pfnLock = LockSpanned;
    t->pfnUnlock = Unlock;
    t->pfnCreateResource = CreateResource;
    t->pfnDestroyResource = DestroyResource;
    t->pfnPresent = Present;
    t->pfnFlush = Flush;
    t->pfnCreateVertexShaderFunc = CreateVertexShaderFunc;
    t->pfnDeleteVertexShaderFunc = DeleteHandle;
    t->pfnSetVertexShaderFunc = SetVertexShaderFunc;
    t->pfnCreateVertexShaderDecl = CreateVertexShaderDecl;
    t->pfnDeleteVertexShaderDecl = DeleteHandle;
    t->pfnSetVertexShaderDecl = SetVertexShaderDecl;
    t->pfnSetVertexShaderConstI = SetVertexShaderConstI;
    t->pfnSetVertexShaderConstB = SetVertexShaderConstB;
    t->pfnSetScissorRect = SetScissorRect;
    t->pfnSetStreamSource = SetStreamSource;
    t->pfnSetStreamSourceFreq = SetStreamSourceFreq;
    t->pfnBlt = Blt;
    t->pfnColorFill = ColorFill;
    t->pfnCreateQuery = CreateQuery;
    t->pfnDestroyQuery = DeleteHandle;
    t->pfnIssueQuery = IssueQuery;
    t->pfnGetQueryData = GetQueryData;
    t->pfnSetRenderTarget = SetRenderTarget;
    t->pfnSetDepthStencil = SetDepthStencil;
    t->pfnGenerateMipSubLevels = GenerateMipSubLevels;
    t->pfnSetPixelShaderConstI = SetPixelShaderConstI;
    t->pfnSetPixelShaderConstB = SetPixelShaderConstB;
    t->pfnCreatePixelShader = CreatePixelShader;
    t->pfnDeletePixelShader = DeleteHandle;
    t->pfnDestroyDevice = DestroyDevice;
    return S_OK;
}

static HRESULT APIENTRY CloseAdapter(HANDLE ha)
{
    delete static_cast<Adapter *>(ha);
    return S_OK;
}

}  // namespace d9

/* Windows' d3d9.dll opens the adapter here. */
extern "C" HRESULT APIENTRY OpenAdapter(D3DDDIARG_OPENADAPTER *a)
{
    ulog("hwd3d_umd: Direct3D 9 OpenAdapter, interface %u version 0x%x\n", a->Interface, a->Version);
    if (!ready()) return E_FAIL;
    d9::Adapter *ad = new d9::Adapter();
    ad->rt = a->hAdapter;
    if (a->pAdapterCallbacks) ad->cb = *a->pAdapterCallbacks;
    a->hAdapter = ad;
    a->pAdapterFuncs->pfnGetCaps = d9::GetCaps;
    a->pAdapterFuncs->pfnCreateDevice = d9::CreateDevice;
    a->pAdapterFuncs->pfnCloseAdapter = d9::CloseAdapter;
    a->DriverVersion = D3D_UMD_INTERFACE_VERSION_WIN7;   /* the entry points of this version: the runtime checks the table against it */
    return S_OK;
}
