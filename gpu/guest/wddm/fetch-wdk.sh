#!/bin/sh
# Fetches the WDK and SDK headers (NuGet Microsoft.Windows.WDK.ARM64 and Microsoft.Windows.SDK.CPP,
# 10.0.26100.6584) into gpu/work/wdk, which git ignores. This Mac only: no WDK header, library, tool
# or binary goes into git, the app or the machine assets, and none is copied into our sources
# (wddm/README.md, "Headers and the WDK"). Prints the include directories' parent.
set -eu
cd "$(dirname "$0")/../../.."
V=10.0.26100.6584
D=gpu/work/wdk
mkdir -p "$D"
for p in microsoft.windows.wdk.arm64 microsoft.windows.sdk.cpp; do
  if [ ! -d "$D/$p" ]; then
    curl -sfL -o "$D/$p.zip" "https://api.nuget.org/v3-flatcontainer/$p/$V/$p.$V.nupkg"
    mkdir "$D/$p"
    unzip -q "$D/$p.zip" -d "$D/$p"
    rm "$D/$p.zip"
  fi
done
echo "$D"
