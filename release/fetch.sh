#!/usr/bin/env bash
# fetch.sh [item...] — download the pinned third-party tools a release is
# built from, verify each against its recorded SHA-256 and unpack it under
# tools/. Without arguments: everything. Items:
#
#   zig-linux zig-windows        Zig 0.13.0 (the C compiler shipped to players; also builds the Linux runtime)
#   python-linux python-windows  python-build-standalone, CPython 3.12.16 (shipped to players)
#   llvm-mingw                   llvm-mingw 20240619, UCRT (Windows runtime objects and launcher)
#   vulkan-headers               Vulkan-Headers 1.3.290 (headers only)
#   ffx-sdk                      AMD FidelityFX SDK 2.4.0 (two signed FSR 3.1 DLLs and AMD's licence; optional)
#   winpthreads-license mingw-license   licence texts of the mingw-w64 parts linked into the Windows game
#
# Archives are kept in tools/dl/<item>/ (a download cache: an archive already
# there with the right hash is not fetched again). Nothing is executed from a
# download before its hash has been checked. Hash sources: Zig, ziglang.org
# download index; Python, the release's SHA256SUMS; llvm-mingw,
# Vulkan-Headers and the FidelityFX SDK publish none, the values are those of
# the files as first downloaded (2026-11).
set -euo pipefail
R="$(cd "$(dirname "$0")/.." && pwd)"
T=$R/tools
UA="curl"

# name | url | sha256 | archive file name
PINS='
zig-linux|https://ziglang.org/download/0.13.0/zig-linux-x86_64-0.13.0.tar.xz|d45312e61ebcc48032b77bc4cf7fd6915c11fa16e4aad116b66c9468211230ea|zig.tar.xz
zig-windows|https://ziglang.org/download/0.13.0/zig-windows-x86_64-0.13.0.zip|d859994725ef9402381e557c60bb57497215682e355204d754ee3df75ee3c158|zig.zip
python-linux|https://github.com/astral-sh/python-build-standalone/releases/download/20261110/cpython-3.12.16%2B20261110-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz|6a9e62dad033790aef722dbd370e0ce76ce17b73627ba3ae679f537049350b39|python.tar.gz
python-windows|https://github.com/astral-sh/python-build-standalone/releases/download/20261110/cpython-3.12.16%2B20261110-x86_64-pc-windows-msvc-install_only_stripped.tar.gz|355ee61b8207b91574491b1c6dc9b0ebb4408fc56bfafe498f3ef439314b52dc|python.tar.gz
llvm-mingw|https://github.com/mstorsjo/llvm-mingw/releases/download/20240619/llvm-mingw-20240619-ucrt-ubuntu-20.04-x86_64.tar.xz|27d33157cc252c29ad6f777a96a0d94176fea1b534ff09b5071485def143b90e|llvm-mingw.tar.xz
vulkan-headers|https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.3.290.tar.gz|f38a653bf93cab7a2a229a53d2d53b1cba9a2819e4c0a7de13c54085bde9bcf5|vulkan-headers.tar.gz
ffx-sdk|https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/archive/refs/tags/v2.4.0.zip|27120e3bbe0d0b30213825ad40f19077563f04f04572e252109a392d5fb522cb|ffx-sdk.zip
winpthreads-license|https://raw.githubusercontent.com/mingw-w64/mingw-w64/v12.0.0/mingw-w64-libraries/winpthreads/COPYING|63263614cdd29f2f93cba85e992f041b31f9fc7b4033692f31269489a8a1b177|COPYING
mingw-license|https://raw.githubusercontent.com/mingw-w64/mingw-w64/v12.0.0/COPYING.MinGW-w64-runtime/COPYING.MinGW-w64-runtime.txt|e9b2dc02451ea29092a1f25fa0f3c07207ed421f1807dffb0c4e6dce69dee7bd|COPYING.MinGW-w64-runtime.txt
'

download() {   # item -> path of the verified archive on stdout
  local item=$1 url sha file line
  line=$(printf '%s\n' "$PINS" | grep "^$item|") || { echo "fetch: unknown item $item" >&2; return 2; }
  IFS='|' read -r _ url sha file <<< "$line"
  local d=$T/dl/$item; mkdir -p "$d"
  if [ ! -f "$d/$file" ] || [ "$(sha256sum "$d/$file" | cut -d' ' -f1)" != "$sha" ]; then
    echo "fetch: $item <- $url" >&2
    curl -fsSL -A "$UA" --retry 3 -o "$d/$file.part" "$url"
    if [ "$(sha256sum "$d/$file.part" | cut -d' ' -f1)" != "$sha" ]; then
      echo "fetch: $item: SHA-256 mismatch (got $(sha256sum "$d/$file.part" | cut -d' ' -f1), want $sha)" >&2
      rm -f "$d/$file.part"; return 1
    fi
    mv "$d/$file.part" "$d/$file"
  fi
  echo "$d/$file"
}
unzip_py() { python3 -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$1" "$2"; }

fetch_one() {
  local item=$1 a
  case "$item" in
    zig-linux)
      [ -x "$T/zig/zig-linux-x86_64-0.13.0/zig" ] && return 0
      a=$(download "$item"); mkdir -p "$T/zig"; tar -C "$T/zig" -xf "$a" ;;
    zig-windows)
      [ -f "$T/zig/zig-windows-x86_64-0.13.0/zig.exe" ] && return 0
      a=$(download "$item"); mkdir -p "$T/zig"; unzip_py "$a" "$T/zig" ;;
    python-linux|python-windows)
      [ -d "$T/$item/python" ] && return 0
      a=$(download "$item"); mkdir -p "$T/$item"; tar -C "$T/$item" -xzf "$a" ;;
    llvm-mingw)
      [ -x "$T/llvm-mingw/bin/x86_64-w64-mingw32-clang" ] && return 0
      a=$(download "$item"); mkdir -p "$T/llvm-mingw"; tar -C "$T/llvm-mingw" --strip-components=1 -xf "$a" ;;
    vulkan-headers)
      [ -f "$T/vulkan/Vulkan-Headers-1.3.290/include/vulkan/vulkan_core.h" ] && return 0
      a=$(download "$item"); mkdir -p "$T/vulkan"; tar -C "$T/vulkan" -xzf "$a" ;;
    ffx-sdk)
      [ -f "$T/ffx/sdk-v2.4.0/FidelityFX-SDK-2.4.0/Kits/FidelityFX/signedbin/amd_fidelityfx_loader_vk.dll" ] && return 0
      # optional: without it the Windows package has no FSR 3.1 DLLs and --filter fsr3 works as fsr
      a=$(download "$item") || { echo "fetch: ffx-sdk not available; continuing without the FSR 3.1 DLLs" >&2; return 0; }
      mkdir -p "$T/ffx/sdk-v2.4.0"
      python3 - "$a" "$T/ffx/sdk-v2.4.0" <<'PY'
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
want = ('Kits/FidelityFX/signedbin/amd_fidelityfx_loader_vk.dll', 'Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_vk.dll', 'docs/license.md', 'LICENSE.txt')
for n in z.namelist():
    if n.endswith(want): z.extract(n, sys.argv[2])
PY
      ;;
    winpthreads-license|mingw-license)
      download "$item" > /dev/null ;;          # a text file, used from tools/dl/ as it is
    *) echo "fetch: unknown item $item" >&2; return 2 ;;
  esac
  echo "fetch: $item ready" >&2
}

[ $# -gt 0 ] || set -- zig-linux zig-windows python-linux python-windows llvm-mingw vulkan-headers ffx-sdk winpthreads-license mingw-license
for item in "$@"; do fetch_one "$item"; done
