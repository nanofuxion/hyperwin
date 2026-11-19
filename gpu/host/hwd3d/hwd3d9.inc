// Direct3D 9 on the phone (wddm/README.md phase 7): the Windows Direct3D 9 driver part sends
// HWD3D_OP_D9 requests (include/hw_d3d.h, enum hwd3d9_method) and each becomes a call on DXVK's
// IDirect3DDevice9. Included by hwd3d.cpp. Every object is a handle into the channel's D9State table,
// checked for its kind on every use.

struct D9State {
    IDirect3D9Ex* d3d = nullptr;
    std::vector<IUnknown*> objs{nullptr};   // handle 0 stays empty
    std::vector<uint8_t> kinds{0};
    std::vector<uint32_t> levels{0};        // textures: their level count (subresource = face * levels + level)
    std::vector<uint32_t> owner{0};         // the device handle each object belongs to
};

enum { D9K_NONE, D9K_DEV, D9K_TEX, D9K_CUBE, D9K_VOL, D9K_SURF, D9K_VB, D9K_IB, D9K_VS, D9K_PS, D9K_DECL, D9K_QUERY };

IDirect3D9Ex* (WINAPI* g_create9ex)(UINT) = nullptr;   // Direct3DCreate9Ex's shape, wrapped below

bool load_d3d9() {
    static int state = -1;
    if (state >= 0) return state;
    state = 0;
    void* h = dlopen("libdxvk_d3d9.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        LOG("Direct3D 9: libdxvk_d3d9.so: %s", dlerror());
        return false;
    }
    auto create = reinterpret_cast<HRESULT(WINAPI*)(UINT, IDirect3D9Ex**)>(dlsym(h, "Direct3DCreate9Ex"));
    if (!create) return false;
    static HRESULT(WINAPI * s_create)(UINT, IDirect3D9Ex**) = create;
    g_create9ex = [](UINT v) -> IDirect3D9Ex* {
        IDirect3D9Ex* d = nullptr;
        return SUCCEEDED(s_create(v, &d)) ? d : nullptr;
    };
    state = 1;
    return true;
}

uint32_t d9_put(D9State& s, IUnknown* o, uint8_t kind, uint32_t dev, uint32_t levels = 0) {
    for (uint32_t i = 1; i < s.objs.size(); i++)
        if (!s.objs[i]) {
            s.objs[i] = o;
            s.kinds[i] = kind;
            s.levels[i] = levels;
            s.owner[i] = dev;
            return i;
        }
    s.objs.push_back(o);
    s.kinds.push_back(kind);
    s.levels.push_back(levels);
    s.owner.push_back(dev);
    return uint32_t(s.objs.size() - 1);
}

template <class T> T* d9_get(D9State* s, uint32_t h, uint8_t kind) {
    if (!s || !h || h >= s->objs.size() || s->kinds[h] != kind) return nullptr;
    return static_cast<T*>(s->objs[h]);
}

void d9_release(Chan& c) {
    D9State* s = c.d9;
    if (!s) return;
    // Everything but the devices first, then the devices, then Direct3D.
    for (int pass = 0; pass < 2; pass++)
        for (size_t i = 1; i < s->objs.size(); i++)
            if (s->objs[i] && (pass == 1) == (s->kinds[i] == D9K_DEV)) {
                s->objs[i]->Release();
                s->objs[i] = nullptr;
            }
    if (s->d3d) s->d3d->Release();
    delete s;
    c.d9 = nullptr;
}

// The surface a subresource names: a texture level, a cube face's level, or the surface itself. Held.
IDirect3DSurface9* d9_surface(D9State* s, uint32_t h, uint32_t sub) {
    if (!s || !h || h >= s->objs.size() || !s->objs[h]) return nullptr;
    IDirect3DSurface9* out = nullptr;
    switch (s->kinds[h]) {
    case D9K_SURF:
        out = static_cast<IDirect3DSurface9*>(s->objs[h]);
        out->AddRef();
        break;
    case D9K_TEX:
        static_cast<IDirect3DTexture9*>(s->objs[h])->GetSurfaceLevel(sub, &out);
        break;
    case D9K_CUBE: {
        uint32_t lv = s->levels[h] ? s->levels[h] : 1;
        static_cast<IDirect3DCubeTexture9*>(s->objs[h])->GetCubeMapSurface(D3DCUBEMAP_FACES(sub / lv), sub % lv, &out);
        break;
    }
    }
    return out;
}

