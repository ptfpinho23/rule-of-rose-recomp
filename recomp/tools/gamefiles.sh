#!/usr/bin/env bash
# gamefiles.sh — the build inputs that come from the user's own disc; nothing
# of them is in the repository. Called by recomp/build.sh and build-win.sh.
# GAME=pal (default, the European disc) or GAME=ntsc (the USA disc) selects the
# edition; recomp/games.py is the table of them.
#   gamefiles.sh elf   the game's executable (build/native/SLES_542.18, or build/ntsc/SLUS_214.48):
#                      taken from the disc image if it is not there yet.
#                      Image: $ROR_ISO, or in the repository root "Rule of Rose (Europe) (En,Fr,De,Es,It).iso" /
#                      RuleOfRose.iso / game.iso (European), "Rule of Rose (USA).iso" or build/ntsc/usa.iso (USA).
#   gamefiles.sh vu1   <gen>/vu1_fast_gen.inc: the game's VU1 micro program as straight-line C
#                      (rt/vu.c "generated code"). The program is read out of the executable
#                      (tools/vu1extract.py) and translated by tools/vubench.c `gen`. Needs the generated
#                      headers of gen.py in the gen directory, so it runs after it.
#   gamefiles.sh paths prints "ELF=... GEN=... BASE=..." for the build scripts (BASE: where this edition's
#                      generated code, objects and programs go: build/recomp, or build/recomp/ntsc)
set -euo pipefail
R="$(cd "$(dirname "$0")/../.." && pwd)"
GAME=${GAME:-pal}
NAME=$(python3 "$R/recomp/games.py" "$GAME" elf)       # stops here for an unknown GAME
case "$GAME" in
pal)  ELF=$R/build/native/$NAME; BASE=$R/build/recomp
      ISOS=("$R/Rule of Rose (Europe) (En,Fr,De,Es,It).iso" "$R/RuleOfRose.iso" "$R/game.iso") ;;
*)    ELF=$R/build/$GAME/$NAME; BASE=$R/build/recomp/$GAME
      ISOS=("$R/Rule of Rose (USA).iso" "$R/build/$GAME/usa.iso") ;;
esac
GEN=$BASE/gen
case "${1:-}" in
paths)
  echo "ELF=$ELF GEN=$GEN BASE=$BASE" ;;
elf)
  [ -s "$ELF" ] && exit 0
  iso=${ROR_ISO:-}
  if [ -z "$iso" ]; then for n in "${ISOS[@]}"; do [ -f "$n" ] && { iso="$n"; break; }; done; fi
  [ -n "$iso" ] && [ -f "$iso" ] || { echo "gamefiles: no disc image: put \"$(basename "${ISOS[0]}")\" in $R or set ROR_ISO=path" >&2; exit 1; }
  mkdir -p "$(dirname "$ELF")"
  python3 "$R/recomp/tools/isoextract.py" "$iso" "$NAME" "$ELF.tmp" && mv "$ELF.tmp" "$ELF"
  echo "gamefiles: took $NAME from $iso" >&2 ;;
vu1)
  OUT=$GEN/vu1_fast_gen.inc; W=$BASE/vu1; mkdir -p "$W" "$GEN"
  if [ -s "$OUT" ] && [ -e "$W/.stamp" ] && [ -z "$(find "$ELF" "$R/recomp/tools/vu1extract.py" "$R/recomp/tools/vubench.c" "$R/recomp/tools/vubench.sh" "$R/recomp/tools/gamefiles.sh" \
        "$R/recomp/elf.py" "$R/recomp/games.py" "$R/recomp/rt/vu.c" "$R/recomp/rt/vu_ops.h" "$R/recomp/rt/vif.c" -newer "$W/.stamp" -print -quit)" ]; then exit 0; fi
  python3 "$R/recomp/tools/vu1extract.py" "$ELF" "$W/vu1_micro.bin"
  # the generator itself is built without the fast path (it may not exist yet, or be out of date)
  NAME=vugen EXTRA=-DVU_NO_FAST GENDIR="$GEN" "$R/recomp/tools/vubench.sh" gen "$W/vu1_micro.bin" > "$W/vu1_fast_gen.inc.new"
  grep -q '^static int vu1_fast(' "$W/vu1_fast_gen.inc.new" || { echo "gamefiles: the VU1 generator produced no code" >&2; exit 1; }
  if ! cmp -s "$W/vu1_fast_gen.inc.new" "$OUT"; then cp "$W/vu1_fast_gen.inc.new" "$OUT"; echo "gamefiles: wrote $OUT" >&2; fi   # unchanged: keep its time, nothing is recompiled
  touch "$W/.stamp" ;;
*) echo "usage: [GAME=pal|ntsc] gamefiles.sh elf|vu1|paths" >&2; exit 2 ;;
esac
