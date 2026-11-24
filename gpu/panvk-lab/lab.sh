#!/bin/bash
# Tests a Mali Vulkan driver on the Pixel from the ADB shell only: no screen, no taps, no app start.
#   lab.sh status                     is the phone busy with the main thread's Windows VM? hot?
#   lab.sh probe [driver.so] [VAR=1..] what DXVK 3.1.1 needs, checked on the real GPU
#   lab.sh clean                      removes everything this script put on the phone
# `run-as` gives the shell the HyperWin debug app's permissions, which is what /dev/mali0 needs
# (the plain shell may not open it). Files live in the app's cache/panvk-lab and nothing else of the
# app is touched. With no driver.so, the PanVK the app already holds in files/drivers is used.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
PKG=dev.hyperwin.app
RUN() { adb shell "run-as $PKG sh -c '$1'"; }

status() {
  # The main thread boots Windows on the phone: crosvm_hyperwin running means GPU timings are not clean.
  local vm; vm=$(adb shell pidof crosvm_hyperwin || true)
  local gpu; gpu=$(adb shell "ps -A | grep -c ':gpu'" | tr -d '\r' | grep -v '^0$' || true)
  local therm; therm=$(adb shell dumpsys thermalservice | grep -m1 "Thermal Status" || true)
  local freq; freq=$(adb shell "cat /sys/devices/platform/28000000.mali/cur_freq 2>/dev/null || true" | tr -d '\r')
  echo "windows vm : ${vm:-not running}"
  echo "gpu process: $([ -n "$gpu" ] && echo running || echo "not running")"
  echo "thermal    : ${therm:-unknown}   (0 cool .. 3 hot)"
  [ -n "$freq" ] && echo "gpu clock  : $freq"
  [ -z "$vm" ]
}

probe() {
  local drv=${1:-}; [ $# -gt 0 ] && shift
  local NDK=${ANDROID_HOME:-$HOME/Library/Android/sdk}/ndk/27.2.12479018/toolchains/llvm/prebuilt/darwin-x86_64/bin
  local OUT=$HERE/../work/panvk-lab AND=$HERE/../work/android/arm64-v8a
  mkdir -p "$OUT"
  [ "$OUT/probe" -nt "$HERE/probe.cpp" ] || "$NDK/aarch64-linux-android34-clang++" -std=c++17 -O1 -fPIE -pie -Wall \
    -DVK_NO_PROTOTYPES -I"$HERE/../work/Vulkan-Headers/include" "$HERE/probe.cpp" -ldl -nostdlib++ -lc++_shared -o "$OUT/probe"
  status >/dev/null || echo "note: the Windows VM is running. The probe is light, but do not read timings now."
  local stage=/data/local/tmp/panvk_lab_stage
  adb shell "rm -rf $stage; mkdir -p $stage"
  adb push -q "$OUT/probe" "$AND/libc++_shared.so" "$AND/libhardware.so" "$stage/"
  local d=panvk.so
  if [ -n "$drv" ]; then adb push -q "$drv" "$stage/panvk.so"
  else RUN "cp files/drivers/\$(ls files/drivers | grep -m1 -i panvk) cache/panvk-lab/panvk.so" 2>/dev/null || true; fi
  RUN "mkdir -p cache/panvk-lab && cp $stage/probe $stage/libc++_shared.so $stage/libhardware.so cache/panvk-lab/ && chmod 755 cache/panvk-lab/probe"
  [ -n "$drv" ] && RUN "cp $stage/panvk.so cache/panvk-lab/panvk.so"
  adb shell "rm -rf $stage"
  local envs="${*:-}"
  RUN "cd cache/panvk-lab && env $envs LD_LIBRARY_PATH=. ./probe ./$d 2>&1"
}

clean() { RUN "rm -rf cache/panvk-lab"; adb shell "rm -rf /data/local/tmp/panvk_lab_stage"; echo cleaned; }

case "${1:-status}" in
  status) status ;;
  probe)  shift; probe "$@" ;;
  clean)  clean ;;
  *) sed -n 2,6p "$0"; exit 2 ;;
esac
