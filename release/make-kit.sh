#!/usr/bin/env bash
# make-kit.sh linux|windows OUTDIR VERSION
#
# Builds the "build kit" of a release: what ror-build needs on the player's
# machine to make ror-game from the player's own disc image. Nothing in the
# kit is derived from the game.
#
#   OUTDIR/ror-build(.exe)        the tool (a small starter)
#   OUTDIR/buildkit/
#     ror_build.py  VERSION  KITHASH
#     recomp/       the recompiler (gen.py and its modules), games.py (the table of supported
#                   discs) and vu1extract.py
#     rt/lib/       the runtime, compiled (every recomp/rt source that does not
#                   include generated headers); on Windows also libpthread.a
#     rt/src/       support.c, vu.c, interp.c (they include generated headers
#                   and are compiled on the player's machine) and all headers
#     vugen(.exe)   VU1 micro program -> C translator (recomp/tools/vubench.c)
#     zig/          the C compiler (Zig, pinned)
#     python/       a standalone Python for ror_build.py and the recompiler
#
# Tools come from the directories release/fetch.sh fills; each can be
# overridden:
#   ZIG_HOST      zig that runs here (default tools/zig/zig-linux-x86_64-$ZIG_VERSION)
#   ZIG_LINUX, ZIG_WINDOWS         the zig trees that are shipped
#   PYTHON_LINUX, PYTHON_WINDOWS   the standalone Pythons that are shipped (their python/ folder)
#   LLVM_MINGW    llvm-mingw (Windows runtime objects; default tools/llvm-mingw)
#   VULKAN_INC    Vulkan-Headers include dir
#   X11_INC       directories with X11/Xlib.h etc., colon-separated (Linux runtime objects)
#   JOBS          parallel compilers (default: all processors)
#   RUNTIME_ONLY=1  stop after the runtime, the translator and the tool are built (a compile check)
set -euo pipefail
R="$(cd "$(dirname "$0")/.." && pwd)"
PLAT=${1:?usage: make-kit.sh linux|windows OUTDIR VERSION}; OUT=${2:?}; VERSION=${3:?}
ZIG_VERSION=0.13.0
ZIG_HOST=${ZIG_HOST:-$R/tools/zig/zig-linux-x86_64-$ZIG_VERSION}
ZIG_LINUX=${ZIG_LINUX:-$R/tools/zig/zig-linux-x86_64-$ZIG_VERSION}
ZIG_WINDOWS=${ZIG_WINDOWS:-$R/tools/zig/zig-windows-x86_64-$ZIG_VERSION}
PYTHON_LINUX=${PYTHON_LINUX:-$R/tools/python-linux/python}
PYTHON_WINDOWS=${PYTHON_WINDOWS:-$R/tools/python-windows/python}
LLVM_MINGW=${LLVM_MINGW:-$R/tools/llvm-mingw}
VULKAN_INC=${VULKAN_INC:-$R/tools/vulkan/Vulkan-Headers-1.3.290/include}
X11_INC=${X11_INC:-$R/tools/debs/libx11/usr/include:$R/tools/debs/x11proto/usr/include}
JOBS=${JOBS:-$(nproc)}
K=$OUT/buildkit
W=$R/build/release/kit-$PLAT          # object files and the compiler cache of this script
[ -x "$ZIG_HOST/zig" ] || { echo "make-kit: no zig at $ZIG_HOST (run release/fetch.sh)" >&2; exit 1; }
[ -f "$VULKAN_INC/vulkan/vulkan_core.h" ] || { echo "make-kit: no Vulkan headers at $VULKAN_INC" >&2; exit 1; }
export ZIG_GLOBAL_CACHE_DIR=$W/zig-cache ZIG_LOCAL_CACHE_DIR=$W/zig-cache
mkdir -p "$W/obj" "$K/recomp" "$K/rt/lib" "$K/rt/src"

