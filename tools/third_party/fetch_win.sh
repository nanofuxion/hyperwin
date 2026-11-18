#!/bin/sh
# Downloads what the Windows-side pieces need into build/third_party/win:
#   llvm-mingw  compiler for guest/agent (Windows ARM64 user mode)
#   wintun      network adapter the agent drives (prebuilt, signed)
#   scream      only for its ARM64 devcon.exe, which installs the root sound device (hwaudio)
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DL=$ROOT/build/third_party/dl
WIN=$ROOT/build/third_party/win
mkdir -p "$DL" "$WIN"
get() { [ -f "$DL/$1" ] || curl -fL -o "$DL/$1" "$2"; }
get llvm-mingw.tar.xz https://github.com/mstorsjo/llvm-mingw/releases/download/20261104/llvm-mingw-20261104-ucrt-macos-universal.tar.xz
get wintun.zip https://www.wintun.net/builds/wintun-0.14.1.zip
get Scream4.0.zip https://github.com/duncanthrax/scream/releases/download/4.0/Scream4.0.zip
[ -d "$WIN/llvm-mingw" ] || { mkdir "$WIN/llvm-mingw" && tar -xJf "$DL/llvm-mingw.tar.xz" -C "$WIN/llvm-mingw" --strip-components=1; }
mkdir -p "$WIN/wintun" "$WIN/scream"
unzip -o -q "$DL/wintun.zip" -d "$WIN/wintun"
unzip -o -q "$DL/Scream4.0.zip" -d "$WIN/scream"
ls "$WIN"
