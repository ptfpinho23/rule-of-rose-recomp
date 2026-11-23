#!/usr/bin/env bash
# gsmatrix.sh CAP... — wider identity test: replay each capture with the frame
# and depth formats rewritten (gsreplay -fpsm/-zpsm) and with the renderer
# forced through its generic and serial paths, and compare the images of
# recomp/rt/gs.c (ROR_GS_THREADS, default 11) with the reference renderer's.
set -uo pipefail
R="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
"$R/recomp/tools/gsreplay.sh"; REF=1 "$R/recomp/tools/gsreplay.sh"
export ROR_GS_THREADS=${ROR_GS_THREADS:-11}
fail=0
for c in "$@"; do
  for v in "" "-zpsm 0" "-zpsm 1" "-zpsm 2" "-fpsm 1" "-fpsm 2" "-fpsm 10" "-fpsm 1 -zpsm 0" "-fpsm 0x13"; do
    ROR_GS_THREADS=0 REF=1 "$R/recomp/tools/gsreplay.sh" "$c" -o "$T/ref.raw" -f ${FRAMES:-60} $v >/dev/null
    for force in 0 1 2; do
      [ -n "$v" ] || [ $force != 0 ] || continue
      ROR_GS_FORCE=$force "$R/recomp/tools/gsreplay.sh" "$c" -o "$T/out.raw" -f ${FRAMES:-60} $v >/dev/null
      if cmp -s "$T/out.raw" "$T/ref.raw"; then r=IDENTICAL; else r=DIFFERENT; fail=1; fi
      echo "$(basename "$c") [${v:-as captured}] force=$force: $r"
    done
  done
done
exit $fail
