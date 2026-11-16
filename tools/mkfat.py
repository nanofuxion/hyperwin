#!/usr/bin/env python3
"""FAT16 disk image (MBR + EFI system partition) from a directory tree."""
import os
import struct
import sys

SECTOR = 512
SPC = 4
RESERVED = 1
FATS = 2
ROOT_ENTRIES = 512
# No MBR. EDK2 treats a partitioned USB stick as a hard disk and will not
# look for EFI/BOOT/BOOTAA64.EFI unless the filesystem itself is the disk.


def pad(name):
    if name == ".":
        return b".          "
    if name == "..":
        return b"..         "
    stem, dot, ext = name.partition(".")
    if not dot:
        ext = ""
    raw = ((stem.upper() + "        ")[:8] + (ext.upper() + "   ")[:3]).encode("ascii", "replace")
    return raw


def lfn_checksum(short):
    s = 0
    for b in short:
        s = (((s & 1) << 7) + (s >> 1) + b) & 0xFF
    return s


def lfn_entries(long_name, short):
    encoded = long_name.encode("utf-16le")
    chars = [encoded[i:i + 2] for i in range(0, len(encoded), 2)]
    chars.append(b"\x00\x00")
    while len(chars) % 13:
        chars.append(b"\xff\xff")
    chunks = [chars[i:i + 13] for i in range(0, len(chars), 13)]
    out = []
    total = len(chunks)
    for idx in range(total, 0, -1):
        part = chunks[idx - 1]
        ent = bytearray(32)
        ent[0] = idx | (0x40 if idx == total else 0)
        ent[11] = 0x0F
        ent[13] = lfn_checksum(short)
        slots = (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)
        for n, off in enumerate(slots):
            ent[off:off + 2] = part[n]
        out.append(ent)
    return out


