#!/usr/bin/env python3
"""eelist.py START END [-o OUT] — annotated EE listing of [START, END) (hex).
Marks jal targets as function starts, drops nops, resolves lui+addiu/ori/load
pairs to absolute addresses and shows the C string there if any."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
from elf import Elf  # noqa: E402
from r5900 import decode  # noqa: E402
import disasm  # noqa: E402

e = Elf(os.path.join(os.path.dirname(os.path.dirname(HERE)), 'build/native/SLES_542.18'))
args = sys.argv[1:]
out = sys.stdout
if '-o' in args:
    k = args.index('-o'); out = open(args[k + 1], 'w'); del args[k:k + 2]
start, end = int(args[0], 16), int(args[1], 16)
code = [s for s in e.loads() if s.flags & 1][0]
segs = e.loads()


def cstr(v):
    for s in segs:
        if s.vaddr <= v < s.vaddr + s.filesz:
            d = e.data
            o = s.offset + v - s.vaddr
            z = d.find(b'\0', o, o + 200)
            if z < 0:
                return None
            b = d[o:z]
            if len(b) >= 3 and all(32 <= c < 127 or c in (9, 10) for c in b):
                return b.decode()
    return None


entries = set()
for a in range(code.vaddr, code.vaddr + code.filesz - 3, 4):
    i = decode(e.word(a), a)
    if i.mn == 'jal':
        entries.add(i.target)
last = {}
for a in range(start, end, 4):
    if a in entries:
        out.write(f'\n; ---- fn_{a:06x}\n'); last = {}
    i = decode(e.word(a), a)
    if i.mn == 'nop':
        continue
    note = ''
    if i.mn == 'lui':
        last[i.rt] = i.uimm << 16
    elif i.mn in ('addiu', 'ori') and i.rs in last:
        v = (last[i.rs] + i.imm) & 0xFFFFFFFF if i.mn == 'addiu' else last[i.rs] | i.uimm
        s = cstr(v)
        note = f'= 0x{v:x}' + (f' {s!r}' if s else '')
        last.pop(i.rt, None)
    elif i.mn in disasm.LOADS and i.rs in last:
        note = f'[0x{(last[i.rs] + i.imm) & 0xFFFFFFFF:x}]'
    out.write(f'{a:06x}: {disasm.fmt(i):34s}{" ; " + note if note else ""}\n')
