#!/usr/bin/env python3
"""HwD3D's call thunks, both sides (d3d/README.md), from the mingw COM headers.

  gen_thunks.py <mingw include dir> <out dir>

Writes <out>/hwd3d_guest.inc (C, the thin DLLs in Windows) and <out>/hwd3d_host.inc (C++, libhwd3d.so
on DXVK). A method call crosses as HWD3D_OP_CALL: the object's handle, a method number, then each
argument by value (include/hw_d3d.h). The host looks the handle up and calls the same method on the
DXVK object. Outputs come back in the answer; a method with none returns nothing and is not waited
for.

Each argument gets a kind from its type and name, as LinD3D's gen_dx_thunks.py did. A method with an
argument no kind fits is not sent: its guest side logs once and returns E_NOTIMPL (or nothing). Methods
in CUSTOM are written by hand on both sides (gpu/guest/hwd3d/d3d11.c, gpu/host/hwd3d/hwd3d.cpp).
"""
import re
import sys
from pathlib import Path

# Every interface the guest can hold, base before derived. The headers they are in.
HEADERS = ["d3d11.h", "dxgi.h"]
IFACES = [
    "IUnknown",
    # DXGI
    "IDXGIObject", "IDXGIDeviceSubObject", "IDXGIResource", "IDXGISurface", "IDXGIAdapter", "IDXGIAdapter1",
    "IDXGIOutput", "IDXGISwapChain", "IDXGIFactory", "IDXGIFactory1", "IDXGIDevice", "IDXGIDevice1",
    # D3D11
    "ID3D11DeviceChild", "ID3D11DepthStencilState", "ID3D11BlendState", "ID3D11RasterizerState",
    "ID3D11Resource", "ID3D11Buffer", "ID3D11Texture1D", "ID3D11Texture2D", "ID3D11Texture3D", "ID3D11View",
    "ID3D11ShaderResourceView", "ID3D11RenderTargetView", "ID3D11DepthStencilView", "ID3D11UnorderedAccessView",
    "ID3D11VertexShader", "ID3D11HullShader", "ID3D11DomainShader", "ID3D11GeometryShader", "ID3D11PixelShader",
    "ID3D11ComputeShader", "ID3D11InputLayout", "ID3D11SamplerState", "ID3D11Asynchronous", "ID3D11Query",
    "ID3D11Predicate", "ID3D11Counter", "ID3D11ClassInstance", "ID3D11ClassLinkage", "ID3D11CommandList",
    "ID3D11DeviceContext", "ID3D11Device",
]

# Written by hand. Guest only: the guest answers it alone. Both: the call crosses in a form of its own.
GUEST_ONLY = {("IUnknown", "AddRef"), ("IUnknown", "Release"), ("IDXGIOutput", "GetDesc"),
              ("IDXGIOutput", "GetDisplayModeList"), ("IDXGIOutput", "FindClosestMatchingMode")}
CUSTOM = {
    ("IUnknown", "QueryInterface"), ("ID3D11Device", "CreateBuffer"), ("ID3D11Device", "CreateTexture1D"), ("ID3D11Device", "CreateTexture2D"),
    ("ID3D11Device", "CreateTexture3D"), ("ID3D11Device", "CreateInputLayout"),
    ("ID3D11DeviceContext", "Map"), ("ID3D11DeviceContext", "Unmap"), ("ID3D11DeviceContext", "UpdateSubresource"),
    ("ID3D11DeviceContext", "End"), ("ID3D11DeviceContext", "GetData"),
    ("IDXGIFactory", "CreateSwapChain"), ("IDXGISwapChain", "GetDesc"), ("IDXGISwapChain", "Present"), ("IDXGIAdapter", "GetDesc"),
    ("IDXGIAdapter1", "GetDesc1"),
}

COUNT = re.compile(r"^(Num[A-Z]\w*|NumElements)$")
SIZES = {"UINT64": 8, "SIZE_T": 8, "INT64": 8, "LARGE_INTEGER": 8, "ULONGLONG": 8, "LONGLONG": 8, "DOUBLE": 8}


