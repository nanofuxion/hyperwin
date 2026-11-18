/* HwD3D's d3d11.dll (d3d/README.md): Direct3D 11 and DXGI as thin COM objects. Each one holds a
 * handle to the real object, which lives in DXVK on the phone (gpu/host/hwd3d). A call goes over a
 * GPU channel (gpu/include/hwgpu_ring.h) as include/hw_d3d.h describes; the generated thunks
 * (gpu/host/hwd3d/gen_thunks.py) write the arguments, and this file has the rest: the channel, the
 * objects, the exports, and the methods written by hand.
 *
 * dxgi.dll forwards its exports here, so one process has one channel and one set of objects. With
 * no HwD3D on the phone, the exports hand over to Windows' own d3d11.dll and dxgi.dll. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wire.h"

/* ---- objects ---- */

struct res_info;
struct shadow;

struct shell {
    const void *const *lpVtbl;
    volatile LONG ref;
    uint32_t handle;
    uint16_t iface;
    struct shell *next_free;
    struct res_info *res;   /* a resource's description, when one was needed */
    struct shadow *shadows; /* mapped memory by subresource */
    HWND hwnd;              /* a swap chain's window */
    int qslot;              /* a query's slot in the query page, -1 for none yet */
    uint32_t qseq;          /* its last End */
    int qflushed;           /* a flush went for that End */
};

#include "hwd3d_guest_pre.inc"

static void unsupported(const char *what)
{
    /* Once per method: a program that loops on one fills no log. */
    static const char *seen[64];
    static LONG nseen;
    for (LONG i = 0; i < nseen && i < 64; i++)
        if (seen[i] == what) return;
    LONG i = InterlockedIncrement(&nseen) - 1;
    if (i < 64) seen[i] = what;
    hlog("hwd3d: %s is not done yet\n", what);
}

static struct shell **g_shells;  /* by handle index */
static uint32_t g_nshells;
static struct shell *g_free;

#define HANDLE_INDEX(h) ((h) & 0xFFFFFu)

static int is_shell(const void *p)
{
    if (!p) return 0;
    const void *vt = *(const void *const *)p;
    for (int i = 0; i < HI_COUNT; i++)
        if (vt == hi_vtbl[i]) return 1;
    return 0;
}

static uint32_t handle_of(const void *p) { return p ? ((const struct shell *)p)->handle : 0; }

static int put_obj(struct wbuf *b, const void *p)
{
    if (p && !is_shell(p)) {
        hlog("hwd3d: an object that is not ours was passed in\n");
        return 0;
    }
    put32(b, handle_of(p));
    return 1;
}

static int put_objs(struct wbuf *b, void *const *pp, uint64_t n)
{
    if (!pp || n > 4096) {
        put32(b, 0);
        return 1;
    }
    for (uint64_t i = 0; i < n; i++)
        if (pp[i] && !is_shell(pp[i])) return 0;
    put32(b, 1);
    put32(b, (uint32_t)n);
    for (uint64_t i = 0; i < n; i++) put32(b, handle_of(pp[i]));
    return 1;
}

/* The shell for a handle the phone just gave out, with a reference for the caller. Under g_lock. */
static struct shell *shell_for(uint32_t handle, int iface)
{
    if (!handle || iface < 0 || iface >= HI_COUNT) return NULL;
    uint32_t i = HANDLE_INDEX(handle);
    if (i >= g_nshells) {
        uint32_t n = g_nshells ? g_nshells : 256;
        while (n <= i) n *= 2;
        g_shells = realloc(g_shells, n * sizeof *g_shells);
        memset(g_shells + g_nshells, 0, (n - g_nshells) * sizeof *g_shells);
        g_nshells = n;
    }
    struct shell *s = g_shells[i];
    if (s && s->handle == handle) {
        InterlockedIncrement(&s->ref);
        return s;
    }
    /* Shells are never freed, only reused: a late Release on one stays harmless. */
    if (g_free) {
        s = g_free;
        g_free = s->next_free;
    } else {
        s = calloc(1, sizeof *s);
    }
    s->lpVtbl = hi_vtbl[iface];
    s->ref = 1;
    s->handle = handle;
    s->iface = (uint16_t)iface;
    s->next_free = NULL;
    s->res = NULL;
    s->shadows = NULL;
    s->hwnd = NULL;
    s->qslot = -1;
    s->qseq = 0;
    s->qflushed = 0;
    g_shells[i] = s;
    return s;
}

static struct wbuf *call_begin(void *This, uint32_t method)
{
    return begin(((struct shell *)This)->handle, method, HWD3D_OP_CALL);
}

