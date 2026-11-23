#!/usr/bin/env bash
# gscheck.sh [DIR] — replay every capture DIR/cap_*.gscap (default
# build/recomp/gscap) through the current recomp/rt/gs.c and compare each
# frame's display image with the reference renderer's (recomp/tools/gs_ref.c,
# single-threaded; its output is cached as DIR/ref_*.raw). Prints timing.
#   ROR_GS_THREADS=n gscheck.sh    test with n render threads
#   GSARGS=-c gscheck.sh           extra gsreplay arguments
set -uo pipefail
R="$(cd "$(dirname "$0")/../.." && pwd)"
D=${1:-$R/build/recomp/gscap}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
"$R/recomp/tools/gsreplay.sh"; REF=1 "$R/recomp/tools/gsreplay.sh"
fail=0
for c in "$D"/cap_*.gscap; do
  n=$(basename "$c" .gscap); n=${n#cap_}
  [ -e "$D/ref_$n.raw" ] || REF=1 ROR_GS_THREADS=0 "$R/recomp/tools/gsreplay.sh" "$c" -o "$D/ref_$n.raw" >/dev/null
  "$R/recomp/tools/gsreplay.sh" "$c" -o "$T/out.raw" ${GSARGS:-} | tr '\n' ' '
  if cmp -s "$T/out.raw" "$D/ref_$n.raw"; then echo "IDENTICAL"; else echo "DIFFERENT"; fail=1; fi
done
exit $fail
