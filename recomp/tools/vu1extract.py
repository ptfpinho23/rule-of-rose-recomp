#!/usr/bin/env python3
"""vu1extract.py ELF OUT.bin — the VU1 micro program as the game (SLES-54218) uploads it.

The game keeps its VU micro code in the ELF section `.vutext` as a ready-made
VIF packet stream: MPG commands (0x4A, NUM = instructions, IMMEDIATE = load
address in 64-bit instructions) each followed by their code. The function at
0x221000 builds the DMA tag that sends the VU1 part: a REF tag whose address
and length come from two address constants (lui/addiu pairs) in its first
instructions. Only the MPG blocks inside that range reach VU1 (one more block
and the VU0 program follow it in the section and are not part of it).

The output is VU1's 16 KiB micro memory after that upload, untouched words
zero: the image recomp/tools/vubench `gen` turns into the fast path
(vu1_fast.inc), and the one rt/vu.c compares the live micro memory with."""
import os, struct, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from elf import Elf   # recomp/elf.py

UPLOAD_FN = 0x221000      # lui t6 / sd / lui t7 / addiu t6 / addiu t7: start and last byte of the stream

def word(e, va):
    for s in e.segments:
        if s.vaddr <= va < s.vaddr + s.filesz: return struct.unpack_from('<I', e.data, s.offset + va - s.vaddr)[0]
    raise SystemExit('vu1extract: address 0x%x is not in the executable' % va)

def const(e, lui_va, addiu_va, reg):
    a, b = word(e, lui_va), word(e, addiu_va)
    if a >> 26 != 0x0F or (a >> 16) & 31 != reg or b >> 26 != 0x09 or (b >> 16) & 31 != reg or (b >> 21) & 31 != reg:
        raise SystemExit('vu1extract: the code at 0x%x is not the expected upload routine (another build of the game?)' % lui_va)
    lo = b & 0xFFFF
    return ((a & 0xFFFF) << 16) + (lo - 0x10000 if lo & 0x8000 else lo)

def extract(elf_path):
    e = Elf(elf_path)
    start = const(e, UPLOAD_FN, UPLOAD_FN + 0x0C, 14)
    qwc = (const(e, UPLOAD_FN + 0x08, UPLOAD_FN + 0x10, 15) - start) >> 4
    w = [word(e, start + 4 * i) for i in range(qwc * 4)]
    img = bytearray(0x4000); i = 0; blocks = 0
    while i < len(w):
        c = w[i]
        if (c >> 24) & 0x7F != 0x4A: i += 1; continue
        num = (c >> 16) & 0xFF or 256; addr = c & 0xFFFF
        if i + 1 + num * 2 > len(w) or addr + num > 0x800: raise SystemExit('vu1extract: bad MPG block at 0x%x' % (start + i * 4))
        img[addr * 8:(addr + num) * 8] = struct.pack('<%dI' % (num * 2), *w[i + 1:i + 1 + num * 2])
        blocks += 1; i += 1 + num * 2
    if not blocks: raise SystemExit('vu1extract: no MPG blocks found')
    return bytes(img), blocks

if __name__ == '__main__':
    img, n = extract(sys.argv[1])
    open(sys.argv[2], 'wb').write(img)
    print('vu1extract: %d MPG blocks, %d instructions' % (n, sum(1 for k in range(0, len(img), 8) if img[k:k+8] != bytes(8))), file=sys.stderr)