static void take_obj(struct rd *r, void **pp, int iface)
{
    uint32_t h = take32(r);
    if (pp) *pp = shell_for(h, iface);
}

static void take_objs(struct rd *r, void **pp, uint64_t n, int iface)
{
    if (!pp) return;
    for (uint64_t i = 0; i < n; i++) pp[i] = shell_for(take32(r), iface);
}

static void take_obj_riid(struct rd *r, void **pp, REFIID riid)
{
    (void)riid;
    uint32_t h = take32(r);
    int iface = (int)take32(r);
    if (pp) *pp = shell_for(h, iface);
}

static void free_shadows(struct shell *s);

static ULONG STDMETHODCALLTYPE m_IUnknown_AddRef(void *This)
{
    return (ULONG)InterlockedIncrement(&((struct shell *)This)->ref);
}

static ULONG STDMETHODCALLTYPE m_IUnknown_Release(void *This)
{
    struct shell *s = This;
    LONG n = InterlockedDecrement(&s->ref);
    if (n) return (ULONG)n;
    AcquireSRWLockExclusive(&g_lock);
    uint32_t i = HANDLE_INDEX(s->handle);
    /* Another thread may have been handed this object again meanwhile. */
    if (s->ref == 0 && s->handle && i < g_nshells && g_shells[i] == s) {
        g_shells[i] = NULL;
        struct hwd3d_call c = {{sizeof c, HWD3D_MAGIC, HWD3D_OP_RELEASE, HWD3D_NO_REPLY, ++g_seq}, s->handle, 0};
        raw_send(&c, sizeof c);
        free_shadows(s);
        free(s->res);
        s->res = NULL;
        if (s->qslot >= 0) g_qused[s->qslot] = 0;
        s->qslot = -1;
        s->handle = 0;
        s->next_free = g_free;
        g_free = s;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return 0;
}

static int iface_index(REFIID riid)
{
    for (int i = 0; i < HI_COUNT; i++)
        if (IsEqualGUID(riid, &hi_iid[i])) return i;
    return -1;
}

static HRESULT STDMETHODCALLTYPE m_IUnknown_QueryInterface(void *This, REFIID riid, void **out)
{
    struct shell *s = This;
    if (!out) return E_POINTER;
    *out = NULL;
    /* An interface this one derives from is this object. IUnknown asks the phone, so that every
     * interface of one object answers with the same IUnknown. */
    for (int i = s->iface; i > 0; i = hi_parent[i])
        if (IsEqualGUID(riid, &hi_iid[i])) {
            InterlockedIncrement(&s->ref);
            *out = s;
            return S_OK;
        }
    int want = iface_index(riid);
    if (want < 0) return E_NOINTERFACE;
    struct wbuf *b = begin(s->handle, HM_IUnknown_QueryInterface, HWD3D_OP_CALL);
    put32(b, (uint32_t)want);
    struct rd r;
    HRESULT hr = call_wait(b, &r);
    uint32_t h = take32(&r);
    if (SUCCEEDED(hr)) *out = shell_for(h, want);
    call_end(&r);
    return *out ? hr : (FAILED(hr) ? hr : E_NOINTERFACE);
}

/* ---- resources ---- */

struct res_info {
    struct hwd3d_res r;
    uint32_t subs;  /* subresources: mips times array layers */
};

/* What a resource is, asked of the phone once. */
static struct res_info *res_info(struct shell *s)
{
    if (s->res) return s->res;
    struct res_info *ri = calloc(1, sizeof *ri);
    ID3D11Resource *res = (ID3D11Resource *)s;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    ID3D11Resource_GetType(res, &dim);
    struct hwd3d_res *r = &ri->r;
    r->dim = dim;
    r->depth = r->mips = ri->subs = 1;
    switch (dim) {
    case D3D11_RESOURCE_DIMENSION_BUFFER: {
        D3D11_BUFFER_DESC d;
        ID3D11Buffer_GetDesc((ID3D11Buffer *)res, &d);
        r->width = d.ByteWidth;
        r->height = 1;
        break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
        D3D11_TEXTURE1D_DESC d;
        ID3D11Texture1D_GetDesc((ID3D11Texture1D *)res, &d);
        *r = (struct hwd3d_res){dim, d.Format, d.Width, 1, 1, d.MipLevels};
        ri->subs = d.MipLevels * d.ArraySize;
        break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
        D3D11_TEXTURE2D_DESC d;
        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &d);
        *r = (struct hwd3d_res){dim, d.Format, d.Width, d.Height, 1, d.MipLevels};
        ri->subs = d.MipLevels * d.ArraySize;
        break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
        D3D11_TEXTURE3D_DESC d;
        ID3D11Texture3D_GetDesc((ID3D11Texture3D *)res, &d);
        *r = (struct hwd3d_res){dim, d.Format, d.Width, d.Height, d.Depth, d.MipLevels};
        ri->subs = d.MipLevels;
        break;
    }
    default:
        break;
    }
    s->res = ri;
    return ri;
}

