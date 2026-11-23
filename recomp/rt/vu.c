/* vu.c — vector units in micro mode (VU1 started by VIF1, VU0 by VIF0 or
 * VCALLMS).
 *
 * Micro memory is predecoded into one record per instruction pair (checked
 * against the raw words on every execution, so uploads through VIF MPG and
 * direct EE writes to micro memory are both picked up). The common upper
 * (vector) operations run as SSE2 code that is bit-identical to the scalar
 * definitions in vu_ops.h; the common lower operations have direct cases and
 * the rest go through exec_lower, the general decoder.
 *
 * An instruction is a pair: a lower word (integer, memory, flow control,
 * divider, XGKICK) and an upper word (floating-point vector ops), executed
 * together. The timing behaviours programs depend on are modelled:
 *   - branches take effect after one delay-slot pair;
 *   - a branch reads the value a VI register had before the immediately
 *     preceding instruction wrote it;
 *   - MAC/status/clip flags become visible 4 instructions after the
 *     operation that produced them;
 *   - Q (DIV/SQRT/RSQRT) and P (EFU) results arrive after their latency,
 *     or at WAITQ/WAITP.
 * The vector operations themselves are shared with COP2 (vu_ops.h). */
#include "vu.h"
#include "gs.h"
#include <stddef.h>
#include <time.h>
#include <emmintrin.h>

static int vi_delay = 1;
static int vu_clip_chain = 0;         /* ROR_VUCLIP=1: CLIP shifts into the newest clip flags, in-flight results included (see U_CLIP) */
static u64 FLAG_LATENCY = 4;      /* instructions before MAC/status/clip results are visible */
VU g_vu1 = { .vf = { [0] = { .f = { 0.0f, 0.0f, 0.0f, 1.0f } } } };
static u8 vu0_micro[0x1000], vu0_mem[0x1000], vu1_micro[0x4000], vu1_mem[0x4000];

static u64 vu_dump_frame = ~0ull; static int vu_dump_k, vu_trace_k = -1, vu_tracing, vu_force_trace;
static u64 vu_dump_nframes = 1;      /* ROR_VUDUMPN: number of frames to dump */

static void vu_predecode_init(void);
/* ROR_VUSTAT=1: every 500 frames, log the time spent in VU1 programs and in
 * VIF1 itself per frame (TSC; GS work done from XGKICK / DIRECT excluded). */
