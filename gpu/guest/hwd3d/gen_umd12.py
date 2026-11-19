#!/usr/bin/env python3
"""Reads the Direct3D 12 driver interface header (from the WDK, in gpu/work/wdk, never committed) and
writes into gpu/work/umd-gen, at build time:
  umd12_names.inc  for each function table of each interface version: its table type, entry count and
                   entry names, so the driver can name an entry it has not written when one is called
  umd12_proto.txt  each table's entries with their prototypes, for reading while writing entries by hand
Nothing here is copied into a source file we keep.
  gen_umd12.py <d3d12umddi.h> <outdir>"""
import re, sys, os

hdr, out = sys.argv[1], sys.argv[2]
src = open(hdr, encoding="utf-8", errors="replace").read()
src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
src = re.sub(r"//[^\n]*", "", src)

# Table type numbers by the start of the table's name (D3D12DDI_TABLE_TYPE).
KINDS = [("D3D12DDI_DEVICE_FUNCS_CORE_", 0), ("D3D12DDI_COMMAND_LIST_FUNCS_3D_", 1), ("D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_", 2),
         ("D3D12DDI_DXGI_", 3), ("D3D12DDI_EXTENDED_FEATURES_FUNCS_0096", 27), ("D3D12DDI_EXTENDED_FEATURES_FUNCS_", 8),
         ("D3D12DDI_PASS_EXPERIMENT_FUNCS_", 9), ("D3D12DDI_RENDER_PASS_FUNCS_", 20),
         ("D3D12DDI_DEVICE_DOWNLEVEL_SUPPORT_FUNCS_", 24), ("D3D12DDI_STATE_OBJECTS_EXPERIMENT_FUNCS_", 26)]
protos = dict((m.group(2), " ".join(m.group(0).split())) for m in
              re.finditer(r"typedef\s+[\w\s\*]+?\(\s*APIENTRY\s*(CALLBACK\s*)?\*\s*(\w+)\s*\)\s*\((.*?)\)\s*;", src, flags=re.S))
os.makedirs(out, exist_ok=True)
inc, txt, rows = [], [], []
for m in re.finditer(r"typedef\s+struct\s+(D3D12DDI_\w+)\s*\{(.*?)\}\s*\1\s*;", src, flags=re.S):
    name, body = m.group(1), m.group(2)
    kind = next((k for p, k in KINDS if name.startswith(p)), None)
    if kind is None:
        continue
    entries = re.findall(r"([\w\*]+)\s+(pfn\w+)\s*;", body)
    if not entries:
        continue
    inc.append("static const char *const n12_%s[] = {%s};" % (name, ", ".join('"%s"' % e[1] for e in entries)))
    rows.append('    {%d, %d, n12_%s, "%s"},' % (kind, len(entries), name, name))
    txt.append("== %s (type %d, %d entries)" % (name, kind, len(entries)))
    for i, (t, e) in enumerate(entries):
        txt.append("%3d %s: %s" % (i, e, protos.get(t, t)))
inc.append("static const struct { unsigned type, count; const char *const *names; const char *table; } kTables12[] = {")
inc += rows + ["};"]
open(os.path.join(out, "umd12_names.inc"), "w").write("\n".join(inc) + "\n")
open(os.path.join(out, "umd12_proto.txt"), "w").write("\n".join(txt) + "\n")
