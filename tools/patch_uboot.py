#!/usr/bin/env python3
"""Patch the phone U-Boot to publish a ring frame, then boot Windows.

The first VirtIO disk is Windows. The second is an auxiliary FAT disk carrying
PROBEA64.EFI, which writes a firmware test frame into the AVF-backed ring and
reserves those pages before Windows takes over. The replacement fits the
existing NUL-terminated bootcmd slot.
"""

import argparse

BOOTCMD = (
    b"bootcmd=virtio scan;"
    b"fatload virtio 1:1 0x90000000 PROBE.EFI && "
    b"bootefi 0x90000000 $fdtcontroladdr && "
    b"fatload virtio 1:1 0x90000000 GOP.EFI && "
    b"bootefi 0x90000000 $fdtcontroladdr && "
    b"fatload virtio 1:1 0x90000000 CHECK.EFI && "
    b"bootefi 0x90000000 $fdtcontroladdr && "
    b"fatload virtio 0:1 0x90000000 EFI/BOOT/BOOTAA64.EFI && "
    b"bootefi 0x90000000 $fdtcontroladdr || reset"
)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    image = bytearray(open(args.input, "rb").read())
    markers = (b"bootcmd= virtio scan &&", b"bootcmd=echo HYPERWIN_BOOT;")
    matches = [(image.find(marker), marker) for marker in markers if image.count(marker) == 1]
    if len(matches) != 1:
        if image.count(BOOTCMD) == 1:
            open(args.output, "wb").write(image)
            print(args.output, len(image), "already patched")
            return
        raise SystemExit("expected exactly one supported bootcmd")
    start, _ = matches[0]
    if start < 0:
        raise SystemExit("bootcmd not found")
    end = image.find(b"\0", start)
    if end < 0:
        raise SystemExit("unterminated bootcmd")
    if len(BOOTCMD) > end - start:
        raise SystemExit(f"bootcmd is {len(BOOTCMD)} bytes, slot is {end - start}")
    image[start:start + len(BOOTCMD)] = BOOTCMD
    image[start + len(BOOTCMD):end] = b" " * (end - start - len(BOOTCMD))
    open(args.output, "wb").write(image)
    print(args.output, len(image))


if __name__ == "__main__":
    main()