# the runtime sources that include generated (game-derived) headers are compiled on the player's machine
ONDEVICE="support.c vu.c interp.c"
FLAGS="-O2 -g0 -fwrapv -fno-strict-aliasing -fno-math-errno -ffp-contract=off -w -DGS_HW -I$R/recomp/rt -I$VULKAN_INC"
cd "$R"
srcs=()
for f in recomp/rt/*.c recomp/rt/hw/*.c; do case " $ONDEVICE " in *" $(basename "$f") "*) ;; *) srcs+=("$f");; esac; done

if [ "$PLAT" = linux ]; then
  xinc=""; IFS=: read -ra xd <<< "$X11_INC"; for d in "${xd[@]}"; do xinc="$xinc -isystem $d"; done
  CC="$ZIG_HOST/zig cc -target x86_64-linux-gnu.2.31 -fno-sanitize=undefined $FLAGS $xinc"
  HOSTCC="$ZIG_HOST/zig cc -target x86_64-linux-gnu.2.31 -fno-sanitize=undefined"
  EXE=""
elif [ "$PLAT" = windows ]; then
  MC=$LLVM_MINGW/bin/x86_64-w64-mingw32-clang
  [ -x "$MC" ] || { echo "make-kit: no llvm-mingw at $LLVM_MINGW (run release/fetch.sh)" >&2; exit 1; }
  # llvm-mingw brings its own LLVM libraries; a host LD_LIBRARY_PATH pointing at another clang makes it crash
  unset LD_LIBRARY_PATH
  CC="$MC $FLAGS -D_WIN32_WINNT=0x0A00 -DUNICODE -D_UNICODE -include $R/recomp/rt/win_compat.h"
  HOSTCC="$MC -static"
  EXE=".exe"
else
  echo "make-kit: unknown platform $PLAT" >&2; exit 2
fi

echo "make-kit: compiling the runtime (${#srcs[@]} files)" >&2
export CC W
printf '%s\n' "${srcs[@]}" | xargs -P "$JOBS" -I{} bash -c 'o=$W/obj/$(basename "{}" .c).o; $CC -c "{}" -o "$o" || exit 255'
for f in "${srcs[@]}"; do cp "$W/obj/$(basename "$f" .c).o" "$K/rt/lib/"; done
for f in $ONDEVICE; do cp "recomp/rt/$f" "$K/rt/src/"; done
cp recomp/rt/*.h "$K/rt/src/"
if [ "$PLAT" = windows ]; then
  # the runtime uses pthreads; on Windows that is winpthreads (part of mingw-w64), which Zig does not carry
  cp "$LLVM_MINGW/x86_64-w64-mingw32/lib/libpthread.a" "$K/rt/lib/"
fi

echo "make-kit: VU1 translator, recompiler, tool" >&2
$HOSTCC -O2 -w -Irecomp/rt -DVU_NO_FAST $([ "$PLAT" = windows ] && echo "-include recomp/rt/win_compat.h") recomp/tools/vubench.c -o "$K/vugen$EXE" $([ "$PLAT" = linux ] && echo "-lm -s")
cp recomp/gen.py recomp/sem.py recomp/r5900.py recomp/elf.py recomp/games.py recomp/tools/vu1extract.py "$K/recomp/"
cp release/ror_build.py "$K/"
printf '%s\n' "$VERSION" > "$K/VERSION"
if [ "$PLAT" = linux ]; then
  cp release/ror-build.sh "$OUT/ror-build"; chmod +x "$OUT/ror-build" "$K/vugen"
else
  # version information and manifest, as for the launcher (release/win/app.rc)
  vnum=$(printf '%s' "$VERSION" | sed -n 's/^v\{0,1\}\([0-9]\+\)\.\([0-9]\+\)\.\([0-9]\+\).*/\1,\2,\3,0/p'); vnum=${vnum:-0,0,0,0}
  sed -e "s|@ROR_VER@|$vnum|g" -e "s|@ROR_VER_STR@|$VERSION|g" -e "s|@ROR_DESC@|Rule of Rose PC Port game builder|" \
      -e "s|@ROR_FILE@|ror-build.exe|g" -e "s|@ROR_MANIFEST@|$PWD/release/win/app.manifest|" release/win/app.rc.in > "$K/ror-build.rc"
  "$LLVM_MINGW/bin/x86_64-w64-mingw32-windres" "$K/ror-build.rc" -O coff -o "$K/ror-build.res"
  $MC -O2 -municode -static release/ror-build-win.c "$K/ror-build.res" -o "$OUT/ror-build.exe"
  rm -f "$K/ror-build.res" "$K/ror-build.rc"
  "$LLVM_MINGW/bin/x86_64-w64-mingw32-strip" "$OUT/ror-build.exe" "$K/vugen.exe"
fi
if [ "${RUNTIME_ONLY:-0}" = 1 ]; then echo "make-kit: RUNTIME_ONLY=1, stopping before the compiler and Python are added" >&2; exit 0; fi
# identity of everything that ends up in ror-game besides the game itself
(cd "$K" && find recomp rt vugen$EXE ror_build.py -type f | LC_ALL=C sort | xargs -d '\n' sha256sum | sha256sum | cut -d' ' -f1) > "$K/KITHASH"

echo "make-kit: compiler and Python" >&2
if [ "$PLAT" = linux ]; then ZSRC=$ZIG_LINUX; PSRC=$PYTHON_LINUX; else ZSRC=$ZIG_WINDOWS; PSRC=$PYTHON_WINDOWS; fi
[ -d "$ZSRC/lib" ] || { echo "make-kit: no zig tree at $ZSRC" >&2; exit 1; }
[ -d "$PSRC" ] || { echo "make-kit: no Python at $PSRC" >&2; exit 1; }
rm -rf "$K/zig" "$K/python"
mkdir -p "$K/zig"
cp -r "$ZSRC/zig$EXE" "$ZSRC/lib" "$ZSRC/LICENSE" "$K/zig/"
# Zig carries headers and C libraries for every target it can build for; only x86-64 Linux (glibc) and
# Windows are used here. Removing the rest takes two thirds off the size.
(
  cd "$K/zig/lib"
  rm -rf libcxx libcxxabi libunwind tsan libc/musl libc/wasi libc/darwin
  cd libc/include
  for d in *; do
    case "$d" in
      any-linux-any|generic-glibc|x86_64-linux-gnu|x86-linux-any|x86_64-linux-any|any-windows-any|x86_64-windows-gnu|x86_64-windows-any) ;;
      *) rm -rf "$d" ;;
    esac
  done
)
cp -r "$PSRC" "$K/python"
(
  cd "$K/python"
  if [ "$PLAT" = linux ]; then L=lib/python3.12; rm -rf include share lib/pkgconfig lib/tcl* lib/tk* lib/itcl* lib/thread* lib/libtcl* lib/libtk* "$L/config-3.12-x86_64-linux-gnu"
  else L=Lib; rm -rf include libs tcl Scripts DLLs/tcl*.dll DLLs/tk*.dll DLLs/_tkinter.pyd; fi
  rm -rf "$L/test" "$L/idlelib" "$L/tkinter" "$L/turtledemo" "$L/ensurepip" "$L/site-packages" "$L/lib2to3" "$L/pydoc_data" "$L/unittest" "$L/venv"
  find . -name __pycache__ -type d -prune -exec rm -rf {} +
)
echo "make-kit: $OUT ($(du -sh "$K" | cut -f1) kit)" >&2
