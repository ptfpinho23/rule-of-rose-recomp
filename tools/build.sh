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
# Overridable: CMAKE, NINJA (programs), QT_LINUX (Qt 6 gcc_64 kit),
# ROR_CLANG (the private clang), and ROR_TOOLCHAIN=system to build with the
# system's own C++ compiler and development packages instead of the private
# toolchain of tools/toolchain.cmake.
CMAKE="${CMAKE:-$HOME/.local/bin/cmake}"
NINJA="${NINJA:-$HOME/.local/bin/ninja}"
export QT_LINUX="${QT_LINUX:-$HOME/.local/opt/qt6/6.8.2/gcc_64}"
export ROR_CLANG="${ROR_CLANG:-$HOME/.local/opt/clang}"
SYSTEM_TC=0; [ "${ROR_TOOLCHAIN:-private}" = system ] && SYSTEM_TC=1
# the private clang needs libtinfo.so.5, which lives beside it.
[ $SYSTEM_TC = 1 ] || export LD_LIBRARY_PATH="$ROR_CLANG/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

BUILD="$LAUNCHER/build"; PKG=OFF
case "${1:-}" in
  clean) rm -rf "$BUILD"; echo "cleaned $BUILD" ;;
  dist)  BUILD="$LAUNCHER/build-dist"; PKG=ON; rm -rf "$BUILD" ;;
  "")    ;;
  *)     echo "usage: tools/build.sh [clean|dist]" >&2; exit 2 ;;
esac

if [ $SYSTEM_TC = 1 ]; then
  "$CMAKE" -S "$LAUNCHER" -B "$BUILD" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$NINJA" \
    -DCMAKE_PREFIX_PATH="$QT_LINUX" \
    -DCMAKE_BUILD_TYPE=Release \
    -DROR_PACKAGE=$PKG
else
  if [ ! -e "$REPO/tools/debs/qtdev/usr/include/linux/limits.h" ] ||
     [ ! -e "$REPO/tools/debs/xcbrt/usr/lib/x86_64-linux-gnu/libxcb-cursor.so.0" ]; then
    "$REPO/tools/fetch-debs.sh"
  fi
  "$CMAKE" -S "$LAUNCHER" -B "$BUILD" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$NINJA" \
    -DCMAKE_TOOLCHAIN_FILE="$REPO/tools/toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DROR_PACKAGE=$PKG
fi
"$NINJA" -C "$BUILD"
echo "Built: $BUILD/ror_launcher"
