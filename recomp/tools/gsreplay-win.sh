#!/usr/bin/env bash
# gsreplay-win.sh — build the replay tool with the Vulkan renderer for Windows
# (build/gsreplay/gsreplay-hw.exe) with tools/llvm-mingw. Run it on Windows, from a
# Windows folder, to use the real GPU.
set -euo pipefail
R="$(cd "$(dirname "$0")/../.." && pwd)"
CC=$R/tools/llvm-mingw/bin/x86_64-w64-mingw32-clang
D=$R/build/gsreplay; mkdir -p "$D"
CFLAGS="-O2 -fwrapv -fno-strict-aliasing -fno-math-errno -ffp-contract=off -Wno-unused-function -Wno-unused-variable -DGS_HW -include $R/recomp/rt/win_compat.h -I$R/recomp/rt -I$R/build/recomp/gen -I$R/tools/vulkan/Vulkan-Headers-1.3.290/include"
$CC $CFLAGS "$R/recomp/tools/gsreplay.c" "$R/recomp/rt/hw/gs_hw.c" -static -lpthread -o "$D/gsreplay-hw.exe"
echo "built $D/gsreplay-hw.exe" >&2
