#!/usr/bin/env bash
# rxsnd_test.sh — build and run the stand-alone test of recomp/rt/iop_rxsnd.c
# against a real sample bank from the disc (/DATA/SND/XV/0000.SE).
set -euo pipefail
cd "$(dirname "$0")/../.."
CLANG=~/.local/opt/clang/bin/clang
E=tools/debs/extract/usr
L=/usr/lib/x86_64-linux-gnu
D=build/rxsnd_test; mkdir -p "$D"
INC="-nostdinc -isystem $HOME/.local/opt/clang/lib/clang/18/include -isystem $E/include -isystem $E/include/x86_64-linux-gnu"
CFLAGS="-O1 -g1 -fwrapv -fno-strict-aliasing -Wall -Wno-unused-variable -Irecomp/rt -Ibuild/recomp/gen"
[ -e "$D/0000.SE" ] || python3 -c "
import sys; sys.path.insert(0, '.')
from rorcore.iso9660 import IsoImage
open('$D/0000.SE', 'wb').write(IsoImage('Rule of Rose (Europe) (En,Fr,De,Es,It).iso').read('/DATA/SND/XV/0000.SE'))"
$CLANG $INC $CFLAGS -c recomp/rt/iop_rxsnd.c -o "$D/iop_rxsnd.o"
$CLANG $INC $CFLAGS -c recomp/tools/rxsnd_test.c -o "$D/rxsnd_test.o"
$CLANG -o "$D/rxsnd_test" -nostdlib $E/lib/x86_64-linux-gnu/crt1.o $E/lib/x86_64-linux-gnu/crti.o \
  "$D"/*.o $L/libm.so.6 $L/libc.so.6 $E/lib/x86_64-linux-gnu/libc_nonshared.a \
  $E/lib/gcc/x86_64-linux-gnu/14/libgcc.a $E/lib/x86_64-linux-gnu/crtn.o \
  -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2
"$D/rxsnd_test" "$D/0000.SE"
