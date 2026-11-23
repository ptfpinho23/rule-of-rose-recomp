#!/usr/bin/env python3
"""xref.py STRING|0xADDR — find code that forms the address of a string/datum."""
import sys
from elf import Elf
from r5900 import decode

e = Elf(__file__.rsplit('/', 2)[0] + '/build/native/SLES_542.18')
arg = sys.argv[1]
targets = []
if arg.startswith('0x'):
    targets = [int(arg, 16)]
else:
    for seg in e.loads():
        d = e.data[seg.offset:seg.offset + seg.filesz]
        i = d.find(arg.encode())
        while i >= 0:
            targets.append(seg.vaddr + i)
            i = d.find(arg.encode(), i + 1)
print('targets', [hex(t) for t in targets])
code = [s for s in e.loads() if s.flags & 1][0]
last = {}
for a in range(code.vaddr, code.vaddr + code.filesz - 3, 4):
    i = decode(e.word(a), a)
    if i.mn == 'lui':
        last[i.rt] = (i.uimm << 16, a)
    elif i.mn in ('addiu', 'lw', 'sw', 'lbu', 'lb', 'sb', 'ori') and i.rs in last and a - last[i.rs][1] < 128:
        v = (last[i.rs][0] + i.imm) & 0xFFFFFFFF if i.mn != 'ori' else last[i.rs][0] | i.uimm
        if v in targets:
            print(f'  {a:06x}: {i.mn} -> {v:#x}')