/* ---- methods written by hand ---- */

/* Create* with first contents: the description, then each subresource's rows. */
static HRESULT create_with_data(void *This, uint32_t method, const void *desc, uint32_t desc_size,
                                const D3D11_SUBRESOURCE_DATA *init, struct hwd3d_res r, uint32_t layers, void **out,
                                int iface)
{
    if (out) *out = NULL;
    if (!desc) return E_INVALIDARG;
    if (!r.mips) {
        uint32_t big = r.width > r.height ? r.width : r.height;
        if (r.depth > big) big = r.depth;
        for (r.mips = 1; big > 1; big >>= 1) r.mips++;
    }
    uint32_t subs = r.mips * layers;
    struct wbuf *b = call_begin(This, method);
    put_arr(b, desc, 1, desc_size);
    put32(b, init ? subs : 0);
    for (uint32_t i = 0; init && i < subs; i++) {
        uint32_t row, rows, slices;
        if (!init[i].pSysMem || !hwd3d_region(&r, i, NULL, &row, &rows, &slices)) {
            call_abort(b);
            return E_INVALIDARG;
        }
        put_rows(b, init[i].pSysMem, row, rows, slices, r.dim == 1 ? row : init[i].SysMemPitch, init[i].SysMemSlicePitch);
    }
    put_want(b, out);
    struct rd rd;
    HRESULT hr = call_wait(b, &rd);
    take_obj(&rd, out, iface);
    call_end(&rd);
    return hr;
}

static HRESULT STDMETHODCALLTYPE m_ID3D11Device_CreateBuffer(void *This, const D3D11_BUFFER_DESC *d,
                                                              const D3D11_SUBRESOURCE_DATA *init, ID3D11Buffer **out)
{
    struct hwd3d_res r = {1, 0, d ? d->ByteWidth : 0, 1, 1, 1};
    return create_with_data(This, HM_ID3D11Device_CreateBuffer, d, sizeof *d, init, r, 1, (void **)out, HI_ID3D11Buffer);
}

static HRESULT STDMETHODCALLTYPE m_ID3D11Device_CreateTexture1D(void *This, const D3D11_TEXTURE1D_DESC *d,
                                                                 const D3D11_SUBRESOURCE_DATA *init, ID3D11Texture1D **out)
{
    struct hwd3d_res r = {2, d ? d->Format : 0, d ? d->Width : 0, 1, 1, d ? d->MipLevels : 0};
    return create_with_data(This, HM_ID3D11Device_CreateTexture1D, d, sizeof *d, init, r, d ? d->ArraySize : 0,
                            (void **)out, HI_ID3D11Texture1D);
}

static HRESULT STDMETHODCALLTYPE m_ID3D11Device_CreateTexture2D(void *This, const D3D11_TEXTURE2D_DESC *d,
                                                                 const D3D11_SUBRESOURCE_DATA *init, ID3D11Texture2D **out)
{
    struct hwd3d_res r = {3, d ? d->Format : 0, d ? d->Width : 0, d ? d->Height : 0, 1, d ? d->MipLevels : 0};
    return create_with_data(This, HM_ID3D11Device_CreateTexture2D, d, sizeof *d, init, r, d ? d->ArraySize : 0,
                            (void **)out, HI_ID3D11Texture2D);
}

static HRESULT STDMETHODCALLTYPE m_ID3D11Device_CreateTexture3D(void *This, const D3D11_TEXTURE3D_DESC *d,
                                                                 const D3D11_SUBRESOURCE_DATA *init, ID3D11Texture3D **out)
{
    struct hwd3d_res r = {4, d ? d->Format : 0, d ? d->Width : 0, d ? d->Height : 0, d ? d->Depth : 0, d ? d->MipLevels : 0};
    return create_with_data(This, HM_ID3D11Device_CreateTexture3D, d, sizeof *d, init, r, 1, (void **)out, HI_ID3D11Texture3D);
}

