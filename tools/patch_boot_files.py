#!/usr/bin/env python3
"""Patch the Windows ARM64 boot manager files on the ESP of a raw GPT disk image for AVF.

AVF runs crosvm with --no-pmu, so PMU and debug-register accesses are UNDEF. Windows boot
code uses them unconditionally. This applies the same rewrite as guest/efi/trace.c:patch_pmu
to \\EFI\\BOOT\\BOOTAA64.EFI and \\EFI\\Microsoft\\Boot\\bootmgfw.efi in place (same size).

usage: patch_boot_files.py DISK.img [--dry-run]   (DISK may be a partial prefix of the disk)
Prints "OFFSET LENGTH FILE" runs that were changed, so a remote copy can be patched with dd.
"""
import struct, sys

NOP = 0xd503201f

def patch_pmu(data: bytearray) -> int:
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    assert data[pe:pe + 4] == b'PE\0\0'
    nsec, = struct.unpack_from('<H', data, pe + 6)
    optsz, = struct.unpack_from('<H', data, pe + 20)
    sec = pe + 24 + optsz
    total = 0
    for i in range(nsec):
        s = sec + i * 40
        vsize, va, rawsz, rawptr = struct.unpack_from('<IIII', data, s + 8)
        flags, = struct.unpack_from('<I', data, s + 36)
        if not flags & 0x20000000:
            continue
        n = min(vsize, rawsz)
        for off in range(rawptr, rawptr + n - 3, 4):
            x, = struct.unpack_from('<I', data, off)
            top = x & 0xfff80000
            rt = x & 31
            op1 = (x >> 16) & 7; crn = (x >> 12) & 15; crm = (x >> 8) & 15; op2 = (x >> 5) & 7
            if top in (0xd5380000, 0xd5180000):    # op0 = 3
                if not ((crn == 9 and op1 in (3, 0)) or (crn == 14 and op1 == 3 and crm >= 8)):
                    continue
                if top == 0xd5180000:
                    new = NOP
                elif crn == 9 and crm == 13 and op2 == 0 and op1 == 3:
                    new = 0xd53be040 | rt            # PMCCNTR_EL0 -> CNTVCT_EL0
                else:
                    new = NOP if rt == 31 else 0xaa1f03e0 | rt
            else:
                continue
            struct.pack_into('<I', data, off, new)
            total += 1
    return total

class Fat32:
    def __init__(self, img, base):
        self.img, self.base = img, base
        b = img[base:base + 512]
        self.bps, self.spc, self.rsv, self.nfat = struct.unpack_from('<HBHB', b, 11)
        self.fatsz, = struct.unpack_from('<I', b, 36)
        self.root, = struct.unpack_from('<I', b, 44)
        self.fat0 = base + self.rsv * self.bps
        self.data = self.fat0 + self.nfat * self.fatsz * self.bps
        self.csz = self.spc * self.bps
    def nxt(self, c):
        return struct.unpack_from('<I', self.img, self.fat0 + 4 * c)[0] & 0x0fffffff
    def chain(self, c):
        out = []
        while 2 <= c < 0x0ffffff8:
            out.append(c); c = self.nxt(c)
        return out
    def off(self, c):
        return self.data + (c - 2) * self.csz
    def entries(self, c):
        raw = b''.join(self.img[self.off(k):self.off(k) + self.csz] for k in self.chain(c))
        lfn = []
        for i in range(0, len(raw), 32):
            e = raw[i:i + 32]
            if e[0] == 0: break
            if e[0] == 0xe5: lfn = []; continue
            if e[11] == 0x0f:
                part = e[1:11] + e[14:26] + e[28:32]
                lfn.insert(0, part.decode('utf-16le').split('\0')[0].split('￿')[0]); continue
            name = ''.join(lfn) if lfn else (e[:8].decode().strip() + ('.' + e[8:11].decode().strip() if e[8:11].strip() else ''))
            lfn = []
            cl = (struct.unpack_from('<H', e, 20)[0] << 16) | struct.unpack_from('<H', e, 26)[0]
            yield name, e[11], cl, struct.unpack_from('<I', e, 28)[0]
    def find(self, path):
        c = self.root
        parts = [p for p in path.split('/') if p]
        for k, p in enumerate(parts):
            for name, attr, cl, size in self.entries(c):
                if name.lower() == p.lower():
                    if k == len(parts) - 1: return cl, size
                    c = cl; break
            else:
                raise FileNotFoundError(path)

