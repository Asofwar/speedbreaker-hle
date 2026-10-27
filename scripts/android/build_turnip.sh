#!/bin/bash
# SpeedBreaker. GPL-3.0-or-later (see COPYING).
#
# scripts/android/build_turnip.sh <mesa checkout> <output folder>: Mesa's Turnip
# (Vulkan for Adreno, the KGSL kernel interface Android phones have) built with
# the Android NDK for arm64-v8a, Android 13 (API 33) or later, as
# <output folder>/libvulkan_freedreno.so. The game can run on it instead of the
# phone's own Vulkan driver (NFSMW_VK_DRIVER=turnip; see video/vk_driver.h):
# where that driver's shader compiler gets the game's shaders wrong (Adreno 840,
# Qualcomm V@0842.52: its car vertex shaders hang the GPU).
# Needs meson (1.4+), python-mako, bison and flex; ANDROID_NDK as build_deps.sh.
set -euo pipefail
[ $# -eq 2 ] || { echo "usage: $0 <mesa checkout> <output folder>" >&2; exit 2; }
NDK=${ANDROID_NDK:-/opt/android-ndk-r29}
MESA=$(cd "$1" && pwd)
mkdir -p "$2"
OUT=$(cd "$2" && pwd)
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
meson setup "$BUILD" "$MESA" --cross-file "$CROSS" -Dbuildtype=release -Db_lto=false -Dstrip=true \
    -Dplatforms=android -Dplatform-sdk-version=33 -Dandroid-stub=true -Dandroid-libbacktrace=disabled \
    -Dgallium-drivers= -Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl \
    -Degl=disabled -Dgles1=disabled -Dgles2=disabled -Dglx=disabled -Dopengl=false \
    -Dllvm=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled > "$OUT/configure.log" 2>&1 \
    || { tail -30 "$OUT/configure.log" >&2; exit 1; }
ninja -C "$BUILD" > "$OUT/build.log" 2>&1 || { grep -E "error|FAILED" "$OUT/build.log" | head -20 >&2; exit 1; }
cp "$BUILD/src/freedreno/vulkan/libvulkan_freedreno.so" "$OUT/"
echo "done: $OUT/libvulkan_freedreno.so ($(du -h "$OUT/libvulkan_freedreno.so" | cut -f1)), Mesa $(git -C "$MESA" log -1 --format=%h 2>/dev/null)"
