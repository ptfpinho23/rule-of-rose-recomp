#!/usr/bin/env python3
"""elf-drop-needed.py FILE NAME... — remove DT_NEEDED entries from a 64-bit
little-endian ELF file (what `patchelf --remove-needed` does).

Used by recomp/package.sh on the launcher: Qt's CMake package puts the
system OpenGL libraries on its link line, and with some linkers they stay
recorded as needed although the launcher uses nothing from them; a system
without libOpenGL.so.0 then cannot start it. The entries after a removed one
move up and the table gets one more terminating entry, nothing else in the
file changes. Prints what was removed; exit 0 also when nothing matched."""
import struct, sys

def main(path, names):
    d = bytearray(open(path, 'rb').read())
    if d[:4] != b'\x7fELF' or d[4] != 2 or d[5] != 1: raise SystemExit('%s: not a 64-bit little-endian ELF file' % path)
    shoff, = struct.unpack_from('<Q', d, 0x28); shentsize, shnum = struct.unpack_from('<HH', d, 0x3A)
    dyn = strtab = None
    secs = [struct.unpack_from('<IIQQQQIIQQ', d, shoff + i * shentsize) for i in range(shnum)]
    for s in secs:
        if s[1] == 6: dyn = s; strtab = secs[s[6]]          # SHT_DYNAMIC; sh_link = its string table
    if not dyn: raise SystemExit('%s: no dynamic section' % path)
    off, size = dyn[4], dyn[5]
    ents = [struct.unpack_from('<qQ', d, off + i) for i in range(0, size, 16)]
    def name(v):
        a = strtab[4] + v; return bytes(d[a:d.index(b'\0', a)]).decode()
    keep, gone = [], []
    for tag, val in ents:
        if tag == 1 and name(val) in names: gone.append(name(val))
        else: keep.append((tag, val))
    keep += [(0, 0)] * len(gone)
    for i, (tag, val) in enumerate(keep): struct.pack_into('<qQ', d, off + i * 16, tag, val)
    if gone: open(path, 'wb').write(d)
    print('%s: removed %s' % (path, ', '.join(gone) if gone else 'nothing'))

if __name__ == '__main__':
    main(sys.argv[1], set(sys.argv[2:]))