// What the phone does with each format, in the guest's terms (HWD3D9_FMT_*).
uint32_t d9_format_bits(IDirect3D9Ex* d3d, D3DFORMAT f) {
    const D3DFORMAT disp = D3DFMT_X8R8G8B8;
    auto ok = [&](DWORD usage, D3DRESOURCETYPE t) {
        return SUCCEEDED(d3d->CheckDeviceFormat(0, D3DDEVTYPE_HAL, disp, usage, t, f));
    };
    uint32_t b = 0;
    if (ok(0, D3DRTYPE_TEXTURE)) b |= HWD3D9_FMT_TEXTURE | HWD3D9_FMT_PLAIN;
    if (ok(0, D3DRTYPE_CUBETEXTURE)) b |= HWD3D9_FMT_CUBE;
    if (ok(0, D3DRTYPE_VOLUMETEXTURE)) b |= HWD3D9_FMT_VOLUME;
    if (ok(D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE) || ok(D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE)) b |= HWD3D9_FMT_RT;
    if (ok(D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE) || ok(D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE)) b |= HWD3D9_FMT_DS;
    if (ok(D3DUSAGE_QUERY_SRGBREAD, D3DRTYPE_TEXTURE)) b |= HWD3D9_FMT_SRGBREAD;
    if (ok(D3DUSAGE_QUERY_SRGBWRITE, D3DRTYPE_TEXTURE)) b |= HWD3D9_FMT_SRGBWRITE;
    if (ok(D3DUSAGE_QUERY_VERTEXTEXTURE, D3DRTYPE_TEXTURE)) b |= HWD3D9_FMT_VTX_TEX;
    if (ok(D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING, D3DRTYPE_TEXTURE)) b |= HWD3D9_FMT_BLEND;
    if (ok(D3DUSAGE_QUERY_FILTER, D3DRTYPE_TEXTURE)) b |= HWD3D9_FMT_FILTER;
    if (ok(D3DUSAGE_AUTOGENMIPMAP, D3DRTYPE_TEXTURE)) b |= HWD3D9_FMT_AUTOGEN;
    if (f == D3DFMT_X8R8G8B8 || f == D3DFMT_R5G6B5 || f == D3DFMT_X1R5G5B5 || f == D3DFMT_A2R10G10B10)
        if (SUCCEEDED(d3d->CheckDeviceType(0, D3DDEVTYPE_HAL, f, f, TRUE))) b |= HWD3D9_FMT_DISPLAY;
    return b;
}

// Copies rows between a locked resource and a request or answer.
void d9_copy_rows(uint8_t* dst, uint32_t dpitch, const uint8_t* src, uint32_t spitch, uint32_t row, uint32_t rows) {
    for (uint32_t y = 0; y < rows; y++) memcpy(dst + size_t(y) * dpitch, src + size_t(y) * spitch, row);
}

