"""Instruction semantics as C statement templates.

Single source of truth: gen.py instantiates these with constant fields for the
recompiled code, and with runtime field variables for the reference
interpreter. Placeholders: {rs} {rt} {rd} {sa} {imm} {uimm} {addr} and the
COP2 fields {dest} {bc} {fsf} {ftf} {imm5} {imm15}. COP1/COP2 register
aliases: ft = rt, fs = rd, fd = sa.
"""

EA = '(GU({rs}) + (u32)({imm}))'

SEM = {
    'nop': ';', 'sync': ';', 'cache': ';', 'pref': ';',
    # shifts
    'sll':  'SW({rd}, GU({rt}) << {sa});',
    'srl':  'SW({rd}, GU({rt}) >> {sa});',
    'sra':  'SW({rd}, GW({rt}) >> {sa});',
    'sllv': 'SW({rd}, GU({rt}) << (GU({rs}) & 31));',
    'srlv': 'SW({rd}, GU({rt}) >> (GU({rs}) & 31));',
    'srav': 'SW({rd}, GW({rt}) >> (GU({rs}) & 31));',
    'dsllv': 'SD({rd}, GD({rt}) << (GU({rs}) & 63));',
    'dsrlv': 'SD({rd}, GD({rt}) >> (GU({rs}) & 63));',
    'dsrav': 'SD({rd}, GS({rt}) >> (GU({rs}) & 63));',
    'dsll': 'SD({rd}, GD({rt}) << {sa});',
    'dsrl': 'SD({rd}, GD({rt}) >> {sa});',
    'dsra': 'SD({rd}, GS({rt}) >> {sa});',
    'dsll32': 'SD({rd}, GD({rt}) << ({sa} + 32));',
    'dsrl32': 'SD({rd}, GD({rt}) >> ({sa} + 32));',
    'dsra32': 'SD({rd}, GS({rt}) >> ({sa} + 32));',
    # moves
    'movz': 'if (GD({rt}) == 0) SD({rd}, GD({rs}));',
    'movn': 'if (GD({rt}) != 0) SD({rd}, GD({rs}));',
    'mfhi': 'SD({rd}, c->hi.u64[0]);', 'mthi': 'c->hi.u64[0] = GD({rs});',
    'mflo': 'SD({rd}, c->lo.u64[0]);', 'mtlo': 'c->lo.u64[0] = GD({rs});',
    'mfhi1': 'SD({rd}, c->hi.u64[1]);', 'mthi1': 'c->hi.u64[1] = GD({rs});',
    'mflo1': 'SD({rd}, c->lo.u64[1]);', 'mtlo1': 'c->lo.u64[1] = GD({rs});',
    'mfsa': 'SD({rd}, c->sa);', 'mtsa': 'c->sa = GU({rs});',
    'mtsab': 'c->sa = (GU({rs}) ^ (u32)({imm})) & 15;',
    'mtsah': 'c->sa = ((GU({rs}) ^ (u32)({imm})) & 7) << 1;',
    # multiply / divide
    'mult': 'op_mult(c, 0, {rd}, {rs}, {rt});',
    'multu': 'op_multu(c, 0, {rd}, {rs}, {rt});',
    'mult1': 'op_mult(c, 1, {rd}, {rs}, {rt});',
    'multu1': 'op_multu(c, 1, {rd}, {rs}, {rt});',
    'madd': 'op_madd(c, 0, {rd}, {rs}, {rt});',
    'maddu': 'op_maddu(c, 0, {rd}, {rs}, {rt});',
    'madd1': 'op_madd(c, 1, {rd}, {rs}, {rt});',
    'maddu1': 'op_maddu(c, 1, {rd}, {rs}, {rt});',
    'div': 'op_div(c, 0, {rs}, {rt});', 'divu': 'op_divu(c, 0, {rs}, {rt});',
    'div1': 'op_div(c, 1, {rs}, {rt});', 'divu1': 'op_divu(c, 1, {rs}, {rt});',
    # arithmetic / logic (overflow traps of add/sub/addi are not modelled:
    # compilers only emit them where overflow cannot happen)
    'add': 'SW({rd}, GU({rs}) + GU({rt}));',
    'addu': 'SW({rd}, GU({rs}) + GU({rt}));',
    'sub': 'SW({rd}, GU({rs}) - GU({rt}));',
    'subu': 'SW({rd}, GU({rs}) - GU({rt}));',
    'addi': 'SW({rt}, GU({rs}) + (u32)({imm}));',
    'addiu': 'SW({rt}, GU({rs}) + (u32)({imm}));',
    'dadd': 'SD({rd}, GD({rs}) + GD({rt}));',
    'daddu': 'SD({rd}, GD({rs}) + GD({rt}));',
    'dsub': 'SD({rd}, GD({rs}) - GD({rt}));',
    'dsubu': 'SD({rd}, GD({rs}) - GD({rt}));',
    'daddi': 'SD({rt}, GD({rs}) + (u64)(s64)({imm}));',
    'daddiu': 'SD({rt}, GD({rs}) + (u64)(s64)({imm}));',
    'and': 'SD({rd}, GD({rs}) & GD({rt}));',
    'or': 'SD({rd}, GD({rs}) | GD({rt}));',
    'xor': 'SD({rd}, GD({rs}) ^ GD({rt}));',
    'nor': 'SD({rd}, ~(GD({rs}) | GD({rt})));',
    'andi': 'SD({rt}, GD({rs}) & (u64)({uimm}));',
    'ori': 'SD({rt}, GD({rs}) | (u64)({uimm}));',
    'xori': 'SD({rt}, GD({rs}) ^ (u64)({uimm}));',
    'lui': 'SD({rt}, (s64)(s32)((u32)({uimm}) << 16));',
    'slt': 'SD({rd}, GS({rs}) < GS({rt}));',
    'sltu': 'SD({rd}, GD({rs}) < GD({rt}));',
    'slti': 'SD({rt}, GS({rs}) < (s64)({imm}));',
    'sltiu': 'SD({rt}, GD({rs}) < (u64)(s64)({imm}));',
    # loads / stores
    'lb': f'SD({{rt}}, (s64)(s8)mr8(c, {EA}));',
    'lbu': f'SD({{rt}}, mr8(c, {EA}));',
    'lh': f'SD({{rt}}, (s64)(s16)mr16(c, {EA}));',
    'lhu': f'SD({{rt}}, mr16(c, {EA}));',
    'lw': f'SD({{rt}}, (s64)(s32)mr32(c, {EA}));',
    'lwu': f'SD({{rt}}, mr32(c, {EA}));',
    'ld': f'SD({{rt}}, mr64(c, {EA}));',
    'lq': f'{{{{ r128 t; mr128(c, {EA}, &t); WR128({{rt}}, t); }}}}',
    'sb': f'mw8(c, {EA}, (u8)GU({{rt}}));',
    'sh': f'mw16(c, {EA}, (u16)GU({{rt}}));',
    'sw': f'mw32(c, {EA}, GU({{rt}}));',
    'sd': f'mw64(c, {EA}, GD({{rt}}));',
    'sq': f'mw128(c, {EA}, &c->r[{{rt}}]);',
    'lwl': f'op_lwl(c, {{rt}}, {EA});', 'lwr': f'op_lwr(c, {{rt}}, {EA});',
    'swl': f'op_swl(c, {{rt}}, {EA});', 'swr': f'op_swr(c, {{rt}}, {EA});',
    'ldl': f'op_ldl(c, {{rt}}, {EA});', 'ldr': f'op_ldr(c, {{rt}}, {EA});',
    'sdl': f'op_sdl(c, {{rt}}, {EA});', 'sdr': f'op_sdr(c, {{rt}}, {EA});',
    'lwc1': f'c->fpr[{{rt}}].u = mr32(c, {EA});',
    'swc1': f'mw32(c, {EA}, c->fpr[{{rt}}].u);',
    'lqc2': f'{{{{ r128 t; mr128(c, {EA}, &t); if ({{rt}}) V0->vf[{{rt}}] = t; }}}}',
    'sqc2': f'mw128(c, {EA}, &V0->vf[{{rt}}]);',
    # system
    'syscall': 'c->pc = {addr}; rt_syscall(c);',
    'break': 'rt_break(c, {addr});',
    'tge': 'if (GS({rs}) >= GS({rt})) rt_trap(c, {addr});',
    'tgeu': 'if (GD({rs}) >= GD({rt})) rt_trap(c, {addr});',
    'tlt': 'if (GS({rs}) < GS({rt})) rt_trap(c, {addr});',
    'tltu': 'if (GD({rs}) < GD({rt})) rt_trap(c, {addr});',
    'teq': 'if (GD({rs}) == GD({rt})) rt_trap(c, {addr});',
    'tne': 'if (GD({rs}) != GD({rt})) rt_trap(c, {addr});',
    # COP0
    'mfc0': 'SW({rt}, rt_mfc0(c, {rd}));',
    'mtc0': 'rt_mtc0(c, {rd}, GU({rt}));',
    'ei': 'rt_ei(c);', 'di': 'rt_di(c);',
    'tlbr': ';', 'tlbwi': ';', 'tlbwr': ';', 'tlbp': ';',
    # COP1 (ft = rt, fs = rd, fd = sa)
    'mfc1': 'SW({rt}, c->fpr[{rd}].u);',
    'mtc1': 'c->fpr[{rd}].u = GU({rt});',
    'cfc1': 'SW({rt}, {rd} == 31 ? c->fcr31 : {rd} == 0 ? 0x2E00u : 0u);',
    'ctc1': 'if ({rd} == 31) c->fcr31 = (GU({rt}) & 0x0083C078u) | 0x01000001u;',
    'add.s': '{{ int fs = {rd}, ft = {rt}; c->fpr[{sa}].f = fres(c, FS_ + FT_); }}',
    'sub.s': '{{ int fs = {rd}, ft = {rt}; c->fpr[{sa}].f = fres(c, FS_ - FT_); }}',
    'mul.s': '{{ int fs = {rd}, ft = {rt}; c->fpr[{sa}].f = fres(c, FS_ * FT_); }}',
    'div.s': 'fpu_div(c, {sa}, {rd}, {rt});',
    'sqrt.s': 'fpu_sqrt(c, {sa}, {rt});',
    'rsqrt.s': 'fpu_rsqrt(c, {sa}, {rd}, {rt});',
    'abs.s': 'c->fpr[{sa}].u = c->fpr[{rd}].u & 0x7FFFFFFFu; c->fcr31 &= ~(FCR_O | FCR_U);',
    'mov.s': 'c->fpr[{sa}].u = c->fpr[{rd}].u;',
    'neg.s': 'c->fpr[{sa}].u = c->fpr[{rd}].u ^ 0x80000000u; c->fcr31 &= ~(FCR_O | FCR_U);',
    'adda.s': '{{ int fs = {rd}, ft = {rt}; c->facc.f = fres(c, FS_ + FT_); }}',
    'suba.s': '{{ int fs = {rd}, ft = {rt}; c->facc.f = fres(c, FS_ - FT_); }}',
    'mula.s': '{{ int fs = {rd}, ft = {rt}; c->facc.f = fres(c, FS_ * FT_); }}',
    'madd.s': '{{ int fs = {rd}, ft = {rt}; c->fpr[{sa}].f = fres(c, FACC_ + fclamp(FS_ * FT_)); }}',
    'msub.s': '{{ int fs = {rd}, ft = {rt}; c->fpr[{sa}].f = fres(c, FACC_ - fclamp(FS_ * FT_)); }}',
    'madda.s': '{{ int fs = {rd}, ft = {rt}; c->facc.f = fres(c, FACC_ + fclamp(FS_ * FT_)); }}',
    'msuba.s': '{{ int fs = {rd}, ft = {rt}; c->facc.f = fres(c, FACC_ - fclamp(FS_ * FT_)); }}',
    'cvt.w.s': 'c->fpr[{sa}].s = fpu_cvtw(c->fpr[{rd}]);',
    'cvt.s.w': 'c->fpr[{sa}].f = (float)c->fpr[{rd}].s;',
    'max.s': '{{ int fs = {rd}, ft = {rt}; float a = FS_, b = FT_; c->fpr[{sa}].f = a > b ? a : b; c->fcr31 &= ~(FCR_O | FCR_U); }}',
    'min.s': '{{ int fs = {rd}, ft = {rt}; float a = FS_, b = FT_; c->fpr[{sa}].f = a < b ? a : b; c->fcr31 &= ~(FCR_O | FCR_U); }}',
    'c.f.s': 'c->fcr31 &= ~FCR_C;',
    'c.eq.s': '{{ int fs = {rd}, ft = {rt}; FCMP(FS_ == FT_); }}',
    'c.lt.s': '{{ int fs = {rd}, ft = {rt}; FCMP(FS_ < FT_); }}',
    'c.le.s': '{{ int fs = {rd}, ft = {rt}; FCMP(FS_ <= FT_); }}',
    # MMI
    'plzcw': 'mmi_plzcw(c, {rd}, {rs});',
    'qfsrv': 'mmi_qfsrv(c, {rd}, {rs}, {rt});',
    'pmfhi': 'WR128({rd}, c->hi);', 'pmflo': 'WR128({rd}, c->lo);',
    'pmthi': 'c->hi = c->r[{rs}];', 'pmtlo': 'c->lo = c->r[{rs}];',
    # COP2 transfers
    'qmfc2': 'WR128({rt}, V0->vf[{rd}]);',
    'qmtc2': 'if ({rd}) V0->vf[{rd}] = c->r[{rt}];',
    'cfc2': 'SW({rt}, vu0_cfc2(V0, {rd}));',
    'ctc2': 'vu0_ctc2(V0, {rd}, GU({rt}));',
    # COP2 macro ops (ft = rt, fs = rd, fd = sa)
    'vnop': ';', 'vwaitq': ';',
    'vopmula': 'vu_opmula(V0, {rd}, {rt});',
    'vopmsub': 'vu_opmsub(V0, {sa}, {rd}, {rt});',
    'vmove': 'vu_move(V0, {dest}, {rt}, {rd});',
    'vmr32': 'vu_mr32(V0, {dest}, {rt}, {rd});',
    'vabs': 'vu_abs(V0, {dest}, {rt}, {rd});',
    'vclip': 'vu_clip(V0, {rd}, {rt});',
    'vdiv': 'vu_div(V0, {rd}, {fsf}, {rt}, {ftf});',
    'vsqrt': 'vu_sqrt(V0, {rt}, {ftf});',
    'vrsqrt': 'vu_rsqrt(V0, {rd}, {fsf}, {rt}, {ftf});',
    'vlqi': 'vu_lqi(V0, {dest}, {rt}, {rd});',
    'vsqi': 'vu_sqi(V0, {dest}, {rd}, {rt});',
    'vlqd': 'vu_lqd(V0, {dest}, {rt}, {rd});',
    'vsqd': 'vu_sqd(V0, {dest}, {rd}, {rt});',
    'viadd': 'VI_SET(V0, {sa}, V0->vi[{rd}] + V0->vi[{rt}]);',
    'visub': 'VI_SET(V0, {sa}, V0->vi[{rd}] - V0->vi[{rt}]);',
    'viand': 'VI_SET(V0, {sa}, V0->vi[{rd}] & V0->vi[{rt}]);',
    'vior': 'VI_SET(V0, {sa}, V0->vi[{rd}] | V0->vi[{rt}]);',
    'viaddi': 'VI_SET(V0, {rt}, V0->vi[{rd}] + (u32)((s32)(({imm5}) << 27) >> 27));',
    'vmtir': 'VI_SET(V0, {rt}, V0->vf[{rd}].u32[{fsf}]);',
    'vmfir': 'if ({rt}) {{ s32 t = (s16)V0->vi[{rd}]; for (int i = 0; i < 4; i++) if (({dest}) & (8u >> i)) V0->vf[{rt}].s32[i] = t; }}',
    'vcallms': 'c->pc = {addr}; vu0_callms(c, ({imm15}) * 8);',
    'vcallmsr': 'c->pc = {addr}; vu0_callms(c, V0->cmsar0 * 8);',
}

