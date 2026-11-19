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
#include <vector>

#include <d3d11.h>
#include <d3d10umddi.h>

#include "hwd3d_methods.h"

/* ---- logging ---- */

/* ---- settings: an environment variable, or the same name under HKLM\SOFTWARE\HyperWin\Gpu for programs
 * whose environment cannot be set (dwm.exe, WUDFHost.exe). Returns the length, 0 when unset. ---- */
static DWORD setting(const char *name, char *out, DWORD size)
{
    DWORD n = GetEnvironmentVariableA(name, out, size);
    if (n && n < size) return n;
    HKEY k;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\HyperWin\\Gpu", 0, KEY_READ, &k) != ERROR_SUCCESS) return 0;
    DWORD type = 0, len = size;
    LONG r = RegQueryValueExA(k, name, NULL, &type, (BYTE *)out, &len);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS || !len) return 0;
    if (type == REG_DWORD && len == 4) {
        DWORD v = *(DWORD *)out;
        if (!v) return 0;
        return (DWORD)snprintf(out, size, "%lu", (unsigned long)v);
    }
    out[size - 1] = 0;
    return (DWORD)strlen(out);
}

/* The log file: HWD3D_UMDLOG names it; when it names a folder (ends in \), each process gets its own. */
static bool log_path(char *path, DWORD size)
{
    static char cached[MAX_PATH];
    static int state;   /* 0 unknown, 1 none, 2 cached */
    if (state == 1) return false;
    if (state == 0) {
        DWORD n = setting("HWD3D_UMDLOG", cached, sizeof cached);
        if (!n) { state = 1; return false; }
        if (cached[n - 1] == '\\') {
            char exe[MAX_PATH];
            DWORD m = GetModuleFileNameA(NULL, exe, sizeof exe);
            const char *base = exe + m;
            while (base > exe && base[-1] != '\\') base--;
            snprintf(cached + n, sizeof cached - n, "umd-%s-%lu.log", base, (unsigned long)GetCurrentProcessId());
        }
        state = 2;
    }
    snprintf(path, size, "%s", cached);
    return true;
}


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
    if (log_path(path, sizeof path)) {
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
    static int on = -1;
    char v[16];
    if (on < 0) on = setting("HWD3D_UMDCALLS", v, sizeof v) ? 1 : 0;
    if (on) ulog("hwd3d_umd: %s\n", name);
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
    const DXGI_DDI_BASE_CALLBACKS *dxcb;  /* DXGI's: the kernel present of a shared surface */
    uint32_t hdev, hctx;  /* the phone's device and immediate context */
    HANDLE kctx;          /* the kernel context the runtime wants a driver to have */
    UINT flags;
    struct Obj *pstage;   /* the staging copy a windowed present reads back, kept at the last size */
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
    uint32_t share;        /* shared surfaces: the phone's share id (HWD3D_OP_CREATE_SHARED) */
    uint32_t swap_rb;      /* a BGRA surface the phone keeps as RGBA (see rgba_of): its views are swapped too */
    D3DKMT_HANDLE kalloc;  /* and the kernel allocation that carries it to other programs */
    HANDLE rtres;          /* the runtime's resource, for freeing that allocation */
};

static Dev *DV(D3D10DDI_HDEVICE h) { return static_cast<Dev *>(h.pDrvPrivate); }
static Obj *OB(D3D10DDI_HRESOURCE h) { return static_cast<Obj *>(h.pDrvPrivate); }
static Obj *OB(D3D10DDI_HRENDERTARGETVIEW h) { return static_cast<Obj *>(h.pDrvPrivate); }

static void fail(Dev *d, HRESULT hr)
{
    ulog("hwd3d_umd: error 0x%08lx reported to the runtime\n", (unsigned long)hr);
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

/* Shared BGRA surfaces are RGBA on the phone: the phone's screen reads its buffers as RGBA, and Mali has no
 * BGRA buffer the GPU writes in BGRA order. Their views then name the RGBA format, so shaders still see
 * (r, g, b, a) and the bytes are what the screen expects. */
static DXGI_FORMAT rgba_of(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    default: return f;
    }
}

/* A texture another program will open (DWM's surfaces for the screen driver, a game's shared ones): the
 * phone backs it with an Android hardware buffer, and a kernel allocation carries the share id to the
 * other program (include/hw_wddm.h, struct hww_alloc). False: make an ordinary texture instead. */
static bool create_shared(Dev *d, const D3D11DDIARG_CREATERESOURCE *a, D3D10DDI_HRTRESOURCE rt, Obj *o, const D3D11_TEXTURE2D_DESC *t2)
{
    D3D11_TEXTURE2D_DESC phone = *t2;
    phone.Format = rgba_of(t2->Format);
    struct wbuf *b = begin(d->hdev, 0, HWD3D_OP_CREATE_SHARED);
    put(b, &phone, sizeof phone);
    struct rd r;
    HRESULT hr = (HRESULT)call_wait(b, &r);
    uint32_t h = take32(&r), id = take32(&r);
    call_end(&r);
    if (FAILED(hr) || !h) {
        ulog("hwd3d_umd: no shared surface on the phone for %ux%u format %u (0x%08lx)\n", t2->Width, t2->Height, (unsigned)t2->Format, (unsigned long)hr);
        return false;
    }
    o->h = h;
    o->share = id;
    o->swap_rb = phone.Format != t2->Format;
    struct hww_alloc al = {};
    al.magic = HWW_ALLOC_MAGIC;
    al.kind = a->pPrimaryDesc ? HWW_ALLOC_PRIMARY : HWW_ALLOC_UMD;
    al.width = t2->Width;
    al.height = t2->Height;
    al.format = (uint32_t)t2->Format;
    al.source = a->pPrimaryDesc ? a->pPrimaryDesc->VidPnSourceId : 0;
    al.id = id;
    D3DDDI_ALLOCATIONINFO ai = {};
    ai.pPrivateDriverData = &al;
    ai.PrivateDriverDataSize = sizeof al;
    if (a->pPrimaryDesc) {
        ai.Flags.Primary = 1;
        ai.VidPnSourceId = a->pPrimaryDesc->VidPnSourceId;
    }
    D3DDDICB_ALLOCATE cb = {};
    cb.hResource = rt.handle;
    cb.NumAllocations = 1;
    cb.pAllocationInfo = &ai;
    hr = d->kcb && d->kcb->pfnAllocateCb ? d->kcb->pfnAllocateCb(d->rt.handle, &cb) : E_FAIL;
    if (FAILED(hr)) {
        ulog("hwd3d_umd: the kernel refused the shared surface's allocation (0x%08lx)\n", (unsigned long)hr);
        release_obj(o);
        return false;
    }
    o->kalloc = ai.hAllocation;
    o->rtres = rt.handle;
    ulog("hwd3d_umd: shared surface %u, %ux%u format %u%s\n", id, t2->Width, t2->Height, (unsigned)t2->Format, a->pPrimaryDesc ? ", primary" : "");
    return true;
}

