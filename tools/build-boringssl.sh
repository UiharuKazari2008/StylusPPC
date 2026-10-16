#!/bin/sh
# Rebuilds the prebuilt BoringSSL in third_party/boringssl (static libssl.a + libcrypto.a per ABI,
# headers, licence) that the secure web proxy (core/src/net/tlsproxy.cpp) links against.
# Uses the Android SDK's CMake/Ninja and the newest NDK; no Go or Perl needed (BoringSSL ships its
# generated sources). Run from Git Bash:  sh tools/build-boringssl.sh
#   BSSL_TAG  release tag to build (default below; pin a tag that has been out a few weeks)
#   BSSL_SRC  an existing checkout to use instead of cloning (build-cli/boringssl-src by default)
set -e
export MSYS_NO_PATHCONV=1
HERE=$(cd "$(dirname "$0")/.." && pwd)
TAG="${BSSL_TAG:-0.20260903.0}"
SDK="${ANDROID_SDK:-$LOCALAPPDATA/Android/Sdk}"
NDK=$(ls -d "$SDK"/ndk/* | sort | tail -1)
CMAKE_DIR=$(ls -d "$SDK"/cmake/* | sort | tail -1)/bin
export PATH="$CMAKE_DIR:$PATH"
SRC="${BSSL_SRC:-$HERE/build-cli/boringssl-src}"
OUT="$HERE/third_party/boringssl"
if [ ! -d "$SRC/.git" ]; then
    git -c core.longpaths=true clone -q --depth 1 --branch "$TAG" https://boringssl.googlesource.com/boringssl "$SRC"
fi
STRIP="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip"
# cmake.exe is a Windows program: give it C:/... paths (MSYS_NO_PATHCONV stops Git Bash converting them)
win() { if command -v cygpath >/dev/null; then cygpath -m "$1"; else echo "$1"; fi; }
for ABI in arm64-v8a x86_64; do
    B="$HERE/build-cli/boringssl-$ABI"
    cmake -S "$(win "$SRC")" -B "$(win "$B")" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$(win "$NDK")/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=$ABI -DANDROID_PLATFORM=android-26 -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
        -DCMAKE_C_FLAGS="-fvisibility=hidden" -DCMAKE_CXX_FLAGS="-fvisibility=hidden" >/dev/null
    cmake --build "$(win "$B")" --target ssl crypto
    mkdir -p "$OUT/lib/$ABI"
    for L in ssl crypto; do
        cp "$B/lib$L.a" "$OUT/lib/$ABI/lib$L.a"
        "$STRIP" --strip-debug "$(win "$OUT/lib/$ABI/lib$L.a")"
    done
done
rm -rf "$OUT/include"
mkdir -p "$OUT/include"
cp -r "$SRC/include/openssl" "$OUT/include/"
cp "$SRC/LICENSE" "$OUT/LICENSE"
printf 'BoringSSL %s (%s)\nBuilt by tools/build-boringssl.sh for arm64-v8a and x86_64, Android API 26, NDK %s.\n' \
    "$TAG" "$(git -C "$(win "$SRC")" rev-parse HEAD)" "$(basename "$NDK")" > "$OUT/VERSION"
ls -la "$OUT"/lib/*