# element-wise MMI: rd, rs, rt
for _n in ('paddw psubw pcgtw pceqw pmaxw pminw paddh psubh pcgth pceqh pmaxh '
           'pminh paddb psubb pcgtb pceqb paddsw psubsw paddsh psubsh paddsb '
           'psubsb padduw psubuw padduh psubuh paddub psubub pabsw pabsh '
           'padsbh pand por pxor pnor pextlw pextuw pextlh pextuh pextlb '
           'pextub ppacw ppach ppacb pext5 ppac5 pcpyld pcpyud pcpyh pinth '
           'pinteh pexeh prevh pexch pexew pexcw prot3w psllvw psrlvw '
           'psravw').split():
    SEM[_n] = f'mmi_{_n}(c, {{rd}}, {{rs}}, {{rt}});'
for _n in 'psllh psrlh psrah psllw psrlw psraw'.split():
    SEM[_n] = f'mmi_{_n}(c, {{rd}}, {{rt}}, {{sa}});'

# VU float arithmetic families
_OPS = {'add': 'VUOP_ADD', 'sub': 'VUOP_SUB', 'mul': 'VUOP_MUL',
        'madd': 'VUOP_MADD', 'msub': 'VUOP_MSUB', 'max': 'VUOP_MAX',
        'mini': 'VUOP_MINI'}