static void APIENTRY u_CreateResource(D3D10DDI_HDEVICE hd, const D3D11DDIARG_CREATERESOURCE *a, D3D10DDI_HRESOURCE hr, D3D10DDI_HRTRESOURCE rt)
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
    if (o->dim == 3 && ((a->MiscFlags & D3D10_DDI_RESOURCE_MISC_SHARED) || a->pPrimaryDesc) && o->subs == 1 && o->samples <= 1 &&
        !a->pInitialDataUP && create_shared(d, a, rt, o, &desc.t2))
        return;
    struct wbuf *b = dc(d, method);
    put_arr(b, &desc, 1, desc_size);
    const D3D10_DDIARG_SUBRESOURCE_UP *init = a->pInitialDataUP;
    put32(b, init ? o->subs : 0);
    for (uint32_t i = 0; init && i < o->subs; i++) {
        uint32_t row, rows, slices;
        if (!init[i].pSysMem || !hwd3d_region(&o->r, i, NULL, &row, &rows, &slices)) {
            ulog("hwd3d_umd: initial data of subresource %u cannot be sent: dimension %u, format %u, %ux%ux%u, %u mips, %u layers%s\n", i, o->dim,
                 (unsigned)o->r.format, o->r.width, o->r.height, o->r.depth, o->r.mips, layers, init[i].pSysMem ? "" : ", no memory");
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
    if (FAILED(res) || !o->h) {
        ulog("hwd3d_umd: the phone refused a resource, dimension %u, format %u, %ux%u, bind 0x%x, usage %u (0x%08lx)\n", o->dim, (unsigned)a->Format,
             o->r.width, o->r.height, a->BindFlags, a->Usage, (unsigned long)res);
        fail(d, FAILED(res) ? res : E_FAIL);
    }
}

static void APIENTRY u_DestroyResource(D3D10DDI_HDEVICE hd, D3D10DDI_HRESOURCE h)
{
    Obj *o = OB(h);
    Dev *d = DV(hd);
    if (o->kalloc && d->kcb && d->kcb->pfnDeallocateCb) {
        D3DDDICB_DEALLOCATE cb = {};
        cb.hResource = o->rtres;
        cb.NumAllocations = 1;
        cb.HandleList = &o->kalloc;
        d->kcb->pfnDeallocateCb(d->rt.handle, &cb);
        o->kalloc = 0;
    }
    release_obj(o);
}

/* Another program's shared surface: the share id rides in the allocation's private data. */
static SIZE_T APIENTRY u_CalcPrivateOpenedResourceSize(D3D10DDI_HDEVICE, const D3D10DDIARG_OPENRESOURCE *) { return sizeof(Obj); }

static void APIENTRY u_OpenResource(D3D10DDI_HDEVICE hd, const D3D10DDIARG_OPENRESOURCE *a, D3D10DDI_HRESOURCE hr, D3D10DDI_HRTRESOURCE)
{
    ulog_call("OpenResource");
    Dev *d = DV(hd);
    Obj *o = new (hr.pDrvPrivate) Obj();
    const struct hww_alloc *al = a->NumAllocations && a->pOpenAllocationInfo ? (const struct hww_alloc *)a->pOpenAllocationInfo[0].pPrivateDriverData : NULL;
    if (!al || a->pOpenAllocationInfo[0].PrivateDriverDataSize < sizeof *al || al->magic != HWW_ALLOC_MAGIC || !al->id) {
        ulog("hwd3d_umd: OpenResource of a surface that is not ours\n");
        fail(d, E_INVALIDARG);
        return;
    }
    struct wbuf *b = begin(d->hdev, 0, HWD3D_OP_OPEN_SHARED);
    put32(b, (uint32_t)al->id);
    struct rd r;
    HRESULT res = (HRESULT)call_wait(b, &r);
    o->h = take32(&r);
    D3D11_TEXTURE2D_DESC t2 = {};
    take(&r, &t2, sizeof t2);
    call_end(&r);
    if (FAILED(res) || !o->h) {
        ulog("hwd3d_umd: the phone could not open shared surface %u (0x%08lx)\n", (unsigned)al->id, (unsigned long)res);
        fail(d, FAILED(res) ? res : E_FAIL);
        return;
    }
    o->share = (uint32_t)al->id;
    o->swap_rb = al->format != (uint32_t)t2.Format;   /* made as BGRA, kept as RGBA */
    o->dim = 3;
    o->r = {3, (uint32_t)t2.Format, t2.Width, t2.Height, 1, 1};
    o->subs = 1;
    o->samples = 1;
    o->array = 1;
    o->usage = t2.Usage;
    ulog("hwd3d_umd: opened shared surface %u, %ux%u\n", o->share, t2.Width, t2.Height);
}

/* ---- views ---- */

static SIZE_T APIENTRY u_CalcPrivateRenderTargetViewSize(D3D10DDI_HDEVICE, const D3D10DDIARG_CREATERENDERTARGETVIEW *) { return sizeof(Obj); }

static void APIENTRY u_CreateRenderTargetView(D3D10DDI_HDEVICE hd, const D3D10DDIARG_CREATERENDERTARGETVIEW *a,
                                              D3D10DDI_HRENDERTARGETVIEW hv, D3D10DDI_HRTRENDERTARGETVIEW)
{
    ulog_call("DestroyResource");
    ulog_call("CalcPrivateRenderTargetViewSize");
    ulog_call("CreateRenderTargetView");
    Dev *d = DV(hd);
    Obj *v = new (hv.pDrvPrivate) Obj();
    Obj *res = OB(a->hDrvResource);
    D3D11_RENDER_TARGET_VIEW_DESC vd;
    memset(&vd, 0, sizeof vd);
    vd.Format = res->swap_rb ? rgba_of(a->Format) : a->Format;
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

static BOOL APIENTRY u_Flush(D3D10DDI_HDEVICE hd, UINT)
{
    ulog_call("Flush");
    call_async(cx(DV(hd), HMX_ID3D11DeviceContext_Flush));
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
    ulog_call("StagingResourceMap");
    memset(out, 0, sizeof *out);
    map_phone(hd, hr, sub, type, flags, out);
}

static void APIENTRY u_ResourceMap(D3D10DDI_HDEVICE hd, D3D10DDI_HRESOURCE hr, UINT sub, D3D10_DDI_MAP type, UINT flags,
                                   D3D10DDI_MAPPED_SUBRESOURCE *out)
{
    ulog_call("ResourceMap");
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

static void APIENTRY u_CheckMultisampleQualityLevels(D3D10DDI_HDEVICE hd, DXGI_FORMAT f, UINT samples, UINT, UINT *out)
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

/* ---- the rest of the device: states, shaders, views, draws ---- */

static_assert(sizeof(Obj) <= 128, "the stubs of the Calc entries answer 128");

template <class T> static uint32_t H(T t)
{
    Obj *o = static_cast<Obj *>(t.pDrvPrivate);
    return o ? o->h : 0;
}

template <class T> static Obj *O(T t) { return static_cast<Obj *>(t.pDrvPrivate); }

/* A create on the phone's device with one description in and one object out. */
static uint32_t phone_create(Dev *d, uint32_t method, const void *desc, uint32_t size, uint32_t first = 0)
{
    struct wbuf *b = dc(d, method);
    if (first) put32(b, first);
    put_arr(b, desc, 1, size);
    put32(b, 1);
    struct rd r;
    HRESULT hr = (HRESULT)call_wait(b, &r);
    uint32_t h = take32(&r);
    call_end(&r);
    if (FAILED(hr) || !h) {
        fail(d, FAILED(hr) ? hr : E_FAIL);
        return 0;
    }
    return h;
}

template <class T> static void APIENTRY u_destroy(D3D10DDI_HDEVICE, T h) { release_obj(O(h)); }

/* -- states -- */

static void APIENTRY u_CreateBlendState(D3D10DDI_HDEVICE hd, const D3D11_1_DDI_BLEND_DESC *a, D3D10DDI_HBLENDSTATE hs, D3D10DDI_HRTBLENDSTATE)
{
    ulog_call("destroy");
    ulog_call("CreateBlendState");
    Obj *o = new (hs.pDrvPrivate) Obj();
    D3D11_BLEND_DESC d;
    memset(&d, 0, sizeof d);
    d.AlphaToCoverageEnable = a->AlphaToCoverageEnable;
    d.IndependentBlendEnable = a->IndependentBlendEnable;
    for (int i = 0; i < 8; i++) {
        const D3D11_1_DDI_RENDER_TARGET_BLEND_DESC &s = a->RenderTarget[i];
        D3D11_RENDER_TARGET_BLEND_DESC &t = d.RenderTarget[i];
        t.BlendEnable = s.BlendEnable;
        t.SrcBlend = (D3D11_BLEND)s.SrcBlend;
        t.DestBlend = (D3D11_BLEND)s.DestBlend;
        t.BlendOp = (D3D11_BLEND_OP)s.BlendOp;
        t.SrcBlendAlpha = (D3D11_BLEND)s.SrcBlendAlpha;
        t.DestBlendAlpha = (D3D11_BLEND)s.DestBlendAlpha;
        t.BlendOpAlpha = (D3D11_BLEND_OP)s.BlendOpAlpha;
        t.RenderTargetWriteMask = s.RenderTargetWriteMask;
    }
    o->h = phone_create(DV(hd), HMX_ID3D11Device_CreateBlendState, &d, sizeof d);
}

static void APIENTRY u_CreateDepthStencilState(D3D10DDI_HDEVICE hd, const D3D10_DDI_DEPTH_STENCIL_DESC *a, D3D10DDI_HDEPTHSTENCILSTATE hs, D3D10DDI_HRTDEPTHSTENCILSTATE)
{
    ulog_call("CreateDepthStencilState");
    Obj *o = new (hs.pDrvPrivate) Obj();
    D3D11_DEPTH_STENCIL_DESC d;
    memset(&d, 0, sizeof d);
    d.DepthEnable = a->DepthEnable;
    d.DepthWriteMask = (D3D11_DEPTH_WRITE_MASK)a->DepthWriteMask;
    d.DepthFunc = (D3D11_COMPARISON_FUNC)a->DepthFunc;
    d.StencilEnable = a->StencilEnable;
    d.StencilReadMask = a->StencilReadMask;
    d.StencilWriteMask = a->StencilWriteMask;
    /* A face that is not enabled keeps whatever is there. */
    auto face = [](D3D11_DEPTH_STENCILOP_DESC &t, const D3D10_DDI_DEPTH_STENCILOP_DESC &s, BOOL on) {
        t.StencilFailOp = on ? (D3D11_STENCIL_OP)s.StencilFailOp : D3D11_STENCIL_OP_KEEP;
        t.StencilDepthFailOp = on ? (D3D11_STENCIL_OP)s.StencilDepthFailOp : D3D11_STENCIL_OP_KEEP;
        t.StencilPassOp = on ? (D3D11_STENCIL_OP)s.StencilPassOp : D3D11_STENCIL_OP_KEEP;
        t.StencilFunc = on ? (D3D11_COMPARISON_FUNC)s.StencilFunc : D3D11_COMPARISON_ALWAYS;
    };
    face(d.FrontFace, a->FrontFace, a->FrontEnable);
    face(d.BackFace, a->BackFace, a->BackEnable);
    o->h = phone_create(DV(hd), HMX_ID3D11Device_CreateDepthStencilState, &d, sizeof d);
}

static void APIENTRY u_CreateRasterizerState(D3D10DDI_HDEVICE hd, const D3D11_1_DDI_RASTERIZER_DESC *a, D3D10DDI_HRASTERIZERSTATE hs, D3D10DDI_HRTRASTERIZERSTATE)
{
    ulog_call("CreateRasterizerState");
    Obj *o = new (hs.pDrvPrivate) Obj();
    D3D11_RASTERIZER_DESC d;
    d.FillMode = (D3D11_FILL_MODE)a->FillMode;
    d.CullMode = (D3D11_CULL_MODE)a->CullMode;
    d.FrontCounterClockwise = a->FrontCounterClockwise;
    d.DepthBias = a->DepthBias;
    d.DepthBiasClamp = a->DepthBiasClamp;
    d.SlopeScaledDepthBias = a->SlopeScaledDepthBias;
    d.DepthClipEnable = a->DepthClipEnable;
    d.ScissorEnable = a->ScissorEnable;
    d.MultisampleEnable = a->MultisampleEnable;
    d.AntialiasedLineEnable = a->AntialiasedLineEnable;
    o->h = phone_create(DV(hd), HMX_ID3D11Device_CreateRasterizerState, &d, sizeof d);
}

static void APIENTRY u_CreateSampler(D3D10DDI_HDEVICE hd, const D3D10_DDI_SAMPLER_DESC *a, D3D10DDI_HSAMPLER hs, D3D10DDI_HRTSAMPLER)
{
    ulog_call("CreateSampler");
    Obj *o = new (hs.pDrvPrivate) Obj();
    D3D11_SAMPLER_DESC d;
    d.Filter = (D3D11_FILTER)a->Filter;
    d.AddressU = (D3D11_TEXTURE_ADDRESS_MODE)a->AddressU;
    d.AddressV = (D3D11_TEXTURE_ADDRESS_MODE)a->AddressV;
    d.AddressW = (D3D11_TEXTURE_ADDRESS_MODE)a->AddressW;
    d.MipLODBias = a->MipLODBias;
    d.MaxAnisotropy = a->MaxAnisotropy;
    d.ComparisonFunc = (D3D11_COMPARISON_FUNC)a->ComparisonFunc;
    memcpy(d.BorderColor, a->BorderColor, sizeof d.BorderColor);
    d.MinLOD = a->MinLOD;
    d.MaxLOD = a->MaxLOD;
    o->h = phone_create(DV(hd), HMX_ID3D11Device_CreateSamplerState, &d, sizeof d);
}

static void APIENTRY u_SetBlendState(D3D10DDI_HDEVICE hd, D3D10DDI_HBLENDSTATE s, const FLOAT factor[4], UINT mask)
{
    ulog_call("SetBlendState");
    struct wbuf *b = cx(DV(hd), HMX_ID3D11DeviceContext_OMSetBlendState);
    put32(b, H(s));
    put_arr(b, factor, 4, sizeof(FLOAT));
    put32(b, mask);
    call_async(b);
}

static void APIENTRY u_SetDepthStencilState(D3D10DDI_HDEVICE hd, D3D10DDI_HDEPTHSTENCILSTATE s, UINT ref)
{
    ulog_call("SetDepthStencilState");
    struct wbuf *b = cx(DV(hd), HMX_ID3D11DeviceContext_OMSetDepthStencilState);
    put32(b, H(s));
    put32(b, ref);
    call_async(b);
}

static void APIENTRY u_SetRasterizerState(D3D10DDI_HDEVICE hd, D3D10DDI_HRASTERIZERSTATE s)
{
    ulog_call("SetRasterizerState");
    struct wbuf *b = cx(DV(hd), HMX_ID3D11DeviceContext_RSSetState);
    put32(b, H(s));
    call_async(b);
}

static void APIENTRY u_SetViewports(D3D10DDI_HDEVICE hd, UINT n, UINT, const D3D10_DDI_VIEWPORT *v)
{
    ulog_call("SetViewports");
    struct wbuf *b = cx(DV(hd), HMX_ID3D11DeviceContext_RSSetViewports);
    put32(b, n);
    put_arr(b, v, n, sizeof *v);
    call_async(b);
}

static void APIENTRY u_SetScissorRects(D3D10DDI_HDEVICE hd, UINT n, UINT, const D3D10_DDI_RECT *r)
{
    ulog_call("SetScissorRects");
    struct wbuf *b = cx(DV(hd), HMX_ID3D11DeviceContext_RSSetScissorRects);
    put32(b, n);
    put_arr(b, r, n, sizeof *r);
    call_async(b);
}

/* -- shaders -- */

/* The runtime hands a shader as bare tokens plus its signatures. DXVK reads a DXBC container, so one is built:
 * the signatures (names made from the registers, since only registers cross this interface) and the code. */
struct Sig {
    uint32_t sys, reg, mask, type;
};

static const char *sv_name(uint32_t sv, bool output)
{
    switch (sv) {
    case 1: return "SV_Position";
    case 2: return "SV_ClipDistance";
    case 3: return "SV_CullDistance";
    case 4: return "SV_RenderTargetArrayIndex";
    case 5: return "SV_ViewportArrayIndex";
    case 6: return "SV_VertexID";
    case 7: return "SV_PrimitiveID";
    case 8: return "SV_InstanceID";
    case 9: return "SV_IsFrontFace";
    case 10: return "SV_SampleIndex";
    case 64: return "SV_Target";
    case 65: return "SV_Depth";
    case 66: return "SV_Coverage";
    case 67: return "SV_DepthGreaterEqual";
    case 68: return "SV_DepthLessEqual";
    default: return output ? "TEXCOORD" : "TEXCOORD";
    }
}

static void put_u32(std::vector<uint8_t> &v, uint32_t x) { v.insert(v.end(), (uint8_t *)&x, (uint8_t *)&x + 4); }

/* One signature chunk's body: count, 8, the entries, then the names. */
static std::vector<uint8_t> sig_chunk(const Sig *e, uint32_t n, bool output)
{
    std::vector<uint8_t> body, names;
    uint32_t base = 8 + n * 24;
    put_u32(body, n);
    put_u32(body, 8);
    for (uint32_t i = 0; i < n; i++) {
        const char *nm = sv_name(e[i].sys, output);
        uint32_t index = (e[i].sys == 0 || e[i].sys == 64) ? e[i].reg : 0;
        put_u32(body, base + (uint32_t)names.size());
        put_u32(body, index);
        put_u32(body, (e[i].sys == 64 || e[i].sys == 65 || e[i].sys == 66 || e[i].sys == 67 || e[i].sys == 68) ? 0 : e[i].sys);
        put_u32(body, e[i].type);
        put_u32(body, e[i].reg);
        uint32_t m = e[i].mask & 0xF;
        put_u32(body, m | (output ? ((~m & 0xF) << 8) : (m << 8)));
        names.insert(names.end(), nm, nm + strlen(nm) + 1);
    }
    while (names.size() & 3) names.push_back(0);
    body.insert(body.end(), names.begin(), names.end());
    return body;
}

/* The container's checksum. DXVK checks it. It is MD5 over everything after the checksum, with the padding
 * and the length written the way the format does (the length in bits first, then (size << 1) | 1 last). */
static void md5_block(uint32_t st[4], const uint8_t *blk)
{
    static const uint8_t sh[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                                   4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    static uint32_t K[64];
    static bool init;
    if (!init) {
        /* floor(abs(sin(i + 1)) * 2^32), written out so there is no math library */
        static const uint32_t k[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1,
            0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453,
            0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942,
            0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
            0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d,
            0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
        memcpy(K, k, sizeof K);
        init = true;
    }
    uint32_t m[16];
    memcpy(m, blk, 64);
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
    for (uint32_t i = 0; i < 64; i++) {
        uint32_t f, g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
        else { f = c ^ (b | ~d); g = (7 * i) % 16; }
        f = f + a + K[i] + m[g];
        a = d;
        d = c;
        c = b;
        b = b + ((f << sh[i]) | (f >> (32 - sh[i])));
    }
    st[0] += a;
    st[1] += b;
    st[2] += c;
    st[3] += d;
}

static void dxbc_checksum(std::vector<uint8_t> &v)
{
    const uint8_t *d = v.data() + 20;
    uint32_t size = (uint32_t)v.size() - 20, st[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476}, i = 0;
    for (; size - i >= 64; i += 64) md5_block(st, d + i);
    uint32_t n = size - i;
    uint8_t blk[64];
    uint32_t bits = size * 8, tail = (size * 2) | 1;
    if (n >= 56) {
        memset(blk, 0, 64);
        memcpy(blk, d + i, n);
        blk[n] = 0x80;
        md5_block(st, blk);
        memset(blk, 0, 64);
        memcpy(blk, &bits, 4);
        memcpy(blk + 60, &tail, 4);
        md5_block(st, blk);
    } else {
        memset(blk, 0, 64);
        memcpy(blk, &bits, 4);
        memcpy(blk + 4, d + i, n);
        blk[4 + n] = 0x80;
        memcpy(blk + 60, &tail, 4);
        md5_block(st, blk);
    }
    memcpy(v.data() + 4, st, 16);
}

static std::vector<uint8_t> dxbc(const UINT *code, const std::vector<Sig> *in, const std::vector<Sig> *out)
{
    uint32_t words = code[1], major = (code[0] >> 4) & 0xF;
    struct Chunk { uint32_t tag; std::vector<uint8_t> body; };
    std::vector<Chunk> chunks;
    if (in) chunks.push_back({(uint32_t)'NGSI', sig_chunk(in->data(), (uint32_t)in->size(), false)});
    if (out) chunks.push_back({(uint32_t)'NGSO', sig_chunk(out->data(), (uint32_t)out->size(), true)});
    Chunk sh{(uint32_t)(major >= 5 ? 'XEHS' : 'RDHS'), {}};
    sh.body.assign((const uint8_t *)code, (const uint8_t *)code + words * 4);
    chunks.push_back(sh);
    uint32_t head = 32 + 4 * (uint32_t)chunks.size(), total = head;
    for (auto &c : chunks) total += 8 + (uint32_t)c.body.size();
    std::vector<uint8_t> v;
    put_u32(v, 'CBXD');
    for (int i = 0; i < 4; i++) put_u32(v, 0);  /* the checksum, set at the end */
    put_u32(v, 1);
    put_u32(v, total);
    put_u32(v, (uint32_t)chunks.size());
    uint32_t at = head;
    for (auto &c : chunks) {
        put_u32(v, at);
        at += 8 + (uint32_t)c.body.size();
    }
    for (auto &c : chunks) {
        put_u32(v, c.tag);
        put_u32(v, (uint32_t)c.body.size());
        v.insert(v.end(), c.body.begin(), c.body.end());
    }
    dxbc_checksum(v);
    return v;
}

static std::vector<Sig> sigs_of(const D3D11_1DDIARG_SIGNATURE_ENTRY2 *e, UINT n)
{
    std::vector<Sig> v;
    for (UINT i = 0; e && i < n; i++) v.push_back({(uint32_t)e[i].SystemValue, e[i].Register, e[i].Mask, e[i].RegisterComponentType ? (uint32_t)e[i].RegisterComponentType : 3u});
    return v;
}

static void make_shader_sigs(D3D10DDI_HDEVICE hd, uint32_t method, const UINT *code, D3D10DDI_HSHADER hs, const std::vector<Sig> *sin,
                             const std::vector<Sig> *sout)
{
    Dev *d = DV(hd);
    Obj *o = new (hs.pDrvPrivate) Obj();
    std::vector<uint8_t> bc = dxbc(code, sin, sout);
    char dump[MAX_PATH];
    if (GetEnvironmentVariableA("HWD3D_DUMPSHADER", dump, sizeof dump)) {  /* the container built, for reading it */
        HANDLE f = CreateFileA(dump, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD n;
            WriteFile(f, bc.data(), (DWORD)bc.size(), &n, NULL);
            CloseHandle(f);
        }
    }
    struct wbuf *b = dc(d, method);
    put_arr(b, bc.data(), bc.size(), 1);
    uint64_t len = bc.size();
    put(b, &len, 8);
    put32(b, 0);
    put32(b, 1);
    struct rd r;
    HRESULT hr = (HRESULT)call_wait(b, &r);
    o->h = take32(&r);
    call_end(&r);
    if (FAILED(hr) || !o->h) {
        ulog("hwd3d_umd: the phone refused a shader (0x%08lx)\n", (unsigned long)hr);
        fail(d, FAILED(hr) ? hr : E_FAIL);
    }
}

static void make_shader(D3D10DDI_HDEVICE hd, uint32_t method, const UINT *code, D3D10DDI_HSHADER hs, const D3D11_1DDIARG_STAGE_IO_SIGNATURES *sg)
{
    std::vector<Sig> in, out;
    if (sg) {
        in = sigs_of(sg->pInputSignature, sg->NumInputSignatureEntries);
        out = sigs_of(sg->pOutputSignature, sg->NumOutputSignatureEntries);
    }
    make_shader_sigs(hd, method, code, hs, sg ? &in : nullptr, sg ? &out : nullptr);
}

#define SHADER_CREATE(Name, Method, Hs) \
    static void APIENTRY u_Create##Name##Shader(D3D10DDI_HDEVICE hd, const UINT *code, D3D10DDI_HSHADER hs, D3D10DDI_HRTSHADER, \
                                                const D3D11_1DDIARG_STAGE_IO_SIGNATURES *sg) \
    { \
        make_shader(hd, HMX_ID3D11Device_Create##Method, code, hs, sg); \
    }
SHADER_CREATE(Vertex, VertexShader, 0)
SHADER_CREATE(Geometry, GeometryShader, 0)
SHADER_CREATE(Pixel, PixelShader, 0)

static void APIENTRY u_CreateComputeShader(D3D10DDI_HDEVICE hd, const UINT *code, D3D10DDI_HSHADER hs, D3D10DDI_HRTSHADER)
{
    ulog_call("CreateComputeShader");
    make_shader(hd, HMX_ID3D11Device_CreateComputeShader, code, hs, nullptr);
}

/* Input layouts: the runtime has turned semantic names into registers, so the phone gets a made-up shader
 * whose input signature names the registers, and the elements named alike. */
static void APIENTRY u_CreateElementLayout(D3D10DDI_HDEVICE hd, const D3D10DDIARG_CREATEELEMENTLAYOUT *a, D3D10DDI_HELEMENTLAYOUT hl, D3D10DDI_HRTELEMENTLAYOUT)
{
    ulog_call("CreateElementLayout");
    Dev *d = DV(hd);
    Obj *o = new (hl.pDrvPrivate) Obj();
    std::vector<Sig> in;
    for (UINT i = 0; i < a->NumElements; i++) in.push_back({0, a->pVertexElements[i].InputRegister, 0xF, 3});
    static const UINT vs[2] = {0x00010040, 2};  /* vs_4_0 with no instructions */
    std::vector<uint8_t> bc = dxbc(vs, &in, nullptr);
    struct wbuf *b = dc(d, HMX_ID3D11Device_CreateInputLayout);
    put32(b, a->NumElements);
    for (UINT i = 0; i < a->NumElements; i++) {
        const D3D10DDIARG_INPUT_ELEMENT_DESC &e = a->pVertexElements[i];
        put_arr(b, "TEXCOORD", 9, 1);
        uint32_t f[6] = {e.InputRegister, (uint32_t)e.Format, e.InputSlot, e.AlignedByteOffset, (uint32_t)e.InputSlotClass, e.InstanceDataStepRate};
        put(b, f, sizeof f);
    }
    put_arr(b, bc.data(), bc.size(), 1);
    uint64_t len = bc.size();
    put(b, &len, 8);
    put32(b, 1);
    struct rd r;
    HRESULT hr = (HRESULT)call_wait(b, &r);
    o->h = take32(&r);
    call_end(&r);
    if (FAILED(hr) || !o->h) fail(d, FAILED(hr) ? hr : E_FAIL);
}

static void APIENTRY u_SetInputLayout(D3D10DDI_HDEVICE hd, D3D10DDI_HELEMENTLAYOUT l)
{
    ulog_call("SetInputLayout");
    struct wbuf *b = cx(DV(hd), HMX_ID3D11DeviceContext_IASetInputLayout);
    put32(b, H(l));
    call_async(b);
}

template <class T> static void put_handles(struct wbuf *b, const T *h, UINT n)
{
    if (!h) {
        put32(b, 0);
        return;
    }
    put32(b, 1);
    put32(b, n);
    for (UINT i = 0; i < n; i++) put32(b, H(h[i]));
}

#define STAGE(Stage, S) \
    static void APIENTRY u_##Stage##SetShader(D3D10DDI_HDEVICE hd, D3D10DDI_HSHADER s) \
    { \
        struct wbuf *b = cx(DV(hd), HMX_ID3D11DeviceContext_##S##SetShader); \
        put32(b, H(s)); \
        put32(b, 0); \
        put32(b, 0); \
        call_async(b); \
    } \
    static void APIENTRY u_##Stage##SetShaderResources(D3D10DDI_HDEVICE hd, UINT start, UINT n, const D3D10DDI_HSHADERRESOURCEVIEW *v) \
    { \
        struct wbuf *b = cx(DV(hd), HMX_ID3D11DeviceContext_##S##SetShaderResources); \
        put32(b, start); \
        put32(b, n); \
        put_handles(b, v, n); \
        call_async(b); \
    } \
    static void APIENTRY u_##Stage##SetSamplers(D3D10DDI_HDEVICE hd, UINT start, UINT n, const D3D10DDI_HSAMPLER *v) \
    { \
        struct wbuf *b = cx(DV(hd), HMX_ID3D11DeviceContext_##S##SetSamplers); \
        put32(b, start); \
        put32(b, n); \
        put_handles(b, v, n); \
        call_async(b); \
    } \
    static void APIENTRY u_##Stage##SetConstantBuffers(D3D10DDI_HDEVICE hd, UINT start, UINT n, const D3D10DDI_HRESOURCE *v, const UINT *first, const UINT *num) \
    { \
        if (first || num) note_constant_ranges(first, num, n); \
        struct wbuf *b = cx(DV(hd), HMX_ID3D11DeviceContext_##S##SetConstantBuffers); \
        put32(b, start); \
        put32(b, n); \
        put_handles(b, v, n); \
        call_async(b); \
    }

/* The phone's Direct3D 11.0 context binds a whole buffer. A range of it (11.1) is noted, not honoured yet. */
static void note_constant_ranges(const UINT *first, const UINT *num, UINT n)
{
    for (UINT i = 0; i < n; i++)
        if ((first && first[i]) || (num && num[i] && num[i] != 4096)) {
            ulog("hwd3d_umd: a constant buffer range is not done yet\n");
            return;
        }
}

STAGE(Vs, VS)
STAGE(Ps, PS)
STAGE(Gs, GS)
STAGE(Hs, HS)
STAGE(Ds, DS)
STAGE(Cs, CS)

static void APIENTRY u_SetTopology(D3D10DDI_HDEVICE hd, D3D10_DDI_PRIMITIVE_TOPOLOGY t)
{
    ulog_call("SetTopology");
    struct wbuf *b = cx(DV(hd), HMX_ID3D11DeviceContext_IASetPrimitiveTopology);
    put32(b, (uint32_t)t);
    call_async(b);
}

static void APIENTRY u_SetVertexBuffers(D3D10DDI_HDEVICE hd, UINT start, UINT n, const D3D10DDI_HRESOURCE *v, const UINT *strides, const UINT *offsets)
{
    ulog_call("SetVertexBuffers");
    struct wbuf *b = cx(DV(hd), HMX_ID3D11DeviceContext_IASetVertexBuffers);
    put32(b, start);
    put32(b, n);
    put_handles(b, v, n);
    put_arr(b, strides, n, 4);
    put_arr(b, offsets, n, 4);
    call_async(b);
}

static void APIENTRY u_SetIndexBuffer(D3D10DDI_HDEVICE hd, D3D10DDI_HRESOURCE r, DXGI_FORMAT f, UINT offset)
{
    ulog_call("SetIndexBuffer");
    struct wbuf *b = cx(DV(hd), HMX_ID3D11DeviceContext_IASetIndexBuffer);
    put32(b, H(r));
    put32(b, (uint32_t)f);
    put32(b, offset);
    call_async(b);
}

/* -- views -- */

static void APIENTRY u_CreateShaderResourceView(D3D10DDI_HDEVICE hd, const D3D11DDIARG_CREATESHADERRESOURCEVIEW *a, D3D10DDI_HSHADERRESOURCEVIEW hv, D3D10DDI_HRTSHADERRESOURCEVIEW)
{
    ulog_call("CreateShaderResourceView");
    Dev *d = DV(hd);
    Obj *v = new (hv.pDrvPrivate) Obj();
    Obj *res = O(a->hDrvResource);
    D3D11_SHADER_RESOURCE_VIEW_DESC vd;
    memset(&vd, 0, sizeof vd);
    vd.Format = res->swap_rb ? rgba_of(a->Format) : a->Format;
    switch (a->ResourceDimension) {
    case D3D10DDIRESOURCE_BUFFER:
        if (a->BufferEx.Flags & D3D11_DDI_BUFFEREX_SRV_FLAG_RAW) {
            vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
            vd.BufferEx = {a->BufferEx.FirstElement, a->BufferEx.NumElements, D3D11_BUFFEREX_SRV_FLAG_RAW};
        } else {
            vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            vd.Buffer.FirstElement = a->Buffer.FirstElement;
            vd.Buffer.NumElements = a->Buffer.NumElements;
        }
        break;
    case D3D10DDIRESOURCE_TEXTURE1D:
        if (a->Tex1D.ArraySize > 1 || a->Tex1D.FirstArraySlice) {
            vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1DARRAY;
            vd.Texture1DArray = {a->Tex1D.MostDetailedMip, a->Tex1D.MipLevels, a->Tex1D.FirstArraySlice, a->Tex1D.ArraySize};
        } else {
            vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1D;
            vd.Texture1D = {a->Tex1D.MostDetailedMip, a->Tex1D.MipLevels};
        }
        break;
    case D3D10DDIRESOURCE_TEXTURE3D:
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
        vd.Texture3D = {a->Tex3D.MostDetailedMip, a->Tex3D.MipLevels};
        break;
    case D3D10DDIRESOURCE_TEXTURECUBE:
        if (a->TexCube.NumCubes > 1 || a->TexCube.First2DArrayFace) {
            vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
            vd.TextureCubeArray = {a->TexCube.MostDetailedMip, a->TexCube.MipLevels, a->TexCube.First2DArrayFace, a->TexCube.NumCubes};
        } else {
            vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
            vd.TextureCube = {a->TexCube.MostDetailedMip, a->TexCube.MipLevels};
        }
        break;
    default:
        if (res->samples > 1) {
            if (a->Tex2D.ArraySize > 1 || a->Tex2D.FirstArraySlice) {
                vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
                vd.Texture2DMSArray = {a->Tex2D.FirstArraySlice, a->Tex2D.ArraySize};
            } else {
                vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
            }
        } else if (a->Tex2D.ArraySize > 1 || a->Tex2D.FirstArraySlice) {
            vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            vd.Texture2DArray = {a->Tex2D.MostDetailedMip, a->Tex2D.MipLevels, a->Tex2D.FirstArraySlice, a->Tex2D.ArraySize};
        } else {
            vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            vd.Texture2D = {a->Tex2D.MostDetailedMip, a->Tex2D.MipLevels};
        }
        break;
    }
    v->h = phone_create(d, HMX_ID3D11Device_CreateShaderResourceView, &vd, sizeof vd, res->h);
}

static void APIENTRY u_CreateDepthStencilView(D3D10DDI_HDEVICE hd, const D3D11DDIARG_CREATEDEPTHSTENCILVIEW *a, D3D10DDI_HDEPTHSTENCILVIEW hv, D3D10DDI_HRTDEPTHSTENCILVIEW)
{
    ulog_call("CreateDepthStencilView");
    Dev *d = DV(hd);
    Obj *v = new (hv.pDrvPrivate) Obj();
    Obj *res = O(a->hDrvResource);
    D3D11_DEPTH_STENCIL_VIEW_DESC vd;
    memset(&vd, 0, sizeof vd);
    vd.Format = res->swap_rb ? rgba_of(a->Format) : a->Format;
    vd.Flags = a->Flags;
    if (a->ResourceDimension == D3D10DDIRESOURCE_TEXTURE1D) {
        if (res->array > 1) {
            vd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE1DARRAY;
            vd.Texture1DArray = {a->Tex1D.MipSlice, a->Tex1D.FirstArraySlice, a->Tex1D.ArraySize};
        } else {
            vd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE1D;
            vd.Texture1D.MipSlice = a->Tex1D.MipSlice;
        }
    } else if (res->samples > 1) {
        if (res->array > 1) {
            vd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY;
            vd.Texture2DMSArray = {a->Tex2D.FirstArraySlice, a->Tex2D.ArraySize};
        } else {
            vd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
        }
    } else if (res->array > 1) {
        vd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        vd.Texture2DArray = {a->Tex2D.MipSlice, a->Tex2D.FirstArraySlice, a->Tex2D.ArraySize};
    } else {
        vd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        vd.Texture2D.MipSlice = a->Tex2D.MipSlice;
    }
    v->h = phone_create(d, HMX_ID3D11Device_CreateDepthStencilView, &vd, sizeof vd, res->h);
}

static void APIENTRY u_CreateUnorderedAccessView(D3D10DDI_HDEVICE hd, const D3D11DDIARG_CREATEUNORDEREDACCESSVIEW *a, D3D11DDI_HUNORDEREDACCESSVIEW hv, D3D11DDI_HRTUNORDEREDACCESSVIEW)
{
    ulog_call("CreateUnorderedAccessView");
    Dev *d = DV(hd);
    Obj *v = new (hv.pDrvPrivate) Obj();
    Obj *res = O(a->hDrvResource);
    D3D11_UNORDERED_ACCESS_VIEW_DESC vd;
    memset(&vd, 0, sizeof vd);
    vd.Format = res->swap_rb ? rgba_of(a->Format) : a->Format;
    switch (a->ResourceDimension) {
    case D3D10DDIRESOURCE_BUFFER:
        vd.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        vd.Buffer = {a->Buffer.FirstElement, a->Buffer.NumElements, a->Buffer.Flags};
        break;
    case D3D10DDIRESOURCE_TEXTURE1D:
        if (res->array > 1) {
            vd.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE1DARRAY;
            vd.Texture1DArray = {a->Tex1D.MipSlice, a->Tex1D.FirstArraySlice, a->Tex1D.ArraySize};
        } else {
            vd.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE1D;
            vd.Texture1D.MipSlice = a->Tex1D.MipSlice;
        }
        break;
    case D3D10DDIRESOURCE_TEXTURE3D:
        vd.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
        vd.Texture3D = {a->Tex3D.MipSlice, a->Tex3D.FirstW, a->Tex3D.WSize};
        break;
    default:
        if (res->array > 1) {
            vd.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
            vd.Texture2DArray = {a->Tex2D.MipSlice, a->Tex2D.FirstArraySlice, a->Tex2D.ArraySize};
        } else {
            vd.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            vd.Texture2D.MipSlice = a->Tex2D.MipSlice;
        }
        break;
    }
    v->h = phone_create(d, HMX_ID3D11Device_CreateUnorderedAccessView, &vd, sizeof vd, res->h);
}

static void APIENTRY u_SetRenderTargets(D3D10DDI_HDEVICE hd, const D3D10DDI_HRENDERTARGETVIEW *rtv, UINT n, UINT, D3D10DDI_HDEPTHSTENCILVIEW dsv,
                                        const D3D11DDI_HUNORDEREDACCESSVIEW *uav, const UINT *counts, UINT uavStart, UINT nUav, UINT, UINT)
{
    ulog_call("SetRenderTargets");
    Dev *d = DV(hd);
    struct wbuf *b;
    if (!nUav) {
        b = cx(d, HMX_ID3D11DeviceContext_OMSetRenderTargets);
        put32(b, n);
        put_handles(b, rtv, n);
        put32(b, H(dsv));
    } else {
        b = cx(d, HMX_ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews);
        put32(b, n);
        put_handles(b, rtv, n);
        put32(b, H(dsv));
        put32(b, uavStart);
        put32(b, nUav);
        put_handles(b, uav, nUav);
        put_arr(b, counts, nUav, 4);
    }
    call_async(b);
}

static void APIENTRY u_ClearDepthStencilView(D3D10DDI_HDEVICE hd, D3D10DDI_HDEPTHSTENCILVIEW v, UINT flags, FLOAT depth, UINT8 stencil)
{
    ulog_call("ClearDepthStencilView");
    struct wbuf *b = cx(DV(hd), HMX_ID3D11DeviceContext_ClearDepthStencilView);
    put32(b, H(v));
    put32(b, flags);
    put(b, &depth, 4);
    put32(b, stencil);
    call_async(b);
}

static void APIENTRY u_ClearUnorderedAccessViewUint(D3D10DDI_HDEVICE hd, D3D11DDI_HUNORDEREDACCESSVIEW v, const UINT c[4])
{
    ulog_call("ClearUnorderedAccessViewUint");
    struct wbuf *b = cx(DV(hd), HMX_ID3D11DeviceContext_ClearUnorderedAccessViewUint);
    put32(b, H(v));
    put_arr(b, c, 4, 4);
    call_async(b);
}

static void APIENTRY u_ClearUnorderedAccessViewFloat(D3D10DDI_HDEVICE hd, D3D11DDI_HUNORDEREDACCESSVIEW v, const FLOAT c[4])
{
    ulog_call("ClearUnorderedAccessViewFloat");
    struct wbuf *b = cx(DV(hd), HMX_ID3D11DeviceContext_ClearUnorderedAccessViewFloat);
    put32(b, H(v));
    put_arr(b, c, 4, 4);
    call_async(b);
}

static void APIENTRY u_CsSetUnorderedAccessViews(D3D10DDI_HDEVICE hd, UINT start, UINT n, const D3D11DDI_HUNORDEREDACCESSVIEW *v, const UINT *counts)
{
    ulog_call("CsSetUnorderedAccessViews");
    struct wbuf *b = cx(DV(hd), HMX_ID3D11DeviceContext_CSSetUnorderedAccessViews);
    put32(b, start);
    put32(b, n);
    put_handles(b, v, n);
    put_arr(b, counts, n, 4);
    call_async(b);
}

/* -- draws -- */

static void APIENTRY u_Draw(D3D10DDI_HDEVICE hd, UINT count, UINT start)
{
    ulog_call("Draw");
    struct wbuf *b = cx(DV(hd), HMX_ID3D11DeviceContext_Draw);
    put32(b, count);
    put32(b, start);
    call_async(b);
}

static void APIENTRY u_DrawIndexed(D3D10DDI_HDEVICE hd, UINT count, UINT start, INT base)
{
    ulog_call("DrawIndexed");
    struct wbuf *b = cx(DV(hd), HMX_ID3D11DeviceContext_DrawIndexed);
    put32(b, count);
    put32(b, start);
    put32(b, (uint32_t)base);
    call_async(b);
}

static void APIENTRY u_DrawInstanced(D3D10DDI_HDEVICE hd, UINT count, UINT inst, UINT start, UINT start_inst)
{
    ulog_call("DrawInstanced");
    struct wbuf *b = cx(DV(hd), HMX_ID3D11DeviceContext_DrawInstanced);
    put32(b, count);
    put32(b, inst);
    put32(b, start);
    put32(b, start_inst);
    call_async(b);
}

static void APIENTRY u_DrawIndexedInstanced(D3D10DDI_HDEVICE hd, UINT count, UINT inst, UINT start, INT base, UINT start_inst)
{
    ulog_call("DrawIndexedInstanced");
    struct wbuf *b = cx(DV(hd), HMX_ID3D11DeviceContext_DrawIndexedInstanced);
    put32(b, count);
    put32(b, inst);
    put32(b, start);
    put32(b, (uint32_t)base);
    put32(b, start_inst);
    call_async(b);
}

static void APIENTRY u_DrawInstancedIndirect(D3D10DDI_HDEVICE hd, D3D10DDI_HRESOURCE r, UINT off)
{
    ulog_call("DrawInstancedIndirect");
    struct wbuf *b = cx(DV(hd), HMX_ID3D11DeviceContext_DrawInstancedIndirect);
    put32(b, H(r));
    put32(b, off);
    call_async(b);
}

static void APIENTRY u_DrawIndexedInstancedIndirect(D3D10DDI_HDEVICE hd, D3D10DDI_HRESOURCE r, UINT off)
{
    ulog_call("DrawIndexedInstancedIndirect");
    struct wbuf *b = cx(DV(hd), HMX_ID3D11DeviceContext_DrawIndexedInstancedIndirect);
    put32(b, H(r));
    put32(b, off);
    call_async(b);
}

static void APIENTRY u_Dispatch(D3D10DDI_HDEVICE hd, UINT x, UINT y, UINT z)
{
    ulog_call("Dispatch");
    struct wbuf *b = cx(DV(hd), HMX_ID3D11DeviceContext_Dispatch);
    put32(b, x);
    put32(b, y);
    put32(b, z);
    call_async(b);
}

static void APIENTRY u_DispatchIndirect(D3D10DDI_HDEVICE hd, D3D10DDI_HRESOURCE r, UINT off)
{
    ulog_call("DispatchIndirect");
    struct wbuf *b = cx(DV(hd), HMX_ID3D11DeviceContext_DispatchIndirect);
    put32(b, H(r));
    put32(b, off);
    call_async(b);
}

/* -- copies -- */

static void APIENTRY u_ResourceCopyRegion(D3D10DDI_HDEVICE hd, D3D10DDI_HRESOURCE dst, UINT dsub, UINT x, UINT y, UINT z, D3D10DDI_HRESOURCE src, UINT ssub,
                                          const D3D10_DDI_BOX *box, UINT)
{
    ulog_call("ResourceCopyRegion");
    struct wbuf *b = cx(DV(hd), HMX_ID3D11DeviceContext_CopySubresourceRegion);
    put32(b, H(dst));
    put32(b, dsub);
    put32(b, x);
    put32(b, y);
    put32(b, z);
    put32(b, H(src));
    put32(b, ssub);
    put_arr(b, box, 1, sizeof *box);
    call_async(b);
}

static void update_subresource(D3D10DDI_HDEVICE hd, D3D10DDI_HRESOURCE hr, UINT sub, const D3D10_DDI_BOX *box, const void *src, UINT pitch, UINT slice_pitch)
{
    Obj *o = O(hr);
    uint32_t row, rows, slices;
    if (!hwd3d_region(&o->r, sub, (const uint32_t *)box, &row, &rows, &slices)) {
        ulog("hwd3d_umd: an update of this format or box is not done yet\n");
        return;
    }
    struct wbuf *b = cx(DV(hd), HMX_ID3D11DeviceContext_UpdateSubresource);
    put32(b, o->h);
    put32(b, sub);
    put_arr(b, box, 1, sizeof *box);
    put_rows(b, (const uint8_t *)src, row, rows, slices, o->dim == 2 ? row : pitch, slice_pitch);
    call_async(b);
}

static void APIENTRY u_ResourceUpdateSubresourceUP(D3D10DDI_HDEVICE hd, D3D10DDI_HRESOURCE r, UINT sub, const D3D10_DDI_BOX *box, const void *src, UINT p, UINT sp, UINT)
{
    ulog_call("ResourceUpdateSubresourceUP");
    if (p == HWD3D_SCANOUT_PITCH && sp == HWD3D_SCANOUT_SLICE) {   /* the screen driver hands a frame to the phone */
        Obj *o = OB(r);
        if (o && o->share) {
            struct wbuf *b = begin(DV(hd)->hdev, 0, HWD3D_OP_SCANOUT);
            put32(b, o->share);
            call_async(b);
            return;
        }
    }
    update_subresource(hd, r, sub, box, src, p, sp);
}

static void APIENTRY u_ResourceResolveSubresource(D3D10DDI_HDEVICE hd, D3D10DDI_HRESOURCE dst, UINT dsub, D3D10DDI_HRESOURCE src, UINT ssub, DXGI_FORMAT f)
{
    ulog_call("ResourceResolveSubresource");
    struct wbuf *b = cx(DV(hd), HMX_ID3D11DeviceContext_ResolveSubresource);
    put32(b, H(dst));
    put32(b, dsub);
    put32(b, H(src));
    put32(b, ssub);
    put32(b, (uint32_t)f);
    call_async(b);
}

static void APIENTRY u_GenMips(D3D10DDI_HDEVICE hd, D3D10DDI_HSHADERRESOURCEVIEW v)
{
    ulog_call("GenMips");
    struct wbuf *b = cx(DV(hd), HMX_ID3D11DeviceContext_GenerateMips);
    put32(b, H(v));
    call_async(b);
}

static void APIENTRY u_SetResourceMinLOD(D3D10DDI_HDEVICE hd, D3D10DDI_HRESOURCE r, FLOAT lod)
{
    ulog_call("SetResourceMinLOD");
    struct wbuf *b = cx(DV(hd), HMX_ID3D11DeviceContext_SetResourceMinLOD);
    put32(b, H(r));
    put(b, &lod, 4);
    call_async(b);
}

static void APIENTRY u_CopyStructureCount(D3D10DDI_HDEVICE hd, D3D10DDI_HRESOURCE dst, UINT off, D3D11DDI_HUNORDEREDACCESSVIEW v)
{
    ulog_call("CopyStructureCount");
    struct wbuf *b = cx(DV(hd), HMX_ID3D11DeviceContext_CopyStructureCount);
    put32(b, H(dst));
    put32(b, off);
    put32(b, H(v));
    call_async(b);
}

static BOOL APIENTRY u_ResourceIsStagingBusy(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE) { return FALSE; }

/* -- queries -- */

static uint32_t query_type(uint32_t q)
{
    ulog_call("ResourceIsStagingBusy");
    if (q <= 7) return q;            /* the ten's numbers are the eleven's */
    if (q == 8) return 4;            /* the pipeline statistics of eleven */
    if (q >= 9 && q <= 12) return q - 1;
    if (q >= 13 && q <= 16) return q - 1;
    return 0xFFFFFFFFu;
}

static SIZE_T APIENTRY u_CalcPrivateQuerySize(D3D10DDI_HDEVICE, const D3D10DDIARG_CREATEQUERY *) { return sizeof(Obj); }

static void APIENTRY u_CreateQuery(D3D10DDI_HDEVICE hd, const D3D10DDIARG_CREATEQUERY *a, D3D10DDI_HQUERY hq, D3D10DDI_HRTQUERY)
{
    ulog_call("CalcPrivateQuerySize");
    ulog_call("CreateQuery");
    Obj *o = new (hq.pDrvPrivate) Obj();
    uint32_t t = query_type((uint32_t)a->Query);
    if (t == 0xFFFFFFFFu) {
        fail(DV(hd), E_NOTIMPL);
        return;
    }
    D3D11_QUERY_DESC qd = {(D3D11_QUERY)t, a->MiscFlags};
    o->h = phone_create(DV(hd), HMX_ID3D11Device_CreateQuery, &qd, sizeof qd);
}

static void APIENTRY u_QueryBegin(D3D10DDI_HDEVICE hd, D3D10DDI_HQUERY q)
{
    ulog_call("QueryBegin");
    struct wbuf *b = cx(DV(hd), HMX_ID3D11DeviceContext_Begin);
    put32(b, H(q));
    call_async(b);
}

static void APIENTRY u_QueryEnd(D3D10DDI_HDEVICE hd, D3D10DDI_HQUERY q)
{
    ulog_call("QueryEnd");
    struct wbuf *b = cx(DV(hd), HMX_ID3D11DeviceContext_End);
    put32(b, H(q));
    put32(b, 0);  /* no result slot: GetData asks the phone */
    put32(b, 0);
    call_async(b);
}

static void APIENTRY u_QueryGetData(D3D10DDI_HDEVICE hd, D3D10DDI_HQUERY q, void *data, UINT size, UINT flags)
{
    ulog_call("QueryGetData");
    Dev *d = DV(hd);
    struct wbuf *b = cx(d, HMX_ID3D11DeviceContext_GetData);
    put32(b, H(q));
    put32(b, data ? size : 0);
    put32(b, (flags & D3D10_DDI_GET_DATA_DO_NOT_FLUSH) ? D3D11_ASYNC_GETDATA_DONOTFLUSH : 0);
    struct rd r;
    HRESULT hr = (HRESULT)call_wait(b, &r);
    take(&r, data, data ? size : 0);
    call_end(&r);
    if (hr == S_FALSE) fail(d, DXGI_DDI_ERR_WASSTILLDRAWING);
    else if (FAILED(hr)) fail(d, hr);
}

static void APIENTRY u_SetPredication(D3D10DDI_HDEVICE hd, D3D10DDI_HQUERY q, BOOL v)
{
    ulog_call("SetPredication");
    struct wbuf *b = cx(DV(hd), HMX_ID3D11DeviceContext_SetPredication);
    put32(b, H(q));
    put32(b, (uint32_t)v);
    call_async(b);
}

/* ---- the Direct3D 10.1 table: the same work with the older argument shapes ---- */

static void APIENTRY u10_CreateResource(D3D10DDI_HDEVICE hd, const D3D10DDIARG_CREATERESOURCE *a, D3D10DDI_HRESOURCE hr, D3D10DDI_HRTRESOURCE rt)
{
    D3D11DDIARG_CREATERESOURCE b;
    memset(&b, 0, sizeof b);
    b.pMipInfoList = a->pMipInfoList;
    b.pInitialDataUP = a->pInitialDataUP;
    b.ResourceDimension = a->ResourceDimension;
    b.Usage = a->Usage;
    b.BindFlags = a->BindFlags;
    b.MapFlags = a->MapFlags;
    b.MiscFlags = a->MiscFlags;
    b.Format = a->Format;
    b.SampleDesc = a->SampleDesc;
    b.MipLevels = a->MipLevels;
    b.ArraySize = a->ArraySize;
    b.pPrimaryDesc = a->pPrimaryDesc;
    u_CreateResource(hd, &b, hr, rt);
}

static void APIENTRY u10_CreateShaderResourceView(D3D10DDI_HDEVICE hd, const D3D10_1DDIARG_CREATESHADERRESOURCEVIEW *a, D3D10DDI_HSHADERRESOURCEVIEW hv,
                                                  D3D10DDI_HRTSHADERRESOURCEVIEW rt)
{
    D3D11DDIARG_CREATESHADERRESOURCEVIEW b;
    memset(&b, 0, sizeof b);
    memcpy(&b, a, sizeof *a);  /* the 11 form is this one with a raw flag after a buffer's range */
    u_CreateShaderResourceView(hd, &b, hv, rt);
}

static void APIENTRY u10_CreateDepthStencilView(D3D10DDI_HDEVICE hd, const D3D10DDIARG_CREATEDEPTHSTENCILVIEW *a, D3D10DDI_HDEPTHSTENCILVIEW hv,
                                                D3D10DDI_HRTDEPTHSTENCILVIEW rt)
{
    D3D11DDIARG_CREATEDEPTHSTENCILVIEW b;
    memset(&b, 0, sizeof b);
    b.hDrvResource = a->hDrvResource;
    b.Format = a->Format;
    b.ResourceDimension = a->ResourceDimension;
    b.Tex2D = {a->Tex2D.MipSlice, a->Tex2D.FirstArraySlice, a->Tex2D.ArraySize};
    u_CreateDepthStencilView(hd, &b, hv, rt);
}

static void APIENTRY u10_CreateBlendState(D3D10DDI_HDEVICE hd, const D3D10_1_DDI_BLEND_DESC *a, D3D10DDI_HBLENDSTATE hs, D3D10DDI_HRTBLENDSTATE rt)
{
    D3D11_1_DDI_BLEND_DESC b;
    memset(&b, 0, sizeof b);
    b.AlphaToCoverageEnable = a->AlphaToCoverageEnable;
    b.IndependentBlendEnable = a->IndependentBlendEnable;
    for (int i = 0; i < 8; i++) {
        const D3D10_DDI_RENDER_TARGET_BLEND_DESC1 &s = a->RenderTarget[i];
        D3D11_1_DDI_RENDER_TARGET_BLEND_DESC &t = b.RenderTarget[i];
        t.BlendEnable = s.BlendEnable;
        t.SrcBlend = s.SrcBlend;
        t.DestBlend = s.DestBlend;
        t.BlendOp = s.BlendOp;
        t.SrcBlendAlpha = s.SrcBlendAlpha;
        t.DestBlendAlpha = s.DestBlendAlpha;
        t.BlendOpAlpha = s.BlendOpAlpha;
        t.RenderTargetWriteMask = s.RenderTargetWriteMask;
    }
    u_CreateBlendState(hd, &b, hs, rt);
}

static void APIENTRY u10_CreateRasterizerState(D3D10DDI_HDEVICE hd, const D3D10_DDI_RASTERIZER_DESC *a, D3D10DDI_HRASTERIZERSTATE hs, D3D10DDI_HRTRASTERIZERSTATE rt)
{
    D3D11_1_DDI_RASTERIZER_DESC b = {a->FillMode, a->CullMode, a->FrontCounterClockwise, a->DepthBias, a->DepthBiasClamp, a->SlopeScaledDepthBias,
                                     a->DepthClipEnable, a->ScissorEnable, a->MultisampleEnable, a->AntialiasedLineEnable, 0};
    u_CreateRasterizerState(hd, &b, hs, rt);
}

static std::vector<Sig> sigs10_of(const D3D10DDIARG_SIGNATURE_ENTRY *e, UINT n)
{
    std::vector<Sig> v;
    for (UINT i = 0; e && i < n; i++) v.push_back({(uint32_t)e[i].SystemValue, e[i].Register, e[i].Mask, 3u});
    return v;
}

static void make_shader10(D3D10DDI_HDEVICE hd, uint32_t method, const UINT *code, D3D10DDI_HSHADER hs, const D3D10DDIARG_STAGE_IO_SIGNATURES *sg)
{
    std::vector<Sig> in, out;
    if (sg) {
        in = sigs10_of(sg->pInputSignature, sg->NumInputSignatureEntries);
        out = sigs10_of(sg->pOutputSignature, sg->NumOutputSignatureEntries);
    }
    make_shader_sigs(hd, method, code, hs, sg ? &in : nullptr, sg ? &out : nullptr);
}

#define SHADER10(Name, Method) \
    static void APIENTRY u10_Create##Name##Shader(D3D10DDI_HDEVICE hd, const UINT *code, D3D10DDI_HSHADER hs, D3D10DDI_HRTSHADER, \
                                                  const D3D10DDIARG_STAGE_IO_SIGNATURES *sg) \
    { \
        make_shader10(hd, HMX_ID3D11Device_Create##Method, code, hs, sg); \
    }
SHADER10(Vertex, VertexShader)
SHADER10(Geometry, GeometryShader)
SHADER10(Pixel, PixelShader)

#define CB10(Stage) \
    static void APIENTRY u10_##Stage##SetConstantBuffers(D3D10DDI_HDEVICE hd, UINT start, UINT n, const D3D10DDI_HRESOURCE *v) \
    { \
        u_##Stage##SetConstantBuffers(hd, start, n, v, nullptr, nullptr); \
    }
CB10(Vs)
CB10(Ps)
CB10(Gs)

static void APIENTRY u10_SetRenderTargets(D3D10DDI_HDEVICE hd, const D3D10DDI_HRENDERTARGETVIEW *rtv, UINT n, UINT clear, D3D10DDI_HDEPTHSTENCILVIEW dsv)
{
    u_SetRenderTargets(hd, rtv, n, clear, dsv, nullptr, nullptr, 0, 0, 0, 0);
}

static void APIENTRY u10_ResourceCopyRegion(D3D10DDI_HDEVICE hd, D3D10DDI_HRESOURCE dst, UINT dsub, UINT x, UINT y, UINT z, D3D10DDI_HRESOURCE src, UINT ssub,
                                            const D3D10_DDI_BOX *box)
{
    u_ResourceCopyRegion(hd, dst, dsub, x, y, z, src, ssub, box, 0);
}

static void APIENTRY u10_ResourceUpdateSubresourceUP(D3D10DDI_HDEVICE hd, D3D10DDI_HRESOURCE r, UINT sub, const D3D10_DDI_BOX *box, const void *src, UINT p, UINT sp)
{
    update_subresource(hd, r, sub, box, src, p, sp);
}

static void APIENTRY u10_Flush(D3D10DDI_HDEVICE hd) { u_Flush(hd, 0); }

static void APIENTRY u10_CheckMultisampleQualityLevels(D3D10DDI_HDEVICE hd, DXGI_FORMAT f, UINT samples, UINT *out)
{
    u_CheckMultisampleQualityLevels(hd, f, samples, 0, out);
}

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
    t->pfnDestroyResource = u_DestroyResource;
    t->pfnCalcPrivateRenderTargetViewSize = u_CalcPrivateRenderTargetViewSize;
    t->pfnCreateRenderTargetView = u_CreateRenderTargetView;
    t->pfnDestroyRenderTargetView = u_DestroyRenderTargetView;
    t->pfnDestroyShaderResourceView = u_destroy<D3D10DDI_HSHADERRESOURCEVIEW>;
    t->pfnDestroyDepthStencilView = u_destroy<D3D10DDI_HDEPTHSTENCILVIEW>;
    t->pfnCreateElementLayout = u_CreateElementLayout;
    t->pfnDestroyElementLayout = u_destroy<D3D10DDI_HELEMENTLAYOUT>;
    t->pfnDestroyBlendState = u_destroy<D3D10DDI_HBLENDSTATE>;
    t->pfnCreateDepthStencilState = u_CreateDepthStencilState;
    t->pfnDestroyDepthStencilState = u_destroy<D3D10DDI_HDEPTHSTENCILSTATE>;
    t->pfnDestroyRasterizerState = u_destroy<D3D10DDI_HRASTERIZERSTATE>;
    t->pfnCreateSampler = u_CreateSampler;
    t->pfnDestroySampler = u_destroy<D3D10DDI_HSAMPLER>;
    t->pfnDestroyShader = u_destroy<D3D10DDI_HSHADER>;
    t->pfnCreateQuery = u_CreateQuery;
    t->pfnCalcPrivateQuerySize = u_CalcPrivateQuerySize;
    t->pfnDestroyQuery = u_destroy<D3D10DDI_HQUERY>;
    t->pfnQueryBegin = u_QueryBegin;
    t->pfnQueryEnd = u_QueryEnd;
    t->pfnQueryGetData = u_QueryGetData;
    t->pfnSetPredication = u_SetPredication;
    t->pfnSetBlendState = u_SetBlendState;
    t->pfnSetDepthStencilState = u_SetDepthStencilState;
    t->pfnSetRasterizerState = u_SetRasterizerState;
    t->pfnSetViewports = u_SetViewports;
    t->pfnSetScissorRects = u_SetScissorRects;
    t->pfnIaSetInputLayout = u_SetInputLayout;
    t->pfnIaSetVertexBuffers = u_SetVertexBuffers;
    t->pfnIaSetIndexBuffer = u_SetIndexBuffer;
    t->pfnIaSetTopology = u_SetTopology;
    t->pfnVsSetShader = u_VsSetShader;
    t->pfnVsSetShaderResources = u_VsSetShaderResources;
    t->pfnVsSetSamplers = u_VsSetSamplers;
    t->pfnPsSetShader = u_PsSetShader;
    t->pfnPsSetShaderResources = u_PsSetShaderResources;
    t->pfnPsSetSamplers = u_PsSetSamplers;
    t->pfnGsSetShader = u_GsSetShader;
    t->pfnGsSetShaderResources = u_GsSetShaderResources;
    t->pfnGsSetSamplers = u_GsSetSamplers;
    t->pfnClearRenderTargetView = u_ClearRenderTargetView;
    t->pfnClearDepthStencilView = u_ClearDepthStencilView;
    t->pfnDraw = u_Draw;
    t->pfnDrawIndexed = u_DrawIndexed;
    t->pfnDrawInstanced = u_DrawInstanced;
    t->pfnDrawIndexedInstanced = u_DrawIndexedInstanced;
    t->pfnResourceCopy = u_ResourceCopy;
    t->pfnResourceResolveSubresource = u_ResourceResolveSubresource;
    t->pfnGenMips = u_GenMips;
    t->pfnResourceIsStagingBusy = u_ResourceIsStagingBusy;
    t->pfnStagingResourceMap = u_StagingResourceMap;
    t->pfnStagingResourceUnmap = u_Unmap;
    t->pfnResourceMap = u_ResourceMap;
    t->pfnResourceUnmap = u_Unmap;
    t->pfnDynamicIABufferMapDiscard = u_ResourceMap;
    t->pfnDynamicIABufferMapNoOverwrite = u_ResourceMap;
    t->pfnDynamicIABufferUnmap = u_Unmap;
    t->pfnDynamicConstantBufferMapDiscard = u_ResourceMap;
    t->pfnDynamicConstantBufferUnmap = u_Unmap;
    t->pfnDynamicResourceMapDiscard = u_ResourceMap;
    t->pfnDynamicResourceUnmap = u_Unmap;
    t->pfnCheckFormatSupport = u_CheckFormatSupport;
    t->pfnCheckCounterInfo = u_CheckCounterInfo;
    t->pfnDestroyDevice = u_DestroyDevice;
    if constexpr (Same<Tbl, D3DWDDM1_3DDI_DEVICEFUNCS>::v) {
        t->pfnCalcPrivateResourceSize = u_CalcPrivateResourceSize;
        t->pfnCreateResource = u_CreateResource;
        t->pfnCalcPrivateOpenedResourceSize = u_CalcPrivateOpenedResourceSize;
        t->pfnOpenResource = u_OpenResource;
        t->pfnCreateShaderResourceView = u_CreateShaderResourceView;
        t->pfnCreateDepthStencilView = u_CreateDepthStencilView;
        t->pfnCreateUnorderedAccessView = u_CreateUnorderedAccessView;
        t->pfnDestroyUnorderedAccessView = u_destroy<D3D11DDI_HUNORDEREDACCESSVIEW>;
        t->pfnCreateBlendState = u_CreateBlendState;
        t->pfnCreateRasterizerState = u_CreateRasterizerState;
        t->pfnCreateVertexShader = u_CreateVertexShader;
        t->pfnCreateGeometryShader = u_CreateGeometryShader;
        t->pfnCreatePixelShader = u_CreatePixelShader;
        t->pfnCreateComputeShader = u_CreateComputeShader;
        t->pfnVsSetConstantBuffers = u_VsSetConstantBuffers;
        t->pfnPsSetConstantBuffers = u_PsSetConstantBuffers;
        t->pfnGsSetConstantBuffers = u_GsSetConstantBuffers;
        t->pfnHsSetShader = u_HsSetShader;
        t->pfnHsSetShaderResources = u_HsSetShaderResources;
        t->pfnHsSetSamplers = u_HsSetSamplers;
        t->pfnHsSetConstantBuffers = u_HsSetConstantBuffers;
        t->pfnDsSetShader = u_DsSetShader;
        t->pfnDsSetShaderResources = u_DsSetShaderResources;
        t->pfnDsSetSamplers = u_DsSetSamplers;
        t->pfnDsSetConstantBuffers = u_DsSetConstantBuffers;
        t->pfnCsSetShader = u_CsSetShader;
        t->pfnCsSetShaderResources = u_CsSetShaderResources;
        t->pfnCsSetSamplers = u_CsSetSamplers;
        t->pfnCsSetConstantBuffers = u_CsSetConstantBuffers;
        t->pfnCsSetUnorderedAccessViews = u_CsSetUnorderedAccessViews;
        t->pfnSetRenderTargets = u_SetRenderTargets;
        t->pfnClearUnorderedAccessViewUint = u_ClearUnorderedAccessViewUint;
        t->pfnClearUnorderedAccessViewFloat = u_ClearUnorderedAccessViewFloat;
        t->pfnDrawInstancedIndirect = u_DrawInstancedIndirect;
        t->pfnDrawIndexedInstancedIndirect = u_DrawIndexedInstancedIndirect;
        t->pfnDispatch = u_Dispatch;
        t->pfnDispatchIndirect = u_DispatchIndirect;
        t->pfnResourceCopyRegion = u_ResourceCopyRegion;
        t->pfnResourceUpdateSubresourceUP = u_ResourceUpdateSubresourceUP;
        t->pfnDefaultConstantBufferUpdateSubresourceUP = u_ResourceUpdateSubresourceUP;
        t->pfnSetResourceMinLOD = u_SetResourceMinLOD;
        t->pfnCopyStructureCount = u_CopyStructureCount;
        t->pfnFlush = u_Flush;
        t->pfnDynamicConstantBufferMapNoOverwrite = u_ResourceMap;
        t->pfnCheckMultisampleQualityLevels = u_CheckMultisampleQualityLevels;
    }
}
#undef UMD_FILL

/* The 10.1 table has no compute, no unordered views and no tessellation: what it has is these. */
static void fill_device_funcs_10_1(D3D10_1DDI_DEVICEFUNCS *t)
{
    fill_device_funcs(t);
    t->pfnCreateResource = u10_CreateResource;
    t->pfnDestroyResource = u_DestroyResource;
    t->pfnCreateRenderTargetView = u_CreateRenderTargetView;
    t->pfnDestroyRenderTargetView = u_DestroyRenderTargetView;
    t->pfnCreateShaderResourceView = u10_CreateShaderResourceView;
    t->pfnDestroyShaderResourceView = u_destroy<D3D10DDI_HSHADERRESOURCEVIEW>;
    t->pfnCreateDepthStencilView = u10_CreateDepthStencilView;
    t->pfnDestroyDepthStencilView = u_destroy<D3D10DDI_HDEPTHSTENCILVIEW>;
    t->pfnCreateElementLayout = u_CreateElementLayout;
    t->pfnDestroyElementLayout = u_destroy<D3D10DDI_HELEMENTLAYOUT>;
    t->pfnCreateBlendState = u10_CreateBlendState;
    t->pfnDestroyBlendState = u_destroy<D3D10DDI_HBLENDSTATE>;
    t->pfnCreateDepthStencilState = u_CreateDepthStencilState;
    t->pfnDestroyDepthStencilState = u_destroy<D3D10DDI_HDEPTHSTENCILSTATE>;
    t->pfnCreateRasterizerState = u10_CreateRasterizerState;
    t->pfnDestroyRasterizerState = u_destroy<D3D10DDI_HRASTERIZERSTATE>;
    t->pfnCreateSampler = u_CreateSampler;
    t->pfnDestroySampler = u_destroy<D3D10DDI_HSAMPLER>;
    t->pfnCreateVertexShader = u10_CreateVertexShader;
    t->pfnCreateGeometryShader = u10_CreateGeometryShader;
    t->pfnCreatePixelShader = u10_CreatePixelShader;
    t->pfnDestroyShader = u_destroy<D3D10DDI_HSHADER>;
    t->pfnCreateQuery = u_CreateQuery;
    t->pfnDestroyQuery = u_destroy<D3D10DDI_HQUERY>;
    t->pfnVsSetConstantBuffers = u10_VsSetConstantBuffers;
    t->pfnPsSetConstantBuffers = u10_PsSetConstantBuffers;
    t->pfnGsSetConstantBuffers = u10_GsSetConstantBuffers;
    t->pfnSetRenderTargets = u10_SetRenderTargets;
    t->pfnResourceCopyRegion = u10_ResourceCopyRegion;
    t->pfnResourceUpdateSubresourceUP = u10_ResourceUpdateSubresourceUP;
    t->pfnDefaultConstantBufferUpdateSubresourceUP = u10_ResourceUpdateSubresourceUP;
    t->pfnFlush = u10_Flush;
    t->pfnCheckMultisampleQualityLevels = u10_CheckMultisampleQualityLevels;
}

/* ---- the DXGI entries the runtime asks of the driver ---- */

/* ---- DXGI: present and the rest ---- */

/* The window a program presents to is not told to the driver: DXGI has it, and shows it only to the kernel.
 * Until the display driver (wddm/README.md, phase 6) there is no kernel allocation to hand over, so a frame is
 * read back from the phone and drawn into the program's window with GDI. This is the one copy back to Windows
 * the plan lists, for windowed programs only. The window is the program's visible top-level window whose
 * client area is the size of the frame, the foreground one when there are several. */
struct FindWin {
    DWORD pid;
    UINT w, h;
    HWND best;
};

static BOOL CALLBACK find_win(HWND w, LPARAM lp)
{
    FindWin *f = reinterpret_cast<FindWin *>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    RECT rc;
    if (pid != f->pid || !IsWindowVisible(w) || !GetClientRect(w, &rc)) return TRUE;
    if ((UINT)rc.right != f->w || (UINT)rc.bottom != f->h) return TRUE;
    if (!f->best || w == GetForegroundWindow()) f->best = w;
    return TRUE;
}

static HRESULT present_surface(Dev *d, Obj *src);

/* A surface with a kernel allocation (shared, or a screen's) goes to Windows' own present: DXGI hands it on
 * to whoever shows it, such as the screen driver, and its pixels stay on the phone. */
static HRESULT kernel_present(Dev *d, Obj *src, Obj *dst, void *dxgi_context)
{
    if (!d->dxcb || !d->dxcb->pfnPresentCb) return E_FAIL;
    struct rd r;   /* the frame is drawn on the phone's GPU before anyone takes it */
    call_wait(begin(d->hdev, 0, HWD3D_OP_FINISH), &r);
    call_end(&r);
    ulog_call("kernel present");
    DXGIDDICB_PRESENT p = {};
    p.hSrcAllocation = src->kalloc;
    p.hDstAllocation = dst ? dst->kalloc : 0;
    p.pDXGIContext = dxgi_context;
    p.hContext = d->kctx;
    HRESULT hr = d->dxcb->pfnPresentCb(d->rt.handle, &p);
    if (FAILED(hr)) ulog("hwd3d_umd: kernel present of shared surface %u: 0x%08lx\n", src->share, (unsigned long)hr);
    return hr;
}

static HRESULT __stdcall u_dxgi_present(DXGI_DDI_ARG_PRESENT *a)
{
    ulog_call("dxgi Present");
    Dev *d = reinterpret_cast<Dev *>(a->hDevice);
    Obj *src = reinterpret_cast<Obj *>(a->hSurfaceToPresent);
    if (src && src->kalloc) return kernel_present(d, src, reinterpret_cast<Obj *>(a->hDstResource), a->pDXGIContext);
    return present_surface(d, src);
}

/* The same with a list of surfaces: the first one is the frame. */
static HRESULT __stdcall u_dxgi_present1(DXGI_DDI_ARG_PRESENT1 *a)
{
    ulog_call("dxgi Present1");
    if (!a->SurfacesToPresent) return S_OK;
    Dev *d = reinterpret_cast<Dev *>(a->hDevice);
    Obj *src = reinterpret_cast<Obj *>(a->phSurfacesToPresent[0].hSurface);
    if (src && src->kalloc) return kernel_present(d, src, reinterpret_cast<Obj *>(a->hDstResource), a->pDXGIContext);
    return present_surface(d, src);
}

static HRESULT present_surface(Dev *d, Obj *src)
{
    if (!d || !src || src->dim != 3) {
        ulog("hwd3d_umd: present of something that is not a 2D resource\n");
        return S_OK;
    }
    uint32_t w = src->r.width, h = src->r.height, fmt = src->r.format;
    if (fmt != DXGI_FORMAT_R8G8B8A8_UNORM && fmt != DXGI_FORMAT_B8G8R8A8_UNORM && fmt != DXGI_FORMAT_B8G8R8X8_UNORM &&
        fmt != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && fmt != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
        return S_OK;
    FindWin f = {GetCurrentProcessId(), w, h, nullptr};
    EnumWindows(find_win, reinterpret_cast<LPARAM>(&f));
    if (!f.best) {
        ulog("hwd3d_umd: present: no window of %ux%u\n", w, h);
        return S_OK;
    }
    if (!d->pstage || d->pstage->r.width != w || d->pstage->r.height != h || d->pstage->r.format != fmt) {
        if (d->pstage) {
            release_obj(d->pstage);
        } else {
            d->pstage = new Obj();
        }
        *d->pstage = Obj();
        D3D11_TEXTURE2D_DESC td = {w, h, 1, 1, (DXGI_FORMAT)fmt, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
        {   /* the description, no first data, and the object wanted */
            struct wbuf *cb = dc(d, HMX_ID3D11Device_CreateTexture2D);
            put_arr(cb, &td, 1, sizeof td);
            put32(cb, 0);
            put32(cb, 1);
            struct rd cr;
            HRESULT chr = (HRESULT)call_wait(cb, &cr);
            d->pstage->h = take32(&cr);
            call_end(&cr);
            if (FAILED(chr)) d->pstage->h = 0;
        }
        d->pstage->dim = 3;
        d->pstage->r = {3, fmt, w, h, 1, 1};
        d->pstage->subs = 1;
        d->pstage->array = 1;
        d->pstage->samples = 1;
        d->pstage->usage = 3;
    }
    if (!d->pstage->h) return S_OK;
    struct wbuf *b = cx(d, HMX_ID3D11DeviceContext_CopyResource);
    put32(b, d->pstage->h);
    put32(b, src->h);
    call_async(b);
    D3D10DDI_HDEVICE hd = {d};
    D3D10DDI_HRESOURCE hr = {d->pstage};
    D3D10DDI_MAPPED_SUBRESOURCE m = {};
    map_phone(hd, hr, 0, D3D10_DDI_MAP_READ, 0, &m);
    if (m.pData) {
        HDC dc = GetDC(f.best);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = (LONG)(m.RowPitch / 4);
        bi.bmiHeader.biHeight = -(LONG)h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        if (fmt == DXGI_FORMAT_R8G8B8A8_UNORM || fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
            uint8_t *p = static_cast<uint8_t *>(m.pData);  /* the copy is ours: swap red and blue in place */
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++) {
                    uint8_t *px = p + (size_t)y * m.RowPitch + x * 4;
                    uint8_t t = px[0];
                    px[0] = px[2];
                    px[2] = t;
                }
        }
        SetDIBitsToDevice(dc, 0, 0, w, h, 0, 0, 0, h, m.pData, &bi, DIB_RGB_COLORS);
        ReleaseDC(f.best, dc);
    }
    u_Unmap(hd, hr, 0);
    return S_OK;
}

/* A flip swaps what the back buffers are: the first takes the second's contents, and so on. */
static HRESULT __stdcall u_dxgi_rotate(DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES *a)
{
    ulog_call("dxgi Rotate");
    if (a->Resources < 2) return S_OK;
    Obj *first = reinterpret_cast<Obj *>(a->pResources[0]);
    Obj keep = *first;
    for (UINT i = 0; i + 1 < a->Resources; i++)
        *reinterpret_cast<Obj *>(a->pResources[i]) = *reinterpret_cast<Obj *>(a->pResources[i + 1]);
    *reinterpret_cast<Obj *>(a->pResources[a->Resources - 1]) = keep;
    return S_OK;
}

static HRESULT __stdcall u_dxgi_residency(DXGI_DDI_ARG_QUERYRESOURCERESIDENCY *a)
{
    for (SIZE_T i = 0; i < a->Resources; i++) a->pStatus[i] = DXGI_DDI_RESIDENCY_FULLY_RESIDENT;
    return S_OK;
}

static HRESULT __stdcall u_dxgi_priority(DXGI_DDI_ARG_SETRESOURCEPRIORITY *) { return S_OK; }

/* Makes a shared surface's contents whole before another program reads it (multisample resolve and the
 * like). The surfaces here are single-sample and live on the phone, where the other program's device reads
 * the same buffer; the drawing program flushed before presenting. Nothing to do; an error here makes the
 * runtime drop the device (the screen driver saw DXGI_ERROR_DRIVER_INTERNAL_ERROR after its first frame). */
static HRESULT __stdcall u_dxgi_resolve_shared(DXGI_DDI_ARG_RESOLVESHAREDRESOURCE *) { return S_OK; }

/* The DXGI entries are all one shape, an answer and one pointer to arguments. Each table is the entries of
 * the one before it and more, so the table the runtime gave is filled to its size. */
template <int N> static HRESULT __stdcall dxgi_stub(void *)
{
    static const char *const names[] = {"Present", "GetGammaCaps", "SetDisplayMode", "SetResourcePriority", "QueryResourceResidency",
                                        "RotateResourceIdentities", "Blt", "ResolveSharedResource", "8", "9", "10", "11", "12", "13", "14", "15", "16", "17", "18", "19", "20", "21", "22", "23", "24", "25", "26", "27", "28", "29", "30", "31"};
    ulog("hwd3d_umd: DXGI entry %s is not done yet\n", names[N]);
    return E_NOTIMPL;
}
#define DXGI_STUBS \
    X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15) X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31)

static void fill_dxgi(DXGI_DDI_BASE_ARGS *a, UINT interface_version)
{
    size_t n = sizeof(DXGI1_1_DDI_BASE_FUNCTIONS);
    if (interface_version >= D3DWDDM1_3_DDI_INTERFACE_VERSION) n = sizeof(DXGI1_3_DDI_BASE_FUNCTIONS);
    void **f = reinterpret_cast<void **>(a->pDXGIDDIBaseFunctions2);
    static void *const stubs[] = {
#define X(i) reinterpret_cast<void *>(dxgi_stub<i>),
        DXGI_STUBS
#undef X
    };
    for (size_t i = 0; i < n / sizeof(void *) && i < sizeof stubs / sizeof *stubs; i++) f[i] = stubs[i];
    DXGI1_1_DDI_BASE_FUNCTIONS *t1 = a->pDXGIDDIBaseFunctions2;  /* the shared first entries */
    t1->pfnPresent = u_dxgi_present;
    t1->pfnRotateResourceIdentities = u_dxgi_rotate;
    t1->pfnQueryResourceResidency = u_dxgi_residency;
    t1->pfnSetResourcePriority = u_dxgi_priority;
    t1->pfnResolveSharedResource = u_dxgi_resolve_shared;
    if (interface_version >= D3DWDDM1_3_DDI_INTERFACE_VERSION) a->pDXGIDDIBaseFunctions4->pfnPresent1 = u_dxgi_present1;
}

/* ---- the adapter ---- */

static SIZE_T APIENTRY a_CalcPrivateDeviceSize(D3D10DDI_HADAPTER, const D3D10DDIARG_CALCPRIVATEDEVICESIZE *a)
{
    ulog("hwd3d_umd: CalcPrivateDeviceSize %p\n", (const void *)a);
    ulog("hwd3d_umd: interface 0x%x, version %u, flags 0x%x\n", a->Interface, a->Version, a->Flags);
    return sizeof(Dev);
}

/* Until this adapter shows the desktop (wddm/README.md, phase 6), Windows' own programs (the shell, DWM,
 * search, the start menu) are left to the software renderer as before. Each device here holds one of
 * twenty channels to the phone, and there are dozens of them. A program in Windows' folder is one. */
/* Windows' own programs (under %SystemRoot%) get Windows' software renderer: each device here costs a
 * channel and memory on the phone, and Explorer, the Start menu and the like would hold one each.
 * HWD3D_SYSTEM lets some in: "1" for all, or exe names separated by commas ("dwm.exe,WUDFHost.exe"). */
static bool windows_own_program()
{
    WCHAR exe[MAX_PATH], win[MAX_PATH];
    UINT n = GetWindowsDirectoryW(win, MAX_PATH);
    DWORD m = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (!(n && m > n && !_wcsnicmp(exe, win, n))) return false;
    char allow[256];
    if (!setting("HWD3D_SYSTEM", allow, sizeof allow)) return true;
    if (!strcmp(allow, "1")) return false;
    const WCHAR *base = exe + m;
    while (base > exe && base[-1] != L'\\') base--;
    char name[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, base, -1, name, sizeof name, NULL, NULL);
    for (char *tok = strtok(allow, ","); tok; tok = strtok(NULL, ","))
        if (!_stricmp(tok, name)) return false;
    return true;
}

/* Sub-objects are the video functions; the driver has none, and the runtime calls this to find out. */
static HRESULT APIENTRY u_RetrieveSubObject(D3D10DDI_HDEVICE, UINT32, SIZE_T, void *, SIZE_T, void *) { return E_NOTIMPL; }

static HRESULT APIENTRY a_CreateDevice(D3D10DDI_HADAPTER, D3D10DDIARG_CREATEDEVICE *a)
{
    ulog_call("RetrieveSubObject");
    ulog("hwd3d_umd: CreateDevice, interface 0x%x, version %u, flags 0x%x\n", a->Interface, a->Version, a->Flags);
    if (a->Interface != D3DWDDM1_3_DDI_INTERFACE_VERSION && a->Interface != D3D10_1_DDI_INTERFACE_VERSION) {
        ulog("hwd3d_umd: no table for that interface\n");
        return E_FAIL;
    }
    if (windows_own_program()) {
        ulog("hwd3d_umd: a program of Windows' own is left to the software renderer\n");
        return E_FAIL;
    }
    if (!ready()) {
        ulog("hwd3d_umd: the phone has no HwD3D for this process\n");
        return E_FAIL;
    }
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
    if (a->Interface == D3D10_1_DDI_INTERFACE_VERSION) fill_device_funcs_10_1(a->p10_1DeviceFuncs);
    else fill_device_funcs(a->pWDDM1_3DeviceFuncs);
    if (a->ppfnRetrieveSubObject) *a->ppfnRetrieveSubObject = u_RetrieveSubObject;
    {   /* a table entry left empty is a call to zero when the runtime uses it */
        void **t = reinterpret_cast<void **>(a->pWDDM1_3DeviceFuncs);
        size_t n = a->Interface == D3D10_1_DDI_INTERFACE_VERSION ? sizeof(D3D10_1DDI_DEVICEFUNCS) : sizeof(D3DWDDM1_3DDI_DEVICEFUNCS);
        for (size_t i = 0; i < n / sizeof(void *); i++)
            if (!t[i]) ulog("hwd3d_umd: device table entry %u is empty\n", (unsigned)i);
    }
    fill_dxgi(&a->DXGIBaseDDI, a->Interface);
    d->dxcb = a->DXGIBaseDDI.pDXGIBaseCallbacks;
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
    /* Windows 11's d3d10core sits on the same runtime as d3d11 and takes this version too (d3d10test). The 10.1 table
     * below is written and builds, but no runtime asks for it, so it is not offered: add D3D10_1_DDI_SUPPORTED here to try it. */
    if (v && *n >= 1) v[0] = D3DWDDM1_3_DDI_SUPPORTED;
    *n = 1;
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
    switch (c->Type) {    case D3D11DDICAPS_SHADER:
        /* Compute and raw or structured buffers on every level: without this word the runtime turns the
         * adapter away when it claims 11.0. */
        if (c->DataSize >= 4) *static_cast<UINT *>(c->pData) = D3D11DDICAPS_SHADER_COMPUTE_PLUS_RAW_AND_STRUCTURED_BUFFERS_IN_SHADER_4_X;
        break;
    case D3D11DDICAPS_THREADING:
        /* Calls from several threads are safe: the channel takes one at a time. No command lists: the
         * runtime records deferred contexts itself and replays them on this one. */
        if (c->DataSize >= 4) *static_cast<UINT *>(c->pData) = D3D11DDICAPS_FREETHREADED;
        break;
    case D3D11DDICAPS_3DPIPELINESUPPORT:
        /* Levels 10.0, 10.1 and 11.0, which the phone's DXVK gives. 11.0 needs the shader caps below. */
        if (c->DataSize >= 4) *static_cast<UINT *>(c->pData) = D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(D3D11DDI_3DPIPELINELEVEL_10_0) |
                                                                 D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(D3D11DDI_3DPIPELINELEVEL_10_1) |
                                                                 D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(D3D11DDI_3DPIPELINELEVEL_11_0);
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

/* With HWD3D_UMDLOG set, a crash of the program leaves where it was in the log: the module and offset. */
static LONG WINAPI on_crash(EXCEPTION_POINTERS *p)
{
    HMODULE m = NULL;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)p->ExceptionRecord->ExceptionAddress, &m))
        GetModuleFileNameA(m, name, sizeof name);
    ulog("hwd3d_umd: crash 0x%08lx at %p (%s +0x%llx)\n", (unsigned long)p->ExceptionRecord->ExceptionCode, p->ExceptionRecord->ExceptionAddress, name,
         (unsigned long long)((uintptr_t)p->ExceptionRecord->ExceptionAddress - (uintptr_t)m));
#ifdef _M_ARM64
    {
        HMODULE lm = NULL;
        char lname[MAX_PATH] = "?";
        void *lr = (void *)p->ContextRecord->Lr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)lr, &lm)) GetModuleFileNameA(lm, lname, sizeof lname);
        ulog("hwd3d_umd:   called from %p (%s +0x%llx)\n", lr, lname, (unsigned long long)((uintptr_t)lr - (uintptr_t)lm));
        uintptr_t *fp = (uintptr_t *)p->ContextRecord->Fp;
        for (int i = 0; i < 8 && fp && !IsBadReadPtr(fp, 16); i++) {
            void *ra = (void *)fp[1];
            HMODULE rm = NULL;
            char rn[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ra, &rm)) GetModuleFileNameA(rm, rn, sizeof rn);
            ulog("hwd3d_umd:   frame %p (%s +0x%llx)\n", ra, rn, (unsigned long long)((uintptr_t)ra - (uintptr_t)rm));
            uintptr_t *next = (uintptr_t *)fp[0];
            if (next <= fp) break;
            fp = next;
        }
        ulog("hwd3d_umd:   x0 %llx x1 %llx x2 %llx x16 %llx x17 %llx\n", (unsigned long long)p->ContextRecord->X0, (unsigned long long)p->ContextRecord->X1,
             (unsigned long long)p->ContextRecord->X2, (unsigned long long)p->ContextRecord->X16, (unsigned long long)p->ContextRecord->X17);
    }
#endif
    return EXCEPTION_CONTINUE_SEARCH;
}

/* At exit the program's own submit counts go to the log (HWD3D_UMDLOG), for the baseline in wddm/README.md. */
extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, void *)
{
    char v[8];
    if (reason == DLL_PROCESS_ATTACH && log_path(v, sizeof v)) SetUnhandledExceptionFilter(on_crash);
    if (reason == DLL_PROCESS_DETACH && g_chan) {
        const struct hwgpu_ctl *c = hwgpu_chan_ctl(g_chan);
        ulog("hwd3d_umd: submits to the phone %llu, to Windows %llu\n", (unsigned long long)c->submits_to_phone,
             (unsigned long long)c->submits_to_guest);
    }
    return TRUE;
}
