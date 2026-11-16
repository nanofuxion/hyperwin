#!/usr/bin/env python3
"""Create a minimal FAT12 volume containing 8.3-named EFI images."""

import argparse
import struct
from pathlib import Path

SECTOR = 512
VOLUME_SECTORS = 256
FAT_SECTORS = 1
ROOT_ENTRIES = 16
ROOT_SECTORS = ROOT_ENTRIES * 32 // SECTOR
DATA_LBA = 1 + 2 * FAT_SECTORS + ROOT_SECTORS


def fat12_set(fat: bytearray, cluster: int, value: int) -> None:
    offset = cluster + cluster // 2
    value &= 0xFFF
    if cluster & 1:
        fat[offset] = (fat[offset] & 0x0F) | ((value << 4) & 0xF0)
        fat[offset + 1] = (value >> 4) & 0xFF
    else:
        fat[offset] = value & 0xFF
        fat[offset + 1] = (fat[offset + 1] & 0xF0) | ((value >> 8) & 0x0F)


def short_name(name: str, directory: bool) -> bytes:
    if directory:
        if not name or len(name) > 8 or "." in name:
            raise SystemExit(f"directory name must be 1-8 letters: {name}")
        return name.upper().ljust(8).encode("ascii") + b"   "
    stem, dot, extension = name.upper().partition(".")
    if not dot or len(stem) > 8 or len(extension) > 3:
        raise SystemExit(f"name must use the FAT 8.3 format: {name}")
    return (stem.ljust(8) + extension.ljust(3)).encode("ascii")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--file", nargs=2, action="append", required=True, metavar=("PATH", "NAME"))
    args = parser.parse_args()

    cluster_count = VOLUME_SECTORS - DATA_LBA
    dirs: dict[tuple[str, ...], dict] = {(): {"name": b"", "children": []}}
    files = []
    for filename, raw_name in args.file:
        parts = tuple(part for part in raw_name.replace("\\", "/").split("/") if part)
        if not parts:
            raise SystemExit(f"empty FAT name for {filename}")
        parent = ()
        for part in parts[:-1]:
            key = parent + (part,)
            if key not in dirs:
                dirs[key] = {"name": short_name(part, True), "children": []}
                dirs[parent]["children"].append(key)
            parent = key
        payload = Path(filename).read_bytes()
        entry = {
            "name": short_name(parts[-1], False),
            "path": raw_name,
            "payload": payload,
            "parent": parent,
        }
        dirs[parent]["children"].append(entry)
        files.append(entry)
    if any(len(node["children"]) + (2 if key else 0) > 16 for key, node in dirs.items()):
        raise SystemExit("a directory needs more than one FAT cluster")

    next_cluster = 2
    for key, node in dirs.items():
        if not key:
            continue
        node["cluster"] = next_cluster
        next_cluster += 1
    for entry in files:
        needed = max(1, (len(entry["payload"]) + SECTOR - 1) // SECTOR)
        entry["cluster"] = next_cluster
        entry["needed"] = needed
        next_cluster += needed
    if next_cluster - 2 > cluster_count:
        raise SystemExit(f"EFI images need {next_cluster - 2} clusters; volume has {cluster_count}")
    if len(dirs[()]["children"]) > ROOT_ENTRIES:
        raise SystemExit("too many root entries")

    image = bytearray(VOLUME_SECTORS * SECTOR)
    boot = bytearray(SECTOR)
    boot[:3] = b"\xeb\x3c\x90"
    boot[3:11] = b"HYPERWIN"
    struct.pack_into("<H", boot, 11, SECTOR)
    boot[13] = 1
    struct.pack_into("<H", boot, 14, 1)
    boot[16] = 2
    struct.pack_into("<H", boot, 17, ROOT_ENTRIES)
    struct.pack_into("<H", boot, 19, VOLUME_SECTORS)
    boot[21] = 0xF8
    struct.pack_into("<H", boot, 22, FAT_SECTORS)
    struct.pack_into("<H", boot, 24, 32)
    struct.pack_into("<H", boot, 26, 64)
    struct.pack_into("<I", boot, 28, 2048)
    boot[36] = 0x80
    boot[38] = 0x29
    struct.pack_into("<I", boot, 39, 0x4857494E)
    boot[43:54] = b"HYPERWIN   "
    boot[54:62] = b"FAT12   "
    boot[510:512] = b"\x55\xaa"
    image[:SECTOR] = boot

    def put_entry(buf: bytearray, index: int, name: bytes, attr: int, cluster: int, size: int) -> None:
        offset = index * 32
        buf[offset:offset + 11] = name
        buf[offset + 11] = attr
        struct.pack_into("<H", buf, offset + 26, cluster)
        struct.pack_into("<I", buf, offset + 28, size)

    fat = bytearray(FAT_SECTORS * SECTOR)
    fat[:3] = b"\xf8\xff\xff"
    for key, node in dirs.items():
        if key:
            fat12_set(fat, node["cluster"], 0xFFF)
    for entry in files:
        for i in range(entry["needed"]):
            cluster = entry["cluster"] + i
            fat12_set(fat, cluster, cluster + 1 if i + 1 < entry["needed"] else 0xFFF)
    image[SECTOR:2 * SECTOR] = fat
    image[2 * SECTOR:3 * SECTOR] = fat

    def write_dir(key: tuple[str, ...], node: dict) -> bytearray:
        buf = bytearray(SECTOR)
        index = 0
        if key:
            put_entry(buf, 0, b".          ", 0x10, node["cluster"], 0)
            parent_key = key[:-1]
            parent_cluster = dirs[parent_key]["cluster"] if parent_key else 0
            put_entry(buf, 1, b"..         ", 0x10, parent_cluster, 0)
            index = 2
        for child in node["children"]:
            if isinstance(child, tuple):
                child_node = dirs[child]
                put_entry(buf, index, child_node["name"], 0x10, child_node["cluster"], 0)
            else:
                put_entry(buf, index, child["name"], 0x20, child["cluster"], len(child["payload"]))
                data_offset = (DATA_LBA + child["cluster"] - 2) * SECTOR
                image[data_offset:data_offset + len(child["payload"])] = child["payload"]
            index += 1
        if key:
            data_offset = (DATA_LBA + node["cluster"] - 2) * SECTOR
            image[data_offset:data_offset + len(buf)] = buf
        return buf

    root = write_dir((), dirs[()])
    for key, node in dirs.items():
        if key:
            write_dir(key, node)
    root_lba = 1 + 2 * FAT_SECTORS
    image[root_lba * SECTOR:(root_lba + ROOT_SECTORS) * SECTOR] = root
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    listing = ", ".join(f"{entry['path']} ({len(entry['payload'])} bytes)" for entry in files)
    print(f"{args.output}: FAT12, {VOLUME_SECTORS} sectors, {listing}")


if __name__ == "__main__":
    main()
