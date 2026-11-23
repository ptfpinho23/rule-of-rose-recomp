#!/usr/bin/env python3
"""gen.py — static recompiler driver: PS2 ELF -> C.

  1. decode every executable section (except VU micro code);
  2. find function entries (call targets, section starts, code pointers in
     data, constants formed in code);
  3. for each entry emit one C function covering the code reachable without
     crossing another entry; transfers to other entries are tail calls;
  4. write the lookup table for indirect jumps and the predecoded
     instruction table + op switch for the reference interpreter.
"""
import os
import struct
import sys

from elf import Elf
from r5900 import decode, BRANCHES, has_delay_slot
from sem import SEM, COND, fields

LIKELY = {m for m, l in BRANCHES.items() if l}
UNSUPPORTED_CTRL = {'bltzal', 'bgezal', 'bltzall', 'bgezall',
                    'bc2f', 'bc2t', 'bc2fl', 'bc2tl'}
FUNCS_PER_FILE_INSNS = 5000


class Program:
    def __init__(self, path):
        self.elf = e = Elf(path)
        self.code = {}          # addr -> Ins
        self.code_ranges = []
        for s in e.sections:
            if not (s.flags & 4) or not s.addr or s.name == '.vutext':
                continue
            self.code_ranges.append((s.addr, s.addr + s.size, s.name))
            for a in range(s.addr, s.addr + s.size - 3, 4):
                self.code[a] = decode(e.word(a), a)
        self.lo = min(r[0] for r in self.code_ranges)
        self.hi = max(r[1] for r in self.code_ranges)
        self.entries = set()
        self.entry_why = {}

    def is_code(self, a):
        return a in self.code

    def add_entry(self, a, why):
        if a in self.code and a not in self.entries:
            self.entries.add(a)
            self.entry_why[a] = why

    def find_entries(self):
        e = self.elf
        self.add_entry(e.entry, 'entry')
        for lo, _hi, name in self.code_ranges:
            if name.startswith('.gnu.linkonce.t.'):
                self.add_entry(lo, 'section')
        for a, i in self.code.items():
            if i.mn == 'jal':
                self.add_entry(i.target, 'jal')
        # code pointers stored in data (vtables, jump tables, callbacks)
        for seg in e.loads():
            if seg.flags & 1:       # executable segment: skip
                continue
            d = e.data[seg.offset:seg.offset + seg.filesz]
            for off in range(0, len(d) - 3, 4):
                v = struct.unpack_from('<I', d, off)[0]
                if not v & 3:
                    self.add_entry(v, 'data')
        # constants formed in code: lui + addiu/ori
        for lo, hi, _name in self.code_ranges:
            known = {}
            for a in range(lo, hi - 3, 4):
                i = self.code[a]
                if i.mn == 'lui':
                    known[i.rt] = (i.uimm << 16) & 0xFFFFFFFF
                elif i.mn in ('addiu', 'ori') and i.rs in known:
                    base = known[i.rs]
                    v = (base + i.imm) & 0xFFFFFFFF if i.mn == 'addiu' \
                        else base | i.uimm
                    known[i.rt] = v
                    if not v & 3:
                        self.add_entry(v, 'const')
                else:
                    known.pop(i.rt, None)
                    known.pop(i.rd, None)

    def walk(self, entry):
        """Addresses of instructions executed as normal (non-delay-slot)
        nodes of the function starting at entry."""
        nodes, work = set(), [entry]
        while work:
            a = work.pop()
            if a in nodes:
                continue
            nodes.add(a)
            i = self.code[a]
            for s in self.succs(i):
                if s in self.code and (s == entry or s not in self.entries):
                    work.append(s)
        return nodes

    def succs(self, i):
        mn, a = i.mn, i.addr
        if mn is None or mn in UNSUPPORTED_CTRL or mn == 'eret':
            return []
        if mn in BRANCHES:
            return [i.target, a + 8]
        if mn == 'j':
            return [i.target]
        if mn in ('jal', 'jalr'):
            return [a + 8]
        if mn == 'jr':
            return []
        return [a + 4]


