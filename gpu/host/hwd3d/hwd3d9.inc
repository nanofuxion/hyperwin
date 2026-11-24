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
    IDirect3DSurface9* read_tmp = nullptr;  // cached systemmem staging surface for HWD3D9_READ
    IDirect3DDevice9Ex* read_dev = nullptr;
    UINT read_w = 0, read_h = 0;
    D3DFORMAT read_fmt = D3DFMT_UNKNOWN;
};

enum { D9K_NONE, D9K_DEV, D9K_TEX, D9K_CUBE, D9K_VOL, D9K_SURF, D9K_VB, D9K_IB, D9K_VS, D9K_PS, D9K_DECL, D9K_QUERY };

const Dxvk& chan_dxvk(Chan& c);
PFN_vkGetInstanceProcAddr vk_gipa(bool desktop);

IDirect3D9Ex* create_d3d9ex(Chan& c, UINT v) {
    const Dxvk& dx = chan_dxvk(c);
    if (!dx.create9ex) return nullptr;
    dx.apply_program(c.exe, c.conf);
    IDirect3D9Ex* d = nullptr;
    return SUCCEEDED(dx.create9ex(v, &d)) ? d : nullptr;
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
    if (s->read_tmp) {
        s->read_tmp->Release();
        s->read_tmp = nullptr;
        s->read_dev = nullptr;
    }
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

void d9_probe_up(Chan& c, IDirect3DDevice9Ex* dev, const char* what, uint32_t type, uint32_t count, uint32_t stride,
                 const void* v, uint64_t bytes) {
    static std::atomic<uint32_t> n{0};
    uint32_t k = n.fetch_add(1);
    if (k % 500 != 7) return;
    char on[PROP_VALUE_MAX] = "";
    __system_property_get("debug.hwd3d.frames", on);
    if (on[0] != '1') return;
    float f[8] = {};
    memcpy(f, v, std::min<uint64_t>(bytes, sizeof f));
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    dev->GetVertexShader(&vs);
    dev->GetPixelShader(&ps);
    DWORD fvf = 0, zen = 0, cull = 0, light = 0;
    dev->GetFVF(&fvf);
    dev->GetRenderState(D3DRS_ZENABLE, &zen);
    dev->GetRenderState(D3DRS_CULLMODE, &cull);
    dev->GetRenderState(D3DRS_LIGHTING, &light);
    D3DMATRIX world{}, view{}, proj{};
    dev->GetTransform(D3DTS_WORLD, &world);
    dev->GetTransform(D3DTS_VIEW, &view);
    dev->GetTransform(D3DTS_PROJECTION, &proj);
    LOG("channel %u: %s #%u type %u count %u stride %u bytes %llu floats %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f vs %p ps %p fvf 0x%x z %u cull %u light %u "
        "proj %.2f %.2f %.2f %.2f view43 %.2f world11 %.2f",
        c.slot, what, k, type, count, stride, (unsigned long long)bytes, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], (void*)vs, (void*)ps,
        unsigned(fvf), unsigned(zen), unsigned(cull), unsigned(light), proj._11, proj._22, proj._33, proj._43, view._43, world._11);
    if (vs) vs->Release();
    if (ps) ps->Release();
}

