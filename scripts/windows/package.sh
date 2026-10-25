#!/usr/bin/env bash
# SpeedBreaker. GPL-3.0-or-later (see COPYING).
#
#   scripts/windows/package.sh <deps prefix> <build dir> <out dir>
#
# The Windows download from scripts/windows/build.sh's build: <out dir>/
# SpeedBreaker/ with SpeedBreaker.exe (stripped; its debug information in
# <out dir>/symbols/), README.txt and licenses/, zipped as
# <out dir>/SpeedBreaker-windows-x86_64.zip with its .sha256. The exe is
# static: SDL3, glslang, SPIRV-Tools, FFmpeg-XMA and mingw-w64's C and C++
# runtime are inside it, so licenses/ holds each of theirs next to the
# game's own notices. The Vulkan loader (vulkan-1.dll) is the graphics
# driver's. Never any game data.
set -euo pipefail
[ $# -eq 3 ] || { echo "usage: $0 <deps prefix> <build dir> <out dir>" >&2; exit 2; }
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEPS="$(cd "$1" && pwd)"
BUILD="$(cd "$2" && pwd)"
OUT="$3"
EXE="$BUILD/runtime/SpeedBreaker.exe"
[ -f "$EXE" ] || { echo "package.sh: no $EXE (scripts/windows/build.sh)" >&2; exit 1; }
VERSION="$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$BUILD/CMakeCache.txt")"
REVISION="$(git -C "$ROOT" describe --always --dirty 2>/dev/null || echo unknown)"
case "$REVISION" in *-dirty) echo "package.sh: warning: the build is $REVISION (uncommitted changes): not for release" >&2 ;; esac
STAGE="$OUT/SpeedBreaker"
rm -rf "$STAGE" "$OUT/symbols" "$OUT/SpeedBreaker-windows-x86_64.zip"
mkdir -p "$STAGE/licenses" "$OUT/symbols"

# The exe, stripped, and its debug information for crash reports.
x86_64-w64-mingw32-objcopy --only-keep-debug "$EXE" "$OUT/symbols/SpeedBreaker-$VERSION-$REVISION-windows-x86_64.debug"
x86_64-w64-mingw32-strip -o "$STAGE/SpeedBreaker.exe" "$EXE"
# The multiplayer server (multiplayer/MULTIPLAYER_GUIDE.md), its races.
x86_64-w64-mingw32-strip -o "$STAGE/mp_server.exe" "$BUILD/runtime/mp_server.exe"
mkdir -p "$STAGE/multiplayer"
cp "$ROOT/scripts/mp/races.example.txt" "$ROOT/docs/MULTIPLAYER_GUIDE.md" "$STAGE/multiplayer/"

# No folder of the build machine in it (as make_bundle.sh checks the
# SteamOS binary): the home folder only where a path starts.
leaks="$( { grep -aoF -e "$ROOT" -e "$BUILD" -e "$DEPS" "$STAGE/SpeedBreaker.exe" || true
           [ "${#HOME}" -gt 1 ] && grep -aoP "(?<![[:alnum:]._/-])\Q$HOME/\E" "$STAGE/SpeedBreaker.exe" || true; } | sort | uniq -c)"
if [ -n "$leaks" ]; then
    echo "package.sh: SpeedBreaker.exe names the build machine's folders:" >&2
    echo "$leaks" >&2
    [ "${SB_ALLOW_PATHS:-0}" = 1 ] || exit 1
fi

