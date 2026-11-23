/* interp.c — reference interpreter.
 *
 * Executes the same instruction semantics as the recompiled code (the
 * per-instruction C is generated from the same templates), with the same
 * tick accounting, so the two can be compared event-for-event. */
#include "rt.h"
#include "interp_ops.h"

typedef struct { u16 op, pad; u32 word; } Insn;
static Insn *g_insn;
static u32 g_lo, g_hi;

void interp_init(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    u32 hdr[3];
    if (fread(hdr, 4, 3, f) != 3) { perror("insn.bin"); exit(1); }
    g_lo = hdr[0]; g_hi = hdr[1];
    g_insn = malloc(sizeof(Insn) * hdr[2]);
    if (fread(g_insn, sizeof(Insn), hdr[2], f) != hdr[2]) { perror("insn.bin"); exit(1); }
    fclose(f);
}

static inline const Insn *fetch(Ctx *c, u32 pc) {
    if (UNLIKELY(pc < g_lo || pc >= g_hi || (pc & 3))) {
        c->pc = pc;
        rt_fatal(c, "interp: pc 0x%08x outside code", pc);
    }
    return &g_insn[(pc - g_lo) >> 2];
}

/* Execute one non-control instruction. Returns 0 if it was not plain. */
static inline __attribute__((always_inline))
int exec_plain(Ctx *c, const Insn *in, u32 pc) {
    u32 word = in->word;
    int rs = (word >> 21) & 31, rt = (word >> 16) & 31, rd = (word >> 11) & 31;
    int sa = (word >> 6) & 31;
    s32 imm = (s16)word;
    int dest = (word >> 21) & 15, bc = word & 3, fsf = (word >> 21) & 3, ftf = (word >> 23) & 3;
    u32 imm15 = (word >> 6) & 0x7FFF;
    (void)rs; (void)rt; (void)rd; (void)sa; (void)imm; (void)dest; (void)bc;
    (void)fsf; (void)ftf; (void)imm15; (void)pc;
    switch (in->op) {
#include "interp_sem.inc"
    default: return 0;
    }
    return 1;
}

static void slot(Ctx *c, u32 pc) {
    if (!exec_plain(c, fetch(c, pc), pc)) rt_invalid(c, pc);
}

void interp_run(Ctx *c) {
    u32 pc = c->pc;
    for (;;) {
        if (pc == 0) { c->pc = 0; return; }
        const Insn *in = fetch(c, pc);
        if (exec_plain(c, in, pc)) { pc += 4; continue; }
        u32 word = in->word;
        int rs = (word >> 21) & 31, rt = (word >> 16) & 31, rd = (word >> 11) & 31;
        (void)rt;
        switch (in->op) {
        case IOP_J: case IOP_JAL: {
            u32 t = ((pc + 4) & 0xF0000000u) | ((word & 0x03FFFFFFu) << 2);
            if (in->op == IOP_JAL) {
                c->r[31].u64[0] = pc + 8;
                slot(c, pc + 4);
                TICK(8);
                rt_trace_call(c, pc, t);
            } else {
                slot(c, pc + 4);
                if (t <= pc) TICK((pc - t) / 4 + 2);
            }
            pc = t;
            break;
        }
        case IOP_JALR: {
            u32 t = GU(rs);
            SD(rd, pc + 8);
            slot(c, pc + 4);
            TICK(8);
            c->pc = t;
            rt_trace_call(c, pc, t);
            pc = t;
            break;
        }
        case IOP_JR: {
            u32 t = GU(rs);
            slot(c, pc + 4);
            pc = t;
            break;
        }
        case IOP_ERET:
            pc = rt_eret(c, pc);
            break;
        case IOP_INVALID:
            rt_invalid(c, pc);
            break;
        default: {
            int cond, likely;
            s32 imm = (s16)word;
            switch (in->op) {
#include "interp_cond.inc"
            default:
                c->pc = pc;
                rt_fatal(c, "interp: unhandled op %s at 0x%08x", iop_names[in->op], pc);
            }
            u32 t = pc + 4 + ((u32)imm << 2);
            if (likely && !cond) { pc += 8; break; }
            slot(c, pc + 4);
            if (cond) {
                if (t <= pc) TICK((pc - t) / 4 + 2);
                pc = t;
            } else pc += 8;
            break;
        }
        }
    }
}
