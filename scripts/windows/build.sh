#!/usr/bin/env bash
# SpeedBreaker. GPL-3.0-or-later (see COPYING).
#
#   scripts/windows/build.sh <deps prefix> [build dir]
#
# SpeedBreaker.exe for Windows x86-64, cross-compiled on Linux from this
# checkout: ppc/ generated (scripts/recompile.sh) and tools/ set up
# (scripts/setup_tools.sh) as for the Linux build, the libraries from
# scripts/windows/deps.sh <deps prefix>. The build dir defaults to
# build-windows. The game is one static .exe: next to it it needs only the
# graphics driver's vulkan-1.dll, which Windows has with any Vulkan driver.
set -euo pipefail
[ $# -ge 1 ] || { echo "usage: $0 <deps prefix> [build dir]" >&2; exit 2; }
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEPS="$(cd "$1" && pwd)"
BUILD="${2:-$ROOT/build-windows}"
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ROOT/scripts/windows/mingw-toolchain.cmake" \
    -DSB_WIN_DEPS="$DEPS" -DCMAKE_BUILD_TYPE=Release -DFFMPEG_XMA="$DEPS/ffmpeg-xma" -DCMAKE_PREFIX_PATH="$DEPS" \
    -DVulkan_INCLUDE_DIR="$DEPS/include" -DVulkan_LIBRARY="$DEPS/lib/libvulkan-1.a" > "$BUILD.configure.log" ||
    { tail -30 "$BUILD.configure.log" >&2; exit 1; }
ninja -C "$BUILD" SpeedBreaker mp_server
echo "build.sh: $BUILD/runtime/SpeedBreaker.exe"