int g_vu_stat; u64 g_vu_stat_ticks[5];        /* [0] vu_exec(1), [1] path-1 kicks inside it, [2] vif_write(1), [3] vu_exec + path-2 GIF inside it, [4] VU1 starts */
static __attribute__((noinline)) void vu_kick_timed(const u8 *mem, u32 addr, u32 mask) {
    u64 t0 = __builtin_ia32_rdtsc();
    gif_write_path1(mem, addr, mask);
    g_vu_stat_ticks[1] += __builtin_ia32_rdtsc() - t0;
}
#define VU_KICK(mem, addr, mask) do { if (UNLIKELY(g_vu_stat)) vu_kick_timed(mem, addr, mask); else gif_write_path1(mem, addr, mask); } while (0)
void vu_stat_report(void) {            /* called by vif_write(1), outside its timed section */
    static u64 next, t_last, c_last; static double ns_per_tick;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    u64 now = (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec, tsc = __builtin_ia32_rdtsc();
    if (!next) { next = g_gs_frames + 500; t_last = now; c_last = tsc; memset(g_vu_stat_ticks, 0, sizeof g_vu_stat_ticks); return; }
    if (g_gs_frames < next) return;
    ns_per_tick = (double)(now - t_last) / (double)(tsc - c_last);
    double k = ns_per_tick * 1e-6 / 500.0;
    rt_log("[vu] frames %llu-%llu: VU1 %.3f ms/frame (%llu starts/frame), VIF1 %.3f ms/frame\n", (unsigned long long)next - 500, (unsigned long long)next,
           (double)(g_vu_stat_ticks[0] - g_vu_stat_ticks[1]) * k, (unsigned long long)g_vu_stat_ticks[4] / 500,
           (double)(g_vu_stat_ticks[2] - g_vu_stat_ticks[3]) * k);
    next += 500; t_last = now; c_last = tsc; memset(g_vu_stat_ticks, 0, sizeof g_vu_stat_ticks);
}

static int vu1_image_state;           /* generated code vs micro memory: 0 not compared, 1 same program, -1 different */
static int vu_use_fast = 1;          /* ROR_VUFAST=0: interpreter only, no generated code */
void vu_init(void) {
    { const char *e = getenv("ROR_VUFLAG"); if (e) FLAG_LATENCY = strtoull(e, 0, 0); }
    { const char *e = getenv("ROR_VUVIDELAY"); if (e) vi_delay = atoi(e); }
    { const char *e = getenv("ROR_VUCLIP"); if (e) vu_clip_chain = atoi(e); }
    { const char *e = getenv("ROR_VUSTAT"); if (e) g_vu_stat = atoi(e); }
    { const char *e = getenv("ROR_VUDUMP"); if (e) vu_dump_frame = strtoull(e, 0, 0); }
    { const char *e = getenv("ROR_VUDUMPN"); if (e) vu_dump_nframes = strtoull(e, 0, 0); }
    { const char *e = getenv("ROR_VUTRACE"); if (e) vu_trace_k = atoi(e); }
    { const char *e = getenv("ROR_VUFAST"); if (e) vu_use_fast = atoi(e); }
    g_vu0.micro = vu0_micro; g_vu0.micro_mask = 0xFFF; g_vu0.mem = vu0_mem; g_vu0.mem_mask = 0xFFF;
    g_vu1.micro = vu1_micro; g_vu1.micro_mask = 0x3FFF; g_vu1.mem = vu1_mem; g_vu1.mem_mask = 0x3FFF;
    if (FLAG_LATENCY < 1) FLAG_LATENCY = 1;
    if (FLAG_LATENCY > 4) FLAG_LATENCY = 4;
    vu_predecode_init();
}
/* Micro memory of VU n was (or may have been) written. The predecoded records
 * validate themselves; the generated code is only used while micro memory is
 * the program it was generated from, which is re-established here. */
void vu_micro_changed(int n) { if (n) vu1_image_state = 0; }

/* Debug: ROR_VUDUMP=<frame> writes, for every VU1 program start in that
 * frame, <dir>/vu1_<k>.bin = registers + data memory + micro memory
 * (dir = ROR_VUDUMPDIR, default "."); ROR_VUTRACE=<k> additionally prints
 * every instruction executed by start number k (pc, VF/VI written). */
/* State of VU n as one record: pc, TOP, ITOP, registers, data memory, micro
 * memory (the layout recomp/tools reads; also the header of a ROR_VIFDUMP stream). */
void vu_write_state(FILE *f, int n, u32 pc) {
    VU *v = n ? &g_vu1 : &g_vu0;
    u32 hdr[4] = { pc, v->vi_top, v->vi_itop, 0 };
    fwrite(hdr, 4, 4, f); fwrite(v->vf, 16, 32, f); fwrite(v->vi, 4, 16, f);
    u32 ext[8] = { v->q.u, v->p.u, v->i.u, v->r, v->status, v->mac, v->clip, 0 };
    fwrite(&v->acc, 16, 1, f); fwrite(ext, 4, 8, f);
    u8 pad[0x4000] = { 0 };
    fwrite(v->mem, 1, v->mem_mask + 1, f); fwrite(pad, 1, 0x4000 - (v->mem_mask + 1), f);
    fwrite(v->micro, 1, v->micro_mask + 1, f); fwrite(pad, 1, 0x4000 - (v->micro_mask + 1), f);
}
static void vu_dump_start(VU *v, u32 pc) {
    char name[512]; const char *d = getenv("ROR_VUDUMPDIR");
    snprintf(name, sizeof name, "%s/vu1_%05d.bin", d ? d : ".", vu_dump_k);
    FILE *f = fopen(name, "wb");
    if (!f) return;
    (void)v; vu_write_state(f, 1, pc);
    fclose(f);
}


/* Flag results (MAC + status, or clip) become visible FLAG_LATENCY (1..4)
 * instructions after the one that produced them. They wait in a four-stage
 * shift register that moves once per instruction: stage 0 is what becomes
 * visible at the start of the next instruction, and an instruction's own
 * result enters at stage FLAG_LATENCY - 1.
 *
 * A MAC result travels as a vector (EV_MACV) from which the flags can be read
 * when — and only if — something looks at them: a component that is zero has
 * Z, a negative one S, an infinite one O (U never occurs, see the arithmetic
 * below). Normally that vector is the result itself with the components
 * outside dest replaced by 1.0; the exact fallback and OPMULA/OPMSUB build
 * one from their MAC bits. Once visible it is "visx" and v->mac is stale
 * (vst bit 0) until mac_sync().
 *
 * The status register's Z/S/U/O bits and their sticky copies are a function
 * of the MAC values that have become visible (low bits: the current MAC;
 * sticky: any bit of any MAC since), so they are not updated per event:
 * accS / accMin / accMax collect what became visible and status_sync() folds
 * it in before anything reads or writes the status register.
 *
 * Generated code keeps FlagRegs in locals; the interpreter uses Pipe.fr.
 * Q and P results count down the instructions left until they land. */
enum { F_IBIT = 1, F_EBIT = 2, F_SAVE = 4, F_STALLQ = 8, F_STALLP = 16 };     /* Rec.fl */
#define EV_CLIP 0x02000000u                  /* stage = EV_CLIP | 24 clip bits, EV_MACV, or 0 = none */
#define EV_MACV 0x04000000u
#define POP_DYN (-1)                         /* flags_step: stage 0 holds whatever ev[0] says */
#define K_ABS  _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF))
#define K_FMAX _mm_castsi128_ps(_mm_set1_epi32(0x7F7FFFFF))
#define K_EXP  _mm_set1_epi32(0x7F800000)
#define K_SIGN _mm_set1_epi32((int)0x80000000u)
typedef struct {
    __m128 evx[4];                /* vectors of the EV_MACV stages */
    __m128 visx;                  /* the visible MAC as a vector, when vst & 1 */
    __m128 accS, accMin, accMax;  /* since the last status_sync: OR of the visible vectors (sign bits), min and max of their magnitudes */
    u32 ev[4];
    u32 vst;                      /* bit 0: v->mac is stale, visx holds it; bit 1: a vector became visible since the last status_sync */
} FlagRegs;
typedef struct {
    FlagRegs fr;
    u32 pend;                     /* bit 0: Q result in flight, bit 1: P */
    u32 q_left, p_left, q_status; fpr_t q_val, p_val;
    int vi_wr_reg; u32 vi_wr_old; /* VI register written by the previous instruction (0 = none) and its old value */
} Pipe;
static Pipe vu_pipes[2];

static inline u32 mac_to_st(u32 mac) {
    return (u32)((mac & 0x000F) != 0) | ((u32)((mac & 0x00F0) != 0) << 1) | ((u32)((mac & 0x0F00) != 0) << 2) | ((u32)((mac & 0xF000) != 0) << 3);
}
/* MAC bits of a flag vector. The nibbles are Z, S, U, O with x in bit 3, so the masks come from the reversed vector. */
static inline __attribute__((always_inline)) u32 vec_to_mac(__m128 x) {
    __m128 xr = _mm_shuffle_ps(x, x, 0x1B);
    return (u32)_mm_movemask_ps(_mm_cmpeq_ps(xr, _mm_setzero_ps())) | ((u32)_mm_movemask_ps(xr) << 4) |
           ((u32)_mm_movemask_ps(_mm_cmpnle_ps(_mm_and_ps(xr, K_ABS), K_FMAX)) << 12);
}
/* A flag vector for given MAC bits (no U: it cannot be set with FTZ on). */
static __attribute__((noinline)) __m128 mac_to_vec(u32 mac) {
    r128 t;
    if (mac & 0x0F00) rt_fatal(0, "VU: underflow flag with FTZ set (MAC 0x%04x)", mac);
    for (int i = 0; i < 4; i++)
        t.u32[i] = ((mac >> (15 - i)) & 1 ? 0x7F800000u : (mac >> (3 - i)) & 1 ? 0u : 0x3F800000u) | (((mac >> (7 - i)) & 1) << 31);
    return _mm_loadu_ps(t.f);
}
/* Make v->mac current. */
static inline __attribute__((always_inline)) void mac_sync(VU *v, FlagRegs *f) {
    if (f->vst & 1) { v->mac = vec_to_mac(f->visx); f->vst &= ~1u; }
}
/* Make v->mac and v->status current. */
static inline __attribute__((always_inline)) void status_sync(VU *v, FlagRegs *f) {
    mac_sync(v, f);
    if (f->vst & 2) {
        u32 sticky = (u32)(_mm_movemask_ps(_mm_cmpeq_ps(f->accMin, _mm_setzero_ps())) != 0) | ((u32)(_mm_movemask_ps(f->accS) != 0) << 1) |
                     ((u32)(_mm_movemask_ps(_mm_cmpnle_ps(f->accMax, K_FMAX)) != 0) << 3);
        v->status = (v->status & 0xFF0) | mac_to_st(v->mac) | (sticky << 6);
        f->vst = 0; f->accS = f->accMax = _mm_setzero_ps(); f->accMin = _mm_set1_ps(1.0f);
    }
}
static inline __attribute__((always_inline)) void flag_apply1(VU *v, FlagRegs *f, u32 e, __m128 x) {
    if (e == EV_MACV) {
        __m128 a = _mm_and_ps(x, K_ABS);
        f->visx = x; f->vst = 3;
        f->accS = _mm_or_ps(f->accS, x); f->accMin = _mm_min_ps(f->accMin, a); f->accMax = _mm_max_ps(f->accMax, a);
    } else v->clip = e & 0xFFFFFF;
}
/* Start of an instruction: the oldest stage becomes visible, the rest move
 * up. popk = what stage 0 holds if that is known (generated code: the kind of
 * result the instruction four back produces), else POP_DYN. */
static inline __attribute__((always_inline)) void flags_step(VU *v, FlagRegs *f, int popk) {
    u32 e = popk == (int)EV_MACV ? EV_MACV : popk == 0 ? 0 : f->ev[0];     /* a clip stage carries its value (and FCSET may have dropped it) */
    __m128 x = f->evx[0];
    f->ev[0] = f->ev[1]; f->ev[1] = f->ev[2]; f->ev[2] = f->ev[3]; f->ev[3] = 0;
    f->evx[0] = f->evx[1]; f->evx[1] = f->evx[2]; f->evx[2] = f->evx[3];
    if (e) flag_apply1(v, f, e, x);
}
static void flags_clear(Pipe *p) {
    memset(&p->fr, 0, sizeof p->fr); p->fr.accMin = _mm_set1_ps(1.0f);
}
static void flags_flush(VU *v, Pipe *p) {              /* program end: everything still in flight lands, oldest first */
    for (int k = 0; k < 4; k++) { if (p->fr.ev[k]) flag_apply1(v, &p->fr, p->fr.ev[k], p->fr.evx[k]); p->fr.ev[k] = 0; }
    status_sync(v, &p->fr);
}
static void q_land(VU *v, Pipe *p) {                   /* the divider result arrives (or is waited for) */
    if (p->pend & 1) {
        v->q = p->q_val;
        v->status = (v->status & ~0x30u) | (p->q_status & 0x30) | ((p->q_status & 0x30) << 6);
        p->pend &= ~1u;
    }
}
static void p_land(VU *v, Pipe *p) {
    if (p->pend & 2) { v->p = p->p_val; p->pend &= ~2u; }
}
/* Start of an instruction while something is in flight. A lower instruction
 * that waits for the divider or the EFU (WAITQ / WAITP, or a new
 * DIV/SQRT/RSQRT or E* op while one is in flight) stalls the whole pair, so
 * the upper half already sees the finished Q / P ("MULq ... | WAITQ"). */
static inline __attribute__((always_inline)) void pend_step(VU *v, Pipe *p, int fl) {
    if ((p->pend & 1) && --p->q_left == 0) q_land(v, p);
    if ((p->pend & 2) && --p->p_left == 0) p_land(v, p);
    if (fl & F_STALLQ) q_land(v, p);
    else if (fl & F_STALLP) p_land(v, p);
}
/* VI write by integer arithmetic or a load: a branch in the next instruction
 * still sees the old value, so the write is recorded (`rec`; generated code
 * passes 0 where the next instruction is known not to be a branch). Flag
 * tests (FCAND, FMAND, ...) and XTOP/XITOP use VI_SET instead: their result
 * is visible at once, and "FCAND; IBNE vi1" pairs depend on that. */
static inline __attribute__((always_inline)) void vi_wr(VU *v, Pipe *p, int reg, u32 val, int rec) {
    if (!reg) return;
    if (rec) { p->vi_wr_reg = reg; p->vi_wr_old = v->vi[reg]; }
    v->vi[reg] = val & 0xFFFF;
}
/* VI value as seen by a branch; pr/po = the previous instruction's record. */
static inline __attribute__((always_inline)) u32 vi_br(const VU *v, int reg, int pr, u32 po) {
    if (vi_delay && reg && pr == reg) return po;
    return v->vi[reg];
}
#define vi_write(v, p, reg, val) vi_wr(v, p, reg, val, 1)
#define vi_branch(v, p, reg) vi_br(v, reg, pr, po)

/* ---- lower instruction ------------------------------------------------------- */
static inline s32 sx11(u32 w) { return (s32)(w << 21) >> 21; }
static inline u32 mem_addr(const VU *v, s32 imm, u32 base) { return ((u32)(imm + (s32)base) * 16) & v->mem_mask; }

static float efu_sqsum(const VU *v, int fs) {
    float x = fclamp(v->vf[fs].f[0]), y = fclamp(v->vf[fs].f[1]), z = fclamp(v->vf[fs].f[2]);
    return x * x + y * y + z * z;
}
static void set_p(VU *v, Pipe *p, float val, int latency) {
    p_land(v, p);
    p->p_val.f = fclamp(val); p->p_left = (u32)latency; p->pend |= 2;
}
static void start_q(VU *v, Pipe *p, int latency) {
    /* the divider op wrote v->q / v->status directly: move the result into the pipeline */
    p->q_val = v->q; p->q_status = v->status; p->q_left = (u32)latency; p->pend |= 1;
}

/* Returns 0, or 1 with *target set when a branch/jump is taken. */
static int exec_lower(int n, VU *v, Pipe *p, u32 w, u32 pc, u32 *target, int pr, u32 po) {
    u32 op = w >> 25, dest = (w >> 21) & 15;
    int it = (w >> 16) & 31, is = (w >> 11) & 31, id = (w >> 6) & 31;
    s32 imm11 = sx11(w);
    u32 imm15 = ((w >> 10) & 0x7800) | (w & 0x7FF), imm24 = w & 0xFFFFFF;
    u32 imm12 = ((w >> 10) & 0x800) | (w & 0x7FF);
    switch (op) {
    case 0x00: {   /* LQ ft, imm(is) */
        if (it) { r128 m; memcpy(&m, v->mem + mem_addr(v, imm11, v->vi[is & 15]), 16);
            for (int i = 0; i < 4; i++) if (dest & (8u >> i)) v->vf[it].u32[i] = m.u32[i]; }
        return 0;
    }
    case 0x01: {   /* SQ fs, imm(it) */
        u32 a = mem_addr(v, imm11, v->vi[it & 15]);
        for (int i = 0; i < 4; i++) if (dest & (8u >> i)) memcpy(v->mem + a + i * 4, &v->vf[is].u32[i], 4);
        return 0;
    }
    case 0x04: {   /* ILW it, imm(is) */
        u32 a = mem_addr(v, imm11, v->vi[is & 15]), val = v->vi[it & 15];
        for (int i = 0; i < 4; i++) if (dest & (8u >> i)) { u16 h; memcpy(&h, v->mem + a + i * 4, 2); val = h; }
        vi_write(v, p, it & 15, val);
        return 0;
    }
    case 0x05: {   /* ISW it, imm(is) */
        u32 a = mem_addr(v, imm11, v->vi[is & 15]), val = v->vi[it & 15];
        for (int i = 0; i < 4; i++) if (dest & (8u >> i)) memcpy(v->mem + a + i * 4, &val, 4);
        return 0;
    }
    case 0x08: vi_write(v, p, it & 15, v->vi[is & 15] + imm15); return 0;
    case 0x09: vi_write(v, p, it & 15, v->vi[is & 15] - imm15); return 0;
    case 0x10: VI_SET(v, 1, (v->clip & 0xFFFFFF) == imm24); return 0;
    case 0x11: v->clip = imm24; return 0;
    case 0x12: VI_SET(v, 1, (v->clip & imm24) != 0); return 0;
    case 0x13: VI_SET(v, 1, ((v->clip | imm24) & 0xFFFFFF) == 0xFFFFFF); return 0;
    case 0x14: VI_SET(v, it & 15, (v->status & 0xFFF) == imm12); return 0;
    case 0x15: v->status = (v->status & 0x3F) | (imm12 & 0xFC0); return 0;
    case 0x16: VI_SET(v, it & 15, v->status & imm12); return 0;
    case 0x17: VI_SET(v, it & 15, (v->status | imm12) & 0xFFF); return 0;
    case 0x18: VI_SET(v, it & 15, (v->mac & 0xFFFF) == v->vi[is & 15]); return 0;
    case 0x1A: VI_SET(v, it & 15, v->mac & v->vi[is & 15]); return 0;
    case 0x1B: VI_SET(v, it & 15, (v->mac | v->vi[is & 15]) & 0xFFFF); return 0;
    case 0x1C: VI_SET(v, it & 15, v->clip & 0xFFF); return 0;
    case 0x20: *target = pc + 8 + (u32)(imm11 * 8); return 1;
    case 0x21: VI_SET(v, it & 15, (pc + 16) / 8); *target = pc + 8 + (u32)(imm11 * 8); return 1;
    case 0x24: *target = vi_branch(v, p, is & 15) * 8; return 1;
    case 0x25: { u32 t = vi_branch(v, p, is & 15) * 8; VI_SET(v, it & 15, (pc + 16) / 8); *target = t; return 1; }
    case 0x28: if (vi_branch(v, p, it & 15) == vi_branch(v, p, is & 15)) { *target = pc + 8 + (u32)(imm11 * 8); return 1; } return 0;
    case 0x29: if (vi_branch(v, p, it & 15) != vi_branch(v, p, is & 15)) { *target = pc + 8 + (u32)(imm11 * 8); return 1; } return 0;
    case 0x2C: if ((s16)vi_branch(v, p, is & 15) < 0)  { *target = pc + 8 + (u32)(imm11 * 8); return 1; } return 0;
    case 0x2D: if ((s16)vi_branch(v, p, is & 15) > 0)  { *target = pc + 8 + (u32)(imm11 * 8); return 1; } return 0;
    case 0x2E: if ((s16)vi_branch(v, p, is & 15) <= 0) { *target = pc + 8 + (u32)(imm11 * 8); return 1; } return 0;
    case 0x2F: if ((s16)vi_branch(v, p, is & 15) >= 0) { *target = pc + 8 + (u32)(imm11 * 8); return 1; } return 0;
    case 0x40: break;
    default:
        rt_fatal(0, "VU%d: invalid lower instruction 0x%08x at 0x%x", n, w, pc);
    }
    u32 fn = w & 63;
    int fsf = (w >> 21) & 3, ftf = (w >> 23) & 3;
    switch (fn) {
    case 0x30: vi_write(v, p, id & 15, v->vi[is & 15] + v->vi[it & 15]); return 0;
    case 0x31: vi_write(v, p, id & 15, v->vi[is & 15] - v->vi[it & 15]); return 0;
    case 0x32: vi_write(v, p, it & 15, v->vi[is & 15] + (u32)((s32)(id << 27) >> 27)); return 0;
    case 0x34: vi_write(v, p, id & 15, v->vi[is & 15] & v->vi[it & 15]); return 0;
    case 0x35: vi_write(v, p, id & 15, v->vi[is & 15] | v->vi[it & 15]); return 0;
    }
    if (fn < 0x3C) rt_fatal(0, "VU%d: invalid lower instruction 0x%08x at 0x%x", n, w, pc);
    u32 x = (u32)(id << 2) | (w & 3);
    float f = fclamp(v->vf[is].f[fsf]);
    switch (x) {
    case 48: vu_move(v, dest, it, is); break;
    case 49: vu_mr32(v, dest, it, is); break;
    case 52: { u32 old = v->vi[is & 15]; vu_lqi(v, dest, it, is & 15); if (is & 15) { u32 nv = v->vi[is & 15]; v->vi[is & 15] = old; vi_write(v, p, is & 15, nv); } break; }
    case 53: { u32 old = v->vi[it & 15]; vu_sqi(v, dest, is, it & 15); if (it & 15) { u32 nv = v->vi[it & 15]; v->vi[it & 15] = old; vi_write(v, p, it & 15, nv); } break; }
    case 54: { u32 old = v->vi[is & 15]; vu_lqd(v, dest, it, is & 15); if (is & 15) { u32 nv = v->vi[is & 15]; v->vi[is & 15] = old; vi_write(v, p, is & 15, nv); } break; }
    case 55: { u32 old = v->vi[it & 15]; vu_sqd(v, dest, is, it & 15); if (it & 15) { u32 nv = v->vi[it & 15]; v->vi[it & 15] = old; vi_write(v, p, it & 15, nv); } break; }
    case 56: case 57: case 58: {                       /* DIV / SQRT / RSQRT */
        q_land(v, p);
        fpr_t oq = v->q; u32 ost = v->status;
        if (x == 56) vu_div(v, is, fsf, it, ftf);
        else if (x == 57) vu_sqrt(v, it, ftf);
        else vu_rsqrt(v, is, fsf, it, ftf);
        start_q(v, p, x == 58 ? 13 : 7);
        v->q = oq; v->status = ost;
        break;
    }
    case 59: q_land(v, p); break;                      /* WAITQ */
    case 60: vi_write(v, p, it & 15, v->vf[is].u32[fsf]); break;                         /* MTIR */
    case 61: if (it) { s32 t = (s16)v->vi[is & 15]; for (int i = 0; i < 4; i++) if (dest & (8u >> i)) v->vf[it].s32[i] = t; } break;
    case 62: {                                          /* ILWR it, (is) */
        u32 a = mem_addr(v, 0, v->vi[is & 15]), val = v->vi[it & 15];
        for (int i = 0; i < 4; i++) if (dest & (8u >> i)) { u16 h; memcpy(&h, v->mem + a + i * 4, 2); val = h; }
        vi_write(v, p, it & 15, val);
        break;
    }
    case 63: {                                          /* ISWR it, (is) */
        u32 a = mem_addr(v, 0, v->vi[is & 15]), val = v->vi[it & 15];
        for (int i = 0; i < 4; i++) if (dest & (8u >> i)) memcpy(v->mem + a + i * 4, &val, 4);
        break;
    }
    case 64: {                                          /* RNEXT */
        u32 r = v->r, b = ((r >> 4) ^ (r >> 22)) & 1;
        v->r = ((r << 1) | b) & 0x7FFFFF;
    }   /* fall through */
    case 65:                                            /* RGET */
        if (it) for (int i = 0; i < 4; i++) if (dest & (8u >> i)) v->vf[it].u32[i] = 0x3F800000u | v->r;
        break;
    case 66: v->r = v->vf[is].u32[fsf] & 0x7FFFFF; break;
    case 67: v->r = (v->r ^ v->vf[is].u32[fsf]) & 0x7FFFFF; break;
    case 100:                                           /* MFP */
        if (it) for (int i = 0; i < 4; i++) if (dest & (8u >> i)) v->vf[it].u32[i] = v->p.u;
        break;
    case 104: VI_SET(v, it & 15, v->vi_top & 0x3FF); break;    /* XTOP */
    case 105: VI_SET(v, it & 15, v->vi_itop & (n ? 0x3FF : 0xFF)); break;   /* XITOP */
    case 108:                                           /* XGKICK */
        if (n) VU_KICK(v->mem, (v->vi[is & 15] * 16) & v->mem_mask, v->mem_mask);
        break;
    case 112: set_p(v, p, efu_sqsum(v, is), 11); break;                       /* ESADD */
    case 113: { float s = efu_sqsum(v, is); set_p(v, p, s != 0.0f ? 1.0f / s : 3.4028235e38f, 18); break; }
    case 114: set_p(v, p, sqrtf(efu_sqsum(v, is)), 18); break;                 /* ELENG */
    case 115: { float s = sqrtf(efu_sqsum(v, is)); set_p(v, p, s != 0.0f ? 1.0f / s : 3.4028235e38f, 24); break; }
    case 116: set_p(v, p, atan2f(fclamp(v->vf[is].f[1]), fclamp(v->vf[is].f[0])), 54); break;   /* EATANxy */
    case 117: set_p(v, p, atan2f(fclamp(v->vf[is].f[2]), fclamp(v->vf[is].f[0])), 54); break;   /* EATANxz */
    case 118: set_p(v, p, fclamp(v->vf[is].f[0]) + fclamp(v->vf[is].f[1]) + fclamp(v->vf[is].f[2]) + fclamp(v->vf[is].f[3]), 12); break;
    case 120: set_p(v, p, sqrtf(fabsf(f)), 12); break;
    case 121: set_p(v, p, f != 0.0f ? 1.0f / sqrtf(fabsf(f)) : 3.4028235e38f, 18); break;
    case 122: set_p(v, p, f != 0.0f ? 1.0f / f : 3.4028235e38f, 12); break;
    case 123: p_land(v, p); break;                      /* WAITP */
    case 124: set_p(v, p, sinf(f), 29); break;
    case 125: set_p(v, p, atanf(f), 54); break;
    case 126: set_p(v, p, expf(-f), 44); break;
    default:
        rt_fatal(0, "VU%d: invalid lower instruction 0x%08x at 0x%x", n, w, pc);
    }
    return 0;
}


/* ---- predecoded records ------------------------------------------------------- */
#define VU_HAS_PREDECODE 1
enum { U_NOP, U_ADD, U_SUB, U_MUL, U_MADD, U_MSUB, U_MAX, U_MINI,      /* U_ADD.. = VUOP_* + 1 */
       U_ITOF, U_FTOI, U_ABS, U_CLIP, U_OPMULA, U_OPMSUB, U_BAD };
enum { SRC_VF, SRC_BC, SRC_Q, SRC_I };
enum { L_SLOW, L_SLOWF, L_NOP, L_LQ, L_SQ, L_ILW, L_ISW, L_IADDIU, L_ISUBIU, L_FCSET, L_FCEQ, L_FCAND, L_FCOR, L_FCGET,
       L_FSAND, L_FMAND, L_B, L_BAL, L_JR, L_JALR, L_IBEQ, L_IBNE, L_IBLTZ, L_IBGTZ, L_IBLEZ, L_IBGEZ,
       L_IADD, L_ISUB, L_IADDI, L_IAND, L_IOR, L_MOVE, L_MR32, L_LQI, L_SQI, L_LQD, L_SQD,
       L_MTIR, L_MFIR, L_ILWR, L_ISWR, L_XTOP, L_XITOP, L_XGKICK };
typedef struct {
    u64 raw;                      /* lower | upper << 32, as found in micro memory when decoded */
    u8  uk, usrc, ufs, uft;
    u8  ubc, udst, udest, wr;     /* udst: 0 discard, 1..31 VF, 32 ACC; wr: VF the upper writes (0 none) */
    u8  lk, lit, lis, lid;
    u8  ldest, fl; u16 mask16;    /* mask16: dest replicated into the four MAC nibbles */
    s32 limm; u32 ltarget;
    float scale; u32 pad;
} Rec;
static Rec vu0_recs[0x1000 / 8], vu1_recs[0x4000 / 8];

static void predecode(Rec *r, u32 lower, u32 upper, u32 pc) {
    memset(r, 0, sizeof *r);
    r->raw = lower | ((u64)upper << 32);
    u32 w = upper, dest = (w >> 21) & 15;
    int ft = (w >> 16) & 31, fs = (w >> 11) & 31, fd = (w >> 6) & 31;
    r->udest = (u8)dest; r->mask16 = (u16)(dest * 0x1111u); r->ufs = (u8)fs; r->uft = (u8)ft; r->ubc = w & 3;
    r->usrc = SRC_VF;
    if ((w & 0x3C) != 0x3C) {
        u32 op = w & 63;
        static const u8 kind[7] = { U_ADD, U_SUB, U_MADD, U_MSUB, U_MAX, U_MINI, U_MUL };
        r->udst = (u8)fd; r->wr = (u8)fd;
        if (op < 28) { r->uk = kind[op >> 2]; r->usrc = SRC_BC; }
        else switch (op) {
        case 28: r->uk = U_MUL;  r->usrc = SRC_Q; break;   case 29: r->uk = U_MAX;  r->usrc = SRC_I; break;
        case 30: r->uk = U_MUL;  r->usrc = SRC_I; break;   case 31: r->uk = U_MINI; r->usrc = SRC_I; break;
        case 32: r->uk = U_ADD;  r->usrc = SRC_Q; break;   case 33: r->uk = U_MADD; r->usrc = SRC_Q; break;
        case 34: r->uk = U_ADD;  r->usrc = SRC_I; break;   case 35: r->uk = U_MADD; r->usrc = SRC_I; break;
        case 36: r->uk = U_SUB;  r->usrc = SRC_Q; break;   case 37: r->uk = U_MSUB; r->usrc = SRC_Q; break;
        case 38: r->uk = U_SUB;  r->usrc = SRC_I; break;   case 39: r->uk = U_MSUB; r->usrc = SRC_I; break;
        case 40: r->uk = U_ADD; break;   case 41: r->uk = U_MADD; break;
        case 42: r->uk = U_MUL; break;   case 43: r->uk = U_MAX; break;
        case 44: r->uk = U_SUB; break;   case 45: r->uk = U_MSUB; break;
        case 46: r->uk = U_OPMSUB; break;
        case 47: r->uk = U_MINI; break;
        default: r->uk = U_BAD;
        }
    } else {
        u32 op = (((w >> 6) & 31) << 2) | (w & 3);
        static const float sc[4] = { 1.0f, 16.0f, 4096.0f, 32768.0f };
        r->udst = 32;
        switch (op) {
        case 0: case 1: case 2: case 3:     r->uk = U_ADD;  r->usrc = SRC_BC; break;
        case 4: case 5: case 6: case 7:     r->uk = U_SUB;  r->usrc = SRC_BC; break;
        case 8: case 9: case 10: case 11:   r->uk = U_MADD; r->usrc = SRC_BC; break;
        case 12: case 13: case 14: case 15: r->uk = U_MSUB; r->usrc = SRC_BC; break;
        case 24: case 25: case 26: case 27: r->uk = U_MUL;  r->usrc = SRC_BC; break;
        case 16: case 17: case 18: case 19: r->uk = U_ITOF; r->scale = 1.0f / sc[op - 16]; r->udst = (u8)ft; r->wr = (u8)ft; break;
        case 20: case 21: case 22: case 23: r->uk = U_FTOI; r->scale = sc[op - 20]; r->udst = (u8)ft; r->wr = (u8)ft; break;
        case 28: r->uk = U_MUL;  r->usrc = SRC_Q; break;
        case 29: r->uk = U_ABS;  r->udst = (u8)ft; r->wr = (u8)ft; break;
        case 30: r->uk = U_MUL;  r->usrc = SRC_I; break;
        case 31: r->uk = U_CLIP; break;
        case 32: r->uk = U_ADD;  r->usrc = SRC_Q; break;   case 33: r->uk = U_MADD; r->usrc = SRC_Q; break;
        case 34: r->uk = U_ADD;  r->usrc = SRC_I; break;   case 35: r->uk = U_MADD; r->usrc = SRC_I; break;
        case 36: r->uk = U_SUB;  r->usrc = SRC_Q; break;   case 37: r->uk = U_MSUB; r->usrc = SRC_Q; break;
        case 38: r->uk = U_SUB;  r->usrc = SRC_I; break;   case 39: r->uk = U_MSUB; r->usrc = SRC_I; break;
        case 40: r->uk = U_ADD; break;   case 41: r->uk = U_MADD; break;
        case 42: r->uk = U_MUL; break;   case 44: r->uk = U_SUB; break;   case 45: r->uk = U_MSUB; break;
        case 46: r->uk = U_OPMULA; break;
        case 47: r->uk = U_NOP; break;
        default: r->uk = U_BAD;
        }
    }
    if (upper & 0x40000000u) r->fl |= F_EBIT;
    if (upper & 0x80000000u) { r->fl |= F_IBIT; return; }

    w = lower;
    u32 op = w >> 25;
    int it = (w >> 16) & 31, is = (w >> 11) & 31, id = (w >> 6) & 31;
    s32 imm11 = sx11(w);
    u32 imm15 = ((w >> 10) & 0x7800) | (w & 0x7FF);
    r->lit = (u8)it; r->lis = (u8)is; r->lid = (u8)id; r->ldest = (w >> 21) & 15;
    r->ltarget = pc + 8 + (u32)(imm11 * 8);
    r->limm = imm11;
    if (r->wr && (r->wr == it || r->wr == is)) r->fl |= F_SAVE;     /* lower may read or write the VF the upper writes */
    r->lk = (op >= 0x14 && op <= 0x1B) ? L_SLOWF : L_SLOW;      /* L_SLOWF: reads or writes status / MAC */
    switch (op) {
    case 0x00: r->lk = L_LQ; break;
    case 0x01: r->lk = L_SQ; break;
    case 0x04: r->lk = L_ILW; break;
    case 0x05: r->lk = L_ISW; break;
    case 0x08: r->lk = L_IADDIU; r->limm = (s32)imm15; break;
    case 0x09: r->lk = L_ISUBIU; r->limm = (s32)imm15; break;
    case 0x10: r->lk = L_FCEQ;  r->limm = (s32)(w & 0xFFFFFF); break;
    case 0x11: r->lk = L_FCSET; r->limm = (s32)(w & 0xFFFFFF); break;
    case 0x12: r->lk = L_FCAND; r->limm = (s32)(w & 0xFFFFFF); break;
    case 0x13: r->lk = L_FCOR;  r->limm = (s32)(w & 0xFFFFFF); break;
    case 0x16: r->lk = L_FSAND; r->limm = (s32)(((w >> 10) & 0x800) | (w & 0x7FF)); break;
    case 0x1A: r->lk = L_FMAND; break;
    case 0x1C: r->lk = L_FCGET; break;
    case 0x20: r->lk = L_B; break;
    case 0x21: r->lk = L_BAL; break;
    case 0x24: r->lk = L_JR; break;
    case 0x25: r->lk = L_JALR; break;
    case 0x28: r->lk = L_IBEQ; break;
    case 0x29: r->lk = L_IBNE; break;
    case 0x2C: r->lk = L_IBLTZ; break;
    case 0x2D: r->lk = L_IBGTZ; break;
    case 0x2E: r->lk = L_IBLEZ; break;
    case 0x2F: r->lk = L_IBGEZ; break;
    case 0x40: {
        u32 fn = w & 63, x = (u32)(id << 2) | (w & 3);
        if (fn == 0x30) r->lk = L_IADD;
        else if (fn == 0x31) r->lk = L_ISUB;
        else if (fn == 0x32) { r->lk = L_IADDI; r->limm = (s32)(id << 27) >> 27; }
        else if (fn == 0x34) r->lk = L_IAND;
        else if (fn == 0x35) r->lk = L_IOR;
        else if (fn >= 0x3C) switch (x) {
        case 48: r->lk = it ? L_MOVE : L_NOP; break;
        case 49: r->lk = it ? L_MR32 : L_NOP; break;
        case 52: r->lk = L_LQI; break;
        case 53: r->lk = L_SQI; break;
        case 54: r->lk = L_LQD; break;
        case 55: r->lk = L_SQD; break;
        case 56: case 57: case 58: case 59: r->fl |= F_STALLQ; break;
        case 60: r->lk = L_MTIR; break;
        case 61: r->lk = L_MFIR; break;
        case 62: r->lk = L_ILWR; break;
        case 63: r->lk = L_ISWR; break;
        case 104: r->lk = L_XTOP; break;
        case 105: r->lk = L_XITOP; break;
        case 108: r->lk = L_XGKICK; break;
        default: if (x >= 112 && x <= 126) r->fl |= F_STALLP;
        }
        break;
    }
    }
}
static void vu_predecode_init(void) {
    for (u32 i = 0; i < 0x1000 / 8; i++) predecode(&vu0_recs[i], 0, 0, i * 8);
    for (u32 i = 0; i < 0x4000 / 8; i++) predecode(&vu1_recs[i], 0, 0, i * 8);
}

/* ---- SSE2 forms of the vu_ops.h arithmetic ----------------------------------------
 * Bit-identical to the scalar definitions provided MXCSR has FTZ and DAZ set
 * (the runtime sets both at start-up; vu_exec checks), by this argument:
 *
 * vu_arith clamps every operand and the product of a MADD/MSUB (fclamp:
 * infinity/NaN -> +-FLT_MAX) and fixes up the result (vu_res: infinity ->
 * +-FLT_MAX with the O flag, denormal -> 0 with the U flag). If the same
 * operation is done with plain IEEE arithmetic on the unclamped operands and
 * the result is finite, then no operand was infinite or NaN and nothing
 * overflowed on the way (any of those makes a sum or product infinite or
 * NaN), so every fclamp was the identity; and with FTZ/DAZ a result is never
 * denormal and a denormal operand counts as zero either way. The plain result
 * is then exactly the reference result, O and U are clear, Z is "== 0" and S
 * is the sign bit. Only when a result component selected by dest is not
 * finite is the operation redone by vu_arith itself (upper_exact).
 * MAX/MINI have no result test, so their operands are tested instead. */
static const r128 DMASK[16] = {      /* component i selected when dest & (8 >> i) */
#define DM(d) { .u32 = { (d) & 8 ? ~0u : 0, (d) & 4 ? ~0u : 0, (d) & 2 ? ~0u : 0, (d) & 1 ? ~0u : 0 } }
    DM(0), DM(1), DM(2), DM(3), DM(4), DM(5), DM(6), DM(7), DM(8), DM(9), DM(10), DM(11), DM(12), DM(13), DM(14), DM(15)
#undef DM
};
static const r128 NDONE[16] = {      /* 1.0 in the components dest does not select */
#define ND(d) { .u32 = { (d) & 8 ? 0 : 0x3F800000u, (d) & 4 ? 0 : 0x3F800000u, (d) & 2 ? 0 : 0x3F800000u, (d) & 1 ? 0 : 0x3F800000u } }
    ND(0), ND(1), ND(2), ND(3), ND(4), ND(5), ND(6), ND(7), ND(8), ND(9), ND(10), ND(11), ND(12), ND(13), ND(14), ND(15)
#undef ND
};
static inline __attribute__((always_inline)) __m128i v_sel(__m128i m, __m128i a, __m128i b) { return _mm_or_si128(_mm_and_si128(m, a), _mm_andnot_si128(m, b)); }
static inline __attribute__((always_inline)) void v_store(r128 *d, int dest, __m128i x) {
    if (dest == 15) _mm_storeu_si128((__m128i *)d, x);
    else _mm_storeu_si128((__m128i *)d, v_sel(_mm_loadu_si128((const __m128i *)&DMASK[dest]), x, _mm_loadu_si128((const __m128i *)d)));
}
/* dest (x = bit 3) as a movemask (x = bit 0) */
#define DEST_MM(d) ((((d) & 8) >> 3) | (((d) & 4) >> 1) | (((d) & 2) << 1) | (((d) & 1) << 3))

/* Second operand: a VF register, one of its components, Q or I, as four
 * floats. Components are taken from a load of the whole register and Q / I
 * from a 4-byte load: a 16-byte load starting inside a register would not be
 * forwarded from the store that just wrote it (a ~12 cycle stall each time). */
static inline __attribute__((always_inline)) __m128 v_src(const VU *v, int usrc, int uft, int ubc) {
    __m128 b;
    switch (usrc) {
    case SRC_VF: return _mm_loadu_ps(v->vf[uft].f);
    case SRC_BC:
        b = _mm_loadu_ps(v->vf[uft].f);
        switch (ubc) {
        case 0:  return _mm_shuffle_ps(b, b, 0x00);
        case 1:  return _mm_shuffle_ps(b, b, 0x55);
        case 2:  return _mm_shuffle_ps(b, b, 0xAA);
        default: return _mm_shuffle_ps(b, b, 0xFF);
        }
    default: {
        u32 t = usrc == SRC_Q ? v->q.u : v->i.u;
        __asm__("" : "+r"(t));                           /* keeps the compiler from widening this into a 16-byte load */
        return _mm_castsi128_ps(_mm_shuffle_epi32(_mm_cvtsi32_si128((int)t), 0));
    }
    }
}

/* The reference operation itself, for the rare non-finite cases. Returns the MAC bits. */
static __attribute__((noinline, cold))
u32 upper_exact(VU *v, r128 *acc, int uk, int usrc, int ufs, int uft, int ubc, int udst, int udest) {
    u32 omac = v->mac, ostat = v->status;
    r128 b, scratch;
    v->acc = *acc;
    switch (usrc) {
    case SRC_VF: b = v->vf[uft]; break;
    case SRC_BC: b = vu_splat(v->vf[uft].f[ubc]); break;
    case SRC_Q:  b = vu_splat(v->q.f); break;
    default:     b = vu_splat(v->i.f); break;
    }
    vu_arith(v, uk - U_ADD + VUOP_ADD, udst == 32 ? &v->acc : udst ? &v->vf[udst] : &scratch, (u32)udest, &v->vf[ufs], &b);
    *acc = v->acc;
    u32 mac = v->mac;
    v->mac = omac; v->status = ostat;
    return mac;
}

/* ---- one instruction, in two halves ------------------------------------------------
 * These are always inlined: the interpreter passes the fields of a predecoded
 * record, generated code (vu1_fast.inc) passes constants, so that the compiler
 * specialises every instruction of a known micro program. acc = where ACC
 * lives (generated code keeps it in a local). */
static inline __attribute__((always_inline))
void do_upper(VU *v, r128 *acc, FlagRegs *f, int lat, int uk, int usrc, int ufs, int uft, int ubc, int udst, int udest, u32 mask16, float scale, u32 raw_upper) {
    switch (uk) {
    case U_NOP: break;
    case U_ADD: case U_SUB: case U_MUL: case U_MADD: case U_MSUB: {
        __m128 a = _mm_loadu_ps(v->vf[ufs].f), b, x;
        b = v_src(v, usrc, uft, ubc);
        switch (uk) {
        case U_ADD:  x = _mm_add_ps(a, b); break;
        case U_SUB:  x = _mm_sub_ps(a, b); break;
        case U_MUL:  x = _mm_mul_ps(a, b); break;
        case U_MADD: x = _mm_add_ps(_mm_loadu_ps(acc->f), _mm_mul_ps(a, b)); break;
        default:     x = _mm_sub_ps(_mm_loadu_ps(acc->f), _mm_mul_ps(a, b)); break;
        }
        if (UNLIKELY(_mm_movemask_ps(_mm_cmpnle_ps(_mm_and_ps(x, K_ABS), K_FMAX)) & DEST_MM(udest)))      /* infinite or NaN */
            f->evx[lat - 1] = mac_to_vec(upper_exact(v, acc, uk, usrc, ufs, uft, ubc, udst, udest) & mask16);
        else {
            if (udst) v_store(udst == 32 ? acc : &v->vf[udst], udest, _mm_castps_si128(x));
            f->evx[lat - 1] = udest == 15 ? x : _mm_or_ps(_mm_and_ps(x, _mm_loadu_ps(DMASK[udest].f)), _mm_loadu_ps(NDONE[udest].f));
        }
        f->ev[lat - 1] = EV_MACV;
        break;
    }
    case U_MAX: case U_MINI: {
        if (!udst) break;
        __m128 a = _mm_loadu_ps(v->vf[ufs].f), b;
        b = v_src(v, usrc, uft, ubc);
        /* y * 0 is NaN exactly when y is infinite or NaN */
        __m128 nf = _mm_cmpunord_ps(_mm_mul_ps(a, _mm_setzero_ps()), _mm_mul_ps(b, _mm_setzero_ps()));
        if (UNLIKELY(_mm_movemask_ps(nf) & DEST_MM(udest))) upper_exact(v, acc, uk, usrc, ufs, uft, ubc, udst, udest);
        else v_store(&v->vf[udst], udest, _mm_castps_si128(uk == U_MAX ? _mm_max_ps(a, b) : _mm_min_ps(a, b)));   /* x > y ? x : y  /  x < y ? x : y */
        break;
    }
    case U_ITOF:
        if (udst) v_store(&v->vf[udst], udest, _mm_castps_si128(_mm_mul_ps(_mm_cvtepi32_ps(_mm_loadu_si128((const __m128i *)&v->vf[ufs])), _mm_set1_ps(scale))));
        break;
    case U_FTOI:
        if (udst) {
            /* fclamp is not needed: an infinite or NaN operand stays one after the multiply (scale > 0, sign
             * kept), and like the clamped value it then saturates by its sign */
            __m128 a = _mm_mul_ps(_mm_loadu_ps(v->vf[ufs].f), _mm_set1_ps(scale));
            __m128i ai = _mm_castps_si128(a);
            __m128i big = _mm_cmpgt_epi32(_mm_and_si128(ai, K_EXP), _mm_set1_epi32(0x4E800000));
            __m128i pos = _mm_cmpeq_epi32(_mm_and_si128(ai, K_SIGN), _mm_setzero_si128());
            v_store(&v->vf[udst], udest, _mm_xor_si128(_mm_cvttps_epi32(a), _mm_and_si128(big, pos)));   /* 0x80000000 -> INT32_MAX */
        }
        break;
    case U_ABS: vu_abs(v, (u32)udest, udst, ufs); break;
    case U_CLIP: {
        /* The new judgement is shifted into the newest clip flags, i.e. including results that are
         * still in flight: "CLIP; CLIP; ...; FCAND" sequences closer than the flag latency (0x2528,
         * 0x3ae0 in this game's program) collect all their judgements. That is believed to be the
         * hardware behaviour, but it changes two reference screenshots and is unconfirmed, so it is
         * off by default (ROR_VUCLIP=1): by default the judgement is shifted into the visible flags. */
        u32 oclip = v->clip;
        if (vu_clip_chain) for (int k = 0; k < 4; k++) if (f->ev[k] & EV_CLIP) v->clip = f->ev[k] & 0xFFFFFF;
        vu_clip(v, ufs, uft);
        f->ev[lat - 1] = EV_CLIP | v->clip;
        v->clip = oclip;
        break;
    }
    case U_OPMULA: case U_OPMSUB: {                     /* outer product halves: xyz <- fs.yzx * ft.zxy (ACC - that) */
        __m128 a = _mm_loadu_ps(v->vf[ufs].f), b = _mm_loadu_ps(v->vf[uft].f);
        __m128 x = _mm_mul_ps(_mm_shuffle_ps(a, a, 0xC9), _mm_shuffle_ps(b, b, 0xD2));
        if (uk == U_OPMSUB) x = _mm_sub_ps(_mm_loadu_ps(acc->f), x);
        if (UNLIKELY(_mm_movemask_ps(_mm_cmpnle_ps(_mm_and_ps(x, K_ABS), K_FMAX)) & 7)) {       /* infinite or NaN: the reference code */
            u32 omac = v->mac, ostat = v->status;
            v->acc = *acc;
            if (uk == U_OPMULA) vu_opmula(v, ufs, uft); else vu_opmsub(v, udst, ufs, uft);
            *acc = v->acc;
            f->evx[lat - 1] = mac_to_vec(v->mac);
            v->mac = omac; v->status = ostat;
        } else {
            if (uk == U_OPMULA) v_store(acc, 14, _mm_castps_si128(x));
            else if (udst) v_store(&v->vf[udst], 14, _mm_castps_si128(x));
            f->evx[lat - 1] = _mm_or_ps(_mm_and_ps(x, _mm_loadu_ps(DMASK[14].f)), _mm_loadu_ps(NDONE[14].f));
        }
        f->ev[lat - 1] = EV_MACV;
        break;
    }
    default: rt_fatal(0, "VU: invalid upper instruction 0x%08x", raw_upper);
    }
}

/* Returns 1 with *target set when a branch/jump is taken. `rec`: see vi_wr. */
static inline __attribute__((always_inline))
int do_lower(int n, VU *v, Pipe *p, FlagRegs *f, u8 *const mem, const u32 dmask, int pr, u32 po, u32 pc, int lk, int it, int is, int id, int ldest, s32 limm, u32 ltarget, u32 lower, int rec, u32 *target) {
    switch (lk) {
    case L_NOP: return 0;
    case L_LQ:
        if (it) v_store(&v->vf[it], ldest, _mm_loadu_si128((const __m128i *)(mem + ((((u32)(limm + (s32)v->vi[is & 15])) * 16) & dmask))));
        return 0;
    case L_SQ: v_store((r128 *)(mem + ((((u32)(limm + (s32)v->vi[it & 15])) * 16) & dmask)), ldest, _mm_loadu_si128((const __m128i *)&v->vf[is])); return 0;
    case L_ILW: case L_ILWR: {
        u32 a = (((u32)((lk == L_ILW ? limm : 0) + (s32)v->vi[is & 15])) * 16) & dmask, val = v->vi[it & 15];
        for (int i = 0; i < 4; i++) if (ldest & (8 >> i)) { u16 h; memcpy(&h, mem + a + i * 4, 2); val = h; }
        vi_wr(v, p, it & 15, val, rec);
        return 0;
    }
    case L_ISW: case L_ISWR: {
        u32 a = (((u32)((lk == L_ISW ? limm : 0) + (s32)v->vi[is & 15])) * 16) & dmask, val = v->vi[it & 15];
        for (int i = 0; i < 4; i++) if (ldest & (8 >> i)) memcpy(mem + a + i * 4, &val, 4);
        return 0;
    }
    case L_IADDIU: vi_wr(v, p, it & 15, v->vi[is & 15] + (u32)limm, rec); return 0;
    case L_ISUBIU: vi_wr(v, p, it & 15, v->vi[is & 15] - (u32)limm, rec); return 0;
    case L_FCSET:                                       /* takes effect at once; clip results still in flight are dropped */
        v->clip = (u32)limm;
        if (vu_clip_chain) for (int k = 0; k < 4; k++) if (f->ev[k] & EV_CLIP) f->ev[k] = 0;
        return 0;
    case L_FCEQ:  VI_SET(v, 1, (v->clip & 0xFFFFFF) == (u32)limm); return 0;
    case L_FCAND: VI_SET(v, 1, (v->clip & (u32)limm) != 0); return 0;
    case L_FCOR:  VI_SET(v, 1, ((v->clip | (u32)limm) & 0xFFFFFF) == 0xFFFFFF); return 0;
    case L_FCGET: VI_SET(v, it & 15, v->clip & 0xFFF); return 0;
    case L_FSAND: status_sync(v, f); VI_SET(v, it & 15, v->status & (u32)limm); return 0;
    case L_FMAND: mac_sync(v, f); VI_SET(v, it & 15, v->mac & v->vi[is & 15]); return 0;
    case L_B:    *target = ltarget; return 1;
    case L_BAL:  VI_SET(v, it & 15, (pc + 16) / 8); *target = ltarget; return 1;
    case L_JR:   *target = vi_br(v, is & 15, pr, po) * 8; return 1;
    case L_JALR: { u32 t = vi_br(v, is & 15, pr, po) * 8; VI_SET(v, it & 15, (pc + 16) / 8); *target = t; return 1; }
    case L_IBEQ:  if (vi_br(v, it & 15, pr, po) == vi_br(v, is & 15, pr, po)) { *target = ltarget; return 1; } return 0;
    case L_IBNE:  if (vi_br(v, it & 15, pr, po) != vi_br(v, is & 15, pr, po)) { *target = ltarget; return 1; } return 0;
    case L_IBLTZ: if ((s16)vi_br(v, is & 15, pr, po) < 0)  { *target = ltarget; return 1; } return 0;
    case L_IBGTZ: if ((s16)vi_br(v, is & 15, pr, po) > 0)  { *target = ltarget; return 1; } return 0;
    case L_IBLEZ: if ((s16)vi_br(v, is & 15, pr, po) <= 0) { *target = ltarget; return 1; } return 0;
    case L_IBGEZ: if ((s16)vi_br(v, is & 15, pr, po) >= 0) { *target = ltarget; return 1; } return 0;
    case L_IADD:  vi_wr(v, p, id & 15, v->vi[is & 15] + v->vi[it & 15], rec); return 0;
    case L_ISUB:  vi_wr(v, p, id & 15, v->vi[is & 15] - v->vi[it & 15], rec); return 0;
    case L_IADDI: vi_wr(v, p, it & 15, v->vi[is & 15] + (u32)limm, rec); return 0;
    case L_IAND:  vi_wr(v, p, id & 15, v->vi[is & 15] & v->vi[it & 15], rec); return 0;
    case L_IOR:   vi_wr(v, p, id & 15, v->vi[is & 15] | v->vi[it & 15], rec); return 0;
    case L_MOVE: v_store(&v->vf[it], ldest, _mm_loadu_si128((const __m128i *)&v->vf[is])); return 0;
    case L_MR32: vu_mr32(v, (u32)ldest, it, is); return 0;
    case L_LQI: {                                       /* ft <- mem[is], is++ */
        u32 a = v->vi[is & 15];
        if (it) v_store(&v->vf[it], ldest, _mm_loadu_si128((const __m128i *)(mem + ((a * 16) & dmask))));
        vi_wr(v, p, is & 15, a + 1, rec);
        return 0;
    }
    case L_SQI: {
        u32 a = v->vi[it & 15];
        v_store((r128 *)(mem + ((a * 16) & dmask)), ldest, _mm_loadu_si128((const __m128i *)&v->vf[is]));
        vi_wr(v, p, it & 15, a + 1, rec);
        return 0;
    }
    case L_LQD: {                                       /* --is, ft <- mem[is] */
        u32 a = (is & 15) ? (v->vi[is & 15] - 1) & 0xFFFF : 0;
        if (it) v_store(&v->vf[it], ldest, _mm_loadu_si128((const __m128i *)(mem + ((a * 16) & dmask))));
        vi_wr(v, p, is & 15, a, rec);
        return 0;
    }
    case L_SQD: {
        u32 a = (it & 15) ? (v->vi[it & 15] - 1) & 0xFFFF : 0;
        v_store((r128 *)(mem + ((a * 16) & dmask)), ldest, _mm_loadu_si128((const __m128i *)&v->vf[is]));
        vi_wr(v, p, it & 15, a, rec);
        return 0;
    }
    case L_MTIR: vi_wr(v, p, it & 15, v->vf[is].u32[(lower >> 21) & 3], rec); return 0;
    case L_MFIR: if (it) v_store(&v->vf[it], ldest, _mm_set1_epi32((s32)(s16)v->vi[is & 15])); return 0;
    case L_XTOP:  VI_SET(v, it & 15, v->vi_top & 0x3FF); return 0;
    case L_XITOP: VI_SET(v, it & 15, v->vi_itop & (n ? 0x3FF : 0xFF)); return 0;
    case L_XGKICK: if (n) VU_KICK(mem, (v->vi[is & 15] * 16) & dmask, dmask); return 0;
    case L_SLOWF: status_sync(v, f);                 /* flag tests without a case above: they need MAC / status current */
        /* fall through */
    default:                                            /* everything else: the general decoder */
        return exec_lower(n, v, p, lower, pc, target, pr, po);
    }
}

/* One whole instruction. uk..ltarget as in Rec; ev = the flag pipeline stages;
 * popk: see flags_step; rec = keep the VI write record for a branch that may follow (and read the
 * previous one: every branch instruction is run with rec = 1, as is every
 * instruction that can be followed by one). */
#define VU_STEP(n, v, p, acc, f, popk, mem, dmask, lat, pc, raw, uk, usrc, ufs, uft, ubc, udst, udest, wr, mask16, scale, lk, lit, lis, lid, ldest, fl, limm, ltarget, rec, taken, target) do { \
    int pr_ = 0; u32 po_ = 0; \
    if (rec) { pr_ = (p)->vi_wr_reg; po_ = (p)->vi_wr_old; (p)->vi_wr_reg = 0; } \
    flags_step(v, f, popk); \
    if (UNLIKELY((p)->pend)) pend_step(v, p, fl); \
    /* Upper and lower execute together: the lower instruction sees the registers as they were before the \
     * upper one wrote, and if both write the same VF register the upper result wins. The upper's flag \
     * results enter the pipeline; the visible MAC/status/clip are not touched by it. */ \
    r128 old_vf_; \
    if ((fl) & F_SAVE) old_vf_ = (v)->vf[wr]; \
    do_upper(v, acc, f, lat, uk, usrc, ufs, uft, ubc, udst, udest, mask16, scale, (u32)((u64)(raw) >> 32)); \
    (taken) = 0; \
    if ((fl) & F_IBIT) (v)->i.u = (u32)(raw);                  /* I bit: lower word is a float literal */ \
    else if ((fl) & F_SAVE) { \
        r128 new_vf_ = (v)->vf[wr]; (v)->vf[wr] = old_vf_; \
        (taken) = do_lower(n, v, p, f, mem, dmask, pr_, po_, pc, lk, lit, lis, lid, ldest, limm, ltarget, (u32)(raw), rec, &(target)); \
        (v)->vf[wr] = new_vf_; \
    } else (taken) = do_lower(n, v, p, f, mem, dmask, pr_, po_, pc, lk, lit, lis, lid, ldest, limm, ltarget, (u32)(raw), rec, &(target)); \
} while (0)