# Licenses: the game's notices, then each library compiled into the exe.
for f in COPYING NOTICE THIRD_PARTY_NOTICES.md; do cp "$ROOT/$f" "$STAGE/licenses/"; done
cp -R "$ROOT/LICENSES" "$STAGE/licenses/LICENSES"
copy_license() {  # copy_license <name> <file>...
    local name="$1"
    shift
    mkdir -p "$STAGE/licenses/$name"
    for f in "$@"; do
        [ -f "$f" ] || { echo "package.sh: no $f" >&2; exit 1; }
        cp "$f" "$STAGE/licenses/$name/"
    done
}
copy_license sdl3 "$DEPS/src/SDL/LICENSE.txt"
copy_license glslang "$DEPS/src/glslang/LICENSE.txt" "$DEPS/src/glslang/LICENSES/"*
copy_license spirv-tools "$DEPS/src/SPIRV-Tools/LICENSE"
copy_license mingw-w64 /usr/share/licenses/mingw-w64-crt/COPYING.MinGW-w64.txt /usr/share/licenses/mingw-w64-crt/COPYING.MinGW-w64-runtime.txt
copy_license libstdc++ /usr/share/licenses/libstdc++/*
copy_license libgcc /usr/share/licenses/libgcc/*
gcc_version="$(x86_64-w64-mingw32-gcc -dumpfullversion)"
mingw_version="$(pacman -Q mingw-w64-crt 2>/dev/null | cut -d' ' -f2 || echo unknown)"
{
    echo "SpeedBreaker is free software under the GPL-3.0-or-later (COPYING, NOTICE). What is"
    echo "compiled into it, and each license, is in THIRD_PARTY_NOTICES.md and LICENSES/."
    echo
    echo "Also compiled into SpeedBreaker.exe (a static program: no DLLs of its own):"
    echo "  sdl3/: SDL $(sed -n 's/^SDL=release-//p' "$ROOT/scripts/windows/deps.sh"), zlib"
    echo "  glslang/, spirv-tools/: $(sed -n 's/^VK=//p' "$ROOT/scripts/windows/deps.sh")"
    echo "  FFmpeg 7.1.1 (XMA decoders), LGPL-2.1-or-later: LICENSES/FFmpeg-LGPL-2.1.txt"
    echo "  mingw-w64/: the mingw-w64 runtime $mingw_version (CRT and winpthreads)"
    echo "  libstdc++/, libgcc/: GCC $gcc_version's runtime, GPL-3.0 with the GCC Runtime Library Exception"
    echo
    echo "The complete source of FFmpeg as built here is the release's"
    echo "SpeedBreaker-$VERSION-third-party-source.tar.xz (THIRD_PARTY_NOTICES.md)."
} > "$STAGE/licenses/README.txt"

cat > "$STAGE/README.txt" <<EOF
SpeedBreaker $VERSION for Windows (x86-64)
https://github.com/Asofwar/speedbreaker-hle

Need for Speed: Most Wanted (Xbox 360), recompiled to run natively. No game
files are included: the first start asks for your own USA disc image (.iso)
or the files extracted from it, and installs the game from it.

1. Unzip this folder anywhere (not into Program Files).
2. Start SpeedBreaker.exe. A Vulkan graphics driver is needed (any NVIDIA,
   AMD or Intel driver of the last few years has one).

Saves, settings and the installed game are in %LOCALAPPDATA%\\SpeedBreaker,
logs in its logs\\ folder. F1 (or Back + Start on a controller) opens the
settings. Online play: docs/online.md in the repository.
EOF
sed -i 's/$/\r/' "$STAGE/README.txt" "$STAGE/licenses/README.txt"
echo "SpeedBreaker $VERSION ($REVISION), Windows x86-64, built with clang $(clang -dumpversion) and mingw-w64 $mingw_version" > "$STAGE/build-info.txt"

# The zip, its entries in name order with the exe's time.
python3 - "$OUT" <<'PY'
import os, sys, time, zipfile
out = sys.argv[1]
stage = os.path.join(out, "SpeedBreaker")
stamp = time.gmtime(os.path.getmtime(os.path.join(stage, "SpeedBreaker.exe")))[:6]
with zipfile.ZipFile(os.path.join(out, "SpeedBreaker-windows-x86_64.zip"), "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for dirpath, dirnames, filenames in os.walk(stage):
        dirnames.sort()
        for name in sorted(filenames):
            path = os.path.join(dirpath, name)
            info = zipfile.ZipInfo(os.path.relpath(path, out).replace(os.sep, "/"), stamp)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            with open(path, "rb") as f:
                z.writestr(info, f.read())
PY
(cd "$OUT" && sha256sum SpeedBreaker-windows-x86_64.zip > SpeedBreaker-windows-x86_64.zip.sha256)
echo "package.sh: $OUT/SpeedBreaker-windows-x86_64.zip ($(du -h "$OUT/SpeedBreaker-windows-x86_64.zip" | cut -f1)), $VERSION ($REVISION)"