class Emitter:
    def __init__(self, prog):
        self.p = prog
        self.unhandled = {}

    def stmt(self, i):
        t = SEM.get(i.mn)
        if t is None:
            self.unhandled[i.mn] = self.unhandled.get(i.mn, 0) + 1
            return f'rt_invalid(c, 0x{i.addr:x}u); /* no template: {i.mn} */'
        return t.format(**fields(i))

    def slot(self, a):
        """C for the delay-slot instruction at a."""
        i = self.p.code.get(a)
        if i is None or i.mn is None or has_delay_slot(i.mn):
            return f'rt_invalid(c, 0x{a:x}u);'
        return self.stmt(i)

    def function(self, entry):
        p = self.p
        nodes = p.walk(entry)
        order = sorted(nodes)
        nxt = {a: b for a, b in zip(order, order[1:])}
        labels = set()
        out = []

        def goto(src, t):
            tick = f'TICK({(src - t) // 4 + 2}); ' if t <= src else ''
            if t in nodes:
                labels.add(t)
                return f'{tick}goto L_{t:08x};'
            if t in p.entries:
                return f'{tick}MUSTTAIL return f_{t:08x}(c);'
            return f'c->pc = 0x{t:x}u; rt_invalid(c, 0x{src:x}u); return;'

        def fall(src, n):
            if n in nodes:
                if nxt.get(src) == n:
                    return ''
                labels.add(n)
                return f' goto L_{n:08x};'
            if n in p.entries:
                return f' MUSTTAIL return f_{n:08x}(c);'
            return f' c->pc = 0x{n:x}u; rt_invalid(c, 0x{src:x}u); return;'

        for a in order:
            i = p.code[a]
            mn = i.mn
            ra = a + 8
            if mn is None or mn in UNSUPPORTED_CTRL:
                if mn:
                    self.unhandled[mn] = self.unhandled.get(mn, 0) + 1
                body = f'rt_invalid(c, 0x{a:x}u); return;'
            elif mn in BRANCHES:
                cond = COND[mn].format(**fields(i))
                s = self.slot(a + 4)
                if mn in LIKELY:
                    body = f'if ({cond}) {{ {s} {goto(a, i.target)} }}'
                else:
                    body = (f'{{ int b = {cond}; {s} '
                            f'if (b) {{ {goto(a, i.target)} }} }}')
                body += fall(a, ra)
            elif mn == 'j':
                body = f'{self.slot(a + 4)} {goto(a, i.target)}'
            elif mn == 'jal':
                s = self.slot(a + 4)
                if i.target in p.entries:
                    call = f'TRACE_CALL(0x{a:x}u, 0x{i.target:x}u); f_{i.target:08x}(c);'
                else:
                    call = f'c->pc = 0x{i.target:x}u; rt_invalid(c, 0x{a:x}u);'
                body = (f'c->r[31].u64[0] = 0x{ra:x}u; {s} TICK(8); {call} '
                        f'if (UNLIKELY(c->pc != 0x{ra:x}u)) return;')
                body += fall(a, ra)
            elif mn == 'jalr':
                s = self.slot(a + 4)
                body = (f'{{ u32 t = GU({i.rs}); SD({i.rd}, 0x{ra:x}u); {s} '
                        f'TICK(8); c->pc = t; TRACE_CALL(0x{a:x}u, t); '
                        f'rt_lookup(c, t)(c); '
                        f'if (UNLIKELY(c->pc != 0x{ra:x}u)) return; }}')
                body += fall(a, ra)
            elif mn == 'jr':
                s = self.slot(a + 4)
                if i.rs == 31:
                    body = f'{{ u32 t = GU(31); {s} c->pc = t; return; }}'
                else:
                    body = (f'{{ u32 t = GU({i.rs}); {s} c->pc = t; '
                            f'MUSTTAIL return rt_lookup(c, t)(c); }}')
            elif mn == 'eret':
                # exception return used by SDK code as a jump to EPC/ErrorEPC
                body = f'c->pc = rt_eret(c, 0x{a:x}u); return;'
            else:
                body = self.stmt(i) + fall(a, a + 4)
            out.append((a, body))
        lines = [f'void f_{entry:08x}(Ctx *c) {{', f'    TRACE_FN(0x{entry:x}u);']
        if order[0] != entry:
            # the entry is not the lowest block (the function branches back
            # into code placed before it): start at the entry explicitly
            labels.add(entry)
            lines.append(f'    goto L_{entry:08x};')
        for a, body in out:
            if a in labels:
                lines.append(f'L_{a:08x}:')
            lines.append(f'    /* {a:06x} {p.code[a].mn or "?"} */ {body}')
        lines.append('}')
        return '\n'.join(lines), len(nodes)


