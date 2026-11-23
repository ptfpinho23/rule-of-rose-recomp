#!/usr/bin/env bash
# vurun.sh DUMP... — build recomp/tools/vurun.c (off-line VU1 runner using the
# real recomp/rt/vu.c) and run it on each ROR_VUDUMP file given.
set -euo pipefail
R="$(cd "$(dirname "$0")/../.." && pwd)"
CLANG=~/.local/opt/clang/bin/clang
E=$R/tools/debs/extract/usr
L=/usr/lib/x86_64-linux-gnu
D=$R/build/vurun; mkdir -p "$D"
INC="-nostdinc -isystem $HOME/.local/opt/clang/lib/clang/18/include -isystem $E/include -isystem $E/include/x86_64-linux-gnu"
CFLAGS="-O1 -g1 -fwrapv -fno-strict-aliasing -fno-math-errno -ffp-contract=off -Wall -Wno-unused-variable -Wno-unused-function -Wno-unused-label -I$R/recomp/rt -I$R/build/recomp/gen ${EXTRA:-}"
if [ ! -e "$D/vurun" ] || [ -n "$(find "$R/recomp/rt" "$R/recomp/tools/vurun.c" -newer "$D/vurun" \( -name 'vu*' -o -name cpu.h \) -print -quit)" ]; then
  $CLANG $INC $CFLAGS -c "$R/recomp/tools/vurun.c" -o "$D/vurun.o"
  $CLANG -o "$D/vurun" -nostdlib $E/lib/x86_64-linux-gnu/crt1.o $E/lib/x86_64-linux-gnu/crti.o \
    "$D/vurun.o" $L/libm.so.6 $L/libc.so.6 $E/lib/x86_64-linux-gnu/libc_nonshared.a \
    $E/lib/gcc/x86_64-linux-gnu/14/libgcc.a $E/lib/x86_64-linux-gnu/crtn.o \
    -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2
fi
for f in "$@"; do "$D/vurun" "$f"; done
