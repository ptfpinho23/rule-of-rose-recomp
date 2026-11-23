#!/usr/bin/env bash
# build.sh — configure + build the Rule of Rose launcher (Linux).
#   tools/build.sh           dev build   -> launcher/build/ror_launcher
#                            (RUNPATH points at the Qt/libc++/xcb trees on this host)
#   tools/build.sh clean     same, from scratch
#   tools/build.sh dist      package build -> launcher/build-dist/ror_launcher
#                            (RUNPATH $ORIGIN/lib; used by recomp/package.sh)
# Needs tools/debs/{qtdev,xcbrt}; they are fetched on first use.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LAUNCHER="$REPO/launcher"
CMAKE="$HOME/.local/bin/cmake"
NINJA="$HOME/.local/bin/ninja"
# clang itself needs libtinfo.so.5, which lives beside it.
export LD_LIBRARY_PATH="$HOME/.local/opt/clang/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

BUILD="$LAUNCHER/build"; PKG=OFF
case "${1:-}" in
  clean) rm -rf "$BUILD"; echo "cleaned $BUILD" ;;
  dist)  BUILD="$LAUNCHER/build-dist"; PKG=ON; rm -rf "$BUILD" ;;
  "")    ;;
  *)     echo "usage: tools/build.sh [clean|dist]" >&2; exit 2 ;;
esac

if [ ! -e "$REPO/tools/debs/qtdev/usr/include/linux/limits.h" ] ||
   [ ! -e "$REPO/tools/debs/xcbrt/usr/lib/x86_64-linux-gnu/libxcb-cursor.so.0" ]; then
  "$REPO/tools/fetch-debs.sh"
fi

"$CMAKE" -S "$LAUNCHER" -B "$BUILD" -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$NINJA" \
  -DCMAKE_TOOLCHAIN_FILE="$REPO/tools/toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DROR_PACKAGE=$PKG
"$NINJA" -C "$BUILD"
echo "Built: $BUILD/ror_launcher"