def interp_tables(prog, outdir):
    """Predecoded instruction table (binary) + C op switch."""
    mns = sorted(set(SEM) | set(COND) | {'j', 'jal', 'jr', 'jalr', 'eret', 'invalid'})
    ids = {m: n for n, m in enumerate(mns)}
    with open(os.path.join(outdir, 'interp_ops.h'), 'w') as f:
        f.write('/* generated by gen.py */\nenum {\n')
        for m in mns:
            f.write(f'    IOP_{cname(m)},\n')
        f.write('    IOP_COUNT\n};\n')
        f.write('static const char *const iop_names[] = {\n')
        for m in mns:
            f.write(f'    "{m}",\n')
        f.write('};\n')
    with open(os.path.join(outdir, 'interp_sem.inc'), 'w') as f:
        f.write('/* generated by gen.py: plain (non-control) instructions */\n')
        v = fields(None, as_vars=True)
        for m in sorted(SEM):
            f.write(f'case IOP_{cname(m)}: {{ {SEM[m].format(**v)} }} break;\n')
    with open(os.path.join(outdir, 'interp_cond.inc'), 'w') as f:
        f.write('/* generated by gen.py: branch conditions */\n')
        v = fields(None, as_vars=True)
        for m in sorted(COND):
            lk = 1 if m in LIKELY else 0
            f.write(f'case IOP_{cname(m)}: cond = ({COND[m].format(**v)}); '
                    f'likely = {lk}; break;\n')
    n = (prog.hi - prog.lo) // 4
    blob = bytearray()
    for k in range(n):
        a = prog.lo + k * 4
        i = prog.code.get(a)
        if i is None or i.mn is None or i.mn in UNSUPPORTED_CTRL:
            blob += struct.pack('<HHI', ids['invalid'], 0, 0)
        else:
            blob += struct.pack('<HHI', ids[i.mn], 0, i.word)
    with open(os.path.join(outdir, 'insn.bin'), 'wb') as f:
        f.write(struct.pack('<III', prog.lo, prog.hi, n))
        f.write(blob)


def cname(m):
    return m.replace('.', '_').upper()


def main():
    elf_path, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    for f in os.listdir(outdir):
        if f.startswith('fn_') and f.endswith('.c'):
            os.remove(os.path.join(outdir, f))
    prog = Program(elf_path)
    prog.find_entries()
    em = Emitter(prog)
    ents = sorted(prog.entries)
    why = {}
    for a in ents:
        why[prog.entry_why[a]] = why.get(prog.entry_why[a], 0) + 1
    print(f'code {prog.lo:#x}..{prog.hi:#x}, {len(prog.code)} insns, '
          f'{len(ents)} entries {why}')

    with open(os.path.join(outdir, 'funcs.h'), 'w') as f:
        f.write('/* generated by gen.py */\n#include "cpu.h"\n')
        f.write('#define MUSTTAIL __attribute__((musttail))\n'
                '#ifndef TRACE_FN\n#define TRACE_FN(a)\n#endif\n'
                '#ifdef ROR_TRACE\n#define TRACE_CALL(from, to) rt_trace_call(c, from, to)\n#else\n#define TRACE_CALL(from, to)\n#endif\n')
        f.write(f'#define ROR_CODE_LO 0x{prog.lo:x}u\n'
                f'#define ROR_CODE_HI 0x{prog.hi:x}u\n'
                f'#define ROR_ENTRY 0x{prog.elf.entry:x}u\n')
        for a in ents:
            f.write(f'void f_{a:08x}(Ctx *c);\n')

    total = emitted = 0
    nfile = 0
    buf, cnt = [], 0

    def flush():
        nonlocal buf, cnt, nfile
        if not buf:
            return
        with open(os.path.join(outdir, f'fn_{nfile:03d}.c'), 'w') as f:
            f.write('/* generated by gen.py */\n#include "funcs.h"\n\n')
            f.write('\n\n'.join(buf))
            f.write('\n')
        nfile += 1
        buf, cnt = [], 0

    covered = set()
    biggest = (0, 0)
    for a in ents:
        src, n = em.function(a)
        buf.append(src)
        cnt += n
        emitted += n
        biggest = max(biggest, (n, a))
        if cnt >= FUNCS_PER_FILE_INSNS:
            flush()
    flush()

    with open(os.path.join(outdir, 'fn_table.c'), 'w') as f:
        f.write('/* generated by gen.py */\n#include "funcs.h"\n')
        f.write('const recomp_fn ror_fn_table[] = {\n')
        es = prog.entries
        for a in range(prog.lo, prog.hi, 4):
            f.write(f'f_{a:08x},\n' if a in es else '0,\n')
        f.write('};\n')

    interp_tables(prog, outdir)
    print(f'emitted {emitted} instruction nodes in {len(ents)} functions, '
          f'{nfile} files; largest function {biggest[0]} nodes @ {biggest[1]:#x}')
    if em.unhandled:
        print('UNHANDLED:', em.unhandled)
        sys.exit(1)


if __name__ == '__main__':
    main()
