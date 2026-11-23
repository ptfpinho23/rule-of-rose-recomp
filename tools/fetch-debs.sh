#!/usr/bin/env bash
# fetch-debs.sh — download and unpack the Ubuntu packages the launcher needs
# (no root: nothing is installed, the .debs are only extracted under tools/debs).
#   tools/debs/pkgs/    the downloaded .deb files (sha512-checked)
#   tools/debs/qtdev/   build-time headers: kernel, GL/EGL (GLVND), xkbcommon
#   tools/debs/xcbrt/   run-time libraries Qt's xcb platform plugin needs and a
#                       stock Ubuntu install lacks (libxcb-cursor & friends)
# tools/debs/extract/ (glibc headers + CRT objects + libgcc 14) is shared with
# the game build and is not touched here.
# The list was produced with `apt-get download --print-uris <packages>` on
# Ubuntu 26.04; re-run that to refresh versions. Safe to re-run.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/debs"
M=http://archive.ubuntu.com/ubuntu/pool
mkdir -p pkgs qtdev xcbrt
get() { # get <dest-tree> <pool-path> <sha512>
  local tree=$1 url=$M/$2 sum=$3 f=pkgs/$(basename "$2")
  if ! { [ -f "$f" ] && echo "$sum  $f" | sha512sum -c --status; }; then
    curl -fsSL -A "curl" -o "$f" "$url"
    echo "$sum  $f" | sha512sum -c --status || { echo "checksum mismatch: $f" >&2; exit 1; }
  fi
  dpkg-deb -x "$f" "$tree"
  echo "ok $tree <- $(basename "$f")"
}
# build-time headers
get qtdev main/l/linux/linux-libc-dev_7.0.0-44.44_amd64.deb 489c2a30521d6228e764b537c51cc1e2566199a36b7c144bcd0c9c5e49d5133a7515aa234f70478c92fda0cc23ddb539cbe4f519ab454d49db6afd0131397e08
get qtdev main/libg/libglvnd/libgl-dev_1.7.0-3_amd64.deb 3f8b54f2576236dcb584b4add0086e598a9577dc4bba43e5d764034b337ad9724ee6c06abe0298e60fc10e556c2fd1201a74092a2d85b8b14581b754fae4af45
get qtdev main/libg/libglvnd/libglx-dev_1.7.0-3_amd64.deb 5e3b88e6ee4fa13ee892e64e9aa66d94b0a8dd9990647afc1790a0e2d9a921fdf5569212036570892fa1203346506ecc37e591975190a7416d8622c9cd003899
get qtdev main/libg/libglvnd/libegl-dev_1.7.0-3_amd64.deb fbb4d18dbde7e85d65a3748d1ef2704f1991514d73eb6b86cf63695781c958f1f96323589dda53a6a5a38f051955d1b761cdb5b59c9b64dcf2cb94ba222c59e1
get qtdev main/libx/libxkbcommon/libxkbcommon-dev_1.13.1-1_amd64.deb 9a43fa917e9f1f6837bce99967d3b37b859d81f522add6b6c2ab8e19e451ca2da654b6368ed89809f5ef568180a34c39dd73b502b21e9c9af0fcfd0dedef650a
# xcb platform plugin run-time libraries
get xcbrt universe/x/xcb-util-cursor/libxcb-cursor0_0.1.6-1_amd64.deb 4bc54701fb0f5a77fb0c2bd814466addc2bbc0c103dbd8b199ee5938344ef7168d1f23ed6e113d173670480775afbf3a0fabcf869ad3aa4c31a0eb3eed539a3d
get xcbrt universe/x/xcb-util-wm/libxcb-icccm4_0.4.2-1build1_amd64.deb a4ce114d89dd75538c9592b79c3b534861f492f423275feb3cd4c14bd0f5113f0d5c689c33e878f49726ebe857adb7d58726d5095a4016de1237a6e34b5f670d
get xcbrt universe/x/xcb-util-image/libxcb-image0_0.4.0-2build2_amd64.deb 65dd572c260c8a9eb9524e0f2181a015718e97a6260baae65c2b4028858cfc855f0b2254d66cbf881eed7f957aff9294e27022dbc9bc9d279d089547a3b06a3e
get xcbrt universe/x/xcb-util-keysyms/libxcb-keysyms1_0.4.1-1build1_amd64.deb 5da07f0d9e4b5dcf82f44a9e3850025a751c89251191a06d06d330e12b2124ff528d80a5e10796d3ff95c0b420fe361de5689592de7802362a5d2b2c7270dabe
get xcbrt universe/x/xcb-util-renderutil/libxcb-render-util0_0.3.10-1build1_amd64.deb 1cd4f7a82a47c1d49d3ba7e519c340d5500084c6483895894b464d178fa965adccd8cbd16871b64492e264dbdd620c33f3295bfc80d7f0c2c1ec3d9df8cb3274
get xcbrt main/x/xcb-util/libxcb-util1_0.4.1-1build1_amd64.deb e62d82599e70adca095087422c8cd4a62e3a27a763f4450f2c97e80d8698634169f138e9a3554054663114e9c4f33f5d3f5ad8060002aff0f8f1cd9a01bc65d5
get xcbrt main/libx/libxcb/libxcb-shape0_1.17.0-2ubuntu1_amd64.deb 89b4a41d5de8524a03b493cf907b593df7f46b14cac8202a449ba45cbc3ab9b05ebd72e96f856b1930528f34dd77381e416a6df81b61909442cee3e7e038ecb3
get xcbrt main/libx/libxcb/libxcb-xkb1_1.17.0-2ubuntu1_amd64.deb 836bd65c41cd7634c310a954d4804bfeb5d5306f76fe39e59fd71cc232a242d2aa2cf93e8ea1b2a7405fdf49d34db4634a0ea952e58ef2d9955f8a5fc82f73b7
get xcbrt main/libx/libxkbcommon/libxkbcommon-x11-0_1.13.1-1_amd64.deb 9b0427c6c9cbfc184a469d7e80ad466413798dc2b3954fb48c47f2f025f6626dff0f78fd616d43fb4c5e3b2aa01585a8a66f7f32cb442f8c1903098908322786
