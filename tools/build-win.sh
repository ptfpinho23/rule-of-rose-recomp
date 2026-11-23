#!/usr/bin/env bash
# build-win.sh — cross-build the launcher for Windows x64.
#   tools/build-win.sh [clean]   -> launcher/build-win/ror_launcher.exe
# See tools/toolchain-win.cmake for what must be installed. Overridable:
# CMAKE, NINJA, LLVM_MINGW (toolchain), QT_WIN (Qt llvm-mingw_64 kit), QT_LINUX
# (host Qt, for moc/rcc).
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$REPO/launcher/build-win"
CMAKE="${CMAKE:-$HOME/.local/bin/cmake}"
NINJA="${NINJA:-$HOME/.local/bin/ninja}"
export LLVM_MINGW="${LLVM_MINGW:-$REPO/tools/llvm-mingw}"
export QT_WIN="${QT_WIN:-$HOME/.local/opt/qt6/6.8.2/llvm-mingw_64}"
export QT_LINUX="${QT_LINUX:-$HOME/.local/opt/qt6/6.8.2/gcc_64}"
QTWIN="$QT_WIN"
[ -x "$LLVM_MINGW/bin/x86_64-w64-mingw32-clang++" ] ||
  { echo "build-win: llvm-mingw is missing ($LLVM_MINGW)" >&2; exit 1; }
[ -f "$QTWIN/bin/Qt6Core.dll" ] ||
  { echo "build-win: $QTWIN is missing; install it with:
  python3 -m aqt install-qt windows desktop 6.8.2 win64_llvm_mingw -O ~/.local/opt/qt6 --archives qtbase" >&2; exit 1; }
# llvm-mingw brings its own LLVM libraries; a host LD_LIBRARY_PATH pointing at
# another clang makes its compiler crash.
unset LD_LIBRARY_PATH
case "${1:-}" in
  clean) rm -rf "$BUILD" ;;
  "")    ;;
  *)     echo "usage: tools/build-win.sh [clean]" >&2; exit 2 ;;
esac
"$CMAKE" -S "$REPO/launcher" -B "$BUILD" -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$NINJA" \
  -DCMAKE_TOOLCHAIN_FILE="$REPO/tools/toolchain-win.cmake" \
  -DCMAKE_BUILD_TYPE=Release
"$NINJA" -C "$BUILD"

# Put the DLLs the launcher imports next to it, so the build folder runs as is
# (Windows looks for them beside the .exe). Same walk as recomp/package.sh.
MINGW="$LLVM_MINGW"
mkdir -p "$BUILD/platforms"
cp -u "$QTWIN/plugins/platforms/qwindows.dll" "$BUILD/platforms/"
todo=("$BUILD/ror_launcher.exe" "$BUILD/platforms/qwindows.dll")
while [ ${#todo[@]} -gt 0 ]; do
  f=${todo[0]}; todo=("${todo[@]:1}")
  while read -r dll; do
    [ -e "$BUILD/$dll" ] && continue
    for dir in "$QTWIN/bin" "$MINGW/x86_64-w64-mingw32/bin"; do
      if [ -f "$dir/$dll" ]; then
        cp "$dir/$dll" "$BUILD/$dll"
        todo+=("$BUILD/$dll")
        break
      fi
    done
  done < <("$MINGW/bin/x86_64-w64-mingw32-objdump" -p "$f" | sed -n 's/^[[:space:]]*DLL Name: //p' | tr -d '\r')
done
echo "Built: $BUILD/ror_launcher.exe (runs from that folder; the game is started from ror-game.exe beside it, if present)"