DEBUG_NOPS = {  # bootmgfw.efi VA -> expected word: exception-vector and OS-lock writes (debug builds only)
    0x100c5c20: 0xd518c008, 0x100c805c: 0xd518c008, 0x100f9d10: 0xd518c008,
    0x100f9eb0: 0xd518c008, 0x100f9fe4: 0xd518c008, 0x100c808c: 0xd5101088,
}

TTBR_SITES = [0x1001a418, 0x1001a420, 0x1001a428, 0x1001a520, 0x1001a530, 0x1001a538, 0x100f9f88, 0x100f9f94,
              0x100f9fa0, 0x100f9fac, 0x100fa008, 0x100fa014, 0x100fa020, 0x100fa02c, 0x1011b014, 0x1011b01c,
              0x1011b024, 0x1011b02c]
TTBR_PATTERNS = (0xd5182000, 0xd5182020, 0xd5182040, 0xd518a200)  # TTBR0/TTBR1/TCR/MAIR _EL1 writes, Rt masked

def debug_nops(data: bytearray) -> int:
    n = 0
    for va in TTBR_SITES:
        base = 0x400 + (va - 0x10001000) if va < 0x1011b000 else None
        if base is None:   # .trans section: raw 0x119e00 at VA 0x1011b000
            base = 0x119e00 + (va - 0x1011b000)
        if struct.unpack_from('<I', data, base)[0] & 0xffffffe0 in TTBR_PATTERNS:
            struct.pack_into('<I', data, base, NOP); n += 1
    for va, word in DEBUG_NOPS.items():
        off = 0x400 + (va - 0x10001000)
        if struct.unpack_from('<I', data, off)[0] == word:
            struct.pack_into('<I', data, off, NOP); n += 1
    return n

def find_vector_table_va(data: bytearray):
    """VA of the table the image writes to VBAR_EL1: adrp xN + add xN,xN,#0x800 before msr VBAR_EL1."""
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    nsec, = struct.unpack_from('<H', data, pe + 6); optsz, = struct.unpack_from('<H', data, pe + 20)
    imagebase, = struct.unpack_from('<Q', data, pe + 24 + 24)
    sec = pe + 24 + optsz
    for i in range(nsec):
        s = sec + i * 40
        vsize, va, rawsz, rawptr = struct.unpack_from('<IIII', data, s + 8)
        flags, = struct.unpack_from('<I', data, s + 36)
        if not flags & 0x20000000: continue
        for off in range(rawptr + 32, rawptr + min(vsize, rawsz), 4):
            x, = struct.unpack_from('<I', data, off)
            if x & 0xffffffe0 != 0xd518c000: continue
            for back in range(1, 8):
                add, = struct.unpack_from('<I', data, off - 4 * back)
                if add & 0xffc00000 == 0x91000000 and (add >> 10) & 0xfff == 0x800 and ((add >> 5) & 31) == (add & 31):
                    for b2 in range(back + 1, back + 6):
                        adrp, = struct.unpack_from('<I', data, off - 4 * b2)
                        if adrp & 0x9f000000 == 0x90000000 and (adrp & 31) == (add & 31):
                            immlo = (adrp >> 29) & 3; immhi = (adrp >> 5) & 0x7ffff
                            imm = ((immhi << 2) | immlo) << 12
                            if imm & (1 << 32): imm -= 1 << 33
                            pc = imagebase + va + (off - 4 * b2 - rawptr)
                            return (pc & ~0xfff) + imm + 0x800, imagebase, va, rawptr
    return None

