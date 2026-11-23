#!/usr/bin/env bash
# gsreplay.sh ARGS... — build recomp/tools/gsreplay.c (stand-alone GS replay
# using the real recomp/rt/gs.c) and run it with ARGS (see gsreplay.c).
#   REF=1 gsreplay.sh ...   use the reference renderer recomp/tools/gs_ref.c
#   HW=1 gsreplay.sh ...    build with the Vulkan renderer (enable with -hw N)
#   EXTRA="-DX" ...         extra compiler flags (separate binary per flag set)
set -euo pipefail
R="$(cd "$(dirname "$0")/../.." && pwd)"
CLANG=~/.local/opt/clang/bin/clang
E=$R/tools/debs/extract/usr
L=/usr/lib/x86_64-linux-gnu
D=$R/build/gsreplay; mkdir -p "$D"
INC="-nostdinc -isystem $HOME/.local/opt/clang/lib/clang/18/include -isystem $E/include -isystem $E/include/x86_64-linux-gnu"
CFLAGS="-O2 -g1 -fwrapv -fno-strict-aliasing -fno-math-errno -ffp-contract=off -Wall -Wno-unused-variable -Wno-unused-function -I$R/recomp/rt -I$R/build/recomp/gen ${EXTRA:-}"
B=gsreplay; SRC="$R/recomp/rt/gs.c $R/recomp/rt/gs.h"
if [ "${HW:-0}" = 1 ]; then B=gsreplay-hw; CFLAGS="$CFLAGS -DGS_HW -I$R/tools/vulkan/Vulkan-Headers-1.3.290/include"; SRC="$SRC $R/recomp/rt/hw/gs_hw.c $R/recomp/rt/hw/gs_hw.h $R/recomp/rt/hw/gs_hw_spv.h"; fi
if [ "${REF:-0}" = 1 ]; then B=gsreplay-ref; CFLAGS="$CFLAGS -DGS_REF"; SRC="$R/recomp/tools/gs_ref.c"; fi
if [ -n "${EXTRA:-}" ]; then B="$B-$(echo "$EXTRA" | md5sum | cut -c1-6)"; fi
if [ ! -e "$D/$B" ] || [ -n "$(find $SRC "$R/recomp/tools/gsreplay.c" "$R/recomp/tools/gsreplay.sh" -newer "$D/$B" -print -quit)" ]; then
  $CLANG $INC $CFLAGS -c "$R/recomp/tools/gsreplay.c" -o "$D/$B.o"
  HWO=""
  if [ "${HW:-0}" = 1 ]; then $CLANG $INC $CFLAGS -c "$R/recomp/rt/hw/gs_hw.c" -o "$D/$B-hw.o"; HWO="$D/$B-hw.o"; fi
  $CLANG -o "$D/$B" -nostdlib $E/lib/x86_64-linux-gnu/crt1.o $E/lib/x86_64-linux-gnu/crti.o \
    "$D/$B.o" $HWO $L/libm.so.6 $L/libc.so.6 $E/lib/x86_64-linux-gnu/libc_nonshared.a \
    $E/lib/gcc/x86_64-linux-gnu/14/libgcc.a $E/lib/x86_64-linux-gnu/crtn.o \
    -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2
fi
[ $# -eq 0 ] || exec "$D/$B" "$@"
