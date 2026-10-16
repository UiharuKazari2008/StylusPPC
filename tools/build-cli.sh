#!/bin/sh
# Builds the headless runner with the NDK clang and pushes it to a device.
#   ARCH=aarch64 (default, phone) or x86_64 (Android emulator on the PC; interpreter only)
#   ANDROID_SERIAL selects the adb device when several are connected.
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
SRCS="core/src/arm/cpu.cpp core/src/arm/disasm.cpp core/src/arm/jit.cpp core/src/board/bus.cpp core/src/board/s3c2410.cpp core/src/board/devices.cpp core/src/board/de_devices.cpp core/src/board/nled.cpp core/src/board/modem.cpp core/src/board/irda.cpp core/src/board/audio.cpp core/src/board/folder_share.cpp core/src/board/pcmcia.cpp core/src/board/camera.cpp core/src/board/codepatch.cpp core/src/net/usernet.cpp core/src/board/snapshot.cpp core/src/board/machine.cpp core/src/util/log.cpp core/cli/main.cpp core/cli/irda_link.cpp"
"$CXX" -std=c++17 -O2 -g -Wall -Wno-unused-parameter -static-libstdc++ -Icore/src $SRCS -llog -o "$OUT/wmcore-cli"
ADB="$SDK/platform-tools/adb"
"$ADB" shell mkdir -p /data/local/tmp/wm
"$ADB" push "$OUT/wmcore-cli" /data/local/tmp/wm/wmcore-cli >/dev/null
"$ADB" shell chmod 755 /data/local/tmp/wm/wmcore-cli
echo "built and pushed $OUT/wmcore-cli to ${ANDROID_SERIAL:-default device}"
