#!/usr/bin/env bash
# posttool.sh — build recomp/tools/posttool.c with recomp/rt/post.c into
# build/posttool/posttool (same compiler and FP flags as the game).
set -euo pipefail
cd "$(dirname "$0")/../.."
CLANG=~/.local/opt/clang/bin/clang
E=tools/debs/extract/usr
L=/usr/lib/x86_64-linux-gnu
D=build/posttool; mkdir -p "$D"
INC="-nostdinc -isystem $HOME/.local/opt/clang/lib/clang/18/include -isystem $E/include -isystem $E/include/x86_64-linux-gnu"
CFLAGS="-O2 -g1 -fwrapv -fno-strict-aliasing -fno-math-errno -ffp-contract=off -Wall -Irecomp/rt"
$CLANG $INC $CFLAGS -c recomp/rt/post.c -o "$D/post.o"
$CLANG $INC $CFLAGS -c recomp/tools/posttool.c -o "$D/posttool.o"
$CLANG -o "$D/posttool" -nostdlib $E/lib/x86_64-linux-gnu/crt1.o $E/lib/x86_64-linux-gnu/crti.o \
  "$D/post.o" "$D/posttool.o" $L/libm.so.6 $L/libc.so.6 $E/lib/x86_64-linux-gnu/libc_nonshared.a \
  $E/lib/gcc/x86_64-linux-gnu/14/libgcc.a $E/lib/x86_64-linux-gnu/crtn.o \
  -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2
echo "built $D/posttool" >&2
