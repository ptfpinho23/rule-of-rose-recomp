#!/usr/bin/env bash
# gamefiles.sh — the build inputs that come from the user's own disc; nothing
# of them is in the repository. Called by recomp/build.sh and build-win.sh.
#   gamefiles.sh elf   build/native/SLES_542.18: taken from the disc image if it is not there yet.
#                      Image: $ROR_ISO, or "Rule of Rose (Europe) (En,Fr,De,Es,It).iso" / RuleOfRose.iso /
#                      game.iso in the repository root.
#   gamefiles.sh vu1   build/recomp/gen/vu1_fast_gen.inc: the game's VU1 micro program as straight-line C
#                      (rt/vu.c "generated code"). The program is read out of the executable
#                      (tools/vu1extract.py) and translated by tools/vubench.c `gen`. Needs the generated
#                      headers of gen.py in build/recomp/gen, so it runs after it.
set -euo pipefail
R="$(cd "$(dirname "$0")/../.." && pwd)"
ELF=$R/build/native/SLES_542.18
GEN=$R/build/recomp/gen
case "${1:-}" in
elf)
  [ -s "$ELF" ] && exit 0
  iso=${ROR_ISO:-}
  if [ -z "$iso" ]; then for n in "Rule of Rose (Europe) (En,Fr,De,Es,It).iso" RuleOfRose.iso game.iso; do [ -f "$R/$n" ] && { iso="$R/$n"; break; }; done; fi
  [ -n "$iso" ] && [ -f "$iso" ] || { echo "gamefiles: no disc image: put \"Rule of Rose (Europe) (En,Fr,De,Es,It).iso\" in $R or set ROR_ISO=path" >&2; exit 1; }
  mkdir -p "$(dirname "$ELF")"
  python3 "$R/recomp/tools/isoextract.py" "$iso" SLES_542.18 "$ELF.tmp" && mv "$ELF.tmp" "$ELF"
  echo "gamefiles: took SLES_542.18 from $iso" >&2 ;;
vu1)
  OUT=$GEN/vu1_fast_gen.inc; W=$R/build/recomp/vu1; mkdir -p "$W" "$GEN"
  if [ -s "$OUT" ] && [ -e "$W/.stamp" ] && [ -z "$(find "$ELF" "$R/recomp/tools/vu1extract.py" "$R/recomp/tools/vubench.c" "$R/recomp/tools/vubench.sh" "$R/recomp/tools/gamefiles.sh" \
        "$R/recomp/elf.py" "$R/recomp/rt/vu.c" "$R/recomp/rt/vu_ops.h" "$R/recomp/rt/vif.c" -newer "$W/.stamp" -print -quit)" ]; then exit 0; fi
  python3 "$R/recomp/tools/vu1extract.py" "$ELF" "$W/vu1_micro.bin"
  # the generator itself is built without the fast path (it may not exist yet, or be out of date)
  NAME=vugen EXTRA=-DVU_NO_FAST "$R/recomp/tools/vubench.sh" gen "$W/vu1_micro.bin" > "$W/vu1_fast_gen.inc.new"
  grep -q '^static int vu1_fast(' "$W/vu1_fast_gen.inc.new" || { echo "gamefiles: the VU1 generator produced no code" >&2; exit 1; }
  if ! cmp -s "$W/vu1_fast_gen.inc.new" "$OUT"; then cp "$W/vu1_fast_gen.inc.new" "$OUT"; echo "gamefiles: wrote $OUT" >&2; fi   # unchanged: keep its time, nothing is recompiled
  touch "$W/.stamp" ;;
*) echo "usage: gamefiles.sh elf|vu1" >&2; exit 2 ;;
esac
