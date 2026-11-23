#!/usr/bin/env python3
"""ee_calls.py HEXADDR — list every `jal HEXADDR` in the EE ELF, with the
enclosing function start (nearest preceding jal target / `addiu sp,sp,-N`)
and any constant loaded into a0..a3 in the 12 instructions before the call
(plus the delay slot)."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
from elf import Elf  # noqa: E402
from r5900 import decode  # noqa: E402

e = Elf(os.path.join(os.path.dirname(os.path.dirname(HERE)), 'build/native/SLES_542.18'))
target = int(sys.argv[1], 16)
code = [s for s in e.loads() if s.flags & 1][0]
lo, hi = code.vaddr, code.vaddr + code.filesz
ins = {}
entries = set()
for a in range(lo, hi - 3, 4):
    i = decode(e.word(a), a)
    ins[a] = i
    if i.mn == 'jal':
        entries.add(i.target)
for a in range(lo, hi - 3, 4):
    i = ins[a]
    if i.mn != 'jal' or i.target != target:
        continue
    consts = {}
    for b in list(range(a - 48, a, 4)) + [a + 4]:
        j = ins.get(b)
        if j is None or j.mn is None:
            continue
        if j.mn in ('addiu', 'ori') and j.rs == 0 and 4 <= j.rt <= 8:
            consts[j.rt] = j.imm if j.mn == 'addiu' else j.uimm
        elif j.mn in ('addu', 'daddu', 'or') and j.rd in consts:
            del consts[j.rd]
    f = a
    while f > lo and f not in entries:
        f -= 4
    names = {4: 'a0', 5: 'a1', 6: 'a2', 7: 'a3', 8: 't0'}
    print(f'{a:06x} in fn {f:06x}: ' + ' '.join(f'{names[k]}={v}' for k, v in sorted(consts.items())))
