#!/usr/bin/env bash
# build-win.sh — cross-build the game for Windows x64 (build/recomp/win/ror.exe)
# with the llvm-mingw toolchain in tools/llvm-mingw (target x86_64-w64-mingw32,
# UCRT). Same sources, optimisation and floating-point flags as build.sh.
#   recomp/build-win.sh         build (uses the generated C in build/recomp/gen;
#                               runs gen.py only if that directory is missing)
#   GEN=1 recomp/build-win.sh   regenerate the C first, like build.sh does
#   OBJ=dir OUT=file EXTRA="-D..."   as in build.sh
# A PDB with line tables is written next to the .exe (crash reports print
# image+0xOFFSET; resolve with: llvm-symbolizer --obj=ror.exe 0xOFFSET... or
# tools/llvm-mingw/bin/llvm-pdbutil).
set -euo pipefail
cd "$(dirname "$0")/.."
TC=tools/llvm-mingw/bin
CLANG=$TC/x86_64-w64-mingw32-clang
[ -x "$CLANG" ] || { echo "llvm-mingw not found in tools/llvm-mingw (see recomp/NOTES.md, Windows build)" >&2; exit 1; }
GEN=build/recomp/gen
OBJ=${OBJ:-build/recomp/win/obj}
OUT=${OUT:-build/recomp/win/ror.exe}
CFLAGS="-O2 -g1 -gcodeview -fwrapv -fno-strict-aliasing -fno-math-errno -ffp-contract=off -Wall -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable -D_WIN32_WINNT=0x0A00 -DUNICODE -D_UNICODE -include recomp/rt/win_compat.h -DGS_HW -Irecomp/rt -Itools/vulkan/Vulkan-Headers-1.3.290/include -I$GEN ${EXTRA:-}"
mkdir -p "$OBJ" "$(dirname "$OUT")"
recomp/tools/gamefiles.sh elf        # build/native/SLES_542.18 from the disc image, if it is not there
if [ "${GEN:-0}" = 1 ] || [ ! -e "$GEN/funcs.h" ]; then
  python3 recomp/gen.py build/native/SLES_542.18 "$GEN.new" >&2
  mkdir -p "$GEN"
  for f in "$GEN.new"/*; do
    b=$(basename "$f")
    if ! cmp -s "$f" "$GEN/$b"; then cp "$f" "$GEN/$b"; fi
  done
  for f in "$GEN"/fn_*.c; do [ -e "$GEN.new/$(basename "$f")" ] || rm -f "$f"; done
  rm -rf "$GEN.new"
fi
recomp/tools/gamefiles.sh vu1        # $GEN/vu1_fast_gen.inc: the game's VU1 program as C (built with the host compiler)
STAMP="$OBJ/.flags"; echo "$CFLAGS" | cmp -s - "$STAMP" || { rm -f "$OBJ"/*.o; echo "$CFLAGS" > "$STAMP"; }
compile() {
  src=$1; o="$OBJ/$(basename "${src%.c}").o"
  if [ ! -e "$o" ] || [ "$src" -nt "$o" ] || [ -n "$(find recomp/rt recomp/rt/hw "$GEN" -maxdepth 1 \( -name '*.h' -o -name '*.inc' \) -newer "$o" -print -quit)" ]; then
    $CLANG $CFLAGS -c "$src" -o "$o" || exit 255
  fi
}
export -f compile; export CLANG CFLAGS OBJ GEN
ls "$GEN"/fn_*.c recomp/rt/*.c recomp/rt/hw/*.c | xargs -P "$(nproc)" -I{} bash -c 'compile {}'
for o in "$OBJ"/*.o; do b=$(basename "${o%.o}"); [ -e "$GEN/$b.c" ] || [ -e "recomp/rt/$b.c" ] || [ -e "recomp/rt/hw/$b.c" ] || rm -f "$o"; done
# -static: winpthreads and compiler-rt go into the .exe; what remains are
# DLLs that ship with Windows 10/11 (kernel32, user32, gdi32, shell32, winmm,
# and the UCRT api-ms-win-crt-* set). 8 MiB main-thread stack as on Linux.
$CLANG -static -o "$OUT" "$OBJ"/*.o -lpthread -lwinmm -lgdi32 -luser32 -lshell32 \
  -Wl,--stack,8388608 -Wl,--pdb="${OUT%.exe}.pdb"
echo "built $OUT" >&2
