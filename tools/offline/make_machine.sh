#!/bin/sh
# Host-side (macOS) machine prep: a bootable GPT disk with Windows applied offline, HyperWin
# drivers registered in the offline SYSTEM hive, a BCD pointing at the NTFS partition, and an
# answer file that skips OOBE. Bring-up twin of the on-phone prep; no Windows Setup involved.
# usage: make_machine.sh ISO INDEX SIZE_GB OUT_DIR [USER]
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
ISO=$1; INDEX=$2; SIZE_GB=$3; OUT=$4; USERNAME=${5:-hyperwin}
WIMBIN=$ROOT/build/third_party/mac/bin
mkdir -p "$OUT"
cd "$OUT"

[ -f install.esd ] || 7z e -y "$ISO" sources/install.esd >/dev/null
"$WIMBIN/wiminfo" install.esd "$INDEX" | grep -E "^(Name|Edition ID|Architecture)" || true

rm -f system.img
python3 "$ROOT/tools/offline/gpt.py" system.img "$SIZE_GB" > layout.txt
. ./layout.txt

DEV=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount system.img | awk 'NR==1{print $1}')
trap 'hdiutil detach "$DEV" >/dev/null 2>&1 || true' EXIT
sleep 1
newfs_msdos -F 32 -v SYSTEM "${DEV}s1" >/dev/null
mkntfs -Q -F -L Windows -p "$NTFS_START" -H 255 -S 63 "${DEV}s2" >/dev/null
"$WIMBIN/wimlib-imagex" apply install.esd "$INDEX" "${DEV}s2"

rm -rf stage && mkdir -p stage
"$WIMBIN/wimextract" install.esd "$INDEX" /Windows/System32/config/SYSTEM /Windows/Boot/EFI /Windows/Boot/Fonts \
  --dest-dir=stage --no-acls >/dev/null

# Drivers and the offline SYSTEM hive.
python3 "$ROOT/tools/offline/chntpw_script.py" "$ROOT/guest/drivers/drivers.json" > stage/hive.cmds
cp stage/SYSTEM stage/SYSTEM.new
chntpw -e stage/SYSTEM.new < stage/hive.cmds > stage/chntpw.log 2>&1 || true
grep -q "OK" stage/chntpw.log
python3 - "$ROOT/guest/drivers/drivers.json" "$ROOT" > stage/driver_files.txt <<'EOF'
import json, sys
for d in json.load(open(sys.argv[1]))['drivers']:
    print(sys.argv[2] + '/' + d['file'])
EOF
ntfscp -f "${DEV}s2" stage/SYSTEM.new /Windows/System32/config/SYSTEM
while read -r f; do ntfscp -f "${DEV}s2" "$f" "/Windows/System32/drivers/$(basename "$f")"; done < stage/driver_files.txt

sed "s/@USER@/$USERNAME/g" "$ROOT/tools/offline/unattend.xml" > stage/unattend.xml
ntfscp -f "${DEV}s2" stage/unattend.xml /Windows/System32/Sysprep/unattend.xml
ntfscp -f "${DEV}s2" stage/unattend.xml /Windows/Panther/unattend.xml 2>/dev/null || true

# ESP: our loader chains \EFI\Microsoft\Boot\bootmgfw.efi on the same disk.
python3 "$ROOT/tools/offline/bcd_retarget.py" "$ROOT/build/originals/BCD-testsigning" stage/BCD "$DISK_GUID" "$ESP_GUID" "$NTFS_GUID"
MNT=$(mktemp -d)
mount -t msdos "${DEV}s1" "$MNT"
mkdir -p "$MNT/EFI/BOOT" "$MNT/EFI/Microsoft/Boot"
cp -R stage/EFI/* "$MNT/EFI/Microsoft/Boot/"
mkdir -p "$MNT/EFI/Microsoft/Boot/Fonts" && cp stage/Fonts/* "$MNT/EFI/Microsoft/Boot/Fonts/"
cp stage/BCD "$MNT/EFI/Microsoft/Boot/BCD"
cp "$ROOT/build/avf-esp/BOOTAA64.EFI" "$ROOT/build/avf-esp/GOP.EFI" "$ROOT/build/avf-esp/COMPAT.EFI" "$MNT/EFI/BOOT/"
rm -rf "$MNT/.fseventsd" "$MNT/.Spotlight-V100" "$MNT/.Trashes"
umount "$MNT"
rmdir "$MNT"
hdiutil detach "$DEV" >/dev/null
trap - EXIT
ls -la system.img
du -h system.img
