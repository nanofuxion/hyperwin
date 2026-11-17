#!/usr/bin/env python3
"""Point the reference installed-system BCD (build/originals/BCD-testsigning, from the 17134 phone disk)
at a new GPT disk. Partition device elements hold the partition GUID at +0x10 and the disk GUID at +0x28;
the same-length replacement leaves the hive structure untouched.
usage: bcd_retarget.py IN OUT DISK_HEX ESP_HEX NTFS_HEX   (GUIDs as on-disk little-endian hex)"""
import sys

OLD_DISK = bytes.fromhex('4b10bbd47772e81181f02816ad97ccb3')
OLD_ESP = bytes.fromhex('4c10bbd47772e81181f02816ad97ccb3')   # {bootmgr} device
OLD_OS = bytes.fromhex('61855b972e5f224ba0d9654b098007e0')    # Windows loader device and osdevice


def main():
    src, dst = sys.argv[1], sys.argv[2]
    disk, esp, os_part = (bytes.fromhex(x) for x in sys.argv[3:6])
    d = open(src, 'rb').read()
    for old in (OLD_DISK, OLD_ESP, OLD_OS):
        if old not in d:
            raise SystemExit('reference BCD does not contain %s' % old.hex())
    d = d.replace(OLD_DISK, disk).replace(OLD_ESP, esp).replace(OLD_OS, os_part)
    open(dst, 'wb').write(d)


main()
