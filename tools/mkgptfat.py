#!/usr/bin/env python3
"""Wrap a raw FAT volume in a small GPT disk for U-Boot's virtio 1:1 syntax."""
import argparse
import struct
import uuid
import zlib

SECTOR = 512
DISK_BYTES = 4 * 1024 * 1024
PART_START = 2048
PART_SECTORS = 256
EFI_SYSTEM_GUID = "c12a7328-f81f-11d2-ba4b-00a0c93ec93b"


def guid(value: str) -> bytes:
    return uuid.UUID(value).bytes_le


def header(current: int, backup: int, entries_lba: int, entries_crc: int, sectors: int) -> bytes:
    raw = bytearray(SECTOR)
    raw[:8] = b"EFI PART"
    struct.pack_into("<I", raw, 8, 0x00010000)
    struct.pack_into("<I", raw, 12, 92)
    struct.pack_into("<Q", raw, 24, current)
    struct.pack_into("<Q", raw, 32, backup)
    struct.pack_into("<Q", raw, 40, 34)
    struct.pack_into("<Q", raw, 48, sectors - 34)
    raw[56:72] = guid("6f0c8a14-7b2e-4d55-9c31-0a11b7e4d902")
    struct.pack_into("<Q", raw, 72, entries_lba)
    struct.pack_into("<I", raw, 80, 128)
    struct.pack_into("<I", raw, 84, 128)
    struct.pack_into("<I", raw, 88, entries_crc)
    struct.pack_into("<I", raw, 16, zlib.crc32(raw[:92]) & 0xFFFFFFFF)
    return raw


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("fat_image")
    parser.add_argument("output")
    args = parser.parse_args()
    fat = open(args.fat_image, "rb").read()
    sectors = DISK_BYTES // SECTOR
    part_bytes = PART_SECTORS * SECTOR
    if len(fat) > part_bytes:
        raise SystemExit(f"FAT image is too large for partition: {len(fat)} > {part_bytes}")

    disk = bytearray(DISK_BYTES)
    entries = bytearray(128 * 128)
    entry = bytearray(128)
    entry[:16] = guid(EFI_SYSTEM_GUID)
    entry[16:32] = uuid.UUID("a1000000-0000-4000-8000-000000000001").bytes_le
    struct.pack_into("<QQQ", entry, 32, PART_START, PART_START + PART_SECTORS - 1, 0)
    name = "HyperWin EFI".encode("utf-16le")
    entry[56:56 + len(name)] = name
    entries[:128] = entry
    entries_crc = zlib.crc32(entries) & 0xFFFFFFFF

    disk[SECTOR:2 * SECTOR] = header(1, sectors - 1, 2, entries_crc, sectors)
    disk[2 * SECTOR:2 * SECTOR + len(entries)] = entries
    backup_entries_lba = sectors - 33
    backup_offset = backup_entries_lba * SECTOR
    disk[backup_offset:backup_offset + len(entries)] = entries
    disk[(sectors - 1) * SECTOR:sectors * SECTOR] = header(
        sectors - 1, 1, backup_entries_lba, entries_crc, sectors,
    )

    mbr = bytearray(SECTOR)
    mbr[446] = 0
    mbr[450] = 0xEE
    struct.pack_into("<II", mbr, 454, 1, sectors - 1)
    mbr[510:512] = b"\x55\xaa"
    disk[:SECTOR] = mbr
    partition_offset = PART_START * SECTOR
    disk[partition_offset:partition_offset + len(fat)] = fat
    with open(args.output, "wb") as output:
        output.write(disk)
    print(f"{args.output}: {DISK_BYTES} bytes, FAT partition {PART_START}:{PART_START + PART_SECTORS - 1}")


if __name__ == "__main__":
    main()
