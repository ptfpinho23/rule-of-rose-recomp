"""R5900 (PS2 Emotion Engine) instruction decoder.

Table-driven, covers the full EE user-mode instruction set:
MIPS III core, R5900-specific (MMI, 3-operand mult, sa register, lq/sq),
COP0, COP1 (single float only) and COP2 (VU0 macro mode).

decode(word, addr) -> Ins.  Ins.mn is None for an undefined encoding.
"""
from dataclasses import dataclass


@dataclass(slots=True)
class Ins:
    addr: int
    word: int
    mn: str | None      # mnemonic, lower case; None = invalid
    rs: int
    rt: int
    rd: int
    sa: int
    imm: int            # sign-extended 16-bit immediate
    target: int = 0     # absolute branch / jump target (when applicable)

    @property
    def uimm(self):
        return self.word & 0xFFFF

    # COP1 aliases
    @property
    def ft(self): return self.rt
    @property
    def fs(self): return self.rd
    @property
    def fd(self): return self.sa
    # COP2 fields
    @property
    def dest(self): return (self.word >> 21) & 0xF
    @property
    def bc(self): return self.word & 3
    @property
    def fsf(self): return (self.word >> 21) & 3
    @property
    def ftf(self): return (self.word >> 23) & 3
    @property
    def imm5(self): return (self.word >> 6) & 0x1F
    @property
    def imm15(self): return (self.word >> 6) & 0x7FFF


def _tab(n, d):
    t = [None] * n
    for k, v in d.items():
        t[k] = v
    return t


OPCODE = _tab(64, {
    2: 'j', 3: 'jal', 4: 'beq', 5: 'bne', 6: 'blez', 7: 'bgtz',
    8: 'addi', 9: 'addiu', 10: 'slti', 11: 'sltiu', 12: 'andi', 13: 'ori',
    14: 'xori', 15: 'lui', 20: 'beql', 21: 'bnel', 22: 'blezl', 23: 'bgtzl',
    24: 'daddi', 25: 'daddiu', 26: 'ldl', 27: 'ldr', 30: 'lq', 31: 'sq',
    32: 'lb', 33: 'lh', 34: 'lwl', 35: 'lw', 36: 'lbu', 37: 'lhu', 38: 'lwr',
    39: 'lwu', 40: 'sb', 41: 'sh', 42: 'swl', 43: 'sw', 44: 'sdl', 45: 'sdr',
    46: 'swr', 47: 'cache', 49: 'lwc1', 51: 'pref', 54: 'lqc2', 55: 'ld',
    57: 'swc1', 62: 'sqc2', 63: 'sd',
})

SPECIAL = _tab(64, {
    0: 'sll', 2: 'srl', 3: 'sra', 4: 'sllv', 6: 'srlv', 7: 'srav',
    8: 'jr', 9: 'jalr', 10: 'movz', 11: 'movn', 12: 'syscall', 13: 'break',
    15: 'sync', 16: 'mfhi', 17: 'mthi', 18: 'mflo', 19: 'mtlo',
    20: 'dsllv', 22: 'dsrlv', 23: 'dsrav', 24: 'mult', 25: 'multu',
    26: 'div', 27: 'divu', 32: 'add', 33: 'addu', 34: 'sub', 35: 'subu',
    36: 'and', 37: 'or', 38: 'xor', 39: 'nor', 40: 'mfsa', 41: 'mtsa',
    42: 'slt', 43: 'sltu', 44: 'dadd', 45: 'daddu', 46: 'dsub', 47: 'dsubu',
    48: 'tge', 49: 'tgeu', 50: 'tlt', 51: 'tltu', 52: 'teq', 54: 'tne',
    56: 'dsll', 58: 'dsrl', 59: 'dsra', 60: 'dsll32', 62: 'dsrl32',
    63: 'dsra32',
})

REGIMM = _tab(32, {
    0: 'bltz', 1: 'bgez', 2: 'bltzl', 3: 'bgezl', 8: 'tgei', 9: 'tgeiu',
    10: 'tlti', 11: 'tltiu', 12: 'teqi', 14: 'tnei', 16: 'bltzal',
    17: 'bgezal', 18: 'bltzall', 19: 'bgezall', 24: 'mtsab', 25: 'mtsah',
})

