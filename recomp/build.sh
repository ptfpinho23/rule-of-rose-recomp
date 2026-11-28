#!/usr/bin/env bash
# build.sh — regenerate the recompiled C and build the game binary.
#   recomp/build.sh            full build of the European game (SLES-54218) -> build/recomp/ror
#   GAME=ntsc recomp/build.sh  the USA game (SLUS-21448) -> build/recomp/ntsc/ror; its generated code and
#                              objects are under build/recomp/ntsc/ too, so the two builds do not touch each other
#   EXTRA="-DROR_TRACE" ...    extra compiler flags
# Toolchain (defaults are this project's private set-up: clang 18 under
# ~/.local/opt/clang and the headers/CRT fetched by tools/fetch-debs.sh):
#   CLANG=path                 the compiler
#   ROR_TOOLCHAIN=system       use $CC (default cc) with the system's own headers and libraries instead
#                              (needs the X11 and Vulkan headers installed, or VULKAN_INC=dir)
set -euo pipefail
cd "$(dirname "$0")/.."
CLANG=${CLANG:-$HOME/.local/opt/clang/bin/clang}
VULKAN_INC=${VULKAN_INC:-tools/vulkan/Vulkan-Headers-1.3.290/include}
E=tools/debs/extract/usr
L=/usr/lib/x86_64-linux-gnu
export GAME=${GAME:-pal}
eval "$(recomp/tools/gamefiles.sh paths | sed "s|$PWD/||g")"     # ELF, GEN, BASE of this edition (recomp/games.py)
OBJ=${OBJ:-$BASE/obj}
OUT=${OUT:-$BASE/ror}
if [ "${ROR_TOOLCHAIN:-private}" = system ]; then
  CLANG=${CC:-cc}; INC=""
else
  INC="-nostdinc -isystem $HOME/.local/opt/clang/lib/clang/18/include -isystem $E/include -isystem $E/include/x86_64-linux-gnu -isystem tools/debs/libx11/usr/include -isystem tools/debs/x11proto/usr/include -isystem tools/debs/qtdev/usr/include -isystem tools/debs/qtdev/usr/include/x86_64-linux-gnu"
  # (tools/debs/qtdev: linux-libc-dev, fetched by tools/fetch-debs.sh — the kernel headers, for <linux/input.h> in rt/pad_evdev.c)
  [ -e tools/debs/qtdev/usr/include/linux/input.h ] || { echo "kernel headers missing: run tools/fetch-debs.sh (tools/debs/qtdev)" >&2; exit 1; }
fi
CFLAGS="-O2 -g1 -fwrapv -fno-strict-aliasing -fno-math-errno -ffp-contract=off -Wall -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable -DGS_HW -Irecomp/rt -I$VULKAN_INC -I$GEN ${EXTRA:-}"
mkdir -p "$OBJ"
recomp/tools/gamefiles.sh elf        # the game's executable from the disc image, if it is not there
if [ "${NOGEN:-0}" != 1 ]; then
  python3 recomp/gen.py "$ELF" "$GEN.new" >&2
  # keep timestamps of unchanged generated files so they are not recompiled
  mkdir -p "$GEN"
  for f in "$GEN.new"/*; do
    b=$(basename "$f")
    if ! cmp -s "$f" "$GEN/$b"; then cp "$f" "$GEN/$b"; fi
  done
  for f in "$GEN"/fn_*.c; do [ -e "$GEN.new/$(basename "$f")" ] || rm -f "$f"; done
  rm -rf "$GEN.new"
fi
recomp/tools/gamefiles.sh vu1        # $GEN/vu1_fast_gen.inc: the game's VU1 program as C (included by rt/vu.c)
STAMP="$OBJ/.flags"; echo "$CFLAGS" | cmp -s - "$STAMP" || { rm -f "$OBJ"/*.o; echo "$CFLAGS" > "$STAMP"; }
compile() {
  src=$1; o="$OBJ/$(basename "${src%.c}").o"
  if [ ! -e "$o" ] || [ "$src" -nt "$o" ] || [ -n "$(find recomp/rt recomp/rt/hw "$GEN" -maxdepth 1 -name '*.h' -newer "$o" -print -quit)" ] || { [[ $src == recomp/rt/interp.c ]] && [ -n "$(find "$GEN" -name 'interp_*.inc' -newer "$o" -print -quit)" ]; } || { [[ $src == recomp/rt/vu.c ]] && [ "$GEN/vu1_fast_gen.inc" -nt "$o" ]; }; then
    $CLANG $INC $CFLAGS -c "$src" -o "$o" || exit 255
  fi
}
export -f compile; export CLANG INC CFLAGS OBJ GEN
ls "$GEN"/fn_*.c recomp/rt/*.c recomp/rt/hw/*.c | xargs -P "$(nproc)" -I{} bash -c 'compile {}'
for o in "$OBJ"/fn_*.o; do [ -e "$GEN/$(basename "${o%.o}").c" ] || rm -f "$o"; done
if [ "${ROR_TOOLCHAIN:-private}" = system ]; then
  $CLANG -o "$OUT" "$OBJ"/*.o -lm -ldl -lpthread
else
  $CLANG -o "$OUT" -nostdlib $E/lib/x86_64-linux-gnu/crt1.o $E/lib/x86_64-linux-gnu/crti.o \
    "$OBJ"/*.o $L/libm.so.6 $L/libc.so.6 $E/lib/x86_64-linux-gnu/libc_nonshared.a \
    $E/lib/gcc/x86_64-linux-gnu/14/libgcc.a $E/lib/x86_64-linux-gnu/crtn.o \
    -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2
fi
echo "built $OUT" >&2
