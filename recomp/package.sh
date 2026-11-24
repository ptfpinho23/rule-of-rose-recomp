#!/usr/bin/env bash
# package.sh — assemble the self-contained game folders under dist/.
#   recomp/package.sh [linux|windows|all]        (default: all)
#
#   dist/RuleOfRose-linux/      RuleOfRose (launcher), ror-game, lib/, plugins/, README.txt
#   dist/RuleOfRose-windows/    RuleOfRose.exe (launcher), ror-game.exe, Qt + runtime DLLs,
#                               platforms/, styles/, README.txt
#
# Inputs:
#   linux    game ${BIN:-build/recomp/ror}              (recomp/build.sh)
#            launcher built here via  tools/build.sh dist
#   windows  game ${WINBIN:-build/recomp/win/ror.exe}   (recomp/build-win.sh)
#            launcher built here via  tools/build-win.sh
#            Skipped with a message when the game .exe does not exist yet.
#   DIST=dir output root instead of dist/
# The disc image is never copied. A saves/ folder already in a dist folder
# is left alone when repackaging.
set -euo pipefail
cd "$(dirname "$0")/.."
REPO=$PWD
DIST=${DIST:-dist}     # output root (override for trial runs)
# The host clang tools need libraries that live beside them. Set that only for
# those tools: exported globally it makes llvm-mingw's clang (the Windows
# cross compiler) load the wrong LLVM libraries and crash.
CLANGLIB=${ROR_CLANG:-$HOME/.local/opt/clang}/lib
# Overridable locations (the defaults are this project's private set-up):
#   QT_LINUX, QT_WIN   Qt 6 kits (gcc_64, llvm-mingw_64)      LLVM_MINGW   the Windows toolchain
#   ROR_CLANG          the private clang (strip, readelf, libc++)
#   ROR_TOOLCHAIN=system   the launcher was built with the system compiler: use the system's strip/readelf,
#                      bundle no libc++, take the xcb helper libraries from the system (XCBRT=dir overrides)
#   GAME=none          leave ror-game out (release layout: the player's machine builds it, see release/)
if [ "${ROR_TOOLCHAIN:-private}" = system ]; then
  host_strip()   { strip "$@"; }
  host_readelf() { readelf "$@"; }
else
  host_strip()   { LD_LIBRARY_PATH="$CLANGLIB" "${ROR_CLANG:-$HOME/.local/opt/clang}/bin/llvm-strip" "$@"; }
  host_readelf() { LD_LIBRARY_PATH="$CLANGLIB" "${ROR_CLANG:-$HOME/.local/opt/clang}/bin/llvm-readelf" "$@"; }
fi
GAME=${GAME:-full}

# licenses/: the bundled typeface, and AMD's MIT licence for FSR 1.0 (the
# game's upscaler) when the game sources provide it under recomp/rt/.
copy_licenses() {
  local D=$1 fsr
  mkdir -p "$D/licenses"
  cp launcher/assets/fonts/OFL.txt "$D/licenses/Cormorant-Garamond-OFL.txt"
  fsr=$(find recomp/rt -maxdepth 2 -type f \( -iname '*fsr*licen*' -o -iname '*licen*fsr*' \
        -o -iname '*fidelityfx*licen*' -o -iname '*licen*fidelityfx*' \
        -o -iname '*amd*licen*' -o -iname '*licen*amd*' \) | sort | head -n 1)
  if [ -n "$fsr" ]; then
    cp "$fsr" "$D/licenses/AMD-FidelityFX-FSR1-LICENSE.txt"
  else
    echo "package: WARNING - no FSR licence file found under recomp/rt/; licenses/ ships without it" >&2
  fi
}