static HRESULT STDMETHODCALLTYPE m_ID3D11Device_CreateInputLayout(void *This, const D3D11_INPUT_ELEMENT_DESC *el, UINT n,
                                                                   const void *code, SIZE_T len, ID3D11InputLayout **out)
{
    if (out) *out = NULL;
    if ((!el && n) || n > D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT) return E_INVALIDARG;
    struct wbuf *b = begin(((struct shell *)This)->handle, HM_ID3D11Device_CreateInputLayout, HWD3D_OP_CALL);
    put32(b, n);
    for (UINT i = 0; i < n; i++) {
        const char *name = el[i].SemanticName ? el[i].SemanticName : "";
        put_arr(b, name, strlen(name) + 1, 1);
        uint32_t f[6] = {el[i].SemanticIndex, el[i].Format, el[i].InputSlot, el[i].AlignedByteOffset,
                         el[i].InputSlotClass, el[i].InstanceDataStepRate};
        put(b, f, sizeof f);
    }
    put_arr(b, code, len, 1);
    put_want(b, out);
    struct rd r;
    HRESULT hr = call_wait(b, &r);
    take_obj(&r, (void **)out, HI_ID3D11InputLayout);
    call_end(&r);
    return hr;
}

static void STDMETHODCALLTYPE m_ID3D11DeviceContext_UpdateSubresource(void *This, ID3D11Resource *dst, UINT sub,
                                                                       const D3D11_BOX *box, const void *src, UINT pitch,
                                                                       UINT slice_pitch)
{
    if (!dst || !src || !is_shell(dst)) return;
    struct res_info *ri = res_info((struct shell *)dst);
    uint32_t row, rows, slices;
    if (!hwd3d_region(&ri->r, sub, (const uint32_t *)box, &row, &rows, &slices)) {
        unsupported("ID3D11DeviceContext::UpdateSubresource of this format or box");
        return;
    }
    struct wbuf *b = call_begin(This, HM_ID3D11DeviceContext_UpdateSubresource);
    put32(b, handle_of(dst));
    put32(b, sub);
    put_arr(b, box, 1, sizeof *box);
    put_rows(b, src, row, rows, slices, ri->r.dim == 1 ? row : pitch, slice_pitch);
    call_async(b);
}

/* Map: phase 2 keeps a copy of the subresource here. The phone sends its contents when the map
 * reads them; Unmap sends them back when the map writes. include/hw_d3d.h phase 3 replaces the copy
 * with memory both sides see. */
struct shadow {
    struct shadow *next;
    UINT sub;
    D3D11_MAP type;
    uint32_t size, row_pitch, depth_pitch;
    int local;  /* answered without the phone: Unmap does the map */
    uint8_t *mem;
};

static void free_shadows(struct shell *s)
{
    while (s->shadows) {
        struct shadow *m = s->shadows;
        s->shadows = m->next;
        free(m->mem);
        free(m);
    }
}

static HRESULT STDMETHODCALLTYPE m_ID3D11DeviceContext_Map(void *This, ID3D11Resource *res, UINT sub, D3D11_MAP type,
                                                           UINT flags, D3D11_MAPPED_SUBRESOURCE *out)
{
    if (out) memset(out, 0, sizeof *out);
    if (!res || !is_shell(res)) return E_INVALIDARG;
    struct shell *s = (struct shell *)res;
    if (type == D3D11_MAP_WRITE_DISCARD || type == D3D11_MAP_WRITE_NO_OVERWRITE) {
        /* Answered here: the program writes into a copy kept beside the resource, and Unmap sends
         * it through the staging area. No wait. Dynamic resources are written only this way, so
         * the copy is always what the resource holds. */
        struct res_info *ri = res_info(s);
        uint32_t row, rows, slices;
        if (!hwd3d_region(&ri->r, sub, NULL, &row, &rows, &slices)) return E_INVALIDARG;
        uint32_t size = row * rows * slices;
        AcquireSRWLockExclusive(&g_lock);
        struct shadow *m = s->shadows;
        while (m && m->sub != sub) m = m->next;
        if (!m) {
            m = calloc(1, sizeof *m);
            m->sub = sub;
            m->next = s->shadows;
            s->shadows = m;
        }
        if (m->size != size) {
            free(m->mem);
            m->mem = calloc(1, size ? size : 1);
            m->size = size;
        }
        m->type = type;
        m->row_pitch = row;
        m->depth_pitch = row * rows;
        m->local = 1;
        ReleaseSRWLockExclusive(&g_lock);
        if (out) {
            out->pData = m->mem;
            out->RowPitch = row;
            out->DepthPitch = row * rows;
        }
        return S_OK;
    }
    struct wbuf *b = begin(((struct shell *)This)->handle, HM_ID3D11DeviceContext_Map, HWD3D_OP_CALL);
    put32(b, s->handle);
    put32(b, sub);
    put32(b, type);
    put32(b, flags);
    struct rd r;
    HRESULT hr = call_wait(b, &r);
    uint32_t row_pitch = take32(&r), depth_pitch = take32(&r), size = take32(&r), has = take32(&r);
    if (FAILED(hr)) {
        call_end(&r);
        return hr;
    }
    struct shadow *m = s->shadows;
    while (m && m->sub != sub) m = m->next;
    if (!m) {
        m = calloc(1, sizeof *m);
        m->sub = sub;
        m->next = s->shadows;
        s->shadows = m;
    }
    if (m->size != size) {
        free(m->mem);
        m->mem = calloc(1, size ? size : 1);
        m->size = size;
    }
    m->type = type;
    m->row_pitch = row_pitch;
    m->depth_pitch = depth_pitch;
    m->local = 0;
    if (has) take(&r, m->mem, size);
    call_end(&r);
    if (out) {
        out->pData = m->mem;
        out->RowPitch = row_pitch;
        out->DepthPitch = depth_pitch;
    }
    return hr;
}

