#!/bin/sh
# Builds the headless runner inside WSL: native x86-64 (interpreter) and static AArch64 (JIT, run it
# with qemu-aarch64). Usage from Windows: wsl -d Ubuntu -- sh tools/build-wsl.sh
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT="$HOME/wmbuild"
mkdir -p "$OUT"
cd "$HERE"
SRCS="core/src/arm/cpu.cpp core/src/arm/disasm.cpp core/src/arm/jit.cpp core/src/board/bus.cpp core/src/board/s3c2410.cpp core/src/board/devices.cpp core/src/board/de_devices.cpp core/src/board/nled.cpp core/src/board/audio.cpp core/src/board/folder_share.cpp core/src/board/snapshot.cpp core/src/board/machine.cpp core/src/util/log.cpp core/cli/main.cpp"
FLAGS="-std=c++17 -O2 -g -Wall -Wno-unused-parameter -Icore/src -pthread"
g++ $FLAGS $SRCS -o "$OUT/wmcore-cli-x86_64" & P1=$!
aarch64-linux-gnu-g++ $FLAGS -static $SRCS -o "$OUT/wmcore-cli-aarch64" & P2=$!
wait $P1 && wait $P2
echo "built $OUT/wmcore-cli-x86_64 and $OUT/wmcore-cli-aarch64"
