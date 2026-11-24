#!/usr/bin/env bash
# padsim.sh — build and run the gamepad device simulation (recomp/tools/padsim.c):
# recomp/rt/pad_evdev.c and pad.c, unchanged, against simulated /dev/input and
# /sys/class/input devices (keyboard-like virtual devices, Xbox pads on the
# xpad / xone / generic HID layouts, a DualSense with its extra nodes, a node
# without access rights, hot-plug, Steam's virtual pad).
#   recomp/tools/padsim.sh                 test recomp/rt/pad_evdev.c
#   recomp/tools/padsim.sh FILE.c          test another version of pad_evdev.c (e.g. one taken from git)
#   recomp/tools/padsim.sh FILE.c N        only scenario N
# Exit status 0 if every scenario passed. Toolchain as in recomp/build.sh.
set -euo pipefail
cd "$(dirname "$0")/../.."
SRC=${1:-recomp/rt/pad_evdev.c}
CLANG=${CLANG:-$HOME/.local/opt/clang/bin/clang}
E=tools/debs/extract/usr
L=/usr/lib/x86_64-linux-gnu
D=build/padsim; mkdir -p "$D"
CFLAGS="-O1 -g1 -fwrapv -fno-strict-aliasing -Wall -Wno-unused-variable -Wno-unused-function -Irecomp/rt -Ibuild/recomp/gen"
WRAP="-Wl,--wrap=opendir,--wrap=readdir,--wrap=closedir,--wrap=stat,--wrap=fopen,--wrap=open,--wrap=close,--wrap=read,--wrap=write,--wrap=ioctl"
if [ "${ROR_TOOLCHAIN:-private}" = system ]; then
  ${CC:-cc} $CFLAGS -DPADSIM_SYSTEM_CRT -o "$D/padsim" "$SRC" recomp/rt/pad.c recomp/tools/padsim.c $WRAP -lm
else
  Q=tools/debs/qtdev/usr/include
  INC="-nostdinc -isystem $HOME/.local/opt/clang/lib/clang/18/include -isystem $E/include -isystem $E/include/x86_64-linux-gnu -isystem $Q -isystem $Q/x86_64-linux-gnu"
  $CLANG $INC $CFLAGS -c "$SRC" -o "$D/pad_evdev.o"
  $CLANG $INC $CFLAGS -c recomp/rt/pad.c -o "$D/pad.o"
  $CLANG $INC $CFLAGS -c recomp/tools/padsim.c -o "$D/padsim.o"
  $CLANG -o "$D/padsim" -nostdlib $E/lib/x86_64-linux-gnu/crt1.o $E/lib/x86_64-linux-gnu/crti.o \
    "$D/pad_evdev.o" "$D/pad.o" "$D/padsim.o" $WRAP $L/libm.so.6 $L/libc.so.6 $E/lib/x86_64-linux-gnu/libc_nonshared.a \
    $E/lib/gcc/x86_64-linux-gnu/14/libgcc.a $E/lib/x86_64-linux-gnu/crtn.o \
    -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2
fi
bad=0
if [ -n "${2:-}" ]; then "$D/padsim" "$2" || bad=1
else for i in $(seq 1 "$("$D/padsim")"); do "$D/padsim" "$i" || bad=$((bad + 1)); done; fi
echo "padsim ($SRC): $([ $bad = 0 ] && echo "all scenarios passed" || echo "$bad scenario(s) FAILED")"
[ $bad = 0 ]
