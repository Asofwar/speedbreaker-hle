#!/bin/bash
# SpeedBreaker. GPL-3.0-or-later (see COPYING).
#
# scripts/android/build_deps.sh <deps folder>: the libraries scripts/android/build.sh
# links, built for Android phones and handhelds (arm64-v8a, Android 13 / API 33 or
# later) with the Android NDK. The folder is build.sh's ANDROID_DEPS afterwards:
#   prefix/     SDL3 3.4.16 (shared: libSDL3.so, which SDL's own Java side loads),
#               glslang 16.6.0 (static, without its optimizer) and FFmpeg-XMA
#               (FFmpeg 7.1.1's XMA decoders with patches/ffmpeg,
#               scripts/build_ffmpeg_xma.sh, static), volk 1.4.363 (Vulkan
#               through function pointers: the game can run on a Vulkan
#               driver of its own, video/vk_driver.cpp) and libadrenotools
#               (BSD-2, Billy Laws: loads that driver into the system's Vulkan
#               loader; static, its hook libraries shared)
#   sdl-java/   SDL's Java side (org.libsdl.app) from the same SDL release, for
#               the Android project (android/)
# The sources go to <deps folder>/downloads once (about 60 MB), each checked
# against its SHA-256 below (libadrenotools: its git commit), as
# scripts/ios/build_deps.sh does. FFmpeg's comes from tools/ffmpeg-src when
# scripts/setup_tools.sh has fetched it already. Needs the NDK (ANDROID_NDK,
# default /opt/android-ndk-r29), CMake, Ninja, make, git and curl; a few
# minutes. Run it again to rebuild everything.
set -euo pipefail
[ $# -eq 1 ] || { echo "usage: $0 <deps folder>" >&2; exit 2; }
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
NDK=${ANDROID_NDK:-/opt/android-ndk-r29}
[ -f "$NDK/build/cmake/android.toolchain.cmake" ] || { echo "build_deps.sh: no Android NDK in $NDK (set ANDROID_NDK)" >&2; exit 1; }
mkdir -p "$1"
DEPS=$(cd "$1" && pwd)
DL=$DEPS/downloads
WORK=$DEPS/work
PREFIX=$DEPS/prefix
API=33
ABI=arm64-v8a
JOBS=${JOBS:-$(nproc)}
TOOLCHAIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64

SDL3=SDL3-3.4.16.tar.gz
SDL3_URL=https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/$SDL3
SDL3_SHA256=7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68
GLSLANG=glslang-16.6.0.tar.gz
GLSLANG_URL=https://github.com/KhronosGroup/glslang/archive/refs/tags/16.6.0.tar.gz
GLSLANG_SHA256=9c09b901149c729df745057dafa815278aaa101b84d2b6e14f16a42de52f97f2
FFMPEG=ffmpeg-7.1.1.tar.xz
FFMPEG_URL=https://ffmpeg.org/releases/$FFMPEG
FFMPEG_SHA256=733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1
VOLK=volk-vulkan-sdk-1.4.363.0.tar.gz
VOLK_URL=https://github.com/zeux/volk/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz
VOLK_SHA256=1547d8d74395d4048fb3f4a6313da56db626e89b55db238d0d7e8944c8a645f3
ADRENOTOOLS_URL=https://github.com/bylaws/libadrenotools.git
ADRENOTOOLS_COMMIT=8fae8ce254dfc1344527e05301e43f37dea2df80     # its lib/linkernsbypass: aa3975893d83ef1bc84c321ec60c65fbf1287887

# fetch <file> <url> <sha256>: into downloads/ unless it's there already, then checked.
fetch() {
    local file=$1 url=$2 sum=$3
    if [ ! -f "$DL/$file" ]; then
        curl -fL --retry 3 -o "$DL/$file.part" "$url"
        mv "$DL/$file.part" "$DL/$file"
    fi
    echo "$sum  $DL/$file" | sha256sum -c - > /dev/null ||
        { echo "build_deps.sh: $DL/$file isn't the expected file (SHA-256); delete it and run again" >&2; exit 1; }
}
mkdir -p "$DL"
[ -f "$DL/$FFMPEG" ] || [ ! -f "$ROOT/tools/ffmpeg-src/$FFMPEG" ] || cp "$ROOT/tools/ffmpeg-src/$FFMPEG" "$DL/"
fetch "$SDL3" "$SDL3_URL" "$SDL3_SHA256"
fetch "$GLSLANG" "$GLSLANG_URL" "$GLSLANG_SHA256"
fetch "$FFMPEG" "$FFMPEG_URL" "$FFMPEG_SHA256"
fetch "$VOLK" "$VOLK_URL" "$VOLK_SHA256"
if [ ! -d "$DL/libadrenotools" ]; then
    git clone -q "$ADRENOTOOLS_URL" "$DL/libadrenotools.part"
    git -C "$DL/libadrenotools.part" checkout -q "$ADRENOTOOLS_COMMIT"
    git -C "$DL/libadrenotools.part" submodule -q update --init
    mv "$DL/libadrenotools.part" "$DL/libadrenotools"
fi
[ "$(git -C "$DL/libadrenotools" rev-parse HEAD)" = "$ADRENOTOOLS_COMMIT" ] ||
    { echo "build_deps.sh: $DL/libadrenotools isn't at $ADRENOTOOLS_COMMIT; delete it and run again" >&2; exit 1; }

rm -rf "$WORK" "$PREFIX" "$DEPS/sdl-java"
mkdir -p "$WORK" "$PREFIX"
trap 'echo "build_deps.sh: failed; the logs are in $WORK" >&2' ERR

# CMake projects: a Release build for Android (arm64-v8a, API 33) installed into prefix/.
android_cmake() {  # android_cmake <source> <build> [options...]
    local src=$1 build=$2; shift 2
    cmake -S "$src" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=$ABI -DANDROID_PLATFORM=android-$API \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_FIND_ROOT_PATH="$PREFIX" "$@" > "$build.configure.log" 2>&1
    ninja -C "$build" -j"$JOBS" install > "$build.build.log" 2>&1
}

echo "SDL3 3.4.16"
tar -xzf "$DL/$SDL3" -C "$WORK"
android_cmake "$WORK/SDL3-3.4.16" "$WORK/sdl3" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF
mkdir -p "$DEPS/sdl-java"
cp -R "$WORK/SDL3-3.4.16/android-project/app/src/main/java/org" "$DEPS/sdl-java/"

echo "glslang 16.6.0"
tar -xzf "$DL/$GLSLANG" -C "$WORK"
android_cmake "$WORK/glslang-16.6.0" "$WORK/glslang" -DBUILD_SHARED_LIBS=OFF -DENABLE_OPT=OFF -DGLSLANG_TESTS=OFF \
    -DENABLE_GLSLANG_BINARIES=OFF -DGLSLANG_ENABLE_INSTALL=ON

echo "FFmpeg-XMA 7.1.1"
"$ROOT/scripts/build_ffmpeg_xma.sh" "$DL/$FFMPEG" "$WORK/ffmpeg-xma" \
    --enable-cross-compile --target-os=android --arch=aarch64 \
    --cc="$TOOLCHAIN/bin/aarch64-linux-android$API-clang" --cxx="$TOOLCHAIN/bin/aarch64-linux-android$API-clang++" \
    --ar="$TOOLCHAIN/bin/llvm-ar" --ranlib="$TOOLCHAIN/bin/llvm-ranlib" --nm="$TOOLCHAIN/bin/llvm-nm" \
    --strip="$TOOLCHAIN/bin/llvm-strip"
cp -R "$WORK/ffmpeg-xma/include/." "$PREFIX/include/"
cp "$WORK/ffmpeg-xma/lib/libavcodec.a" "$WORK/ffmpeg-xma/lib/libavutil.a" "$PREFIX/lib/"

echo "volk 1.4.363"
tar -xzf "$DL/$VOLK" -C "$WORK"
mkdir -p "$PREFIX/include/volk"
cp "$WORK/volk-vulkan-sdk-1.4.363.0/volk.h" "$WORK/volk-vulkan-sdk-1.4.363.0/volk.c" "$PREFIX/include/volk/"

echo "libadrenotools ${ADRENOTOOLS_COMMIT:0:7}"
cp -R "$DL/libadrenotools" "$WORK/libadrenotools-src"
android_cmake "$WORK/libadrenotools-src" "$WORK/adrenotools" -DGEN_INSTALL_TARGET=ON
cp "$WORK/adrenotools/lib/linkernsbypass/liblinkernsbypass.a" "$PREFIX/lib/"

# Every library is for arm64 Android.
for lib in "$PREFIX"/lib/libSDL3.so "$PREFIX"/lib/libglslang.a "$PREFIX"/lib/libavcodec.a; do
    # (grep -c, not -q: -q stops reading, and pipefail would take readelf's SIGPIPE for a failure)
    [ "$("$TOOLCHAIN/bin/llvm-readelf" -h "$lib" 2>/dev/null | grep -c "AArch64")" -gt 0 ] ||
        { echo "build_deps.sh: $lib isn't built for AArch64" >&2; exit 1; }
done
rm -rf "$WORK"
echo "done: ANDROID_DEPS=$DEPS ($(du -sh "$PREFIX" | cut -f1) prefix, sources kept in downloads/)"