/* ---- generated code for known micro programs ---------------------------------------
 * vu1_fast_gen.inc is written by `recomp/tools/vubench.sh gen` (run by
 * recomp/tools/gamefiles.sh during the build) from a micro memory image: one VU_STEP with constant arguments per instruction, in address
 * order, with direct gotos for static branches and a jump table for JR/JALR.
 * It is only entered while micro memory equals that image (vu1_image_ok) and
 * runs until
 *   return 0   the program ended (E bit); *ppc = where it stopped
 *   return 1   *ppc is an instruction it does not handle (a branch in a delay
 *              slot, an E bit on a branch, ...): the interpreter below runs
 *              instructions until it can be entered again. */
#define VU_GUARD_MAX 50000000ull
#if !defined(VU_NO_FAST) && defined(__has_include)
/* build/recomp/gen/vu1_fast_gen.inc, written at build time by recomp/tools/gamefiles.sh from the
 * user's own copy of the game; without it everything runs on the interpreter below. */
#if __has_include("vu1_fast_gen.inc")
#include "vu1_fast_gen.inc"
#define VU_HAVE_FAST 1
/* Compared after every vu_micro_changed(1) (VIF1 MPG) and once per frame. The
 * per-frame comparison is only a safety net for writes that arrive without
 * that call (the EE writing VU1 micro memory at 0x11008000 directly). */
