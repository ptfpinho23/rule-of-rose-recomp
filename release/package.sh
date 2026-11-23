#!/usr/bin/env bash
# package.sh linux|windows|all [VERSION]
#
# Builds the release archives:
#   build/release/RuleOfRose-<version>-linux-x64.tar.gz
#   build/release/RuleOfRose-<version>-windows-x64.zip
# Each unpacks to one folder, RuleOfRose-<version>/, holding the launcher with
# its Qt files (laid out by recomp/package.sh with GAME=none), ror-build and
# its build kit (release/make-kit.sh), licences and a README.txt.
#
# A release contains no game code: ror-game is built on the player's machine,
# from the player's own disc image, by ror-build. recomp/package.sh without
# GAME=none still makes the local dist/ folders with a locally built ror-game.
#
# VERSION defaults to the current tag (git describe) or "dev". The tools must
# be in place: release/fetch.sh, plus Qt and the toolchain the launcher is
# built with (see tools/build.sh, tools/build-win.sh and their variables).
set -euo pipefail
R="$(cd "$(dirname "$0")/.." && pwd)"
cd "$R"
WHAT=${1:-all}
VERSION=${2:-$(git describe --tags --always 2>/dev/null || echo dev)}
VERSION=${VERSION#v}
OUT=$R/build/release
LLVM_MINGW=${LLVM_MINGW:-$R/tools/llvm-mingw}

readme() {   # platform
  local exe="" sep=/ build=ror-build
  [ "$1" = windows ] && { exe=.exe; sep='\'; build=ror-build.exe; }
  cat <<EOF
Rule of Rose - native PC build, version $VERSION
==================================================

You need your own copy of the European disc (SLES-54218) as an .iso file.
This package contains no part of the game.

Start:  RuleOfRose$exe
        Choose your disc image and press PLAY. The first time, the game
        program is built on this computer from your disc image; that takes
        about a minute on a recent processor and needs no other software
        and no internet connection. It is built again only when you change
        the disc image or install another version of this package.

What is in this folder
  RuleOfRose$exe      the launcher
  $build         builds the game program from your disc image:
                    $build --iso PATH --out FOLDER
                  writes FOLDER${sep}ror-game$exe; FOLDER${sep}build.log has the details
  buildkit$sep         what $build uses: the recompiler, the runtime (compiled),
                  the Zig C compiler and a Python interpreter
  licenses$sep         licences of everything bundled here

Running the game without the launcher:
  FOLDER${sep}ror-game$exe --iso PATH [options]
The options are listed in docs/OPTIONS.md of the project.

The game's source code is on the project page; it is under the MIT licence
(licenses${sep}LICENSE.txt). Qt is used under the LGPL v3 (https://www.qt.io/licensing);
its libraries are separate files in this folder and can be replaced.
EOF
}

package() {   # platform
  local plat=$1
  local stage=$OUT/stage-$plat top=RuleOfRose-$VERSION
  rm -rf "$stage"; mkdir -p "$stage"
  DIST=$stage GAME=none recomp/package.sh "$plat"
  local D=$stage/RuleOfRose-$plat
  [ -d "$D" ] || { echo "package: recomp/package.sh made no $D" >&2; exit 1; }
  release/make-kit.sh "$plat" "$D" "$VERSION"

  mkdir -p "$D/licenses"
  cp LICENSE "$D/licenses/LICENSE.txt"
  cp "$D/buildkit/zig/LICENSE" "$D/licenses/Zig-LICENSE.txt"
  if [ "$plat" = windows ]; then
    cp "$D/buildkit/python/LICENSE.txt" "$D/licenses/Python-LICENSE.txt"
    # winpthreads (statically linked into the runtime objects and shipped as rt/lib/libpthread.a) is part of mingw-w64
    cp tools/dl/winpthreads-license/COPYING "$D/licenses/winpthreads-COPYING.txt"
    cp tools/dl/mingw-license/COPYING.MinGW-w64-runtime.txt "$D/licenses/MinGW-w64-runtime-COPYING.txt"
    cp "$LLVM_MINGW/LICENSE.TXT" "$D/licenses/LLVM-LICENSE.txt"          # libc++.dll, libunwind.dll (the launcher's C++ runtime)
    readme windows | sed 's/$/\r/' > "$D/README.txt"
  else
    cp "$D/buildkit/python/lib/python3.12/LICENSE.txt" "$D/licenses/Python-LICENSE.txt"
    readme linux > "$D/README.txt"
  fi
  rm -rf "$D/saves"

  rm -rf "$OUT/$top"; mv "$D" "$OUT/$top"; rmdir "$stage" 2>/dev/null || true
  if [ "$plat" = linux ]; then
    local a=$OUT/RuleOfRose-$VERSION-linux-x64.tar.gz
    tar -C "$OUT" --owner=0 --group=0 -czf "$a" "$top"
  else
    local a=$OUT/RuleOfRose-$VERSION-windows-x64.zip
    rm -f "$a"
    (cd "$OUT" && python3 - "$top" "$a" <<'PY'
import os, sys, zipfile
top, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for d, _, files in sorted(os.walk(top)):
        for f in sorted(files):
            z.write(os.path.join(d, f))
PY
    )
  fi
  rm -rf "$OUT/$top"
  echo "package: $a ($(du -h "$a" | cut -f1))" >&2
}

case "$WHAT" in
  linux|windows) package "$WHAT" ;;
  all) package linux; package windows ;;
  *) echo "usage: release/package.sh linux|windows|all [VERSION]" >&2; exit 2 ;;
esac
