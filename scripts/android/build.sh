#!/bin/bash
# SpeedBreaker. GPL-3.0-or-later (see COPYING).
#
# scripts/android/build.sh [output folder]: the game for Android (arm64-v8a,
# Android 13 or later), built with the Android NDK in build/android (or
# $ANDROID_BUILD), then gathered for android/'s Gradle project into the output
# folder (default build/android/native):
#   jniLibs/arm64-v8a/libmain.so   the game (the runtime and the recompiled code)
#   jniLibs/arm64-v8a/libSDL3.so   SDL
#   sdl-java/                      SDL's Java side (org.libsdl.app)
# Then the APK (on any machine with the Android SDK and a JDK 17 or later):
#   cd android && ./gradlew assembleRelease -PsbNative=<output folder>
# which writes android/build/outputs/apk/release/SpeedBreaker-release.apk.
#
# Needs ANDROID_DEPS (scripts/android/build_deps.sh's folder), the NDK
# (ANDROID_NDK, default /opt/android-ndk-r29: r28 or later, for libc++'s
# std::atomic_ref) and ppc/ (scripts/recompile.sh from your own default.xex).
# No game data: the game installs from the player's disc image on the device.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
: "${ANDROID_DEPS:?set ANDROID_DEPS: the folder scripts/android/build_deps.sh made}"
NDK=${ANDROID_NDK:-/opt/android-ndk-r29}
[ -f "$NDK/build/cmake/android.toolchain.cmake" ] || { echo "build.sh: no Android NDK in $NDK (set ANDROID_NDK)" >&2; exit 1; }
PREFIX=$ANDROID_DEPS/prefix
BUILD=${ANDROID_BUILD:-$ROOT/build/android}
OUT=${1:-$BUILD/native}
JOBS=${JOBS:-$(nproc)}
STRIP=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip

if [ ! -f "$BUILD/build.ninja" ]; then
    cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33 \
        -DANDROID_STL=c++_static -DCMAKE_FIND_ROOT_PATH="$PREFIX" \
        -DSDL3_DIR="$PREFIX/lib/cmake/SDL3" -Dglslang_DIR="$PREFIX/lib/cmake/glslang" -DFFMPEG_XMA="$PREFIX" \
        -DSB_ANDROID_DEPS="$PREFIX"
fi
ninja -C "$BUILD" -j"$JOBS" SpeedBreaker

mkdir -p "$OUT/jniLibs/arm64-v8a"
# Without debug information (hundreds of MB), with the symbol table: crash
# reports name the recompiled functions from it.
"$STRIP" --strip-debug -o "$OUT/jniLibs/arm64-v8a/libmain.so" "$BUILD/runtime/libmain.so"
cp "$PREFIX/lib/libSDL3.so" "$OUT/jniLibs/arm64-v8a/"
# libadrenotools' hook libraries (loading a Vulkan driver of the app's own), and
# Turnip when TURNIP names scripts/android/build_turnip.sh's
# libvulkan_freedreno.so (NFSMW_VK_DRIVER=turnip runs the game on it).
cp "$PREFIX"/lib/libhook_impl.so "$PREFIX"/lib/libmain_hook.so "$PREFIX"/lib/libfile_redirect_hook.so "$OUT/jniLibs/arm64-v8a/"
rm -f "$OUT/jniLibs/arm64-v8a/libvulkan_freedreno.so"
[ -z "${TURNIP:-}" ] || cp "$TURNIP" "$OUT/jniLibs/arm64-v8a/libvulkan_freedreno.so"
rm -rf "$OUT/sdl-java"
cp -R "$ANDROID_DEPS/sdl-java" "$OUT/sdl-java"
echo "done: $OUT ($(du -sh "$OUT/jniLibs/arm64-v8a/libmain.so" | cut -f1) libmain.so)"