def parse(inc):
    text = "".join((inc / h).read_text() for h in HEADERS)
    parent = dict(re.findall(r'MIDL_INTERFACE\("[^"]+"\)\s*(\w+)\s*:\s*public\s+(\w+)', text))
    iids = {}
    for name, g in re.findall(r"DEFINE_GUID\(IID_(\w+),\s*([^)]*)\)", text):
        iids[name] = g
    iids["IUnknown"] = "0x00000000,0x0000,0x0000,0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46"
    methods = {}
    for iface in IFACES:
        if iface == "IUnknown":
            methods[iface] = [("IUnknown", "HRESULT", "QueryInterface", [("REFIID", "riid", None), ("void **", "ppvObject", None)]),
                              ("IUnknown", "ULONG", "AddRef", []), ("IUnknown", "ULONG", "Release", [])]
            continue
        m = re.search(r"typedef struct " + iface + r"Vtbl \{(.*?)\} " + iface + r"Vtbl;", text, re.S)
        if not m:
            raise SystemExit("missing " + iface + "Vtbl")
        body, out, owner = m.group(1), [], None
        for mm in re.finditer(r"/\*\*\* (\w+) methods \*\*\*/|([A-Za-z_][\w\s\*]*?)\s*\(STDMETHODCALLTYPE \*(\w+)\)\s*\((.*?)\);", body, re.S):
            if mm.group(1):
                owner = mm.group(1)
                continue
            raw = [" ".join(a.split()) for a in mm.group(4).split(",")][1:]
            args = []
            for a in raw:
                m2 = re.match(r"^(.*?)([A-Za-z_]\w*)(?:\[(\d+)\])?$", a)
                args.append((m2.group(1).strip(), m2.group(2), int(m2.group(3)) if m2.group(3) else None))
            out.append((owner, " ".join(mm.group(2).split()), mm.group(3), args))
        methods[iface] = out
    return parent, iids, methods


def classify(args, mname):
    """One kind per argument, or None when one fits no kind. A kind is a tuple (name, ...)."""
    kinds = []
    count = None  # index of the count argument the arrays that follow use
    for i, (typ, name, fixed) in enumerate(args):
        const = "const" in typ
        t = re.sub(r"\bconst\b|\bstruct\b", "", typ)
        t = re.sub(r"\s*\*\s*", "*", t).strip()
        base = t.replace("*", "").strip()
        depth = t.count("*")
        prev = args[i - 1] if i else None
        nxt = args[i + 1] if i + 1 < len(args) else None
        iface = base in IFACES
        if prev and "*" not in prev[0] and COUNT.match(prev[1]):
            count = i - 1
        after = i + 1 if nxt and "*" not in nxt[0] and COUNT.match(nxt[1]) else None
        if fixed:
            kinds.append(("arr_in" if const else "arr_out", base, str(fixed), True))
            continue
        if depth == 0:
            if base in ("REFIID", "REFGUID"):
                kinds.append(("guid",))
            elif base == "HWND":
                kinds.append(("hwnd",))
            elif base in ("HANDLE", "HMODULE", "HDC"):
                return None
            else:
                kinds.append(("val", SIZES.get(base, 4), typ))
            continue
        if iface and depth == 1:
            kinds.append(("iface_in", base))
        elif iface and depth == 2 and (const or re.search(r"\*\s*const\s*\*", typ)):
            n = count if count is not None else after
            if n is None:
                return None
            kinds.append(("iface_arr_in", base, n))
        elif iface and depth == 2:
            if nxt and nxt[1].startswith("pNum"):  # VSGetShader's class instances: none
                kinds.append(("none_out", nxt[1]))
            elif count is not None and prev is not None and (i - 1 == count or kinds[-1][0].endswith("arr_in") or kinds[-1][0].endswith("arr_out")):
                kinds.append(("iface_arr_out", base, count))
            else:
                kinds.append(("iface_out", base))
        elif base == "void" and depth == 2:
            if prev and prev[0] in ("REFIID",):
                kinds.append(("riid_out", i - 1))
            else:
                return None
        elif base == "void" and depth == 1:
            if nxt and re.search(r"(Length|Size)$", nxt[1]) and "*" not in nxt[0]:
                kinds.append(("buf_in", i + 1) if const else ("buf_inout" if mname == "CheckFeatureSupport" else "buf_out", i + 1))
            elif prev and re.match(r"(?i)data_?size$", prev[1]) and "*" not in prev[0] and const:
                kinds.append(("buf_in", i - 1))
            else:
                return None
        elif kinds and kinds[-1][0] == "none_out":
            kinds.append(("skip",))
        elif depth == 1 and base in ("char", "LPSTR", "HWND", "HANDLE", "HDC", "HMONITOR", "WCHAR"):
            return None
        elif depth == 1 and (i - 1 == count or (kinds and kinds[-1][0] in ("arr_in", "iface_arr_in", "arr_out", "iface_arr_out") and count is not None)) and prev is not None:
            kinds.append(("arr_in" if const else "arr_out", base, count, False))
        elif depth == 1 and after is not None and const:
            kinds.append(("arr_in", base, after, False))
        elif depth == 1 and const:
            kinds.append(("ptr_in", base))
        elif depth == 1:
            if prev and prev[0].replace(" ", "") == "UINT*" and prev[1].startswith("pNum"):
                return None  # a count in and out
            kinds.append(("ptr_out", base))
        else:
            return None
    return kinds