for _k, _op in _OPS.items():
    SEM[f'v{_k}'] = f'VU_ARITH({_op}, {{sa}}, {{dest}}, {{rd}}, {{rt}});'
    SEM[f'v{_k}bc'] = (f'VU_ARITH_S({_op}, {{sa}}, {{dest}}, {{rd}}, '
                       f'V0->vf[{{rt}}].f[{{bc}}]);')
    SEM[f'v{_k}i'] = f'VU_ARITH_S({_op}, {{sa}}, {{dest}}, {{rd}}, V0->i.f);'
    if _k not in ('max', 'mini'):
        SEM[f'v{_k}q'] = f'VU_ARITH_S({_op}, {{sa}}, {{dest}}, {{rd}}, V0->q.f);'
        SEM[f'v{_k}a'] = f'VU_ARITHA({_op}, {{dest}}, {{rd}}, {{rt}});'
        SEM[f'v{_k}abc'] = (f'VU_ARITHA_S({_op}, {{dest}}, {{rd}}, '
                            f'V0->vf[{{rt}}].f[{{bc}}]);')
        SEM[f'v{_k}aq'] = f'VU_ARITHA_S({_op}, {{dest}}, {{rd}}, V0->q.f);'
        SEM[f'v{_k}ai'] = f'VU_ARITHA_S({_op}, {{dest}}, {{rd}}, V0->i.f);'
