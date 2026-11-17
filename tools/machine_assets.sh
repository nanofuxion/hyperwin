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
# Guest agent, Wintun, and the Scream sound driver re-signed with our package certificate (a leaf
# under our own root, which Windows needs to call a signature valid; Scream's own
# certificate has expired; the catalog's file hashes do not cover signatures, so they stay valid).
WIN=$ROOT/build/third_party/win
SIGN="osslsigncode sign -certs $ROOT/build/testcert/hyperwin-pkg-chain.crt -key $ROOT/build/testcert/hyperwin-pkg.key -h sha256"
mkdir -p "$OUT/guest/audio"
cp "$ROOT/build/agent/hwagent.exe" "$WIN/wintun/wintun/bin/arm64/wintun.dll" "$ROOT/build/testcert/hyperwin-ca.cer" "$ROOT/build/testcert/hyperwin-pkg.cer" "$OUT/guest/"
cp "$WIN/scream/Install/driver/arm64/Scream.inf" "$OUT/guest/audio/"
cp "$WIN/scream/Install/helpers/devcon-arm64.exe" "$OUT/guest/audio/devcon.exe"
$SIGN -in "$WIN/scream/Install/driver/arm64/Scream.sys" -out "$OUT/guest/audio/Scream.sys" >/dev/null
$SIGN -in "$WIN/scream/Install/driver/arm64/scream.cat" -out "$OUT/guest/audio/scream.cat" >/dev/null
# guest/ lands in C:\HyperWin. Batch files need CRLF line ends.
for f in "$ROOT"/tools/offline/guest/*.cmd; do
  perl -pe 's/\r?\n/\r\n/' "$f" > "$OUT/guest/$(basename "$f")"
done
find "$OUT" -type f | sort