static void STDMETHODCALLTYPE m_ID3D11DeviceContext_Unmap(void *This, ID3D11Resource *res, UINT sub)
{
    if (!res || !is_shell(res)) return;
    struct shell *s = (struct shell *)res;
    struct wbuf *b = begin(((struct shell *)This)->handle, HM_ID3D11DeviceContext_Unmap, HWD3D_OP_CALL);
    struct shadow *m = s->shadows;
    while (m && m->sub != sub) m = m->next;
    put32(b, s->handle);
    put32(b, sub);
    put32(b, m && m->local);
    if (m && m->local) {
        put32(b, m->type);
        put32(b, m->row_pitch);
        put32(b, m->row_pitch ? m->depth_pitch / m->row_pitch : 0);
        put_arr(b, m->mem, m->size, 1);
        m->local = 0;
    } else if (m && m->type != D3D11_MAP_READ) {
        put_arr(b, m->mem, m->size, 1);
    } else {
        put32(b, 0);
    }
    call_async(b);
}

static HRESULT STDMETHODCALLTYPE m_IDXGIFactory_CreateSwapChain(void *This, IUnknown *device, DXGI_SWAP_CHAIN_DESC *desc,
                                                                IDXGISwapChain **out)
{
    if (out) *out = NULL;
    if (!desc || !out) return DXGI_ERROR_INVALID_CALL;
    if (!is_shell(device)) return DXGI_ERROR_INVALID_CALL;
    DXGI_SWAP_CHAIN_DESC d = *desc;
    if (!d.BufferDesc.Width || !d.BufferDesc.Height) {
        RECT rc = {0};
        GetClientRect(d.OutputWindow, &rc);
        if (!d.BufferDesc.Width) d.BufferDesc.Width = rc.right > 0 ? (UINT)rc.right : 8;
        if (!d.BufferDesc.Height) d.BufferDesc.Height = rc.bottom > 0 ? (UINT)rc.bottom : 8;
    }
    d.OutputWindow = NULL;  /* the phone draws on a surface of its own */
    struct wbuf *b = begin(((struct shell *)This)->handle, HM_IDXGIFactory_CreateSwapChain, HWD3D_OP_CALL);
    put32(b, handle_of(device));
    put_arr(b, &d, 1, sizeof d);
    struct rd r;
    HRESULT hr = call_wait(b, &r);
    take_obj(&r, (void **)out, HI_IDXGISwapChain);
    call_end(&r);
    if (*out) ((struct shell *)*out)->hwnd = desc->OutputWindow;
    return hr;
}

/* Queries: End gives the query a slot in the query page, and GetData reads it there. Only a
 * query whose result has not come yet costs a message: one flush, as Windows' GetData does. */
static void STDMETHODCALLTYPE m_ID3D11DeviceContext_End(void *This, ID3D11Asynchronous *q)
{
    if (!q || !is_shell(q)) return;
    struct shell *s = (struct shell *)q;
    struct wbuf *b = call_begin(This, HM_ID3D11DeviceContext_End);
    if (g_qpage && s->qslot < 0)
        for (int i = 0; i < (int)HWD3D_QUERY_SLOTS; i++)
            if (!g_qused[i]) {
                g_qused[i] = 1;
                s->qslot = i;
                break;
            }
    if (s->qslot >= 0) {
        s->qseq++;
        if (!s->qseq) s->qseq = 1;
        s->qflushed = 0;
    }
    put32(b, s->handle);
    put32(b, s->qslot >= 0 ? (uint32_t)s->qslot : 0);
    put32(b, s->qslot >= 0 ? s->qseq : 0);
    call_async(b);
}