MMI = _tab(64, {
    0: 'madd', 1: 'maddu', 4: 'plzcw', 16: 'mfhi1', 17: 'mthi1', 18: 'mflo1',
    19: 'mtlo1', 24: 'mult1', 25: 'multu1', 26: 'div1', 27: 'divu1',
    32: 'madd1', 33: 'maddu1', 48: 'pmfhl', 49: 'pmthl', 52: 'psllh',
    54: 'psrlh', 55: 'psrah', 60: 'psllw', 62: 'psrlw', 63: 'psraw',
})
MMI0 = _tab(32, {
    0: 'paddw', 1: 'psubw', 2: 'pcgtw', 3: 'pmaxw', 4: 'paddh', 5: 'psubh',
    6: 'pcgth', 7: 'pmaxh', 8: 'paddb', 9: 'psubb', 10: 'pcgtb',
    16: 'paddsw', 17: 'psubsw', 18: 'pextlw', 19: 'ppacw', 20: 'paddsh',
    21: 'psubsh', 22: 'pextlh', 23: 'ppach', 24: 'paddsb', 25: 'psubsb',
    26: 'pextlb', 27: 'ppacb', 30: 'pext5', 31: 'ppac5',
})
MMI1 = _tab(32, {
    1: 'pabsw', 2: 'pceqw', 3: 'pminw', 4: 'padsbh', 5: 'pabsh', 6: 'pceqh',
    7: 'pminh', 10: 'pceqb', 16: 'padduw', 17: 'psubuw', 18: 'pextuw',
    20: 'padduh', 21: 'psubuh', 22: 'pextuh', 24: 'paddub', 25: 'psubub',
    26: 'pextub', 27: 'qfsrv',
})
MMI2 = _tab(32, {
    0: 'pmaddw', 2: 'psllvw', 3: 'psrlvw', 4: 'pmsubw', 8: 'pmfhi',
    9: 'pmflo', 10: 'pinth', 12: 'pmultw', 13: 'pdivw', 14: 'pcpyld',
    16: 'pmaddh', 17: 'phmadh', 18: 'pand', 19: 'pxor', 20: 'pmsubh',
    21: 'phmsbh', 26: 'pexeh', 27: 'prevh', 28: 'pmulth', 29: 'pdivbw',
    30: 'pexew', 31: 'prot3w',
})
MMI3 = _tab(32, {
    0: 'pmadduw', 3: 'psravw', 8: 'pmthi', 9: 'pmtlo', 10: 'pinteh',
    12: 'pmultuw', 13: 'pdivuw', 14: 'pcpyud', 18: 'por', 19: 'pnor',
    26: 'pexch', 27: 'pcpyh', 30: 'pexcw',
})

COP1_S = _tab(64, {
    0: 'add.s', 1: 'sub.s', 2: 'mul.s', 3: 'div.s', 4: 'sqrt.s', 5: 'abs.s',
    6: 'mov.s', 7: 'neg.s', 22: 'rsqrt.s', 24: 'adda.s', 25: 'suba.s',
    26: 'mula.s', 28: 'madd.s', 29: 'msub.s', 30: 'madda.s', 31: 'msuba.s',
    36: 'cvt.w.s', 40: 'max.s', 41: 'min.s', 48: 'c.f.s', 50: 'c.eq.s',
    52: 'c.lt.s', 54: 'c.le.s',
})

_BC = 'xyzw'
VU_S1 = {}
for _i, _n in enumerate(['vaddbc', 'vsubbc', 'vmaddbc', 'vmsubbc', 'vmaxbc',
                         'vminibc', 'vmulbc']):
    for _b in range(4):
        VU_S1[_i * 4 + _b] = _n
VU_S1.update({
    28: 'vmulq', 29: 'vmaxi', 30: 'vmuli', 31: 'vminii', 32: 'vaddq',
    33: 'vmaddq', 34: 'vaddi', 35: 'vmaddi', 36: 'vsubq', 37: 'vmsubq',
    38: 'vsubi', 39: 'vmsubi', 40: 'vadd', 41: 'vmadd', 42: 'vmul',
    43: 'vmax', 44: 'vsub', 45: 'vmsub', 46: 'vopmsub', 47: 'vmini',
    48: 'viadd', 49: 'visub', 50: 'viaddi', 52: 'viand', 53: 'vior',
    56: 'vcallms', 57: 'vcallmsr',
})
VU_S2 = {}
for _i, _n in ((0, 'vaddabc'), (4, 'vsubabc'), (8, 'vmaddabc'),
               (12, 'vmsubabc'), (24, 'vmulabc')):
    for _b in range(4):
        VU_S2[_i + _b] = _n
