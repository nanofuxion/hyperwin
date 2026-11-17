#!/bin/bash
# Writes the meson cross file for ARM64X builds: arm64x/cross.sh out.ini
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(cd "$HERE/../../../build/third_party/win/llvm-mingw/bin" && pwd)
cat > "$1" <<INI
[binaries]
c = '$HERE/clang'
cpp = '$HERE/clang++'
ar = '$T/llvm-ar'
strip = '$T/llvm-strip'
windres = '$T/arm64ec-w64-mingw32-windres'
widl = '$T/arm64ec-w64-mingw32-widl'
widl-mingw-tools-fallback = '$T/arm64ec-w64-mingw32-widl'
[properties]
needs_exe_wrapper = true
[built-in options]
c_args = ['-march=armv8.2-a']
cpp_args = ['-march=armv8.2-a']
c_link_args = ['-static']
cpp_link_args = ['-static']
[host_machine]
system = 'windows'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
INI