static HRESULT STDMETHODCALLTYPE m_ID3D11DeviceContext_GetData(void *This, ID3D11Asynchronous *q, void *data, UINT size,
                                                               UINT flags)
{
    if (!q || !is_shell(q)) return E_INVALIDARG;
    struct shell *s = (struct shell *)q;
    if (s->qslot >= 0 && s->qseq) {
        volatile struct hwd3d_query_slot *slot = &g_qpage[s->qslot];
        if (__atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE) == s->qseq) {
            uint32_t n = slot->size < size ? slot->size : size;
            if (data && n) memcpy(data, (const void *)slot->data, n);
            return S_OK;
        }
        if (!(flags & D3D11_ASYNC_GETDATA_DONOTFLUSH) && !s->qflushed) {
            s->qflushed = 1;
            ID3D11DeviceContext_Flush((ID3D11DeviceContext *)This);
        }
        return S_FALSE;
    }
    struct wbuf *b = call_begin(This, HM_ID3D11DeviceContext_GetData);
    put32(b, s->handle);
    put32(b, data ? size : 0);
    put32(b, flags);
    struct rd r;
    HRESULT hr = call_wait(b, &r);
    take(&r, data, data ? size : 0);
    call_end(&r);
    return hr;
}

static HRESULT STDMETHODCALLTYPE m_IDXGISwapChain_Present(void *This, UINT sync, UINT flags)
{
    struct wbuf *b = call_begin(This, HM_IDXGISwapChain_Present);
    put32(b, sync);
    put32(b, flags);
    struct rd r;
    HRESULT hr = call_wait(b, &r);
    call_end(&r);
    return hr;
}

static HRESULT STDMETHODCALLTYPE m_IDXGISwapChain_GetDesc(void *This, DXGI_SWAP_CHAIN_DESC *desc)
{
    if (!desc) return DXGI_ERROR_INVALID_CALL;
    struct wbuf *b = begin(((struct shell *)This)->handle, HM_IDXGISwapChain_GetDesc, HWD3D_OP_CALL);
    struct rd r;
    HRESULT hr = call_wait(b, &r);
    take(&r, desc, sizeof *desc);
    call_end(&r);
    desc->OutputWindow = ((struct shell *)This)->hwnd;
    return hr;
}

/* The phone's adapter description, in Windows' layout (its WCHAR is four bytes). */
static HRESULT adapter_desc(void *This, uint32_t method, void *desc, uint32_t size)
{
    if (!desc) return E_INVALIDARG;
    struct wbuf *b = begin(((struct shell *)This)->handle, method, HWD3D_OP_CALL);
    struct rd r;
    HRESULT hr = call_wait(b, &r);
    take(&r, desc, size);
    call_end(&r);
    return hr;
}

static HRESULT STDMETHODCALLTYPE m_IDXGIAdapter_GetDesc(void *This, DXGI_ADAPTER_DESC *desc)
{
    return adapter_desc(This, HM_IDXGIAdapter_GetDesc, desc, sizeof *desc);
}

static HRESULT STDMETHODCALLTYPE m_IDXGIAdapter1_GetDesc1(void *This, DXGI_ADAPTER_DESC1 *desc)
{
    return adapter_desc(This, HM_IDXGIAdapter1_GetDesc1, desc, sizeof *desc);
}

/* Outputs are Windows' own screen: its size and modes, not the phone surface DXVK draws on. */
static HRESULT STDMETHODCALLTYPE m_IDXGIOutput_GetDesc(void *This, DXGI_OUTPUT_DESC *desc)
{
    (void)This;
    if (!desc) return E_INVALIDARG;
    memset(desc, 0, sizeof *desc);
    POINT zero = {0, 0};
    HMONITOR mon = MonitorFromPoint(zero, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi = {0};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(mon, (MONITORINFO *)&mi);
    memcpy(desc->DeviceName, mi.szDevice, sizeof desc->DeviceName);
    desc->DesktopCoordinates = mi.rcMonitor;
    desc->AttachedToDesktop = TRUE;
    desc->Rotation = DXGI_MODE_ROTATION_IDENTITY;
    desc->Monitor = mon;
    return S_OK;
}

static int screen_modes(DXGI_FORMAT f, DXGI_MODE_DESC *out, UINT max)
{
    static const uint32_t sizes[][2] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 720}, {1280, 800},
                                        {1366, 768}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
    POINT zero = {0, 0};
    MONITORINFO mi = {0};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromPoint(zero, MONITOR_DEFAULTTOPRIMARY), &mi);
    uint32_t w = (uint32_t)(mi.rcMonitor.right - mi.rcMonitor.left), h = (uint32_t)(mi.rcMonitor.bottom - mi.rcMonitor.top);
    DEVMODEW dm = {0};
    dm.dmSize = sizeof dm;
    uint32_t hz = EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1 ? dm.dmDisplayFrequency : 60;
    int n = 0;
    for (size_t i = 0; i <= sizeof sizes / sizeof *sizes; i++) {
        uint32_t mw = i < sizeof sizes / sizeof *sizes ? sizes[i][0] : w, mh = i < sizeof sizes / sizeof *sizes ? sizes[i][1] : h;
        if (mw > w || mh > h || (i < sizeof sizes / sizeof *sizes && mw == w && mh == h)) continue;
        if (out && (UINT)n < max) {
            DXGI_MODE_DESC m = {mw, mh, {hz, 1}, f, DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE, DXGI_MODE_SCALING_UNSPECIFIED};
            out[n] = m;
        }
        n++;
    }
    return n;
}