// adb shell setprop debug.hwd3d.frames 1: now and then, what a draw is about to use.
void d9_probe_draw(Chan& c, IDirect3DDevice9Ex* dev, const char* what, uint32_t type, uint32_t first, uint32_t count) {
    static std::atomic<uint32_t> n{0};
    uint32_t k = n.fetch_add(1);
    if (k % 500 != 7) return;
    char on[PROP_VALUE_MAX] = "";
    __system_property_get("debug.hwd3d.frames", on);
    if (on[0] != '1') return;
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT off = 0, stride = 0;
    dev->GetStreamSource(0, &vb, &off, &stride);
    uint32_t nz = 0, size = 0;
    float f[6] = {};
    if (vb) {
        D3DVERTEXBUFFER_DESC bd{};
        vb->GetDesc(&bd);
        size = bd.Size;
        void* p = nullptr;
        if (SUCCEEDED(vb->Lock(0, 0, &p, D3DLOCK_READONLY)) && p) {
            const uint8_t* b = static_cast<const uint8_t*>(p);
            for (uint32_t i = 0; i < bd.Size && i < 4096; i++) nz += b[i] != 0;
            if (bd.Size >= off + 24) memcpy(f, b + off, 24);
            vb->Unlock();
        }
        vb->Release();
    }
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    IDirect3DVertexDeclaration9* decl = nullptr;
    dev->GetVertexShader(&vs);
    dev->GetPixelShader(&ps);
    dev->GetVertexDeclaration(&decl);
    DWORD fvf = 0, zen = 0, cull = 0, light = 0;
    dev->GetFVF(&fvf);
    dev->GetRenderState(D3DRS_ZENABLE, &zen);
    dev->GetRenderState(D3DRS_CULLMODE, &cull);
    dev->GetRenderState(D3DRS_LIGHTING, &light);
    D3DVIEWPORT9 vp{};
    dev->GetViewport(&vp);
    LOG("channel %u: %s #%u type %u first %u count %u: stream0 %u bytes off %u stride %u, %u of first 4096 bytes not zero, "
        "first floats %.3f %.3f %.3f %.3f %.3f %.3f; vs %p ps %p decl %p fvf 0x%x z %u cull %u light %u viewport %ux%u at %u,%u",
        c.slot, what, k, type, first, count, size, off, stride, nz, f[0], f[1], f[2], f[3], f[4], f[5], (void*)vs, (void*)ps,
        (void*)decl, unsigned(fvf), unsigned(zen), unsigned(cull), unsigned(light), vp.Width, vp.Height, vp.X, vp.Y);
    if (vs) vs->Release();
    if (ps) ps->Release();
    if (decl) decl->Release();
}

