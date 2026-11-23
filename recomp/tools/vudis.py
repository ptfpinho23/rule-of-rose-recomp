#!/usr/bin/env python3
"""vudis.py — VU micro-code disassembler.

  vudis.py DUMP [START [END]]      dump = vu1_NNNN.bin written by ROR_VUDUMP
  vudis.py --raw FILE [START [END]]  raw micro memory (64-bit pairs, lower first)
  vudis.py --mem DUMP [FROM [N]]   print VU data memory quadwords (addr in qw)
  vudis.py --regs DUMP

Addresses are byte offsets in micro memory (pc); branch targets likewise.
"""
import struct
import sys

HDR = 16 + 32 * 16 + 16 * 4 + 16 + 32
XYZW = 'xyzw'


def dst(d):
    return '.' + ''.join(c for i, c in enumerate(XYZW) if d & (8 >> i)) if d != 15 else ''


def upper(w):
    d = (w >> 21) & 15
    ft, fs, fd, bc = (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 3
    D = dst(d)
    if (w & 0x3C) != 0x3C:
        op = w & 63
        if op < 28:
            n = ['ADD', 'SUB', 'MADD', 'MSUB', 'MAX', 'MINI', 'MUL'][op >> 2]
            return f'{n}{XYZW[bc]}{D} vf{fd}, vf{fs}, vf{ft}{XYZW[bc]}'
        t = {28: 'MULq', 29: 'MAXi', 30: 'MULi', 31: 'MINIi', 32: 'ADDq', 33: 'MADDq', 34: 'ADDi',
             35: 'MADDi', 36: 'SUBq', 37: 'MSUBq', 38: 'SUBi', 39: 'MSUBi'}
        if op in t:
            return f'{t[op]}{D} vf{fd}, vf{fs}, {t[op][-1].upper()}'
        t = {40: 'ADD', 41: 'MADD', 42: 'MUL', 43: 'MAX', 44: 'SUB', 45: 'MSUB', 46: 'OPMSUB', 47: 'MINI'}
        if op in t:
            return f'{t[op]}{D} vf{fd}, vf{fs}, vf{ft}'
        return f'?upper {w:08x}'
    op = (((w >> 6) & 31) << 2) | (w & 3)
    for base, n in ((0, 'ADDA'), (4, 'SUBA'), (8, 'MADDA'), (12, 'MSUBA'), (24, 'MULA')):
        if base <= op < base + 4:
            return f'{n}{XYZW[bc]}{D} ACC, vf{fs}, vf{ft}{XYZW[bc]}'
    t = {16: 'ITOF0', 17: 'ITOF4', 18: 'ITOF12', 19: 'ITOF15', 20: 'FTOI0', 21: 'FTOI4', 22: 'FTOI12',
         23: 'FTOI15', 29: 'ABS'}
    if op in t:
        return f'{t[op]}{D} vf{ft}, vf{fs}'
    t = {28: 'MULAq', 30: 'MULAi', 32: 'ADDAq', 33: 'MADDAq', 34: 'ADDAi', 35: 'MADDAi', 36: 'SUBAq',
         37: 'MSUBAq', 38: 'SUBAi', 39: 'MSUBAi'}
    if op in t:
        return f'{t[op]}{D} ACC, vf{fs}, {t[op][-1].upper()}'
    t = {40: 'ADDA', 41: 'MADDA', 42: 'MULA', 44: 'SUBA', 45: 'MSUBA', 46: 'OPMULA'}
    if op in t:
        return f'{t[op]}{D} ACC, vf{fs}, vf{ft}'
    if op == 31:
        return f'CLIP vf{fs}, vf{ft}w'
    if op == 47:
        return 'NOP'
    return f'?upper {w:08x}'


def sx11(w):
    v = w & 0x7FF
    return v - 0x800 if v & 0x400 else v


def lower(w, pc):
    op = w >> 25
    d = (w >> 21) & 15
    it, is_, id_ = (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    imm11 = sx11(w)
    imm15 = ((w >> 10) & 0x7800) | (w & 0x7FF)
    imm24 = w & 0xFFFFFF
    imm12 = ((w >> 10) & 0x800) | (w & 0x7FF)
    D = dst(d)
    tgt = (pc + 8 + imm11 * 8) & 0x3FFF
    if op == 0x00: return f'LQ{D} vf{it}, {imm11}(vi{is_})'
    if op == 0x01: return f'SQ{D} vf{is_}, {imm11}(vi{it})'
    if op == 0x04: return f'ILW{D} vi{it}, {imm11}(vi{is_})'
    if op == 0x05: return f'ISW{D} vi{it}, {imm11}(vi{is_})'
    if op == 0x08: return f'IADDIU vi{it}, vi{is_}, 0x{imm15:x}'
    if op == 0x09: return f'ISUBIU vi{it}, vi{is_}, 0x{imm15:x}'
    if op == 0x10: return f'FCEQ vi1, 0x{imm24:06x}'
    if op == 0x11: return f'FCSET 0x{imm24:06x}'
    if op == 0x12: return f'FCAND vi1, 0x{imm24:06x}'
    if op == 0x13: return f'FCOR vi1, 0x{imm24:06x}'
    if op == 0x14: return f'FSEQ vi{it}, 0x{imm12:03x}'
    if op == 0x15: return f'FSSET 0x{imm12:03x}'
    if op == 0x16: return f'FSAND vi{it}, 0x{imm12:03x}'
    if op == 0x17: return f'FSOR vi{it}, 0x{imm12:03x}'
    if op == 0x18: return f'FMEQ vi{it}, vi{is_}'
    if op == 0x1A: return f'FMAND vi{it}, vi{is_}'
    if op == 0x1B: return f'FMOR vi{it}, vi{is_}'
    if op == 0x1C: return f'FCGET vi{it}'
    if op == 0x20: return f'B 0x{tgt:04x}'
    if op == 0x21: return f'BAL vi{it}, 0x{tgt:04x}'
    if op == 0x24: return f'JR vi{is_}'
    if op == 0x25: return f'JALR vi{it}, vi{is_}'
    if op == 0x28: return f'IBEQ vi{it}, vi{is_}, 0x{tgt:04x}'
    if op == 0x29: return f'IBNE vi{it}, vi{is_}, 0x{tgt:04x}'
    if op == 0x2C: return f'IBLTZ vi{is_}, 0x{tgt:04x}'
    if op == 0x2D: return f'IBGTZ vi{is_}, 0x{tgt:04x}'
    if op == 0x2E: return f'IBLEZ vi{is_}, 0x{tgt:04x}'
    if op == 0x2F: return f'IBGEZ vi{is_}, 0x{tgt:04x}'
    if op != 0x40:
        return f'?lower {w:08x}'
    fn = w & 63
    if fn == 0x30: return f'IADD vi{id_}, vi{is_}, vi{it}'
    if fn == 0x31: return f'ISUB vi{id_}, vi{is_}, vi{it}'
    if fn == 0x32:
        i5 = id_ - 32 if id_ & 16 else id_
        return f'IADDI vi{it}, vi{is_}, {i5}'
    if fn == 0x34: return f'IAND vi{id_}, vi{is_}, vi{it}'
    if fn == 0x35: return f'IOR vi{id_}, vi{is_}, vi{it}'
    if fn < 0x3C:
        return f'?lower {w:08x}'
    x = (id_ << 2) | (w & 3)
    fsf, ftf = XYZW[(w >> 21) & 3], XYZW[(w >> 23) & 3]
    if x == 48: return f'MOVE{D} vf{it}, vf{is_}' if w != 0x8000033C else 'NOP'
    if x == 49: return f'MR32{D} vf{it}, vf{is_}'
    if x == 52: return f'LQI{D} vf{it}, (vi{is_}++)'
    if x == 53: return f'SQI{D} vf{is_}, (vi{it}++)'
    if x == 54: return f'LQD{D} vf{it}, (--vi{is_})'
    if x == 55: return f'SQD{D} vf{is_}, (--vi{it})'
    if x == 56: return f'DIV Q, vf{is_}{fsf}, vf{it}{ftf}'
    if x == 57: return f'SQRT Q, vf{it}{ftf}'
    if x == 58: return f'RSQRT Q, vf{is_}{fsf}, vf{it}{ftf}'
    if x == 59: return 'WAITQ'
    if x == 60: return f'MTIR vi{it}, vf{is_}{fsf}'
    if x == 61: return f'MFIR{D} vf{it}, vi{is_}'
    if x == 62: return f'ILWR{D} vi{it}, (vi{is_})'
    if x == 63: return f'ISWR{D} vi{it}, (vi{is_})'
    if x == 64: return f'RNEXT{D} vf{it}, R'
    if x == 65: return f'RGET{D} vf{it}, R'
    if x == 66: return f'RINIT R, vf{is_}{fsf}'
    if x == 67: return f'RXOR R, vf{is_}{fsf}'
    if x == 100: return f'MFP{D} vf{it}, P'
    if x == 104: return f'XTOP vi{it}'
    if x == 105: return f'XITOP vi{it}'
    if x == 108: return f'XGKICK vi{is_}'
    t = {112: 'ESADD', 113: 'ERSADD', 114: 'ELENG', 115: 'ERLENG', 116: 'EATANxy', 117: 'EATANxz', 118: 'ESUM'}
    if x in t: return f'{t[x]} P, vf{is_}'
    t = {120: 'ESQRT', 121: 'ERSQRT', 122: 'ERCPR', 124: 'ESIN', 125: 'EATAN', 126: 'EEXP'}
    if x in t: return f'{t[x]} P, vf{is_}{fsf}'
    if x == 123: return 'WAITP'
    return f'?lower {w:08x}'


def disasm(micro, start, end):
    for pc in range(start, min(end, len(micro)), 8):
        lo, up = struct.unpack_from('<II', micro, pc)
        if lo == 0 and up == 0:
            continue
        flags = ''.join(c for b, c in ((31, 'I'), (30, 'E'), (29, 'M'), (28, 'D'), (27, 'T')) if up >> b & 1)
        u = upper(up & 0x07FFFFFF | (up & 0x07FFFFFF))
        if up >> 31 & 1:
            l = f'LOI {struct.unpack("<f", struct.pack("<I", lo))[0]!r} (0x{lo:08x})'
        else:
            l = lower(lo, pc)
        print(f'{pc:04x}: {up:08x} {lo:08x}  {u:34s} {l}{"   [" + flags + "]" if flags else ""}')


def main():
    a = sys.argv[1:]
    if a and a[0] == '--raw':
        micro = open(a[1], 'rb').read()
        s = int(a[2], 0) if len(a) > 2 else 0
        e = int(a[3], 0) if len(a) > 3 else len(micro)
        disasm(micro, s, e)
        return
    mode = None
    if a and a[0] in ('--mem', '--regs'):
        mode = a.pop(0)
    data = open(a[0], 'rb').read()
    pc, top, itop, _ = struct.unpack_from('<4I', data, 0)
    mem = data[HDR:HDR + 0x4000]
    micro = data[HDR + 0x4000:HDR + 0x8000]
    if mode == '--regs':
        print(f'pc=0x{pc:x} top=0x{top:x} itop=0x{itop:x}')
        for i in range(32):
            u = struct.unpack_from('<4I', data, 16 + i * 16)
            f = struct.unpack_from('<4f', data, 16 + i * 16)
            print(f'vf{i:<2d} ' + ' '.join(f'{x:08x}' for x in u) + '  ' + ' '.join(f'{x:.6g}' for x in f))
        print('vi ' + ' '.join(f'{x:x}' for x in struct.unpack_from('<16I', data, 16 + 512)))
        return
    if mode == '--mem':
        s = int(a[1], 0) if len(a) > 1 else 0
        n = int(a[2], 0) if len(a) > 2 else 0x400 - s
        for q in range(s, min(s + n, 0x400)):
            u = struct.unpack_from('<4I', mem, q * 16)
            f = struct.unpack_from('<4f', mem, q * 16)
            if any(u):
                print(f'{q:03x}: ' + ' '.join(f'{x:08x}' for x in u) + '  ' + ' '.join(f'{x:.6g}' for x in f))
        return
    print(f'; start pc=0x{pc:x} top=0x{top:x} itop=0x{itop:x}')
    s = int(a[1], 0) if len(a) > 1 else 0
    e = int(a[2], 0) if len(a) > 2 else len(micro)
    disasm(micro, s, e)


if __name__ == '__main__':
    main()