static HRESULT STDMETHODCALLTYPE m_IDXGIOutput_GetDisplayModeList(void *This, DXGI_FORMAT f, UINT flags, UINT *count,
                                                                  DXGI_MODE_DESC *desc)
{
    (void)This; (void)flags;
    if (!count) return DXGI_ERROR_INVALID_CALL;
    uint32_t bw, bh;
    if (f == DXGI_FORMAT_UNKNOWN || !hwd3d_fmt_block(f, &bw, &bh) || bw != 1) {
        *count = 0;
        return S_OK;
    }
    int n = screen_modes(f, desc, *count);
    if (desc && (UINT)n > *count) return DXGI_ERROR_MORE_DATA;
    *count = (UINT)n;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE m_IDXGIOutput_FindClosestMatchingMode(void *This, const DXGI_MODE_DESC *want,
                                                                       DXGI_MODE_DESC *out, IUnknown *device)
{
    (void)This; (void)device;
    if (!want || !out) return DXGI_ERROR_INVALID_CALL;
    DXGI_MODE_DESC modes[16];
    DXGI_FORMAT f = want->Format ? want->Format : DXGI_FORMAT_R8G8B8A8_UNORM;
    int n = screen_modes(f, modes, 16), best = n - 1;
    uint64_t best_d = ~0ull;
    for (int i = 0; i < n && i < 16; i++) {
        int64_t dw = (int64_t)modes[i].Width - want->Width, dh = (int64_t)modes[i].Height - want->Height;
        uint64_t d = (want->Width ? (uint64_t)(dw * dw) : 0) + (want->Height ? (uint64_t)(dh * dh) : 0);
        if (d < best_d) { best_d = d; best = i; }
    }
    *out = modes[best < 16 ? best : 15];
    return S_OK;
}

/* ---- the generated thunks ---- */

#include "hwd3d_guest.inc"

/* ---- exports ---- */

static HMODULE system_dll(const char *name)
{
    char path[MAX_PATH];
    UINT n = GetSystemDirectoryA(path, MAX_PATH);
    snprintf(path + n, sizeof path - n, "\\%s", name);
    return LoadLibraryA(path);
}

#define SYSTEM(dll, fn, type) ((type)(void *)GetProcAddress(system_dll(dll), fn))

typedef HRESULT(WINAPI *PFN_CREATE_FACTORY)(REFIID, void **);

static HRESULT create_factory(REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    *out = NULL;
    int want = iface_index(riid);
    if (want < 0) return E_NOINTERFACE;
    struct wbuf *b = begin(0, 0, HWD3D_OP_CREATE_FACTORY);
    put32(b, (uint32_t)want);
    struct rd r;
    HRESULT hr = call_wait(b, &r);
    uint32_t h = take32(&r);
    if (SUCCEEDED(hr)) *out = shell_for(h, want);
    call_end(&r);
    return hr;
}

__declspec(dllexport) HRESULT WINAPI HwCreateDXGIFactory(REFIID riid, void **out)
{
    if (!ready()) return SYSTEM("dxgi.dll", "CreateDXGIFactory", PFN_CREATE_FACTORY)(riid, out);
    return create_factory(riid, out);
}

__declspec(dllexport) HRESULT WINAPI HwCreateDXGIFactory1(REFIID riid, void **out)
{
    if (!ready()) return SYSTEM("dxgi.dll", "CreateDXGIFactory1", PFN_CREATE_FACTORY)(riid, out);
    return create_factory(riid, out);
}

typedef HRESULT(WINAPI *PFN_CREATE_FACTORY2)(UINT, REFIID, void **);
__declspec(dllexport) HRESULT WINAPI HwCreateDXGIFactory2(UINT flags, REFIID riid, void **out)
{
    if (!ready()) return SYSTEM("dxgi.dll", "CreateDXGIFactory2", PFN_CREATE_FACTORY2)(flags, riid, out);
    return create_factory(riid, out);
}

static HRESULT create_device(IDXGIAdapter *adapter, D3D_DRIVER_TYPE type, UINT flags, const D3D_FEATURE_LEVEL *levels,
                             UINT nlevels, ID3D11Device **dev, D3D_FEATURE_LEVEL *level, ID3D11DeviceContext **ctx)
{
    if (dev) *dev = NULL;
    if (ctx) *ctx = NULL;
    if (adapter && !is_shell(adapter)) return E_INVALIDARG;
    if (type != D3D_DRIVER_TYPE_HARDWARE && type != D3D_DRIVER_TYPE_UNKNOWN) return E_NOTIMPL;
    struct wbuf *b = begin(handle_of(adapter), 0, HWD3D_OP_CREATE_DEVICE);
    put32(b, flags & ~(UINT)D3D11_CREATE_DEVICE_DEBUG);
    put_arr(b, levels, nlevels, sizeof *levels);
    struct rd r;
    HRESULT hr = call_wait(b, &r);
    uint32_t hd = take32(&r), lv = take32(&r), hc = take32(&r);
    struct shell *d = SUCCEEDED(hr) ? shell_for(hd, HI_ID3D11Device) : NULL;
    struct shell *c = SUCCEEDED(hr) ? shell_for(hc, HI_ID3D11DeviceContext) : NULL;
    call_end(&r);
    if (level) *level = (D3D_FEATURE_LEVEL)lv;
    if (dev) *dev = (ID3D11Device *)d; else if (d) m_IUnknown_Release(d);
    if (ctx) *ctx = (ID3D11DeviceContext *)c; else if (c) m_IUnknown_Release(c);
    return hr;
}

HRESULT WINAPI D3D11CreateDevice(IDXGIAdapter *adapter, D3D_DRIVER_TYPE type, HMODULE sw, UINT flags,
                                                       const D3D_FEATURE_LEVEL *levels, UINT nlevels, UINT sdk,
                                                       ID3D11Device **dev, D3D_FEATURE_LEVEL *level, ID3D11DeviceContext **ctx)
{
    if (!ready()) {
        typedef HRESULT(WINAPI * PFN)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
                                      ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
        return SYSTEM("d3d11.dll", "D3D11CreateDevice", PFN)(adapter, type, sw, flags, levels, nlevels, sdk, dev, level, ctx);
    }
    return create_device(adapter, type, flags, levels, nlevels, dev, level, ctx);
}

HRESULT WINAPI D3D11CreateDeviceAndSwapChain(IDXGIAdapter *adapter, D3D_DRIVER_TYPE type, HMODULE sw,
                                                                   UINT flags, const D3D_FEATURE_LEVEL *levels, UINT nlevels,
                                                                   UINT sdk, const DXGI_SWAP_CHAIN_DESC *desc,
                                                                   IDXGISwapChain **swapchain, ID3D11Device **dev,
                                                                   D3D_FEATURE_LEVEL *level, ID3D11DeviceContext **ctx)
{
    if (!ready()) {
        typedef HRESULT(WINAPI * PFN)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
                                      const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **,
                                      D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
        return SYSTEM("d3d11.dll", "D3D11CreateDeviceAndSwapChain", PFN)(adapter, type, sw, flags, levels, nlevels, sdk, desc,
                                                                         swapchain, dev, level, ctx);
    }
    if (swapchain) *swapchain = NULL;
    if (swapchain && !desc) return E_INVALIDARG;
    ID3D11Device *d = NULL;
    HRESULT hr = create_device(adapter, type, flags, levels, nlevels, &d, level, ctx);
    if (FAILED(hr)) return hr;
    if (swapchain) {
        /* As Windows does: the device's adapter's factory makes the swap chain. */
        IDXGIDevice *dd = NULL;
        IDXGIAdapter *ad = NULL;
        IDXGIFactory *f = NULL;
        hr = ID3D11Device_QueryInterface(d, &IID_IDXGIDevice, (void **)&dd);
        if (SUCCEEDED(hr)) hr = IDXGIDevice_GetAdapter(dd, &ad);
        if (SUCCEEDED(hr)) hr = IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory, (void **)&f);
        if (SUCCEEDED(hr)) hr = IDXGIFactory_CreateSwapChain(f, (IUnknown *)d, (DXGI_SWAP_CHAIN_DESC *)desc, swapchain);
        if (f) IDXGIFactory_Release(f);
        if (ad) IDXGIAdapter_Release(ad);
        if (dd) IDXGIDevice_Release(dd);
        if (FAILED(hr)) {
            if (ctx && *ctx) { ID3D11DeviceContext_Release(*ctx); *ctx = NULL; }
            ID3D11Device_Release(d);
            return hr;
        }
    }
    if (dev) *dev = d; else ID3D11Device_Release(d);
    return S_OK;
}
