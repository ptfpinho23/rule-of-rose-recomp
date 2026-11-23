#!/usr/bin/env python3
"""dis.py ADDR [COUNT] — disassemble game code (R5900), for analysis."""
import sys
from elf import Elf
from r5900 import decode, BRANCHES

R = ('zero at v0 v1 a0 a1 a2 a3 t0 t1 t2 t3 t4 t5 t6 t7 s0 s1 s2 s3 s4 s5 s6 '
     's7 t8 t9 k0 k1 gp sp fp ra').split()
LOADS = set('lb lbu lh lhu lw lwu ld lq sb sh sw sd sq lwl lwr swl swr ldl ldr '
            'sdl sdr'.split())


def fmt(i):
    m = i.mn
    if m is None:
        return f'.word 0x{i.word:08x}'
    if m in BRANCHES:
        ops = {'beq': 2, 'bne': 2, 'beql': 2, 'bnel': 2}.get(m, 1)
        if m.startswith('bc'):
            return f'{m} 0x{i.target:x}'
        regs = f'{R[i.rs]}, {R[i.rt]}' if ops == 2 else R[i.rs]
        return f'{m} {regs}, 0x{i.target:x}'
    if m in ('j', 'jal'):
        return f'{m} 0x{i.target:x}'
    if m == 'jr':
        return f'jr {R[i.rs]}'
    if m == 'jalr':
        return f'jalr {R[i.rs]}' + ('' if i.rd == 31 else f', {R[i.rd]}')
    if m in LOADS:
        return f'{m} {R[i.rt]}, {i.imm}({R[i.rs]})'
    if m in ('lwc1', 'swc1'):
        return f'{m} f{i.rt}, {i.imm}({R[i.rs]})'
    if m in ('lqc2', 'sqc2'):
        return f'{m} vf{i.rt}, {i.imm}({R[i.rs]})'
    if m in ('addiu', 'addi', 'daddiu', 'daddi', 'slti', 'sltiu'):
        return f'{m} {R[i.rt]}, {R[i.rs]}, {i.imm}'
    if m in ('andi', 'ori', 'xori'):
        return f'{m} {R[i.rt]}, {R[i.rs]}, 0x{i.uimm:x}'
    if m == 'lui':
        return f'lui {R[i.rt]}, 0x{i.uimm:x}'
    if m in ('sll', 'srl', 'sra', 'dsll', 'dsrl', 'dsra', 'dsll32', 'dsrl32', 'dsra32'):
        return f'{m} {R[i.rd]}, {R[i.rt]}, {i.sa}'
    if m in ('mfc0', 'mtc0'):
        return f'{m} {R[i.rt]}, ${i.rd}'
    if m in ('mfc1', 'mtc1', 'cfc1', 'ctc1'):
        return f'{m} {R[i.rt]}, f{i.rd}'
    if m.endswith('.s') or m.endswith('.w'):
        return f'{m} f{i.sa}, f{i.rd}, f{i.rt}'
    if m in ('syscall', 'break', 'nop', 'sync', 'ei', 'di', 'eret'):
        return m
    if m.startswith('v') or m in ('qmfc2', 'qmtc2', 'cfc2', 'ctc2'):
        return f'{m} [w={i.word:08x} rt={i.rt} rd/fs={i.rd} fd={i.sa} dest={i.dest:x}]'
    if m in ('mfhi', 'mflo', 'mfhi1', 'mflo1', 'mfsa'):
        return f'{m} {R[i.rd]}'
    if m in ('mthi', 'mtlo', 'mthi1', 'mtlo1', 'mtsa'):
        return f'{m} {R[i.rs]}'
    return f'{m} {R[i.rd]}, {R[i.rs]}, {R[i.rt]}'


def main():
    e = Elf(__file__.rsplit('/', 2)[0] + '/build/native/SLES_542.18')
    a = int(sys.argv[1], 16)
    n = int(sys.argv[2], 0) if len(sys.argv) > 2 else 32
    for k in range(n):
        i = decode(e.word(a + 4 * k), a + 4 * k)
        print(f'{i.addr:06x}: {i.word:08x}  {fmt(i)}')


if __name__ == '__main__':
    main()
