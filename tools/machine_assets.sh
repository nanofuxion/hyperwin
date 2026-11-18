#!/bin/sh
# Stages what on-phone machine prep needs into app/src/main/assets/machine:
# drivers.json + driver binaries, our EFI loader, the reference BCD and the answer file template.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/app/src/main/assets/machine
rm -rf "$OUT"
mkdir -p "$OUT/drivers" "$OUT/efi"
cp "$ROOT/guest/drivers/drivers.json" "$OUT/"
python3 -I - "$ROOT/guest/drivers/drivers.json" <<'PY' | while read -r f; do cp "$ROOT/$f" "$OUT/drivers/"; done
import json, sys
for d in json.load(open(sys.argv[1]))['drivers']:
    print(d['file'])
PY
cp "$ROOT/build/avf-esp/BOOTAA64.EFI" "$ROOT/build/avf-esp/GOP.EFI" "$ROOT/build/avf-esp/COMPAT.EFI" "$OUT/efi/"
cp "$ROOT/build/originals/BCD-testsigning" "$OUT/BCD"
cp "$ROOT/tools/offline/unattend.xml" "$OUT/unattend.xml"
# Guest agent, Wintun, and the certificates Windows trusts our packages by.
WIN=$ROOT/build/third_party/win
mkdir -p "$OUT/guest"
cp "$ROOT/build/agent/hwagent.exe" "$WIN/wintun/wintun/bin/arm64/wintun.dll" "$ROOT/build/testcert/hyperwin-ca.cer" "$ROOT/build/testcert/hyperwin-pkg.cer" "$OUT/guest/"
# GPU (gpu/README.md): the ring driver and the Vulkan driver Windows apps load. Built here when
# missing; the Mesa build takes a while the first time.
[ -f "$ROOT/gpu/work/direct/hwgpu.sys" ] || sh "$ROOT/gpu/guest/direct/build.sh"
[ -f "$ROOT/gpu/work/out/vulkan_hwgpu.dll" ] || bash "$ROOT/gpu/guest/mesa/build.sh"
[ -f "$ROOT/gpu/work/out/vulkan-1.dll" ] || bash "$ROOT/gpu/guest/loader/build.sh"
[ -f "$ROOT/gpu/work/out/dx/zink/opengl32.dll" ] || bash "$ROOT/gpu/guest/dx/build.sh"
[ -f "$ROOT/gpu/work/out/dx/test/gltest-x64.exe" ] || bash "$ROOT/gpu/guest/dx/test/build.sh"
mkdir -p "$OUT/guest/gpu"
cp "$ROOT/gpu/work/direct/hwgpu.sys" "$ROOT/gpu/work/out/vulkan_hwgpu.dll" "$ROOT/gpu/work/out/vulkan_hwgpu.json" \
   "$ROOT/gpu/work/out/vktest.exe" "$ROOT/gpu/work/out/vktest-x64.exe" "$ROOT/gpu/work/out/vulkan-1.dll" "$OUT/guest/gpu/"
# The input driver too, so machines made before a change get it (gpu-install.cmd swaps it in;
# it loads at the next start). New machines already have it from drivers.json.
cp "$ROOT/build/hwvsinput/hwvsinput.sys" "$OUT/guest/gpu/"
# Speakers and microphone (audio/README.md). Windows only installs a sound driver from a package
# with a signed catalog, which nothing here can make, so install.ps1 makes one in Windows and signs
# it with the package key. devcon.exe comes from the Scream download.
# Built only when its sources changed: the signature carries a time, and a new file means every
# Windows reinstalls it.
for f in "$ROOT"/guest/drivers/hwaudio/hwaudio.c "$ROOT"/guest/drivers/hwaudio/hwaudio.inf "$ROOT"/include/hw_audio.h; do
  [ "$ROOT/build/hwaudio/hwaudio.sys" -nt "$f" ] || { sh "$ROOT/guest/drivers/hwaudio/build.sh" >/dev/null; break; }
done
PFX=$ROOT/build/testcert/hyperwin-pkg.pfx
[ -f "$PFX" ] || openssl pkcs12 -export -inkey "$ROOT/build/testcert/hyperwin-pkg.key" -in "$ROOT/build/testcert/hyperwin-pkg.crt" \
  -certfile "$ROOT/build/testcert/hyperwin-ca.crt" -passout pass:hyperwin -out "$PFX"
mkdir -p "$OUT/guest/gpu/audio"
cp "$ROOT/build/hwaudio/hwaudio.sys" "$ROOT/build/hwaudio/hwaudio.inf" "$ROOT/guest/drivers/hwaudio/install.ps1" "$PFX" "$OUT/guest/gpu/audio/"
cp "$WIN/scream/Install/helpers/devcon-arm64.exe" "$OUT/guest/gpu/audio/devcon.exe"
# Phone storage as drives: the service and the WinFsp installer it needs.
[ -f "$ROOT/build/files/hwfiles.exe" ] || sh "$ROOT/guest/files/build.sh"
cp "$ROOT/build/files/hwfiles.exe" "$ROOT/build/files/winfsp.msi" "$OUT/guest/gpu/"
# DXVK, vkd3d-proton and Zink, which use.cmd puts beside a program (Send To in Explorer).
cp -R "$ROOT/gpu/work/out/dx" "$OUT/guest/gpu/"
perl -pe 's/\r?\n/\r\n/' "$ROOT/gpu/guest/dx/use.cmd" > "$OUT/guest/gpu/dx/use.cmd"
# The app compares this with C:\HyperWin\gpu\stamp.txt and sends the files again when they differ.
STAMP=$(cd "$OUT/guest/gpu" && find . -type f | LC_ALL=C sort | xargs shasum | shasum | cut -c1-16)
printf '%s\r\n' "$STAMP" > "$OUT/guest/gpu/stamp.txt"
# guest/ lands in C:\HyperWin. Batch files need CRLF line ends.
for f in "$ROOT"/tools/offline/guest/*.cmd; do
  perl -pe 's/\r?\n/\r\n/' "$f" > "$OUT/guest/$(basename "$f")"
done
find "$OUT" -type f | sort