int32_t d9_call(Chan& c, const hwd3d_call& call, Rd& r, Wr& w) {
    if (!c.d9) c.d9 = new D9State;
    D9State* s = c.d9;
    const uint32_t m = call.method;
    if (m == HWD3D9_CREATE_DEVICE) {
        if (!load_d3d9()) return E_NOTIMPL;
        if (!s->d3d) s->d3d = g_create9ex(D3D_SDK_VERSION);
        if (!s->d3d) return E_FAIL;
        D3DPRESENT_PARAMETERS pp = {};
        pp.BackBufferWidth = 16;
        pp.BackBufferHeight = 16;
        pp.BackBufferFormat = D3DFMT_X8R8G8B8;
        pp.BackBufferCount = 1;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.Windowed = TRUE;
        pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        IDirect3DDevice9Ex* dev = nullptr;
        HRESULT hr = s->d3d->CreateDeviceEx(0, D3DDEVTYPE_HAL, nullptr,
                                            D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, nullptr, &dev);
        LOG("channel %u: Direct3D 9 device 0x%x", c.slot, unsigned(hr));
        if (FAILED(hr)) return hr;
        D3DCAPS9 caps = {};
        dev->GetDeviceCaps(&caps);
        w.u32(d9_put(*s, dev, D9K_DEV, 0));
        w.put(&caps, sizeof caps);
        return 0;
    }
    if (m == HWD3D9_FORMATS) {
        if (!load_d3d9()) return E_NOTIMPL;
        if (!s->d3d) s->d3d = g_create9ex(D3D_SDK_VERSION);
        if (!s->d3d) return E_FAIL;
        uint64_t n = 0;
        const uint32_t* f = static_cast<const uint32_t*>(r.arr(4, &n));
        if (!r.ok || n > 512) return E_INVALIDARG;
        for (uint64_t i = 0; i < n; i++) w.u32(d9_format_bits(s->d3d, D3DFORMAT(f[i])));
        return 0;
    }
    if (m == HWD3D9_RELEASE) {
        uint32_t h = call.object;
        if (!h || h >= s->objs.size() || !s->objs[h]) return E_INVALIDARG;
        s->objs[h]->Release();
        s->objs[h] = nullptr;
        s->kinds[h] = D9K_NONE;
        return 0;
    }
    // Everything else is a call on a device.
    auto* dev = d9_get<IDirect3DDevice9Ex>(s, call.object, D9K_DEV);
    if (!dev) return E_INVALIDARG;
    const uint32_t devh = call.object;
    switch (m) {
    case HWD3D9_CREATE_RES: {
        hwd3d9_res d;
        r.get(&d, sizeof d);
        if (!r.ok || !d.width || d.width > 16384 || d.height > 16384 || d.depth > 2048 || d.levels > 16) return E_INVALIDARG;
        HRESULT hr = E_INVALIDARG;
        IUnknown* o = nullptr;
        uint8_t kind = 0;
        D3DPOOL pool = D3DPOOL(d.pool);
        switch (d.kind) {
        case HWD3D9_TEX: {
            IDirect3DTexture9* t = nullptr;
            hr = dev->CreateTexture(d.width, d.height, d.levels, d.usage, D3DFORMAT(d.format), pool, &t, nullptr);
            o = t;
            kind = D9K_TEX;
            if (t) d.levels = t->GetLevelCount();
            break;
        }
        case HWD3D9_CUBE: {
            IDirect3DCubeTexture9* t = nullptr;
            hr = dev->CreateCubeTexture(d.width, d.levels, d.usage, D3DFORMAT(d.format), pool, &t, nullptr);
            o = t;
            kind = D9K_CUBE;
            if (t) d.levels = t->GetLevelCount();
            break;
        }
        case HWD3D9_VOL: {
            IDirect3DVolumeTexture9* t = nullptr;
            hr = dev->CreateVolumeTexture(d.width, d.height, d.depth, d.levels, d.usage, D3DFORMAT(d.format), pool, &t, nullptr);
            o = t;
            kind = D9K_VOL;
            if (t) d.levels = t->GetLevelCount();
            break;
        }
        case HWD3D9_RTSURF: {
            IDirect3DSurface9* t = nullptr;
            hr = dev->CreateRenderTarget(d.width, d.height, D3DFORMAT(d.format), D3DMULTISAMPLE_TYPE(d.ms_type), d.ms_quality,
                                         d.lockable, &t, nullptr);
            o = t;
            kind = D9K_SURF;
            break;
        }
        case HWD3D9_DSSURF: {
            IDirect3DSurface9* t = nullptr;
            hr = dev->CreateDepthStencilSurface(d.width, d.height, D3DFORMAT(d.format), D3DMULTISAMPLE_TYPE(d.ms_type), d.ms_quality,
                                                FALSE, &t, nullptr);
            o = t;
            kind = D9K_SURF;
            break;
        }
        case HWD3D9_PLAIN: {
            IDirect3DSurface9* t = nullptr;
            hr = dev->CreateOffscreenPlainSurface(d.width, d.height, D3DFORMAT(d.format), pool == D3DPOOL_MANAGED ? D3DPOOL_SYSTEMMEM : pool, &t,
                                                  nullptr);
            o = t;
            kind = D9K_SURF;
            break;
        }
        case HWD3D9_VB: {
            IDirect3DVertexBuffer9* t = nullptr;
            hr = dev->CreateVertexBuffer(d.width, d.usage, d.fvf, pool, &t, nullptr);
            o = t;
            kind = D9K_VB;
            break;
        }
        case HWD3D9_IB: {
            IDirect3DIndexBuffer9* t = nullptr;
            hr = dev->CreateIndexBuffer(d.width, d.usage, D3DFORMAT(d.format), pool, &t, nullptr);
            o = t;
            kind = D9K_IB;
            break;
        }
        }
        if (FAILED(hr) || !o) {
            LOG("channel %u: Direct3D 9 resource kind %u format %u %ux%u: 0x%x", c.slot, d.kind, d.format, d.width, d.height, unsigned(hr));
            return FAILED(hr) ? hr : E_FAIL;
        }
        w.u32(d9_put(*s, o, kind, devh, d.levels));
        return 0;
    }
    case HWD3D9_RS: {
        uint32_t a = r.u32(), b = r.u32();
        return dev->SetRenderState(D3DRENDERSTATETYPE(a), b);
    }
    case HWD3D9_TSS: {
        uint32_t st = r.u32(), t = r.u32(), v = r.u32();
        return dev->SetTextureStageState(st, D3DTEXTURESTAGESTATETYPE(t), v);
    }
    case HWD3D9_SS: {
        uint32_t st = r.u32(), t = r.u32(), v = r.u32();
        return dev->SetSamplerState(st, D3DSAMPLERSTATETYPE(t), v);
    }
    case HWD3D9_TEXTURE: {
        uint32_t st = r.u32(), h = r.u32();
        IDirect3DBaseTexture9* t = nullptr;
        if (h && h < s->objs.size() && (s->kinds[h] == D9K_TEX || s->kinds[h] == D9K_CUBE || s->kinds[h] == D9K_VOL))
            t = static_cast<IDirect3DBaseTexture9*>(s->objs[h]);
        return dev->SetTexture(st, t);
    }
    case HWD3D9_STREAM: {
        uint32_t st = r.u32(), h = r.u32(), off = r.u32(), stride = r.u32();
        return dev->SetStreamSource(st, d9_get<IDirect3DVertexBuffer9>(s, h, D9K_VB), off, stride);
    }
    case HWD3D9_INDICES:
        return dev->SetIndices(d9_get<IDirect3DIndexBuffer9>(s, r.u32(), D9K_IB));
    case HWD3D9_DRAW: {
        uint32_t t = r.u32(), start = r.u32(), n = r.u32();
        return dev->DrawPrimitive(D3DPRIMITIVETYPE(t), start, n);
    }
    case HWD3D9_DRAW_IDX: {
        uint32_t t = r.u32();
        int32_t base = int32_t(r.u32());
        uint32_t mn = r.u32(), nv = r.u32(), start = r.u32(), n = r.u32();
        return dev->DrawIndexedPrimitive(D3DPRIMITIVETYPE(t), base, mn, nv, start, n);
    }
    case HWD3D9_DRAW_UP: {
        uint32_t t = r.u32(), n = r.u32(), stride = r.u32();
        uint64_t bytes = 0;
        const void* v = r.arr(1, &bytes);
        if (!r.ok || !v) return E_INVALIDARG;
        return dev->DrawPrimitiveUP(D3DPRIMITIVETYPE(t), n, v, stride);
    }
    case HWD3D9_DRAW_IDX_UP: {
        uint32_t t = r.u32(), mn = r.u32(), nv = r.u32(), n = r.u32(), fmt = r.u32();
        uint64_t ib = 0, vb = 0;
        const void* idx = r.arr(1, &ib);
        uint32_t stride = r.u32();
        const void* v = r.arr(1, &vb);
        if (!r.ok || !idx || !v) return E_INVALIDARG;
        return dev->DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE(t), mn, nv, n, idx, D3DFORMAT(fmt), v, stride);
    }
    case HWD3D9_CLEAR: {
        uint32_t flags = r.u32(), color = r.u32(), zb = r.u32(), stencil = r.u32();
        float z;
        memcpy(&z, &zb, 4);
        uint64_t n = 0;
        const D3DRECT* rects = static_cast<const D3DRECT*>(r.arr(sizeof(D3DRECT), &n));
        return dev->Clear(DWORD(n), n ? rects : nullptr, flags, color, z, stencil);
    }
    case HWD3D9_VIEWPORT: {
        D3DVIEWPORT9 v;
        r.get(&v, sizeof v);
        return dev->SetViewport(&v);
    }
    case HWD3D9_TRANSFORM: {
        uint32_t st = r.u32();
        D3DMATRIX mx;
        r.get(&mx, sizeof mx);
        return dev->SetTransform(D3DTRANSFORMSTATETYPE(st), &mx);
    }
    case HWD3D9_MATERIAL: {
        D3DMATERIAL9 mt;
        r.get(&mt, sizeof mt);
        return dev->SetMaterial(&mt);
    }
    case HWD3D9_LIGHT: {
        uint32_t i = r.u32();
        D3DLIGHT9 l;
        r.get(&l, sizeof l);
        return dev->SetLight(i, &l);
    }
    case HWD3D9_LIGHT_ENABLE: {
        uint32_t i = r.u32(), on = r.u32();
        return dev->LightEnable(i, on);
    }
    case HWD3D9_CLIP_PLANE: {
        uint32_t i = r.u32();
        float p[4];
        r.get(p, sizeof p);
        return dev->SetClipPlane(i, p);
    }
    case HWD3D9_CREATE_VS:
    case HWD3D9_CREATE_PS: {
        uint64_t n = 0;
        const DWORD* code = static_cast<const DWORD*>(r.arr(4, &n));
        if (!r.ok || !code) return E_INVALIDARG;
        if (m == HWD3D9_CREATE_VS) {
            IDirect3DVertexShader9* sh = nullptr;
            HRESULT hr = dev->CreateVertexShader(code, &sh);
            if (FAILED(hr)) return hr;
            w.u32(d9_put(*s, sh, D9K_VS, devh));
        } else {
            IDirect3DPixelShader9* sh = nullptr;
            HRESULT hr = dev->CreatePixelShader(code, &sh);
            if (FAILED(hr)) return hr;
            w.u32(d9_put(*s, sh, D9K_PS, devh));
        }
        return 0;
    }
    case HWD3D9_CREATE_DECL: {
        uint64_t n = 0;
        const D3DVERTEXELEMENT9* e = static_cast<const D3DVERTEXELEMENT9*>(r.arr(sizeof(D3DVERTEXELEMENT9), &n));
        if (!r.ok || !e || n > 65) return E_INVALIDARG;
        std::vector<D3DVERTEXELEMENT9> v(e, e + n);
        if (v.empty() || v.back().Stream != 0xFF) v.push_back(D3DDECL_END());
        IDirect3DVertexDeclaration9* d = nullptr;
        HRESULT hr = dev->CreateVertexDeclaration(v.data(), &d);
        if (FAILED(hr)) return hr;
        w.u32(d9_put(*s, d, D9K_DECL, devh));
        return 0;
    }
    case HWD3D9_SET_VS:
        return dev->SetVertexShader(d9_get<IDirect3DVertexShader9>(s, r.u32(), D9K_VS));
    case HWD3D9_SET_PS:
        return dev->SetPixelShader(d9_get<IDirect3DPixelShader9>(s, r.u32(), D9K_PS));
    case HWD3D9_SET_DECL:
        return dev->SetVertexDeclaration(d9_get<IDirect3DVertexDeclaration9>(s, r.u32(), D9K_DECL));
    case HWD3D9_VS_F: case HWD3D9_VS_I: case HWD3D9_VS_B: case HWD3D9_PS_F: case HWD3D9_PS_I: case HWD3D9_PS_B: {
        uint32_t first = r.u32(), count = r.u32();
        uint64_t n = 0;
        const void* v = r.arr(4, &n);
        bool b = m == HWD3D9_VS_B || m == HWD3D9_PS_B;
        if (!r.ok || !v || n < uint64_t(count) * (b ? 1 : 4)) return E_INVALIDARG;
        switch (m) {
        case HWD3D9_VS_F: return dev->SetVertexShaderConstantF(first, static_cast<const float*>(v), count);
        case HWD3D9_VS_I: return dev->SetVertexShaderConstantI(first, static_cast<const int*>(v), count);
        case HWD3D9_VS_B: return dev->SetVertexShaderConstantB(first, static_cast<const BOOL*>(v), count);
        case HWD3D9_PS_F: return dev->SetPixelShaderConstantF(first, static_cast<const float*>(v), count);
        case HWD3D9_PS_I: return dev->SetPixelShaderConstantI(first, static_cast<const int*>(v), count);
        default: return dev->SetPixelShaderConstantB(first, static_cast<const BOOL*>(v), count);
        }
    }
    case HWD3D9_SCISSOR: {
        RECT rc;
        r.get(&rc, sizeof rc);
        return dev->SetScissorRect(&rc);
    }
    case HWD3D9_RT: {
        uint32_t i = r.u32(), h = r.u32(), sub = r.u32();
        IDirect3DSurface9* sf = h ? d9_surface(s, h, sub) : nullptr;
        if (h && !sf) return E_INVALIDARG;
        HRESULT hr = dev->SetRenderTarget(i, sf);
        if (sf) sf->Release();
        return hr;
    }
    case HWD3D9_DS: {
        uint32_t h = r.u32();
        IDirect3DSurface9* sf = h ? d9_surface(s, h, 0) : nullptr;
        HRESULT hr = dev->SetDepthStencilSurface(sf);
        if (sf) sf->Release();
        return hr;
    }
    case HWD3D9_WRITE: {
        uint32_t h = r.u32(), sub = r.u32();
        hwd3d9_box bx;
        r.get(&bx, sizeof bx);
        uint32_t pitch = r.u32(), slice = r.u32();
        uint64_t n = 0;
        const uint8_t* data = static_cast<const uint8_t*>(r.arr(1, &n));
        if (!r.ok || !data || h >= s->objs.size()) return E_INVALIDARG;
        uint8_t k = s->kinds[h];
        if (k == D9K_VB || k == D9K_IB) {
            uint32_t off = bx.valid ? bx.left : 0;
            void* p = nullptr;
            HRESULT hr = k == D9K_VB ? static_cast<IDirect3DVertexBuffer9*>(s->objs[h])->Lock(off, UINT(n), &p, D3DLOCK_NOOVERWRITE)
                                     : static_cast<IDirect3DIndexBuffer9*>(s->objs[h])->Lock(off, UINT(n), &p, D3DLOCK_NOOVERWRITE);
            if (FAILED(hr)) return hr;
            memcpy(p, data, size_t(n));
            return k == D9K_VB ? static_cast<IDirect3DVertexBuffer9*>(s->objs[h])->Unlock() : static_cast<IDirect3DIndexBuffer9*>(s->objs[h])->Unlock();
        }
        if (k == D9K_VOL) {
            auto* t = static_cast<IDirect3DVolumeTexture9*>(s->objs[h]);
            D3DBOX b = {bx.left, bx.top, bx.right, bx.bottom, bx.front, bx.back};
            D3DLOCKED_BOX lb;
            HRESULT hr = t->LockBox(sub, &lb, bx.valid ? &b : nullptr, 0);
            if (FAILED(hr)) return hr;
            uint32_t rows = pitch ? uint32_t(slice / pitch) : 0, slices = slice ? uint32_t(n / slice) : 0;
            for (uint32_t z = 0; z < slices; z++)
                d9_copy_rows(static_cast<uint8_t*>(lb.pBits) + size_t(z) * lb.SlicePitch, lb.RowPitch, data + size_t(z) * slice, pitch,
                             std::min<uint32_t>(pitch, lb.RowPitch), rows);
            return t->UnlockBox(sub);
        }
        IDirect3DSurface9* sf = d9_surface(s, h, sub);
        if (!sf) return E_INVALIDARG;
        RECT rc = {LONG(bx.left), LONG(bx.top), LONG(bx.right), LONG(bx.bottom)};
        D3DLOCKED_RECT lr;
        HRESULT hr = sf->LockRect(&lr, bx.valid ? &rc : nullptr, 0);
        if (SUCCEEDED(hr)) {
            uint32_t rows = pitch ? uint32_t(n / pitch) : 0;
            d9_copy_rows(static_cast<uint8_t*>(lr.pBits), lr.Pitch, data, pitch, std::min<uint32_t>(pitch, lr.Pitch), rows);
            hr = sf->UnlockRect();
        } else {
            // Not lockable (a render target, say): through a plain surface of the same size.
            D3DSURFACE_DESC sd;
            sf->GetDesc(&sd);
            IDirect3DSurface9* tmp = nullptr;
            hr = dev->CreateOffscreenPlainSurface(sd.Width, sd.Height, sd.Format, D3DPOOL_SYSTEMMEM, &tmp, nullptr);
            if (SUCCEEDED(hr) && SUCCEEDED(hr = tmp->LockRect(&lr, bx.valid ? &rc : nullptr, 0))) {
                uint32_t rows = pitch ? uint32_t(n / pitch) : 0;
                d9_copy_rows(static_cast<uint8_t*>(lr.pBits), lr.Pitch, data, pitch, std::min<uint32_t>(pitch, lr.Pitch), rows);
                tmp->UnlockRect();
                POINT pt = {rc.left, rc.top};
                hr = dev->UpdateSurface(tmp, bx.valid ? &rc : nullptr, sf, bx.valid ? &pt : nullptr);
            }
            if (tmp) tmp->Release();
        }
        sf->Release();
        return hr;
    }
    case HWD3D9_READ: {
        uint32_t h = r.u32(), sub = r.u32();
        if (h >= s->objs.size()) return E_INVALIDARG;
        uint8_t k = s->kinds[h];
        if (k == D9K_VB || k == D9K_IB) {
            void* p = nullptr;
            UINT size = 0;
            HRESULT hr;
            if (k == D9K_VB) {
                D3DVERTEXBUFFER_DESC bd;
                static_cast<IDirect3DVertexBuffer9*>(s->objs[h])->GetDesc(&bd);
                size = bd.Size;
                hr = static_cast<IDirect3DVertexBuffer9*>(s->objs[h])->Lock(0, 0, &p, D3DLOCK_READONLY);
            } else {
                D3DINDEXBUFFER_DESC bd;
                static_cast<IDirect3DIndexBuffer9*>(s->objs[h])->GetDesc(&bd);
                size = bd.Size;
                hr = static_cast<IDirect3DIndexBuffer9*>(s->objs[h])->Lock(0, 0, &p, D3DLOCK_READONLY);
            }
            if (FAILED(hr)) return hr;
            w.u32(size);
            w.u32(1);
            w.u32(size);
            w.put(p, size);
            k == D9K_VB ? static_cast<IDirect3DVertexBuffer9*>(s->objs[h])->Unlock() : static_cast<IDirect3DIndexBuffer9*>(s->objs[h])->Unlock();
            return 0;
        }
        IDirect3DSurface9* sf = d9_surface(s, h, sub);
        if (!sf) return E_NOTIMPL;
        D3DSURFACE_DESC sd;
        sf->GetDesc(&sd);
        D3DLOCKED_RECT lr;
        IDirect3DSurface9* tmp = nullptr;
        HRESULT hr = sf->LockRect(&lr, nullptr, D3DLOCK_READONLY);
        IDirect3DSurface9* locked = sf;
        if (FAILED(hr)) {
            // A render target: copied into system memory first.
            hr = dev->CreateOffscreenPlainSurface(sd.Width, sd.Height, sd.Format, D3DPOOL_SYSTEMMEM, &tmp, nullptr);
            if (SUCCEEDED(hr)) hr = dev->GetRenderTargetData(sf, tmp);
            if (SUCCEEDED(hr)) hr = tmp->LockRect(&lr, nullptr, D3DLOCK_READONLY);
            locked = tmp;
        }
        if (SUCCEEDED(hr)) {
            // Block-compressed formats are rows of 4x4 blocks.
            bool bc = sd.Format == D3DFMT_DXT1 || sd.Format == D3DFMT_DXT2 || sd.Format == D3DFMT_DXT3 || sd.Format == D3DFMT_DXT4 ||
                      sd.Format == D3DFMT_DXT5;
            uint32_t rows = bc ? (sd.Height + 3) / 4 : sd.Height;
            w.u32(uint32_t(lr.Pitch));
            w.u32(rows);
            w.u32(uint32_t(lr.Pitch) * rows);
            w.put(lr.pBits, size_t(lr.Pitch) * rows);
            locked->UnlockRect();
        }
        if (tmp) tmp->Release();
        sf->Release();
        return hr;
    }
    case HWD3D9_STRETCH: {
        uint32_t sh = r.u32(), ssub = r.u32();
        RECT sr;
        r.get(&sr, sizeof sr);
        uint32_t dh = r.u32(), dsub = r.u32();
        RECT dr;
        r.get(&dr, sizeof dr);
        uint32_t filter = r.u32();
        IDirect3DSurface9* a = d9_surface(s, sh, ssub);
        IDirect3DSurface9* b = d9_surface(s, dh, dsub);
        HRESULT hr = a && b ? dev->StretchRect(a, &sr, b, &dr, D3DTEXTUREFILTERTYPE(filter)) : E_INVALIDARG;
        if (a) a->Release();
        if (b) b->Release();
        return hr;
    }
    case HWD3D9_FILL: {
        uint32_t h = r.u32(), sub = r.u32();
        RECT rc;
        r.get(&rc, sizeof rc);
        uint32_t color = r.u32();
        IDirect3DSurface9* sf = d9_surface(s, h, sub);
        HRESULT hr = sf ? dev->ColorFill(sf, &rc, color) : E_INVALIDARG;
        if (sf) sf->Release();
        return hr;
    }
    case HWD3D9_FREQ: {
        uint32_t st = r.u32(), div = r.u32();
        return dev->SetStreamSourceFreq(st, div);
    }
    case HWD3D9_COPY_RES: {
        uint32_t sh = r.u32(), dh = r.u32();
        if (sh >= s->objs.size() || dh >= s->objs.size()) return E_INVALIDARG;
        auto* a = d9_get<IDirect3DBaseTexture9>(s, sh, s->kinds[sh]);
        auto* b = d9_get<IDirect3DBaseTexture9>(s, dh, s->kinds[dh]);
        if (!a || !b || (s->kinds[sh] != D9K_TEX && s->kinds[sh] != D9K_CUBE && s->kinds[sh] != D9K_VOL)) return E_INVALIDARG;
        return dev->UpdateTexture(a, b);
    }
    case HWD3D9_GENMIPS: {
        uint32_t h = r.u32();
        if (h >= s->objs.size() || (s->kinds[h] != D9K_TEX && s->kinds[h] != D9K_CUBE)) return E_INVALIDARG;
        static_cast<IDirect3DBaseTexture9*>(s->objs[h])->GenerateMipSubLevels();
        return 0;
    }
    case HWD3D9_FINISH: {
        IDirect3DQuery9* q = nullptr;
        if (FAILED(dev->CreateQuery(D3DQUERYTYPE_EVENT, &q)) || !q) return 0;
        q->Issue(D3DISSUE_END);
        auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (q->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE && std::chrono::steady_clock::now() < give_up) sched_yield();
        q->Release();
        return 0;
    }
    case HWD3D9_QUERY: {
        IDirect3DQuery9* q = nullptr;
        HRESULT hr = dev->CreateQuery(D3DQUERYTYPE(r.u32()), &q);
        if (FAILED(hr)) return hr;
        w.u32(d9_put(*s, q, D9K_QUERY, devh));
        return 0;
    }
    case HWD3D9_QUERY_ISSUE: {
        auto* q = d9_get<IDirect3DQuery9>(s, r.u32(), D9K_QUERY);
        uint32_t flags = r.u32();
        return q ? q->Issue(flags) : E_INVALIDARG;
    }
    case HWD3D9_QUERY_DATA: {
        auto* q = d9_get<IDirect3DQuery9>(s, r.u32(), D9K_QUERY);
        uint32_t size = r.u32();
        if (!q || size > 256) return E_INVALIDARG;
        uint8_t buf[256] = {};
        HRESULT hr = q->GetData(size ? buf : nullptr, size, D3DGETDATA_FLUSH);
        w.u32(size);
        w.put(buf, size);
        return hr;
    }
    }
    return E_NOTIMPL;
}
