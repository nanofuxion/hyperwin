#!/usr/bin/env python3
"""Sparse raw disk with a GPT: EFI system partition (260 MiB) then one NTFS data partition.
usage: gpt.py IMAGE SIZE_GB   (prints shell assignments: DISK_GUID ESP_GUID NTFS_GUID as on-disk hex, NTFS_START)"""
import struct, sys, uuid, zlib

SECTOR = 512
ESP_TYPE = uuid.UUID('c12a7328-f81f-11d2-ba4b-00a0c93ec93b')
DATA_TYPE = uuid.UUID('ebd0a0a2-b9e5-4433-87c0-68b6b72699c7')

def entry(type_guid, part_guid, first, last, name):
    n = name.encode('utf-16-le').ljust(72, b'\0')
    return type_guid.bytes_le + part_guid.bytes_le + struct.pack('<QQQ', first, last, 0) + n

def header(cur, backup, first_usable, last_usable, disk, entries_lba, entries_crc):
    h = struct.pack('<8sIIIIQQQQ16sQIII', b'EFI PART', 0x10000, 92, 0, 0, cur, backup, first_usable,
                    last_usable, disk.bytes_le, entries_lba, 128, 128, entries_crc)
    h = h[:16] + struct.pack('<I', zlib.crc32(h)) + h[20:]
    return h.ljust(SECTOR, b'\0')

def main():
    path, gb = sys.argv[1], int(sys.argv[2])
    total = gb * 1024 * 1024 * 1024 // SECTOR
    last = total - 1
    disk, esp, ntfs = uuid.uuid4(), uuid.uuid4(), uuid.uuid4()
    esp_first = 2048
    esp_last = esp_first + 260 * 2048 - 1
    ntfs_first = esp_last + 1
    ntfs_last = (last - 33) // 2048 * 2048 - 1
    table = entry(ESP_TYPE, esp, esp_first, esp_last, 'EFI system partition')
    table += entry(DATA_TYPE, ntfs, ntfs_first, ntfs_last, 'Windows')
    table = table.ljust(128 * 128, b'\0')
    crc = zlib.crc32(table)
    mbr = bytearray(SECTOR)
    mbr[446:462] = struct.pack('<BBBBBBBBII', 0, 0, 2, 0, 0xEE, 0xFF, 0xFF, 0xFF, 1, min(total - 1, 0xFFFFFFFF))
    mbr[510:512] = b'\x55\xaa'
    with open(path, 'wb') as f:
        f.truncate(total * SECTOR)
        f.seek(0); f.write(mbr)
        f.write(header(1, last, 34, last - 33, disk, 2, crc)); f.write(table)
        f.seek((last - 32) * SECTOR); f.write(table)
        f.write(header(last, 1, 34, last - 33, disk, last - 32, crc))
    print('DISK_GUID=%s ESP_GUID=%s NTFS_GUID=%s NTFS_START=%d' % (disk.bytes_le.hex(), esp.bytes_le.hex(), ntfs.bytes_le.hex(), ntfs_first))

main()
