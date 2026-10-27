#!/bin/bash
# SpeedBreaker. GPL-3.0-or-later (see COPYING).
#
# scripts/android/build_turnip.sh <work folder>: Mesa's Turnip (Vulkan for Adreno,
# the KGSL kernel interface Android phones have) with patches/mesa applied, built
# with the Android NDK for arm64-v8a, Android 13 (API 33) or later, as
# <work folder>/libvulkan_freedreno.so. scripts/android/build.sh packages it with
# TURNIP=<that file>, and the game then runs on it instead of the phone's own
# Vulkan driver (NFSMW_VK_DRIVER=system for that one; video/presenter.cpp,
# LoadVulkan): Qualcomm's V@0842.52 on an Adreno 840 hangs the GPU on the
# game's car vertex shaders.
#   patches/mesa/0001  ir3: an fmadz that reached the backend (an assert)
#   patches/mesa/0002  linear swapchain buffers, for a gralloc Mesa can't read
# Mesa is cloned into <work folder>/mesa at the commit below once. Needs git,
# meson (1.4+), python-mako, bison and flex; ANDROID_NDK as build_deps.sh.
set -euo pipefail
[ $# -eq 1 ] || { echo "usage: $0 <work folder>" >&2; exit 2; }
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
NDK=${ANDROID_NDK:-/opt/android-ndk-r29}
mkdir -p "$1"
OUT=$(cd "$1" && pwd)
MESA_URL=https://gitlab.freedesktop.org/mesa/mesa.git
MESA_COMMIT=2770a94bd7f73fc36bbd826cf5e820658ae79655
MESA=$OUT/mesa
if [ ! -d "$MESA/.git" ]; then
    git init -q "$MESA"
    git -C "$MESA" fetch -q --depth 1 "$MESA_URL" "$MESA_COMMIT"
    git -C "$MESA" checkout -q FETCH_HEAD
fi
[ "$(git -C "$MESA" rev-parse HEAD)" = "$MESA_COMMIT" ] || { echo "build_turnip.sh: $MESA isn't at $MESA_COMMIT" >&2; exit 1; }
# The patches on a clean tree (this clone is the script's own).
git -C "$MESA" checkout -q -- .
for p in "$ROOT"/patches/mesa/*.patch; do git -C "$MESA" apply "$p"; done

BIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
BUILD=$OUT/build
CROSS=$OUT/android-aarch64.ini
cat > "$CROSS" <<EOF
[binaries]
c = ['$BIN/aarch64-linux-android33-clang']
cpp = ['$BIN/aarch64-linux-android33-clang++', '-fno-exceptions', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-static-libstdc++', '-Wno-c++11-narrowing']
c_ld = 'lld'
cpp_ld = 'lld'
ar = '$BIN/llvm-ar'
strip = '$BIN/llvm-strip'
pkg-config = ['/usr/bin/false']

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8'
endian = 'little'
EOF
rm -rf "$BUILD"
meson setup "$BUILD" "$MESA" --cross-file "$CROSS" -Dbuildtype=release -Db_lto=false \
    -Dplatforms=android -Dplatform-sdk-version=33 -Dandroid-stub=true -Dandroid-libbacktrace=disabled \
    -Dgallium-drivers= -Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl \
    -Degl=disabled -Dgles1=disabled -Dgles2=disabled -Dglx=disabled -Dopengl=false \
    -Dllvm=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled > "$OUT/configure.log" 2>&1 \
    || { tail -30 "$OUT/configure.log" >&2; exit 1; }
ninja -C "$BUILD" > "$OUT/build.log" 2>&1 || { grep -E "error|FAILED" "$OUT/build.log" | head -20 >&2; exit 1; }
# Without debug information, with the symbol table (crash reports, llvm-symbolizer).
"$BIN/llvm-strip" --strip-debug -o "$OUT/libvulkan_freedreno.so" "$BUILD/src/freedreno/vulkan/libvulkan_freedreno.so"
echo "done: $OUT/libvulkan_freedreno.so ($(du -h "$OUT/libvulkan_freedreno.so" | cut -f1)), Mesa ${MESA_COMMIT:0:10} + $(ls "$ROOT"/patches/mesa/*.patch | wc -l) patches"
