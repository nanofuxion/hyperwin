#!/bin/bash
# Builds hwgpu_host against the macOS gfxstream that SweetCider builds, and runs ring_test.
#   gpu/host/test-mac.sh [gfxstream work dir]
# The work dir defaults to SweetCider's (tools/gfxstream-android/work, with build-mac/ in it).
# 0x28: STREAM_RENDERER_FLAGS_USE_VK_BIT | STREAM_RENDERER_FLAGS_USE_SURFACELESS_BIT
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
GFX=${1:-$HERE/../../../sweetcider-gpu/tools/gfxstream-android/work}
OUT=$HERE/../work/mac
mkdir -p "$OUT"
clang++ -std=c++17 -O1 -g -Wall -Wextra -Wno-missing-field-initializers \
  -I"$HERE/../include" -I"$HERE/../../include" -I"$GFX/include" \
  "$HERE/hwgpu_host.cpp" "$HERE/ring_test.cpp" -L"$GFX/build-mac/host" -lgfxstream_backend \
  -Wl,-rpath,"$GFX/build-mac/host" -o "$OUT/ring_test"
DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib "$OUT/ring_test" 0x28