def has_out(kinds):
    return any(k[0] in ("iface_out", "riid_out", "buf_out", "buf_inout", "ptr_out", "arr_out", "iface_arr_out") for k in kinds)


def emit(inc, out):
    parent, iids, methods = parse(inc)
    idx = {n: i for i, n in enumerate(IFACES)}
    # Methods by the interface that declares them, numbered.
    decl, num = [], {}
    for iface in IFACES:
        for owner, ret, name, args in methods[iface]:
            if (owner, name) not in num:
                num[(owner, name)] = len(decl)
                decl.append((owner, ret, name, args))
    g, h, gp, hp = [], [], [], []
    for w in (gp, hp):
        w.append("/* Generated by gpu/host/hwd3d/gen_thunks.py. Do not edit. */")
        w.append("enum { " + ", ".join("HI_%s = %d" % (n, i) for i, n in enumerate(IFACES)) + ", HI_COUNT = %d };" % len(IFACES))
        w.append("static const int16_t hi_parent[] = {" + ", ".join(str(idx.get(parent.get(n, ""), -1)) for n in IFACES) + "};")
        w.append("static const GUID hi_iid[] = {" + ", ".join("{%s}" % fmt_guid(iids[n]) for n in IFACES) + "};")
    structs = set()

    for mid, (owner, ret, name, args) in enumerate(decl):
        key = (owner, name)
        kinds = None if key in GUEST_ONLY or key in CUSTOM else classify(args, name)
        params = "".join(", %s %s%s" % (t, n, "[%d]" % f if f else "") for t, n, f in args)
        sig = "static %s STDMETHODCALLTYPE m_%s_%s(void *This%s)" % (ret, owner, name, params)
        gp.append("%s;" % sig)
        if key in GUEST_ONLY or key in CUSTOM:
            if key in CUSTOM:
                hp.append("int32_t hc_%s_%s(Chan &c, void *self, Rd &r, Wr &w);" % (owner, name))
            continue
        if kinds is None:
            g.append("%s {" % sig)
            g.append('    unsupported("%s::%s");' % (owner, name))
            if ret == "void":
                pass
            elif ret == "HRESULT":
                g.append("    return E_NOTIMPL;")
            else:
                g.append("    return 0;")
            g.append("}")
            continue
        wait = ret != "void" or has_out(kinds)
        # Guest: write the call.
        g.append("%s {" % sig)
        g.append("    struct wbuf *b = call_begin(This, %d);" % mid)
        for (typ, an, fixed), k in zip(args, kinds):
            kind = k[0]
            if kind == "val":
                g.append("    put(b, &%s, %d);" % (an, k[1]))
            elif kind == "guid":
                g.append("    put(b, %s, 16);" % an)
            elif kind == "hwnd":
                pass
            elif kind == "iface_in":
                g.append("    if (!put_obj(b, %s)) { call_abort(b); return %s; }" % (an, ret_fail(ret)))
            elif kind == "iface_arr_in":
                cn = args[k[2]][1]
                g.append("    if (!put_objs(b, (void *const *)%s, %s)) { call_abort(b); return %s; }" % (an, cn, ret_fail(ret)))
            elif kind == "arr_in":
                n = k[2] if k[3] else args[k[2]][1]
                g.append("    put_arr(b, %s, %s, sizeof(*%s));" % (an, n, an))
                structs.add(k[1])
            elif kind == "ptr_in":
                g.append("    put_arr(b, %s, 1, sizeof(*%s));" % (an, an))
                structs.add(k[1])
            elif kind == "buf_in":
                g.append("    put_arr(b, %s, %s, 1);" % (an, args[k[1]][1]))
            elif kind in ("buf_inout",):
                g.append("    put_arr(b, %s, %s, 1);" % (an, args[k[1]][1]))
            elif kind in ("iface_out", "riid_out", "ptr_out", "buf_out", "arr_out", "iface_arr_out"):
                g.append("    put_want(b, %s);" % an)
                if kind in ("ptr_out", "arr_out"):
                    structs.add(k[1])
            elif kind == "none_out":
                g.append("    if (%s) *%s = 0;" % (k[1], k[1]))
            elif kind == "skip":
                pass
        if not wait:
            g.append("    call_async(b);")
            g.append("}")
        else:
            g.append("    struct rd r;")
            g.append("    int32_t st = call_wait(b, &r);")
            for (typ, an, fixed), k in zip(args, kinds):
                kind = k[0]
                if kind == "iface_out":
                    g.append("    take_obj(&r, (void **)%s, HI_%s);" % (an, k[1]))
                elif kind == "riid_out":
                    g.append("    take_obj_riid(&r, %s, %s);" % (an, args[k[1]][1]))
                elif kind == "iface_arr_out":
                    g.append("    take_objs(&r, (void **)%s, %s, HI_%s);" % (an, args[k[2]][1], k[1]))
                elif kind == "ptr_out":
                    g.append("    take_arr(&r, %s, 1, sizeof(*%s));" % (an, an))
                elif kind == "arr_out":
                    n = k[2] if k[3] else args[k[2]][1]
                    g.append("    take_arr(&r, %s, %s, sizeof(*%s));" % (an, n, an))
                elif kind in ("buf_out", "buf_inout"):
                    g.append("    take_arr(&r, %s, %s, 1);" % (an, args[k[1]][1]))
            g.append("    call_end(&r);")
            if ret == "void":
                g.append("    (void)st;")
            elif ret in ("HRESULT", "UINT", "ULONG", "BOOL", "INT") or ret.startswith(("D3D", "DXGI")):
                g.append("    return (%s)st;" % ret)
            else:
                g.append("    %s v; memcpy(&v, &st, sizeof v); return v;" % ret)
            g.append("}")

        # Host: read the arguments in order, then make room for outputs (a size may come later in
        # the list than the pointer it sizes), make the call, write the outputs.
        h.append("static int32_t hm_%d(Chan &c, void *self, Rd &r, Wr &w) {  // %s::%s" % (mid, owner, name))
        h.append("    (void)c; (void)w;")
        reads, prep, call, post = [], [], [], []
        def cnt(ci, ai):
            return "a%d" % ci
        for ai, ((typ, an, fixed), k) in enumerate(zip(args, kinds)):
            kind = k[0]
            v = "a%d" % ai
            if kind == "val":
                reads.append("%s %s; r.get(&%s, %d);" % (re.sub(r"\bconst\b", "", typ).strip(), v, v, k[1]))
                call.append(v)
            elif kind == "guid":
                reads.append("GUID %s; r.get(&%s, 16);" % (v, v))
                call.append(v)
            elif kind == "hwnd":
                call.append("nullptr")
            elif kind == "iface_in":
                reads.append("void *%sp = c.obj(r.u32(), HI_%s);" % (v, k[1]))
                prep.append("if (%sp == BAD) return E_INVALIDARG;" % v)
                call.append("(%s *)%sp" % (k[1], v))
            elif kind == "iface_arr_in":
                reads.append("std::vector<%s *> %s; bool %sp = c.objs(r, %s, HI_%s);" % (k[1], v, v, v, k[1]))
                prep.append("if (!%sp || (!%s.empty() && %s.size() != size_t(a%d))) return E_INVALIDARG;" % (v, v, v, k[2]))
                call.append("%s.empty() ? nullptr : %s.data()" % (v, v))
            elif kind in ("arr_in", "ptr_in", "buf_in", "buf_inout"):
                el = "1" if kind.startswith("buf") else "sizeof(%s)" % k[1]
                if kind == "ptr_in":
                    n = "1"
                elif kind == "arr_in" and k[3]:
                    n = k[2]
                else:
                    n = "uint64_t(a%d)" % (k[2] if kind == "arr_in" else k[1])
                reads.append("uint64_t %sn = 0; const void *%s = r.arr(%s, &%sn);" % (v, v, el, v))
                prep.append("if (%s && %sn != %s) return E_INVALIDARG;" % (v, v, n))
                if kind == "buf_inout":
                    prep.append("std::vector<uint8_t> %sb((const uint8_t *)%s, (const uint8_t *)%s + (%s ? %sn : 0));" % (v, v, v, v, v))
                    call.append("%s ? (void *)%sb.data() : nullptr" % (v, v))
                    post.append("w.arr(%s ? %sb.data() : nullptr, %sb.size());" % (v, v, v))
                elif kind == "buf_in":
                    call.append("(const void *)%s" % v)
                else:
                    call.append("(const %s *)%s" % (k[1], v))
            elif kind == "iface_out":
                reads.append("bool %sw = r.u32() != 0; %s *%s = nullptr;" % (v, k[1], v))
                call.append("%sw ? &%s : nullptr" % (v, v))
                post.append("w.u32(c.put(%s, HI_%s));" % (v, k[1]))
            elif kind == "riid_out":
                reads.append("bool %sw = r.u32() != 0; void *%s = nullptr;" % (v, v))
                prep.append("int %si = iface_of(a%d); if (%si < 0) return E_NOINTERFACE;" % (v, k[1], v))
                call.append("%sw ? &%s : nullptr" % (v, v))
                post.append("w.u32(c.put(%s, %si)); w.u32(uint32_t(%si));" % (v, v, v))
            elif kind == "iface_arr_out":
                reads.append("bool %sw = r.u32() != 0;" % v)
                prep.append("if (%sw && a%d > 256) return E_INVALIDARG;" % (v, k[2]))
                prep.append("std::vector<%s *> %s(%sw ? a%d : 0);" % (k[1], v, v, k[2]))
                call.append("%sw ? %s.data() : nullptr" % (v, v))
                post.append("for (auto *o : %s) w.u32(c.put(o, HI_%s));" % (v, k[1]))
            elif kind in ("ptr_out", "arr_out", "buf_out"):
                if kind == "ptr_out":
                    n, el = "1", "sizeof(%s)" % k[1]
                elif kind == "arr_out":
                    n, el = (str(k[2]) if k[3] else "a%d" % k[2]), "sizeof(%s)" % k[1]
                else:
                    n, el = "a%d" % k[1], "1"
                reads.append("bool %sw = r.u32() != 0;" % v)
                prep.append("if (%sw && uint64_t(%s) * %s > (64u << 20)) return E_INVALIDARG;" % (v, n, el))
                prep.append("std::vector<uint8_t> %s(%sw ? size_t(%s) * %s : 0);" % (v, v, n, el))
                ct = "void *" if kind == "buf_out" else "%s *" % k[1]
                call.append("%sw ? (%s)%s.data() : nullptr" % (v, ct, v))
                post.append("w.arr(%sw ? %s.data() : nullptr, %s.size());" % (v, v, v))
            elif kind in ("none_out", "skip"):
                call.append("nullptr")
        h.extend("    " + x for x in reads)
        h.append("    if (!r.ok) return E_INVALIDARG;")
        h.extend("    " + x for x in prep)
        target = "((%s *)self)->%s(%s)" % (owner, name, ", ".join(call))
        if ret == "void":
            h.append("    %s;" % target)
            h.extend("    " + x for x in post)
            h.append("    return 0;")
        else:
            h.append("    %s ret = %s;" % (ret, target))
            h.extend("    " + x for x in post)
            h.append("    static_assert(sizeof ret <= 4, \"ret\");")
            h.append("    int32_t st = 0; memcpy(&st, &ret, sizeof ret); return st;")
        h.append("}")

    # Method table, both sides.
    for iface in IFACES:
        gp.append("static const void *const vt_%s[] = {" % iface)
        gp.append("    " + ", ".join("(const void *)m_%s_%s" % (o, n) for o, _, n, _ in methods[iface]))
        gp.append("};")
    gp.append("static const void *const *const hi_vtbl[] = {" + ", ".join("vt_" + n for n in IFACES) + "};")
    h.append("struct HostMethod { HostFn fn; int16_t owner; bool wait; };")
    rows = []
    for mid, (owner, ret, name, args) in enumerate(decl):
        key = (owner, name)
        if key in GUEST_ONLY:
            rows.append("{nullptr, HI_%s, true}" % owner)
        elif key in CUSTOM:
            rows.append("{hc_%s_%s, HI_%s, true}" % (owner, name, owner))
        else:
            kinds = classify(args, name)
            if kinds is None:
                rows.append("{nullptr, HI_%s, true}" % owner)
            else:
                rows.append("{hm_%d, HI_%s, %s}" % (mid, owner, "true" if ret != "void" or has_out(kinds) else "false"))
    h.append("static const HostMethod hm_table[] = {\n    " + ",\n    ".join(rows) + "\n};")
    for w in (gp, hp):
        w.append("enum { HM_COUNT = %d };" % len(decl))
        for mid, (o, _, n, _) in enumerate(decl):
            if (o, n) in CUSTOM or (o, n) in GUEST_ONLY:
                w.append("#define HM_%s_%s %d" % (o, n, mid))
    # Every method's number, for code that makes the calls itself (the Direct3D driver, umd.cpp).
    (out / "hwd3d_methods.h").write_text("\n".join(["/* Generated by gen_thunks.py. Do not edit. */"] +
        ["#define HMX_%s_%s %d" % (o, n, mid) for mid, (o, _, n, _) in enumerate(decl)]) + "\n")
    # Struct sizes as the guest sees them, checked on both sides by the build (structs.h).
    (out / "hwd3d_structs.txt").write_text("\n".join(sorted(structs)) + "\n")
    (out / "hwd3d_guest_pre.inc").write_text("\n".join(gp) + "\n")
    (out / "hwd3d_guest.inc").write_text("\n".join(g) + "\n")
    (out / "hwd3d_host_pre.inc").write_text("\n".join(hp) + "\n")
    (out / "hwd3d_host.inc").write_text("\n".join(h) + "\n")


def ret_fail(ret):
    return {"void": "", "HRESULT": "E_INVALIDARG"}.get(ret, "0")


def fmt_guid(s):
    p = [x.strip() for x in s.split(",")]
    return "%s, %s, %s, {%s}" % (p[0], p[1], p[2], ", ".join(p[3:11]))


if __name__ == "__main__":
    o = Path(sys.argv[2])
    o.mkdir(parents=True, exist_ok=True)
    emit(Path(sys.argv[1]), o)
