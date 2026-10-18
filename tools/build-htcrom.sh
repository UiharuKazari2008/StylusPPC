#!/bin/sh
# Builds htcrom (core/cli/htcrom.cpp: the app's HTC ROM import from a shell) with the NDK clang and
# pushes it to /data/local/tmp/wm on the device. ARCH / ANDROID_SERIAL as in build-cli.sh.
set -e
export MSYS_NO_PATHCONV=1
HERE=$(cd "$(dirname "$0")/.." && pwd)
SDK="${ANDROID_SDK:-$LOCALAPPDATA/Android/Sdk}"
NDK=$(ls -d "$SDK"/ndk/* | sort | tail -1)
ARCH="${ARCH:-aarch64}"
CXX="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/${ARCH}-linux-android29-clang++"
cd "$HERE"
OUT="build-cli/$ARCH"
mkdir -p "$OUT"
"$CXX" -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter -static-libstdc++ -Icore/src core/src/htc/romkit.cpp core/cli/htcrom.cpp -o "$OUT/htcrom"
ADB="$SDK/platform-tools/adb"
"$ADB" shell mkdir -p /data/local/tmp/wm
"$ADB" push "$OUT/htcrom" /data/local/tmp/wm/htcrom >/dev/null
"$ADB" shell chmod 755 /data/local/tmp/wm/htcrom
echo "built and pushed $OUT/htcrom to ${ANDROID_SERIAL:-default device}"
