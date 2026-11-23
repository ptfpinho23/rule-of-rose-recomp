#!/usr/bin/env bash
# gscmp.sh ARGS... — build and run recomp/tools/gscmp.c (see there).
set -euo pipefail
R="$(cd "$(dirname "$0")/../.." && pwd)"
CLANG=~/.local/opt/clang/bin/clang; E=$R/tools/debs/extract/usr; L=/usr/lib/x86_64-linux-gnu
D=$R/build/gsreplay; mkdir -p "$D"
if [ ! -e "$D/gscmp" ] || [ "$R/recomp/tools/gscmp.c" -nt "$D/gscmp" ]; then
  $CLANG -nostdinc -isystem $HOME/.local/opt/clang/lib/clang/18/include -isystem $E/include -isystem $E/include/x86_64-linux-gnu -O2 -c "$R/recomp/tools/gscmp.c" -o "$D/gscmp.o"
  $CLANG -o "$D/gscmp" -nostdlib $E/lib/x86_64-linux-gnu/crt1.o $E/lib/x86_64-linux-gnu/crti.o "$D/gscmp.o" $L/libc.so.6 \
    $E/lib/x86_64-linux-gnu/libc_nonshared.a $E/lib/gcc/x86_64-linux-gnu/14/libgcc.a $E/lib/x86_64-linux-gnu/crtn.o -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2
fi
exec "$D/gscmp" "$@"