def build(src, image_path):
    files = []
    for root, dirs, names in os.walk(src):
        dirs.sort()
        rel = os.path.relpath(root, src)
        parts = [] if rel == "." else rel.split(os.sep)
        for d in sorted(dirs):
            files.append((parts, d, None))
        for n in sorted(names):
            path = os.path.join(root, n)
            files.append((parts, n, open(path, "rb").read()))

    # cluster 2+ assigned after we know sizes
    cluster = 2
    entries = []  # (parts, name, data or None, start, length)
    for parts, name, data in files:
        if data is None:
            entries.append((parts, name, None, cluster, 0))
            cluster += 1
        else:
            start = cluster
            nclus = max(1, (len(data) + SPC * SECTOR - 1) // (SPC * SECTOR))
            entries.append((parts, name, data, start, len(data)))
            cluster += nclus
    cluster_count = cluster  # exclusive end, includes 0 and 1

    root_sectors = (ROOT_ENTRIES * 32) // SECTOR
    fat_bytes = (cluster_count + 2) * 2
    fat_sectors = (fat_bytes + SECTOR - 1) // SECTOR
    data_sectors = (cluster_count - 2) * SPC
    part_sectors = RESERVED + FATS * fat_sectors + root_sectors + data_sectors
    part_sectors = (part_sectors + 63) & ~63
    img = bytearray(part_sectors * SECTOR)

    def put(lba, data):
        img[lba * SECTOR:lba * SECTOR + len(data)] = data

    vbr = bytearray(SECTOR)
    vbr[0:3] = b"\xeb\x3c\x90"
    vbr[3:11] = b"HYPERWIN"
    struct.pack_into("<H", vbr, 11, SECTOR)
    vbr[13] = SPC
    struct.pack_into("<H", vbr, 14, RESERVED)
    vbr[16] = FATS
    struct.pack_into("<H", vbr, 17, ROOT_ENTRIES)
    vbr[21] = 0xF8
    struct.pack_into("<H", vbr, 22, fat_sectors)
    struct.pack_into("<H", vbr, 24, 32)
    struct.pack_into("<H", vbr, 26, 64)
    struct.pack_into("<I", vbr, 28, 0)
    struct.pack_into("<I", vbr, 32, part_sectors)
    vbr[36] = 0x80
    vbr[38] = 0x29
    struct.pack_into("<I", vbr, 39, 0x4857494E)
    vbr[43:54] = b"HYPERWIN   "
    vbr[54:62] = b"FAT16   "
    vbr[510] = 0x55
    vbr[511] = 0xAA
    put(0, vbr)

    fat = bytearray(fat_sectors * SECTOR)
    struct.pack_into("<HH", fat, 0, 0xFFF8, 0xFFFF)

    def mark_eof(c):
        struct.pack_into("<H", fat, c * 2, 0xFFFF)

    def mark_next(c, nxt):
        struct.pack_into("<H", fat, c * 2, nxt)

    for parts, name, data, start, length in entries:
        if data is None:
            mark_eof(start)
        else:
            nclus = max(1, (len(data) + SPC * SECTOR - 1) // (SPC * SECTOR))
            for i in range(nclus - 1):
                mark_next(start + i, start + i + 1)
            mark_eof(start + nclus - 1)

    fat_lba = RESERVED
    put(fat_lba, fat)
    put(fat_lba + fat_sectors, fat)

    root_lba = fat_lba + FATS * fat_sectors
    data_lba = root_lba + root_sectors
    root = bytearray(root_sectors * SECTOR)
    dirs = {(): (0, root)}  # path -> (cluster or 0 for root, bytearray)

    def dirbuf(parts, cluster):
        if parts not in dirs:
            buf = bytearray(SPC * SECTOR)
            dirs[parts] = (cluster, buf)
        return dirs[parts][1]

    shorts = {}

    def short_name(directory, name):
        if name in (".", ".."):
            return pad(name)
        stem, dot, ext = name.partition(".")
        if not dot:
            ext = ""
        plain = stem.upper()
        ok = (
            1 <= len(plain) <= 8
            and len(ext) <= 3
            and name == plain + (("." + ext.upper()) if ext else "")
            and all(c.isalnum() or c in "_-" for c in plain + ext)
        )
        used = shorts.setdefault(directory, set())
        if ok:
            raw = pad(name)
            if raw not in used:
                used.add(raw)
                return raw
        n = 1
        while True:
            alias = f"{plain[:6]}~{n}"
            raw = ((alias + "        ")[:8] + (ext.upper() + "   ")[:3]).encode("ascii", "replace")
            if raw not in used:
                used.add(raw)
                return raw
            n += 1

    def add_entry(buf, directory, name, attr, cluster, size):
        short = short_name(directory, name)
        pieces = [] if name in (".", "..") else lfn_entries(name, short)
        pieces.append(None)
        slot = None
        for i in range(0, len(buf) - 32 * (len(pieces) - 1), 32):
            if all(buf[i + 32 * k] == 0 for k in range(len(pieces))):
                slot = i
                break
        if slot is None:
            raise SystemExit(f"directory full at {name}")
        for ent in lfn_entries(name, short) if name not in (".", "..") else []:
            buf[slot:slot + 32] = ent
            slot += 32
        buf[slot:slot + 11] = short
        buf[slot + 11] = attr
        struct.pack_into("<H", buf, slot + 26, cluster)
        struct.pack_into("<I", buf, slot + 28, size)

    for parts, name, data, start, length in entries:
        parent = dirs.get(tuple(parts))
        if parent is None:
            raise SystemExit(f"missing parent {parts} for {name}")
        attr = 0x10 if data is None else 0x20
        add_entry(parent[1], tuple(parts), name, attr, start, 0 if data is None else length)
        if data is None:
            buf = dirbuf(tuple(parts + [name]), start)
            child = tuple(parts + [name])
            add_entry(buf, child, ".", 0x10, start, 0)
            add_entry(buf, child, "..", 0x10, parent[0], 0)
        else:
            off = (data_lba + (start - 2) * SPC) * SECTOR
            img[off:off + len(data)] = data

    put(root_lba, root)
    for parts, (cluster, buf) in dirs.items():
        if cluster == 0:
            continue
        off = (data_lba + (cluster - 2) * SPC) * SECTOR
        img[off:off + len(buf)] = buf

    with open(image_path, "wb") as f:
        f.write(img)
    return len(entries), len(img)


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: mkfat.py DIR IMAGE")
    n, size = build(sys.argv[1], sys.argv[2])
    print(f"{sys.argv[2]}: {n} entries, {size} bytes")


if __name__ == "__main__":
    main()
