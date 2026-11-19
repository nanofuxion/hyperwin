#!/usr/bin/env python3
"""Reads the Direct3D 10/11 driver interface header (from the WDK, in gpu/work/wdk, never committed) and
writes what the user-mode driver needs at build time:
  umd_tables.inc  one UMD_FILL(table, member) line per function table entry, so every entry exists
                  from the first build (hwd3d_umd.cpp points the ones it has not written at a stub)
  umd_proto.txt   each entry's member name and prototype, for reading while writing entries by hand
Nothing here is copied into a source file we keep: the output stays under gpu/work.
  umd9_tables.inc one UMD9_FILL(member) line per entry of the Direct3D 9 driver's table (d3dumddi.h, the
                  second argument when given)
  gen_umd.py <d3d10umddi.h> <outdir> [d3dumddi.h]"""
import re, sys, os

hdr, out = sys.argv[1], sys.argv[2]
src = open(hdr, encoding="utf-8", errors="replace").read()
src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
src = re.sub(r"//[^\n]*", "", src)
src = re.sub(r"#ifdef D3D10PSGP.*?#endif[^\n]*", "", src, flags=re.S)  # entries only the old pre-processing path had

# typedef RET (APIENTRY *PFNNAME)( args );
protos = {}
for m in re.finditer(r"typedef\s+([\w\s\*]+?)\s*\(\s*(?:APIENTRY|CALLBACK|__stdcall)\s*\*\s*(PFN\w+)\s*\)\s*\((.*?)\)\s*;", src, flags=re.S):
    protos[m.group(2)] = (" ".join(m.group(1).split()), " ".join(m.group(3).split()))

TABLES = ["D3D11DDI_DEVICEFUNCS", "D3D10_1DDI_DEVICEFUNCS", "D3D10DDI_DEVICEFUNCS", "D3D11_1DDI_DEVICEFUNCS", "D3DWDDM1_3DDI_DEVICEFUNCS"]
os.makedirs(out, exist_ok=True)
inc, txt = [], []
for t in TABLES:
    m = re.search(r"typedef\s+struct\s+%s\s*\{(.*?)\}\s*%s\s*;" % (t, t), src, flags=re.S)
    if not m:
        sys.exit("no table " + t)
    body = m.group(1)
    body = re.sub(r"#\s*\w+[^\n]*", "", body)
    txt.append("== " + t)
    for fm in re.finditer(r"(PFN\w+)\s+(pfn\w+)\s*;", body):
        pfn, name = fm.group(1), fm.group(2)
        inc.append("UMD_FILL(%s, %s)" % (t, name))
        r = protos.get(pfn)
        txt.append("%s %s: %s (%s)" % (pfn, name, r[0] if r else "?", r[1] if r else "?"))
open(os.path.join(out, "umd_tables.inc"), "w").write("\n".join(inc) + "\n")
open(os.path.join(out, "umd_proto.txt"), "w").write("\n".join(txt) + "\n")
print(len(inc), "table entries")

# The Direct3D 9 driver's table (wddm/README.md phase 7).
if len(sys.argv) > 3:
    s9 = open(sys.argv[3], encoding="utf-8", errors="replace").read()
    s9 = re.sub(r"/\*.*?\*/", "", s9, flags=re.S)
    s9 = re.sub(r"//[^\n]*", "", s9)
    m = re.search(r"typedef\s+struct\s+_D3DDDI_DEVICEFUNCS\s*\{(.*?)\}\s*D3DDDI_DEVICEFUNCS\s*;", s9, flags=re.S)
    if not m:
        sys.exit("no D3DDDI_DEVICEFUNCS")
    body = re.sub(r"#\s*\w+[^\n]*", "", m.group(1))
    names9 = [fm.group(2) for fm in re.finditer(r"(PFN\w+)\s+(pfn\w+)\s*;", body)]
    open(os.path.join(out, "umd9_tables.inc"), "w").write("".join("UMD9_FILL(%s)\n" % n for n in names9))
    print(len(names9), "Direct3D 9 entries")
