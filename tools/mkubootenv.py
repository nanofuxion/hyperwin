#!/usr/bin/env python3
"""Signed supplementary environment for the phone's AVF U-Boot.

virtio 0 is the Windows disk. virtio 1 is this image. U-Boot runs
`env verified_import virtio 1#uboot_env` and then `run uenvcmd`.
The public key in that U-Boot is the AOSP AVB test key.
"""

import argparse
import os
import struct
import subprocess
import tempfile
import uuid
import zlib

CMD = (
    "echo HYPERWIN_BOOT && "
    "fatload virtio 0:1 0x90000000 EFI/BOOT/BOOTAA64.EFI && "
    "bootefi 0x90000000 ${fdtcontroladdr}"
)

PART = 128 * 1024
DISK = 4 * 1024 * 1024
LINUX_GUID = "0fc63daf-8483-4772-8e79-3d69d8477de4"


def guid(text):
    fields = text.split("-")
    return (
        bytes.fromhex(fields[0])[::-1]
        + bytes.fromhex(fields[1])[::-1]
        + bytes.fromhex(fields[2])[::-1]
        + bytes.fromhex(fields[3])
        + bytes.fromhex(fields[4])
    )


def env_blob():
    data = ("uenvcmd=" + CMD).encode() + b"\0\0"
    crc = zlib.crc32(data) & 0xFFFFFFFF
    return struct.pack("<I", crc) + data


def gpt(path, vbmeta, env):
    assert len(env) == PART
    assert len(vbmeta) <= PART
    sectors = DISK // 512
    blank = bytearray(DISK)
    entries = bytearray(128 * 128)

    def put(index, name, start, blob):
        length = PART // 512
        entry = bytearray(128)
        entry[0:16] = guid(LINUX_GUID)
        entry[16:32] = uuid.UUID(int=0xA100 + index).bytes_le
        struct.pack_into("<QQ", entry, 32, start, start + length - 1)
        encoded = name.encode("utf-16le")
        entry[56:56 + len(encoded)] = encoded
        entries[index * 128:(index + 1) * 128] = entry
        off = start * 512
        blank[off:off + len(blob)] = blob

    put(0, "vbmeta", 2048, vbmeta)
    put(1, "uboot_env", 2048 + PART // 512, env)

    def header(current, backup, entry_lba):
        raw = bytearray(512)
        raw[0:8] = b"EFI PART"
        struct.pack_into("<I", raw, 8, 0x00010000)
        struct.pack_into("<I", raw, 12, 92)
        struct.pack_into("<Q", raw, 24, current)
        struct.pack_into("<Q", raw, 32, backup)
        struct.pack_into("<Q", raw, 40, 34)
        struct.pack_into("<Q", raw, 48, sectors - 34)
        raw[56:72] = uuid.UUID("6f0c8a14-7b2e-4d55-9c31-0a11b7e4d902").bytes_le
        struct.pack_into("<Q", raw, 72, entry_lba)
        struct.pack_into("<I", raw, 80, 128)
        struct.pack_into("<I", raw, 84, 128)
        struct.pack_into("<I", raw, 88, zlib.crc32(entries) & 0xFFFFFFFF)
        struct.pack_into("<I", raw, 16, zlib.crc32(raw[:92]) & 0xFFFFFFFF)
        return raw

    blank[512:1024] = header(1, sectors - 1, 2)
    blank[1024:1024 + len(entries)] = entries
    backup_entries = (sectors - 33) * 512
    blank[backup_entries:backup_entries + len(entries)] = entries
    blank[(sectors - 1) * 512:(sectors - 1) * 512 + 512] = header(sectors - 1, 1, sectors - 33)

    mbr = bytearray(512)
    mbr[446] = 0x00
    mbr[450] = 0xEE
    struct.pack_into("<I", mbr, 454, 1)
    struct.pack_into("<I", mbr, 458, sectors - 1)
    mbr[510] = 0x55
    mbr[511] = 0xAA
    blank[0:512] = mbr
    open(path, "wb").write(blank)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--avbtool", default="avbtool")
    parser.add_argument("--key", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        env = os.path.join(tmp, "env.img")
        open(env, "wb").write(env_blob())
        subprocess.check_call([
            args.avbtool, "add_hash_footer",
            "--image", env,
            "--partition_name", "uboot_env",
            "--partition_size", str(PART),
            "--algorithm", "SHA256_RSA4096",
            "--key", args.key,
            "--salt", "00" * 32,
            "--rollback_index", "0",
        ])
        vbmeta = os.path.join(tmp, "vbmeta.img")
        subprocess.check_call([
            args.avbtool, "make_vbmeta_image",
            "--output", vbmeta,
            "--algorithm", "SHA256_RSA4096",
            "--key", args.key,
            "--include_descriptors_from_image", env,
            "--rollback_index", "0",
            "--padding_size", str(PART),
        ])
        gpt(args.output, open(vbmeta, "rb").read(), open(env, "rb").read())
    print(args.output, os.path.getsize(args.output))


if __name__ == "__main__":
    main()
