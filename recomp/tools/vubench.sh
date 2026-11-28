#!/usr/bin/env bash
# vubench.sh ARGS... — build recomp/tools/vubench.c against the runtime sources
# in $VUSRC (default recomp/rt) into build/vubench/$NAME (default "vubench")
# and run it with ARGS (see vubench.c). Flags match recomp/build.sh.
# GENDIR=dir: the generated headers of another edition (build/recomp/ntsc/gen).
set -euo pipefail
R="$(cd "$(dirname "$0")/../.." && pwd)"
CLANG=~/.local/opt/clang/bin/clang
E=$R/tools/debs/extract/usr
L=/usr/lib/x86_64-linux-gnu
SRC=${VUSRC:-$R/recomp/rt}; NAME=${NAME:-vubench}
D=$R/build/vubench; mkdir -p "$D"
INC="-nostdinc -isystem $HOME/.local/opt/clang/lib/clang/18/include -isystem $E/include -isystem $E/include/x86_64-linux-gnu"
CFLAGS="-O2 -g1 -fwrapv -fno-strict-aliasing -fno-math-errno -ffp-contract=off -Wall -Wno-unused-variable -Wno-unused-function -Wno-unused-label -I$SRC -I${GENDIR:-$R/build/recomp/gen} ${EXTRA:-}"
$CLANG $INC $CFLAGS -c "$R/recomp/tools/vubench.c" -o "$D/$NAME.o"
$CLANG -o "$D/$NAME" -nostdlib $E/lib/x86_64-linux-gnu/crt1.o $E/lib/x86_64-linux-gnu/crti.o \
  "$D/$NAME.o" $L/libm.so.6 $L/libc.so.6 $E/lib/x86_64-linux-gnu/libc_nonshared.a \
  $E/lib/gcc/x86_64-linux-gnu/14/libgcc.a $E/lib/x86_64-linux-gnu/crtn.o \
  -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2
[ $# -eq 0 ] || "$D/$NAME" "$@"