keys_text() {
cat <<'EOF'
Controls (keyboard and gamepad work together; the keyboard always works)
  PS2 pad      Key          Xbox pad        PlayStation pad
  D-pad        Arrow keys   D-pad           D-pad
  Cross        Z            A               Cross
  Circle       X            B               Circle
  Square       A            X               Square
  Triangle     S            Y               Triangle
  L1 / R1      Q / W        LB / RB         L1 / R1
  L2 / R2      E / R        LT / RT         L2 / R2
  L3 / R3      -            stick clicks    L3 / R3
  Sticks       -            left (movement) and right stick, on both
  Start        Enter        Start           Options
  Select       Backspace    Back            Share / Create
  Esc quits the game. PS button and touchpad are not used.

Gamepads: Xbox-style pads (XInput on Windows, evdev on Linux) and
DualShock 4 / DualSense (read directly: USB and Bluetooth on Windows, evdev
on Linux). Auto uses an Xbox-style pad if one is present and a PlayStation
pad only when there is none, so a pad wrapped by Steam Input or DS4Windows
is not read twice. DualShock 3 is not supported on Windows. Vibration is
passed on when the game asks for it.
Status: experimental - not yet tested with a real controller.
EOF
}

# ---------------------------------------------------------------- linux ----
package_linux() {
  local BIN=${BIN:-build/recomp/ror}
  local D=$DIST/RuleOfRose-linux
  local QT=${QT_LINUX:-$HOME/.local/opt/qt6/6.8.2/gcc_64}
  local SYS=/usr/lib/x86_64-linux-gnu
  local CXXRT=${ROR_CLANG:-$HOME/.local/opt/clang}/lib/x86_64-unknown-linux-gnu
  local XCBRT=${XCBRT:-$REPO/tools/debs/xcbrt/usr/lib/x86_64-linux-gnu}
  if [ "${ROR_TOOLCHAIN:-private}" = system ]; then CXXRT=$QT/lib; XCBRT=${XCBRT_SYSTEM:-$SYS}; fi
  [ "$GAME" = none ] || [ -x "$BIN" ] || { echo "package linux: $BIN not found - run recomp/build.sh first" >&2; return 1; }

  tools/build.sh dist >&2
  local L=launcher/build-dist/ror_launcher

  # Start from clean lib/ and plugins/ (stale libraries must not linger).
  rm -rf "$D/lib" "$D/plugins"
  mkdir -p "$D/lib" "$D/plugins/platforms"

  cp "$L" "$D/RuleOfRose"
  host_strip "$D/RuleOfRose"
  if [ "$GAME" = none ]; then rm -f "$D/ror-game"; else cp "$BIN" "$D/ror-game"; host_strip "$D/ror-game"; fi
  cp "$QT/plugins/platforms/libqxcb.so" "$D/plugins/platforms/"
  copy_licenses "$D"

  # Every shared library the launcher, the xcb plugin and the preloaded xcb
  # helpers need that is not part of the system ($SYS): Qt, ICU, libc++,
  # the xcb-util family. ldd resolves the full transitive set.
  local preload
  preload=$(grep -o '"lib[^"]*\.so[^"]*"' launcher/main.cpp | tr -d '"')
  local roots=("$L" "$QT/plugins/platforms/libqxcb.so")
  for n in $preload; do roots+=("$XCBRT/$n"); done
  LD_LIBRARY_PATH="$QT/lib:$CXXRT:$XCBRT" ldd "${roots[@]}" |
    awk '$2 == "=>" && $3 ~ /^\// { print $1, $3 }' | sort -u |
    while read -r name path; do
      case "$path" in
        "$SYS"/*|/lib/*|/lib64/*|/usr/lib64/*) ;;            # system library
        *) cp -L "$path" "$D/lib/$name" ;;
      esac
    done
  for n in $preload; do cp -L "$XCBRT/$n" "$D/lib/$n"; done
  # libxkbcommon-x11 and libxkbcommon are two halves of one library and share
  # internals: a bundled libxkbcommon-x11 with the system's libxkbcommon of
  # another version crashes at start. Ship the libxkbcommon that belongs to
  # the libxkbcommon-x11 just copied (same directory, else the system's).
  if [ -e "$XCBRT/libxkbcommon.so.0" ]; then cp -L "$XCBRT/libxkbcommon.so.0" "$D/lib/libxkbcommon.so.0"
  else cp -L "$SYS/libxkbcommon.so.0" "$D/lib/libxkbcommon.so.0"; fi
  if LD_LIBRARY_PATH="$QT/lib:$CXXRT:$XCBRT" ldd "${roots[@]}" | grep "not found" >&2; then
    echo "package linux: unresolved libraries (see above)" >&2; return 1
  fi

  # The launcher itself must not need an OpenGL library: libOpenGL.so.0 is
  # missing on many desktops (see launcher/CMakeLists.txt). Some linkers keep
  # the entries although nothing is used from them: take them out.
  python3 recomp/tools/elf-drop-needed.py "$D/RuleOfRose" libOpenGL.so.0 libGLX.so.0 >&2
  if host_readelf -d "$D/RuleOfRose" | grep NEEDED | grep -E 'libOpenGL|libGLX|libGL\.so' >&2; then
    echo "package linux: the launcher is linked against an OpenGL library (above); it would not start where that is not installed" >&2; return 1
  fi

  # The launcher must look in lib/ beside itself and nowhere else.
  local rp
  rp=$(host_readelf -d "$D/RuleOfRose" | grep -E 'RUNPATH|RPATH' || true)
  case "$rp" in *'[$ORIGIN/lib]'*) ;; *) echo "package linux: unexpected RUNPATH: $rp" >&2; return 1 ;; esac

  {
  cat <<'EOF'
Rule of Rose — native PC build (Linux x86-64)
===============================================================

Run:      ./RuleOfRose
          This opens the launcher. Choose your disc image, press PLAY.

Needs:    your own copy of the European disc (SLES-54218) as an .iso file.
          The launcher finds "Rule of Rose (Europe) (En,Fr,De,Es,It).iso" or
          "RuleOfRose.iso" by itself when it is in this folder or up to four
          folders above; otherwise use Browse. The choice is remembered.

Saves:    saves/mc0/ in this folder (memory card slot 1).

In the launcher
  Options
    Display  Mode (window / fullscreen; Alt+Enter or F11 switches in game),
             Resolution (the window's size; fullscreen uses the monitor's),
             Picture shape (4:3 is the original; 16:9, 21:9, 32:9 are
             experimental: 3D scenes get a wider view, menus, movies and
             text stay 4:3), Stretch to fill the window, Frame rate
             (original 50 Hz, match the display, or 60-240 by interpolating
             frames) and V-Sync. Frame rate and V-Sync belong to the GPU
             renderer: choosing one switches the renderer to GPU by itself.
    Quality  Renderer: Software, or GPU (Vulkan 1.3). The launcher looks at
             the graphics hardware and starts with GPU at 4x on a separate
             graphics card, GPU at 1x on graphics built into the processor,
             and Software otherwise (then with Bilinear instead of FSR 1.0
             on a small processor or a large picture). Without Vulkan the
             game uses Software by itself. Choosing Software sets the frame
             rate back to the original. Internal resolution (GPU: draws the
             PS2 picture 1x to 8x larger), Smooth distant textures (GPU: far
             floors and walls shimmer less; off is the original look;
             untouched, the game decides), Upscaling
             (Nearest, Bilinear, or FSR 1.0 = AMD FidelityFX Super
             Resolution), Sharpness (FSR)
    Image    Brightness, Contrast, Gamma, Saturation, Bloom, Film grain
             (the game's own grain overlay; 100% original, 0% none)
             Until you set them yourself, Film grain and Sharpness start
             lower where they stand out more: 50% and 50 (of at most 75)
             with the GPU renderer at 2x and above; with Software 75% and
             50 at about 1080p, 50% and 40 (of at most 70) at 1440p and
             above; otherwise 100% and 60.
             (double-click a slider, or Reset, for the defaults)
    Game     Sound on/off; Memory card inserted or not (without a card the
             game cannot save or load); Gamepad (Auto, Xbox, PlayStation,
             Off); Stick dead zone
  Controls   the keyboard and gamepad map
  Enter starts the game; drag the picture to move the window.
  The launcher contains no artwork from the game; its title card is drawn.

EOF
  keys_text
  cat <<'EOF'

Files
  RuleOfRose       the launcher (start this)
  ror-game         the game itself; the launcher starts it
  lib/, plugins/   Qt 6 and the other libraries the launcher needs
  licenses/        licences of the bundled typeface and of FSR
  game.log         written on each start: the game's console output

Running the game without the launcher
  ./ror-game --iso /path/to.iso [switches]
  --res WxH               window size in pixels, e.g. --res 1920x1080
                          (default 1280x960; --scale 1..4 = 640x480 times N)
  --fullscreen            borderless fullscreen, at the desktop's size
  --renderer software|gpu (default software; gpu needs Vulkan 1.3)
  --internal 1..8         gpu: internal resolution multiplier (default 4)
  --vsync 0|1             gpu (default 1)
  --fps 0|display|25..1000   gpu: interpolated frame rate (0 = off)
  --wide off|16:9|21:9|32:9|W:H   widescreen 3D scenes (experimental; default off)
  --aspect 4:3|stretch    4:3 keeps the picture's shape (default), stretch fills
  --filter nearest|bilinear|fsr|fsr3   upscaling (default fsr; fsr3 = FSR 3.1
                          is Windows only and works as fsr here)
  --sharpness 0..100      FSR sharpening (default 60)
  --brightness -100..100, --contrast -100..100   (default 0)
  --gamma 50..200         100 = 1.00; higher brightens midtones
  --saturation 0..200     (default 100)       --bloom 0..100   (default 0)
  --film-grain 0..100     the game's film grain (default 100 = original)
  --texture-filter auto|original|smooth   gpu: filtering of distant textures
                          (default auto: smooth from --internal 2 on a
                          separate graphics card, original otherwise)
  --pad auto|xinput|playstation|off   which gamepad (default auto)
  --pad-deadzone 0..90    stick dead zone in percent (default 24)
  ROR_MC_DIR=dir   memory card folder        ROR_NO_MC=1     no memory card
  ROR_NOAUDIO=1    no sound

Requirements: a 64-bit Linux with X11 (or XWayland) and the usual
desktop libraries (libX11, libxcb, libGL, fontconfig, freetype, glib, dbus).
Qt is licensed under the LGPL v3; see https://www.qt.io/licensing.
The launcher's typeface is Cormorant Garamond, under the SIL Open Font
License. FSR 1.0 is (c) Advanced Micro Devices, Inc., under the MIT licence.
Both texts are in licenses/.
EOF
  } > "$D/README.txt"

  echo "packaged $D" >&2
  ls -la "$D" "$D/lib" "$D/plugins/platforms" >&2
}

# -------------------------------------------------------------- windows ----
package_windows() {
  local WINBIN=${WINBIN:-build/recomp/win/ror.exe}
  local D=$DIST/RuleOfRose-windows
  local QT=${QT_WIN:-$HOME/.local/opt/qt6/6.8.2/llvm-mingw_64}
  local MINGW=${LLVM_MINGW:-$REPO/tools/llvm-mingw}
  local OBJDUMP=$MINGW/bin/x86_64-w64-mingw32-objdump
  if [ "$GAME" != none ] && [ ! -f "$WINBIN" ]; then
    echo "package windows: SKIPPED - $WINBIN does not exist yet (build it with recomp/build-win.sh, then re-run: recomp/package.sh windows)" >&2
    return 0
  fi

  tools/build-win.sh >&2
  local L=launcher/build-win/ror_launcher.exe

  # Remove what an earlier run put here (never saves/).
  rm -rf "$D/platforms"
  mkdir -p "$D/platforms"
  find "$D" -maxdepth 1 -type f -name '*.dll' -delete

  cp "$L" "$D/RuleOfRose.exe"
  "$MINGW/bin/x86_64-w64-mingw32-strip" "$D/RuleOfRose.exe"
  if [ "$GAME" = none ]; then rm -f "$D/ror-game.exe"
  else cp "$WINBIN" "$D/ror-game.exe"; "$MINGW/bin/x86_64-w64-mingw32-strip" "$D/ror-game.exe"; fi
  cp "$QT/plugins/platforms/qwindows.dll" "$D/platforms/"
  copy_licenses "$D"

  # Follow the import tables: copy every DLL that comes from Qt or from the
  # llvm-mingw runtime (libc++, libunwind, ...); the rest are Windows' own.
  local todo=("$D/RuleOfRose.exe" "$D/platforms/qwindows.dll") f dll src dir
  [ "$GAME" = none ] || todo+=("$D/ror-game.exe")
  while [ ${#todo[@]} -gt 0 ]; do
    f=${todo[0]}; todo=("${todo[@]:1}")
    while read -r dll; do
      [ -e "$D/$dll" ] && continue
      src=
      for dir in "$QT/bin" "$MINGW/x86_64-w64-mingw32/bin"; do
        [ -f "$dir/$dll" ] && { src=$dir/$dll; break; }
      done
      [ -n "$src" ] || continue
      cp "$src" "$D/$dll"
      todo+=("$D/$dll")
    done < <("$OBJDUMP" -p "$f" | sed -n 's/^[[:space:]]*DLL Name: //p' | tr -d '\r')
  done

  # FSR 3.1 (--filter fsr3): AMD's signed runtime DLLs go beside ror-game.exe
  # exactly as AMD ships them - never stripped or otherwise touched, or the
  # signatures break. Without them the game uses FSR 1.0 instead.
  local FFX=tools/ffx/sdk-v2.4.0/FidelityFX-SDK-2.4.0
  local ffx_ok=1 n
  for n in amd_fidelityfx_loader_vk.dll amd_fidelityfx_upscaler_vk.dll; do
    [ -f "$FFX/Kits/FidelityFX/signedbin/$n" ] || ffx_ok=0
  done
  if [ $ffx_ok = 1 ]; then
    for n in amd_fidelityfx_loader_vk.dll amd_fidelityfx_upscaler_vk.dll; do
      cp "$FFX/Kits/FidelityFX/signedbin/$n" "$D/$n"
      cmp -s "$FFX/Kits/FidelityFX/signedbin/$n" "$D/$n" || { echo "package windows: $n was not copied intact" >&2; return 1; }
    done
    if [ -f "$FFX/docs/license.md" ]; then
      cp "$FFX/docs/license.md" "$D/licenses/AMD-FidelityFX-SDK-LICENSE.md"
    else
      echo "package windows: WARNING - $FFX/docs/license.md not found; the AMD SDK licence is not in licenses/" >&2
    fi
  else
    rm -f "$D/licenses/AMD-FidelityFX-SDK-LICENSE.md"
    echo "package windows: NOTE - AMD FidelityFX SDK not found under tools/ffx: the package has no FSR 3.1 DLLs, and --filter fsr3 will fall back to FSR 1.0" >&2
  fi

  {
  cat <<'EOF'
Rule of Rose - native PC build (Windows 10/11, 64-bit)
======================================================

Run:      RuleOfRose.exe
          This opens the launcher. Choose your disc image, press PLAY.

Needs:    your own copy of the European disc (SLES-54218) as an .iso file.
          The launcher finds "Rule of Rose (Europe) (En,Fr,De,Es,It).iso" or
          "RuleOfRose.iso" by itself when it is in this folder or up to four
          folders above; otherwise use Browse. The choice is remembered.

Saves:    saves\mc0\ in this folder (memory card slot 1). Keep the game in
          a folder you can write to (not Program Files).

In the launcher
  Options
    Display  Mode (window / fullscreen; Alt+Enter or F11 switches in game),
             Resolution (the window's size; fullscreen uses the monitor's),
             Picture shape (4:3 is the original; 16:9, 21:9, 32:9 are
             experimental: 3D scenes get a wider view, menus, movies and
             text stay 4:3), Stretch to fill the window, Frame rate
             (original 50 Hz, match the display, or 60-240 by interpolating
             frames) and V-Sync. Frame rate and V-Sync belong to the GPU
             renderer: choosing one switches the renderer to GPU by itself.
    Quality  Renderer: Software, or GPU (Vulkan 1.3). The launcher looks at
             the graphics hardware and starts with GPU at 4x on a separate
             graphics card, GPU at 1x on graphics built into the processor,
             and Software otherwise (then with Bilinear instead of FSR 1.0
             on a small processor or a large picture). Without Vulkan the
             game uses Software by itself. Choosing Software sets the frame
             rate back to the original. Internal resolution (GPU: draws the
             PS2 picture 1x to 8x larger), Smooth distant textures (GPU: far
             floors and walls shimmer less; off is the original look;
             untouched, the game decides), Upscaling
             (Nearest, Bilinear, FSR 1.0 = AMD FidelityFX Super
             Resolution, or FSR 3.1 = temporal upscaling: renders at a low
             internal resolution and reconstructs detail over time; GPU
             renderer only, best at 1x or 2x internal resolution; menus and
             movies use FSR 1.0), Sharpness (FSR)
    Image    Brightness, Contrast, Gamma, Saturation, Bloom, Film grain
             (the game's own grain overlay; 100% original, 0% none)
             Until you set them yourself, Film grain and Sharpness start
             lower where they stand out more: 50% and 50 (of at most 75)
             with the GPU renderer at 2x and above; with Software 75% and
             50 at about 1080p, 50% and 40 (of at most 70) at 1440p and
             above; otherwise 100% and 60.
             (double-click a slider, or Reset, for the defaults)
    Game     Sound on/off; Memory card inserted or not (without a card the
             game cannot save or load); Gamepad (Auto, Xbox, PlayStation,
             Off); Stick dead zone
  Controls   the keyboard and gamepad map
  Enter starts the game; drag the picture to move the window.
  The launcher contains no artwork from the game; its title card is drawn.

EOF
  keys_text
  cat <<'EOF'

Files
  RuleOfRose.exe     the launcher (start this)
  ror-game.exe       the game itself; the launcher starts it
  *.dll, platforms\  Qt 6 and the C++ runtime the launcher needs
  amd_fidelityfx_*.dll   AMD FSR 3.1 runtime, used by ror-game.exe
  licenses\          licences of the bundled typeface and of FSR
  game.log           written on each start: the game's console output

Running the game without the launcher
  ror-game.exe --iso C:\path\to.iso [switches]
  --res WxH               window size in pixels, e.g. --res 1920x1080
                          (default 1280x960; --scale 1..4 = 640x480 times N)
  --fullscreen            borderless fullscreen, at the desktop's size
  --renderer software|gpu (default software; gpu needs Vulkan 1.3)
  --internal 1..8         gpu: internal resolution multiplier (default 4)
  --vsync 0|1             gpu (default 1)
  --fps 0|display|25..1000   gpu: interpolated frame rate (0 = off)
  --wide off|16:9|21:9|32:9|W:H   widescreen 3D scenes (experimental; default off)
  --aspect 4:3|stretch    4:3 keeps the picture's shape (default), stretch fills
  --filter nearest|bilinear|fsr|fsr3   upscaling (default fsr; fsr3 = FSR 3.1,
                          needs --renderer gpu and the two amd_fidelityfx DLLs
                          beside ror-game.exe, else it works as fsr)
  --sharpness 0..100      FSR sharpening (default 60)
  --brightness -100..100, --contrast -100..100   (default 0)
  --gamma 50..200         100 = 1.00; higher brightens midtones
  --saturation 0..200     (default 100)       --bloom 0..100   (default 0)
  --film-grain 0..100     the game's film grain (default 100 = original)
  --texture-filter auto|original|smooth   gpu: filtering of distant textures
                          (default auto: smooth from --internal 2 on a
                          separate graphics card, original otherwise)
  --pad auto|xinput|playstation|off   which gamepad (default auto)
  --pad-deadzone 0..90    stick dead zone in percent (default 24)
  set ROR_MC_DIR=dir   memory card folder      set ROR_NO_MC=1   no memory card
  set ROR_NOAUDIO=1    no sound

Qt is licensed under the LGPL v3; see https://www.qt.io/licensing.
The launcher's typeface is Cormorant Garamond, under the SIL Open Font
License. FSR 1.0 is (c) Advanced Micro Devices, Inc., under the MIT licence.
The amd_fidelityfx_*.dll files are AMD's signed binaries, redistributed
unmodified under AMD's FidelityFX SDK binary licence (not MIT).
All three texts are in licenses\.
EOF
  } | sed 's/$/\r/' > "$D/README.txt"

  echo "packaged $D" >&2
  ls -la "$D" "$D/platforms" >&2
}

case "${1:-all}" in
  linux)   package_linux ;;
  windows) package_windows ;;
  all)     package_linux; package_windows ;;
  *)       echo "usage: recomp/package.sh [linux|windows|all]" >&2; exit 2 ;;
esac