for _n, _s in ((0, '1.0f'), (4, '16.0f'), (12, '4096.0f'), (15, '32768.0f')):
    SEM[f'vftoi{_n}'] = f'vu_ftoin(V0, {{dest}}, {{rt}}, {{rd}}, {_s});'
    SEM[f'vitof{_n}'] = f'vu_itofn(V0, {{dest}}, {{rt}}, {{rd}}, 1.0f / {_s});'

# Branch conditions (C expressions).
COND = {
    'beq': 'GD({rs}) == GD({rt})', 'bne': 'GD({rs}) != GD({rt})',
    'blez': 'GS({rs}) <= 0', 'bgtz': 'GS({rs}) > 0',
    'bltz': 'GS({rs}) < 0', 'bgez': 'GS({rs}) >= 0',
    'bc0f': '!rt_cpcond0(c)', 'bc0t': 'rt_cpcond0(c)',
    'bc1f': '!(c->fcr31 & FCR_C)', 'bc1t': '(c->fcr31 & FCR_C) != 0',
}
for _b, _base in (('beql', 'beq'), ('bnel', 'bne'), ('blezl', 'blez'),
                  ('bgtzl', 'bgtz'), ('bltzl', 'bltz'), ('bgezl', 'bgez'),
                  ('bltzal', 'bltz'), ('bgezal', 'bgez'),
                  ('bltzall', 'bltz'), ('bgezall', 'bgez'),
                  ('bc0fl', 'bc0f'), ('bc0tl', 'bc0t'),
                  ('bc1fl', 'bc1f'), ('bc1tl', 'bc1t')):
    COND[_b] = COND[_base]


def fields(ins, as_vars=False):
    """Field substitution dict for an instruction (constants), or for the
    interpreter (variable names)."""
    if as_vars:
        return dict(rs='rs', rt='rt', rd='rd', sa='sa', imm='imm',
                    uimm='((u32)(u16)imm)', addr='pc', dest='dest', bc='bc',
                    fsf='fsf', ftf='ftf', imm5='sa', imm15='imm15')
    return dict(rs=ins.rs, rt=ins.rt, rd=ins.rd, sa=ins.sa, imm=ins.imm,
                uimm=f'0x{ins.uimm:x}u', addr=f'0x{ins.addr:x}u',
                dest=ins.dest, bc=ins.bc, fsf=ins.fsf, ftf=ins.ftf,
                imm5=ins.imm5, imm15=ins.imm15)