int32_t d9_call(Chan& c, const hwd3d_call& call, Rd& r, Wr& w) {
    if (!c.d9) c.d9 = new D9State;
    D9State* s = c.d9;
    const uint32_t m = call.method;
    if (m == HWD3D9_CREATE_DEVICE) {
        if (!s->d3d) s->d3d = create_d3d9ex(c, D3D_SDK_VERSION);
        if (!s->d3d) return E_NOTIMPL;
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
        if (!s->d3d) s->d3d = create_d3d9ex(c, D3D_SDK_VERSION);
        if (!s->d3d) return E_NOTIMPL;
        uint64_t n = 0;
        const uint32_t* f = static_cast<const uint32_t*>(r.arr(4, &n));
        if (!r.ok || n > 512) return E_INVALIDARG;
        for (uint64_t i = 0; i < n; i++) w.u32(d9_format_bits(s->d3d, D3DFORMAT(f[i])));
        return 0;
    }
    if (m == HWD3D9_RELEASE) {
        uint32_t h = call.object;
        if (!h || h >= s->objs.size() || !s->objs[h]) return E_INVALIDARG;
        if (s->kinds[h] == D9K_DEV && s->read_tmp && s->objs[h] == s->read_dev) {
            s->read_tmp->Release();
            s->read_tmp = nullptr;
            s->read_dev = nullptr;
        }
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
        bool is_buf = (d.kind == HWD3D9_VB || d.kind == HWD3D9_IB);
        if (!r.ok || !d.width || (is_buf ? d.width > (256u << 20) : d.width > 16384) || d.height > 16384 || d.depth > 2048 || d.levels > 16) return E_INVALIDARG;
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
        d9_probe_draw(c, dev, "draw", t, start, n);
        return dev->DrawPrimitive(D3DPRIMITIVETYPE(t), start, n);
    }
    case HWD3D9_DRAW_IDX: {
        uint32_t t = r.u32();
        int32_t base = int32_t(r.u32());
        uint32_t mn = r.u32(), nv = r.u32(), start = r.u32(), n = r.u32();
        d9_probe_draw(c, dev, "indexed draw", t, uint32_t(base), n);
        return dev->DrawIndexedPrimitive(D3DPRIMITIVETYPE(t), base, mn, nv, start, n);
    }
    case HWD3D9_DRAW_UP: {
        uint32_t t = r.u32(), n = r.u32(), stride = r.u32();
        uint64_t bytes = 0;
        const void* v = r.arr(1, &bytes);
        if (!r.ok || !v) return E_INVALIDARG;
        d9_probe_up(c, dev, "draw up", t, n, stride, v, bytes);
        return dev->DrawPrimitiveUP(D3DPRIMITIVETYPE(t), n, v, stride);
    }
    case HWD3D9_DRAW_IDX_UP: {
        uint32_t t = r.u32(), mn = r.u32(), nv = r.u32(), n = r.u32(), fmt = r.u32();
        uint64_t ib = 0, vb = 0;
        const void* idx = r.arr(1, &ib);
        uint32_t stride = r.u32();
        const void* v = r.arr(1, &vb);
        if (!r.ok || !idx || !v) return E_INVALIDARG;
        d9_probe_up(c, dev, "indexed draw up", t, n, stride, v, vb);
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
        HRESULT lock_hr = hr;
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
        if (FAILED(hr)) {
            static std::atomic<uint32_t> told{0};
            if (told.fetch_add(1) < 12) {
                D3DSURFACE_DESC sd{};
                sf->GetDesc(&sd);
                LOG("channel %u: write to kind %u sub %u failed 0x%x (lock 0x%x): format %u %ux%u pool %u usage 0x%x box %s %u,%u-%u,%u pitch %u bytes %llu",
                    c.slot, unsigned(k), sub, unsigned(hr), unsigned(lock_hr), unsigned(sd.Format), sd.Width, sd.Height, unsigned(sd.Pool),
                    unsigned(sd.Usage), bx.valid ? "yes" : "no", bx.left, bx.top, bx.right, bx.bottom, pitch, (unsigned long long)n);
            }
        }
        sf->Release();
        return hr;
    }
    case HWD3D9_READ: {
        uint32_t h = r.u32(), raw_sub = r.u32();
        {
            static std::atomic<uint32_t> n{0};
            uint32_t k = n.fetch_add(1);
            if (k < 3 || k % 50 == 0) LOG("channel %u: d9 read request #%u", c.slot, k);
        }
        bool staged_ok = (raw_sub & HWD3D_MAP_STAGED_OK) != 0;
        uint32_t sub = raw_sub & ~HWD3D_MAP_STAGED_OK;
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
            if (staged_ok && c.readback_ptr() && size <= HWD3D_READBACK_SIZE) {
                memcpy(c.readback_ptr(), p, size);
                w.u32(size | 0x80000000u);
            } else {
                w.u32(size);
                w.put(p, size);
            }
            k == D9K_VB ? static_cast<IDirect3DVertexBuffer9*>(s->objs[h])->Unlock() : static_cast<IDirect3DIndexBuffer9*>(s->objs[h])->Unlock();
            return 0;
        }
        IDirect3DSurface9* sf = d9_surface(s, h, sub);
        if (!sf) return E_NOTIMPL;
        D3DSURFACE_DESC sd;
        sf->GetDesc(&sd);
        D3DLOCKED_RECT lr;
        HRESULT hr = sf->LockRect(&lr, nullptr, D3DLOCK_READONLY);
        IDirect3DSurface9* locked = sf;
        if (FAILED(hr)) {
            // A render target: copied into a cached system-memory staging surface first.
            if (!s->read_tmp || s->read_dev != dev || s->read_w != sd.Width || s->read_h != sd.Height || s->read_fmt != sd.Format) {
                if (s->read_tmp) { s->read_tmp->Release(); s->read_tmp = nullptr; }
                hr = dev->CreateOffscreenPlainSurface(sd.Width, sd.Height, sd.Format, D3DPOOL_SYSTEMMEM, &s->read_tmp, nullptr);
                if (SUCCEEDED(hr)) {
                    s->read_dev = dev;
                    s->read_w = sd.Width;
                    s->read_h = sd.Height;
                    s->read_fmt = sd.Format;
                }
            } else {
                hr = S_OK;
            }
            if (SUCCEEDED(hr)) hr = dev->GetRenderTargetData(sf, s->read_tmp);
            if (SUCCEEDED(hr)) hr = s->read_tmp->LockRect(&lr, nullptr, D3DLOCK_READONLY);
            locked = s->read_tmp;
        }
        if (SUCCEEDED(hr)) {
            // Block-compressed formats are rows of 4x4 blocks.
            bool bc = sd.Format == D3DFMT_DXT1 || sd.Format == D3DFMT_DXT2 || sd.Format == D3DFMT_DXT3 || sd.Format == D3DFMT_DXT4 ||
                      sd.Format == D3DFMT_DXT5;
            uint32_t rows = bc ? (sd.Height + 3) / 4 : sd.Height;
            uint32_t total = uint32_t(lr.Pitch) * rows;
            // adb shell setprop debug.hwd3d.frames 1: says now and then how much of a frame read back is not black.
            static std::atomic<uint32_t> reads{0};
            if (!bc && sd.Width >= 640 && (reads.fetch_add(1) % 10) == 2) {
                char on[PROP_VALUE_MAX] = "";
                __system_property_get("debug.hwd3d.frames", on);
                if (on[0] == '1') {
                    uint32_t nz = 0, n = 0, sample = 0;
                    for (uint32_t y = 0; y < sd.Height; y += 8) {
                        auto* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch);
                        for (uint32_t x = 0; x < sd.Width; x += 8, n++)
                            if (row[x] & 0x00ffffffu) { nz++; if (!sample) sample = row[x]; }
                    }
                    LOG("channel %u: frame read back, format %u %ux%u, %u of %u sampled pixels not black, one is 0x%08x",
                        c.slot, unsigned(sd.Format), sd.Width, sd.Height, nz, n, sample);
                    // And every render-target texture the program has: is anything drawn into them at all?
                    static uint32_t rounds;
                    if (rounds++ % 5 == 0) {
                        for (uint32_t oh = 1; oh < s->objs.size(); oh++) {
                            if (s->kinds[oh] != D9K_TEX || !s->objs[oh]) continue;
                            auto* tx = static_cast<IDirect3DTexture9*>(s->objs[oh]);
                            D3DSURFACE_DESC td{};
                            if (FAILED(tx->GetLevelDesc(0, &td)) || !(td.Usage & D3DUSAGE_RENDERTARGET) || td.Width < 64) continue;
                            IDirect3DSurface9* lvl = nullptr;
                            IDirect3DSurface9* sys = nullptr;
                            uint64_t nonzero = 0, bytes = 0;
                            HRESULT rh = tx->GetSurfaceLevel(0, &lvl);
                            if (SUCCEEDED(rh)) rh = dev->CreateOffscreenPlainSurface(td.Width, td.Height, td.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr);
                            if (SUCCEEDED(rh)) rh = dev->GetRenderTargetData(lvl, sys);
                            D3DLOCKED_RECT tl{};
                            if (SUCCEEDED(rh) && SUCCEEDED(rh = sys->LockRect(&tl, nullptr, D3DLOCK_READONLY))) {
                                for (uint32_t y = 0; y < td.Height; y += 4) {
                                    auto* row = static_cast<const uint8_t*>(tl.pBits) + size_t(y) * tl.Pitch;
                                    for (uint32_t b = 0; b < uint32_t(tl.Pitch); b += 16, bytes++) if (row[b] | row[b + 1] | row[b + 2] | row[b + 4] | row[b + 5]) nonzero++;
                                }
                                sys->UnlockRect();
                            }
                            LOG("channel %u:   render target texture %u format %u %ux%u: %llu of %llu samples not zero (0x%x)", c.slot, oh,
                                unsigned(td.Format), td.Width, td.Height, (unsigned long long)nonzero, (unsigned long long)bytes, unsigned(rh));
                            if (sys) sys->Release();
                            if (lvl) lvl->Release();
                        }
                    }
                }
            }
            w.u32(uint32_t(lr.Pitch));
            w.u32(rows);
            if (staged_ok && c.readback_ptr() && total <= HWD3D_READBACK_SIZE) {
                memcpy(c.readback_ptr(), lr.pBits, size_t(total));
                w.u32(total | 0x80000000u);
            } else {
                w.u32(total);
                w.put(lr.pBits, size_t(total));
            }
            locked->UnlockRect();
        }
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
        if (!q) return E_INVALIDARG;
        HRESULT hr = q->Issue(flags);
        if (SUCCEEDED(hr) && flags == D3DISSUE_END && r.end - r.p >= 8 && c.stage &&
            c.stage_size > HWD3D_STAGING_DATA + HWD3D_QUERY_PAGE) {
            uint32_t slot = r.u32(), seq = r.u32();
            if (slot < HWD3D_QUERY_SLOTS && seq) {
                q->GetData(nullptr, 0, D3DGETDATA_FLUSH);
                for (auto p = c.pending_d9.begin(); p != c.pending_d9.end(); ++p) {
                    if (p->slot == slot) {
                        p->q->Release();
                        c.pending_d9.erase(p);
                        break;
                    }
                }
                q->AddRef();
                c.pending_d9.push_back({q, slot, seq});
            }
        }
        return hr;
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
    case HWD3D9_PRESENT: {
        uint32_t sh = r.u32(), raw_sub = r.u32();
        {
            static std::atomic<uint32_t> n{0};
            uint32_t k = n.fetch_add(1);
            if (k < 3 || k % 50 == 0) LOG("channel %u: d9 present request #%u surface %u sub 0x%x", c.slot, k, sh, raw_sub);
        }
        if (!sh) {
            hwgpu_host_hide_game_buffer(c.slot);
            return 0;
        }
        const bool overlay = (raw_sub & 0x80000000u) != 0;
        uint32_t ssub = raw_sub & ~0x80000000u;
        int32_t ov_x = 0, ov_y = 0;
        uint32_t ov_w = 0, ov_h = 0, ov_crop[4] = {0, 0, 0, 0};
        if (overlay) {
            ov_x = int32_t(r.u32());
            ov_y = int32_t(r.u32());
            ov_w = r.u32();
            ov_h = r.u32();
            ov_crop[0] = r.u32();
            ov_crop[1] = r.u32();
            ov_crop[2] = r.u32();
            ov_crop[3] = r.u32();
        }
        IDirect3DSurface9* sf = d9_surface(s, sh, ssub);
        if (!sf) return E_INVALIDARG;
        D3DSURFACE_DESC sd{};
        sf->GetDesc(&sd);
        const Dxvk& dx = chan_dxvk(c);
        // adb shell setprop debug.hwd3d.noscan 1: no frame goes to a layer of its own, so the program
        // falls back to reading its frames back. For telling a present fault from a drawing fault.
        char noscan[PROP_VALUE_MAX] = "";
        __system_property_get("debug.hwd3d.noscan", noscan);
        if (noscan[0] == '1' || !sd.Width || !sd.Height || !dx.d9_get_vk_handles || !dx.d9_blit_to_vkimage) {
            sf->Release();
            return E_NOTIMPL;
        }
        if (c.scan_dev != dev || c.scan_w != sd.Width || c.scan_h != sd.Height || !c.scan_ring[0].vk_img) {
            c.free_scan_ring();
            VkInstance inst = VK_NULL_HANDLE;
            VkPhysicalDevice phys = VK_NULL_HANDLE;
            VkDevice vkd = VK_NULL_HANDLE;
            dx.d9_get_vk_handles(dev, &inst, &phys, &vkd);
            PFN_vkGetInstanceProcAddr gipa = vk_gipa(c.is_desktop);
            auto gdpa = (gipa && inst) ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(inst, "vkGetDeviceProcAddr")) : nullptr;
#define VKF(name) auto name = (gdpa && vkd) ? reinterpret_cast<PFN_##name>(gdpa(vkd, #name)) : nullptr
            VKF(vkGetAndroidHardwareBufferPropertiesANDROID);
            VKF(vkCreateImage);
            VKF(vkAllocateMemory);
            VKF(vkBindImageMemory);
            VKF(vkDestroyImage);
            VKF(vkFreeMemory);
#undef VKF
            if (!vkGetAndroidHardwareBufferPropertiesANDROID || !vkCreateImage || !vkAllocateMemory ||
                !vkBindImageMemory || !vkDestroyImage || !vkFreeMemory) {
                sf->Release();
                return E_NOTIMPL;
            }
            static const bool custom_driver = getenv("HWGPU_VK_DRIVER") && *getenv("HWGPU_VK_DRIVER");
            for (int i = 0; i < 3; i++) {
                // The backbuffer's own byte order where it is BGRA (A8R8G8B8, X8R8G8B8), so DXVK copies
                // instead of blitting: a blit into this buffer lost the device on Mali. 5 is BGRA_8888.
                const bool bgra = sd.Format == D3DFMT_A8R8G8B8 || sd.Format == D3DFMT_X8R8G8B8;
                AHardwareBuffer_Desc ad{sd.Width, sd.Height, 1, bgra ? 5u : uint32_t(AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM),
                                        AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                                            AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY |
                                            (!c.is_desktop && custom_driver ? AHARDWAREBUFFER_USAGE_CPU_READ_RARELY : 0),
                                        0, 0, 0};
                if (AHardwareBuffer_allocate(&ad, &c.scan_ring[i].ahb) != 0) {
                    ad.usage &= ~uint64_t(AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY);
                    if (AHardwareBuffer_allocate(&ad, &c.scan_ring[i].ahb) != 0) {
                        ad.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;  // no BGRA here: a blit then
                        if (AHardwareBuffer_allocate(&ad, &c.scan_ring[i].ahb) == 0) goto allocated;
                        c.free_scan_ring();
                        sf->Release();
                        return E_OUTOFMEMORY;
                    }
                }
            allocated:
                VkAndroidHardwareBufferFormatPropertiesANDROID fp{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
                VkAndroidHardwareBufferPropertiesANDROID props{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID, &fp};
                if (vkGetAndroidHardwareBufferPropertiesANDROID(vkd, c.scan_ring[i].ahb, &props) != VK_SUCCESS ||
                    fp.format == VK_FORMAT_UNDEFINED) {
                    c.free_scan_ring();
                    sf->Release();
                    return E_FAIL;
                }
                VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, nullptr,
                                                    VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
                VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext};
                ci.imageType = VK_IMAGE_TYPE_2D;
                ci.format = fp.format;
                ci.extent = {sd.Width, sd.Height, 1};
                ci.mipLevels = 1;
                ci.arrayLayers = 1;
                ci.samples = VK_SAMPLE_COUNT_1_BIT;
                ci.tiling = VK_IMAGE_TILING_OPTIMAL;
                ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
                ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                if (vkCreateImage(vkd, &ci, nullptr, &c.scan_ring[i].vk_img) != VK_SUCCESS) {
                    c.free_scan_ring();
                    sf->Release();
                    return E_FAIL;
                }
                c.scan_ring[i].vk_dev = vkd;
                c.scan_ring[i].vk_fmt = uint32_t(fp.format);
                c.scan_ring[i].destroy_image = vkDestroyImage;
                c.scan_ring[i].free_memory = vkFreeMemory;
                VkImportAndroidHardwareBufferInfoANDROID imp{VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
                                                             nullptr, c.scan_ring[i].ahb};
                VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &imp,
                                                  c.scan_ring[i].vk_img, VK_NULL_HANDLE};
                VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ded, props.allocationSize, 0};
                while (ai.memoryTypeIndex < 32 && !(props.memoryTypeBits & (1u << ai.memoryTypeIndex))) ai.memoryTypeIndex++;
                if (vkAllocateMemory(vkd, &ai, nullptr, &c.scan_ring[i].vk_mem) != VK_SUCCESS ||
                    vkBindImageMemory(vkd, c.scan_ring[i].vk_img, c.scan_ring[i].vk_mem, 0) != VK_SUCCESS) {
                    c.free_scan_ring();
                    sf->Release();
                    return E_FAIL;
                }
            }
            c.scan_dev = dev;
            c.scan_w = sd.Width;
            c.scan_h = sd.Height;
            LOG("channel %u: fullscreen D3D9 scanout ring %ux%u ready", c.slot, sd.Width, sd.Height);
        }
        auto& buf = c.scan_ring[c.scan_idx++ % 3];
        // adb shell setprop debug.hwd3d.scantest 1: no copy into the buffer (it shows as it is);
        // 2: the copy, but the buffer is not shown. For telling which half faults.
        char scantest[PROP_VALUE_MAX] = "";
        __system_property_get("debug.hwd3d.scantest", scantest);
        HRESULT hr = scantest[0] == '1' ? S_OK : dx.d9_blit_to_vkimage(dev, sf, buf.vk_img, buf.vk_fmt, sd.Width, sd.Height);
        if (scantest[0] == '2') {
            sf->Release();
            return E_NOTIMPL;
        }
        sf->Release();
        if (SUCCEEDED(hr)) {
            if (overlay) hwgpu_host_show_window_buffer(c.slot, buf.ahb, sd.Width, sd.Height, ov_x, ov_y, ov_w, ov_h, ov_crop);
            else hwgpu_host_show_game_buffer(c.slot, c.generation, buf.ahb, sd.Width, sd.Height, hwgpu_host_vsync());
            c.presents++;
        }
        return hr;
    }
    }
    return E_NOTIMPL;
}