static int vu1_image_ok(void) {
    static u64 frame = ~0ull;
    if (UNLIKELY(vu1_image_state == 0 || frame != g_gs_frames)) {
        vu1_image_state = memcmp(vu1_micro, vu1_fast_image, sizeof vu1_micro) ? -1 : 1;
        frame = g_gs_frames;
    }
    return vu1_image_state > 0;
}
#endif
#endif

#ifdef VU_STATS
u64 g_vu_icount, g_vu_ucount[16], g_vu_lcount[64], g_vu_bail[0x800];
#endif
static void vu_exec1(int n, u32 addr);
void vu_exec(int n, u32 addr) {
    if (LIKELY(!g_vu_stat) || !n) { vu_exec1(n, addr); return; }
    g_vu_stat_ticks[4]++;
    u64 t0 = __builtin_ia32_rdtsc();
    vu_exec1(n, addr);
    g_vu_stat_ticks[0] += __builtin_ia32_rdtsc() - t0;
}
static void vu_exec1(int n, u32 addr) {
    VU *v = n ? &g_vu1 : &g_vu0;
    Rec *recs = n ? vu1_recs : vu0_recs;
    Pipe *p = &vu_pipes[n];
    if (addr != 0xFFFFFFFFu) v->pc = addr;
    u32 pc = v->pc & v->micro_mask & ~7u;
    int ebit = 0, branch = 0; u32 branch_target = 0;
    const u32 mmask = v->micro_mask;
    const int lat = (int)FLAG_LATENCY;
    u8 *const mem = v->mem; const u32 dmask = v->mem_mask;
    u64 guard = 0;
    if (UNLIKELY((_mm_getcsr() & 0x8040) != 0x8040)) rt_fatal(0, "VU: MXCSR must have FTZ and DAZ set");
    flags_clear(p); p->pend = 0; p->vi_wr_reg = 0;
    vu_tracing = vu_force_trace;
    if (n && g_gs_frames >= vu_dump_frame && !vu_dump_k) vu_dump_frame = g_gs_frames;   /* first frame with VU1 activity */
    if (n && g_gs_frames >= vu_dump_frame && g_gs_frames < vu_dump_frame + vu_dump_nframes) {
        vu_dump_start(v, pc);
        vu_tracing = (vu_dump_k == vu_trace_k);
        vu_dump_k++;
    }
#ifdef VU_HAVE_FAST
    int fast = n == 1 && vu_use_fast && !vu_tracing && lat == 4 && v->mem == vu1_mem && v->micro == vu1_micro && vu1_image_ok();
#endif
    for (;;) {
#ifdef VU_HAVE_FAST
        if (fast && !branch && !ebit) {
            if (vu1_fast(&pc, &guard) == 0) break;
#ifdef VU_STATS
            g_vu_bail[pc >> 3]++;
#endif
        }
#endif
        if (UNLIKELY(++guard > VU_GUARD_MAX)) rt_fatal(0, "VU%d micro program does not terminate (pc 0x%x)", n, pc);
        Rec *r = &recs[pc >> 3];
        u64 raw; memcpy(&raw, v->micro + pc, 8);
        if (UNLIKELY(raw != r->raw)) predecode(r, (u32)raw, (u32)(raw >> 32), pc);
#ifdef VU_STATS
        g_vu_icount++; g_vu_ucount[r->uk]++; g_vu_lcount[(r->fl & F_IBIT) ? 63 : r->lk]++;
#endif
        int taken; u32 target = 0;
        VU_STEP(n, v, p, &v->acc, &p->fr, POP_DYN, mem, dmask, lat, pc, r->raw, r->uk, r->usrc, r->ufs, r->uft, r->ubc, r->udst, r->udest, r->wr, r->mask16, r->scale,
                r->lk, r->lit, r->lis, r->lid, r->ldest, r->fl, r->limm, r->ltarget, 1, taken, target);

        if (UNLIKELY(vu_tracing)) {
            u32 upper = (u32)(r->raw >> 32), lower = (u32)r->raw; int wr = r->wr;
            status_sync(v, &p->fr);
            fprintf(stderr, "VUT %04x %08x %08x st=%03x mac=%04x clip=%06x q=%g vi:", pc, upper, lower, v->status, v->mac, v->clip, v->q.f);
            for (int i = 1; i < 16; i++) fprintf(stderr, " %x", v->vi[i]);
            if (wr) fprintf(stderr, " | vf%d=(%g %g %g %g)[%08x %08x %08x %08x]", wr, v->vf[wr].f[0], v->vf[wr].f[1], v->vf[wr].f[2], v->vf[wr].f[3], v->vf[wr].u32[0], v->vf[wr].u32[1], v->vf[wr].u32[2], v->vf[wr].u32[3]);
            fprintf(stderr, " acc=(%g %g %g %g)\n", v->acc.f[0], v->acc.f[1], v->acc.f[2], v->acc.f[3]);
        }
        u32 next = (pc + 8) & mmask;
        if (branch) { next = branch_target & mmask & ~7u; branch = 0; }
        if (taken) { branch = 1; branch_target = target; }
        pc = next;
        if (ebit) break;
        if (r->fl & F_EBIT) ebit = 1;
    }
    flags_flush(v, p); q_land(v, p); p_land(v, p);
    v->pc = pc;
}
void vu0_callms(Ctx *c, u32 addr) { (void)c; vu_exec(0, addr); }