VU_S2.update({
    16: 'vitof0', 17: 'vitof4', 18: 'vitof12', 19: 'vitof15', 20: 'vftoi0',
    21: 'vftoi4', 22: 'vftoi12', 23: 'vftoi15', 28: 'vmulaq', 29: 'vabs',
    30: 'vmulai', 31: 'vclip', 32: 'vaddaq', 33: 'vmaddaq', 34: 'vaddai',
    35: 'vmaddai', 36: 'vsubaq', 37: 'vmsubaq', 38: 'vsubai', 39: 'vmsubai',
    40: 'vadda', 41: 'vmadda', 42: 'vmula', 44: 'vsuba', 45: 'vmsuba',
    46: 'vopmula', 47: 'vnop', 48: 'vmove', 49: 'vmr32', 52: 'vlqi',
    53: 'vsqi', 54: 'vlqd', 55: 'vsqd', 56: 'vdiv', 57: 'vsqrt',
    58: 'vrsqrt', 59: 'vwaitq', 60: 'vmtir', 61: 'vmfir', 62: 'vilwr',
    63: 'viswr', 64: 'vrnext', 65: 'vrget', 66: 'vrinit', 67: 'vrxor',
})

BRANCHES = {   # conditional branches -> is_likely
    'beq': 0, 'bne': 0, 'blez': 0, 'bgtz': 0, 'bltz': 0, 'bgez': 0,
    'beql': 1, 'bnel': 1, 'blezl': 1, 'bgtzl': 1, 'bltzl': 1, 'bgezl': 1,
    'bltzal': 0, 'bgezal': 0, 'bltzall': 1, 'bgezall': 1,
    'bc0f': 0, 'bc0t': 0, 'bc0fl': 1, 'bc0tl': 1,
    'bc1f': 0, 'bc1t': 0, 'bc1fl': 1, 'bc1tl': 1,
    'bc2f': 0, 'bc2t': 0, 'bc2fl': 1, 'bc2tl': 1,
}
JUMPS = {'j', 'jal', 'jr', 'jalr'}
LINKING = {'jal', 'jalr', 'bltzal', 'bgezal', 'bltzall', 'bgezall'}


def has_delay_slot(mn):
    return mn in BRANCHES or mn in JUMPS


def decode(word: int, addr: int) -> Ins:
    op = word >> 26
    rs = (word >> 21) & 31
    rt = (word >> 16) & 31
    rd = (word >> 11) & 31
    sa = (word >> 6) & 31
    fn = word & 63
    imm = word & 0xFFFF
    if imm & 0x8000:
        imm -= 0x10000
    ins = Ins(addr, word, None, rs, rt, rd, sa, imm)
    mn = None
    if op == 0:
        mn = SPECIAL[fn]
        if word == 0:
            mn = 'nop'
    elif op == 1:
        mn = REGIMM[rt]
    elif op == 16:
        if rs == 0:
            mn = 'mfc0'
        elif rs == 4:
            mn = 'mtc0'
        elif rs == 8:
            mn = ('bc0f', 'bc0t', 'bc0fl', 'bc0tl')[rt] if rt < 4 else None
        elif rs == 16:
            mn = {1: 'tlbr', 2: 'tlbwi', 6: 'tlbwr', 8: 'tlbp', 24: 'eret',
                  56: 'ei', 57: 'di'}.get(fn)
    elif op == 17:
        if rs == 0:
            mn = 'mfc1'
        elif rs == 2:
            mn = 'cfc1'
        elif rs == 4:
            mn = 'mtc1'
        elif rs == 6:
            mn = 'ctc1'
        elif rs == 8:
            mn = ('bc1f', 'bc1t', 'bc1fl', 'bc1tl')[rt] if rt < 4 else None
        elif rs == 16:
            mn = COP1_S[fn]
        elif rs == 20:
            mn = 'cvt.s.w' if fn == 32 else None
    elif op == 18:
        if rs & 16:
            if fn >= 60:
                mn = VU_S2.get((((word >> 6) & 0x1F) << 2) | (word & 3))
            else:
                mn = VU_S1.get(fn)
        elif rs == 1:
            mn = 'qmfc2'
        elif rs == 2:
            mn = 'cfc2'
        elif rs == 5:
            mn = 'qmtc2'
        elif rs == 6:
            mn = 'ctc2'
        elif rs == 8:
            mn = ('bc2f', 'bc2t', 'bc2fl', 'bc2tl')[rt] if rt < 4 else None
    elif op == 28:
        if fn == 8:
            mn = MMI0[sa]
        elif fn == 40:
            mn = MMI1[sa]
        elif fn == 9:
            mn = MMI2[sa]
        elif fn == 41:
            mn = MMI3[sa]
        else:
            mn = MMI[fn]
    else:
        mn = OPCODE[op]
    ins.mn = mn
    if mn in BRANCHES:
        ins.target = (addr + 4 + (imm << 2)) & 0xFFFFFFFF
    elif mn in ('j', 'jal'):
        ins.target = ((addr + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
    return ins