def plant_brk(data: bytearray) -> int:
    """After 'msr VBAR_EL1,x8; b +8; msr VBAR_EL2,x8' replace the dsb with BRK #0x777 (starts stepping)."""
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    nsec, = struct.unpack_from('<H', data, pe + 6); optsz, = struct.unpack_from('<H', data, pe + 20)
    sec = pe + 24 + optsz
    n = 0
    for i in range(nsec):
        s = sec + i * 40
        vsize, va, rawsz, rawptr = struct.unpack_from('<IIII', data, s + 8)
        flags, = struct.unpack_from('<I', data, s + 36)
        if not flags & 0x20000000: continue
        for off in range(rawptr, rawptr + min(vsize, rawsz) - 16, 4):
            if struct.unpack_from('<4I', data, off) == (0xd518c008, 0x14000002, 0xd51cc008, 0xd5033f9f):
                struct.pack_into('<I', data, off + 12, 0xd4200000 | (0x777 << 5)); n += 1
    return n

def vec_stubs(data: bytearray, blob: bytes) -> int:
    """Restore the VBAR writes and replace the boot manager's own vector table with painting stubs."""
    found = find_vector_table_va(data)
    if not found: return 0
    tva, imagebase, va, rawptr = found
    off = rawptr + (tva - imagebase - va)
    data[off:off + len(blob)] = blob
    return len(blob) // 4

def main():
    disk = sys.argv[1]; dry = '--dry-run' in sys.argv
    orig = open(sys.argv[sys.argv.index('--original') + 1], 'rb').read(110 * 1024 * 1024) if '--original' in sys.argv else None
    img = bytearray(open(disk, 'rb').read(110 * 1024 * 1024))
    lba, = struct.unpack_from('<Q', img, 512 + 72)
    ent = lba * 512
    first, = struct.unpack_from('<Q', img, ent + 32)
    fs = Fat32(img, first * 512)
    for path in ('EFI/BOOT/BOOTAA64.EFI', 'EFI/Microsoft/Boot/bootmgfw.efi', 'EFI/Microsoft/Boot/bootmgr.efi'):
        cl, size = fs.find(path)
        chain = fs.chain(cl)
        data = bytearray()
        src = orig if orig is not None else img
        for k in chain: data += src[fs.off(k):fs.off(k) + fs.csz]
        data = bytearray(data[:size])
        changed = patch_pmu(data)
        if '--debug-nops' in sys.argv:
            changed += debug_nops(data)
        if '--plant-brk' in sys.argv:
            changed += plant_brk(data)
        if '--vec-stubs' in sys.argv:
            changed += vec_stubs(data, open(sys.argv[sys.argv.index('--vec-stubs') + 1], 'rb').read())
        print(f'# {path}: {size} bytes, {len(chain)} clusters, {changed} instructions rewritten')
        pos = 0
        for k in chain:
            n = min(fs.csz, size - pos)
            new = bytes(data[pos:pos + n])
            if not dry and new != bytes(img[fs.off(k):fs.off(k) + n]):
                img[fs.off(k):fs.off(k) + n] = new
            print(f'{fs.off(k)} {n} {path}')
            pos += n
    # --replace PATH FILE: overwrite an ESP file in place with one of the same size (e.g. the BCD store)
    args = sys.argv
    for k, a in enumerate(args):
        if a == '--replace':
            path, new = args[k + 1], open(args[k + 2], 'rb').read()
            cl, size = fs.find(path)
            assert size == len(new), (size, len(new))
            pos = 0
            for c in fs.chain(cl):
                n = min(fs.csz, size - pos)
                img[fs.off(c):fs.off(c) + n] = new[pos:pos + n]; pos += n
            print(f'# {path}: replaced, {size} bytes')
    if not dry:
        open(disk, 'wb').write(img)

main()
