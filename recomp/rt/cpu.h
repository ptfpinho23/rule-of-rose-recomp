/* cpu.h — R5900 CPU state and instruction semantics shared by the
 * recompiled code and the reference interpreter. One definition of each
 * instruction's behaviour lives here (or in the templates in sem.py that
 * call into here). */
#ifndef ROR_CPU_H
#define ROR_CPU_H
#include <stdint.h>
#include <string.h>
#include <math.h>

typedef uint8_t u8;   typedef int8_t s8;
typedef uint16_t u16; typedef int16_t s16;
typedef uint32_t u32; typedef int32_t s32;
typedef uint64_t u64; typedef int64_t s64;

#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

typedef union {
    u64 u64[2]; s64 s64[2];
    u32 u32[4]; s32 s32[4];
    u16 u16[8]; s16 s16[8];
    u8  u8[16]; s8  s8[16];
    float f[4];
} r128;

typedef union { float f; u32 u; s32 s; } fpr_t;

/* VU register file (VU0 here; the same struct is used for VU1). */
typedef struct VU {
    r128 vf[32];          /* vf[0] = (0,0,0,1) constant */
    u32  vi[16];          /* 16-bit integer regs, vi[0] = 0 */
    r128 acc;
    fpr_t q, p, i;
    u32  r;               /* random register */
    u32  status, mac, clip;
    u32  fbrst, vpu_stat, cmsar0, cmsar1, tpc;
    u32  vi_top, vi_itop;         /* TOP / ITOP latched by VIF when a program starts */
    u32  pc;                      /* micro program counter (bytes) */
    u8  *mem;   u32 mem_mask;     /* data memory  (VU0 4K, VU1 16K) */
    u8  *micro; u32 micro_mask;   /* micro memory */
} VU;

typedef struct Ctx {
    r128 r[32];           /* GPRs, 128-bit */
    r128 hi, lo;          /* [0] = HI/LO, [1] = HI1/LO1 */
    u32  sa;              /* shift amount register (bytes) */
    u32  pc;              /* set at calls/returns/syscalls */
    fpr_t fpr[32];
    fpr_t facc;
    u32  fcr31;
    s64  cycles;          /* countdown to next rt_tick */
    u64  total_cycles;
    void *thread;         /* runtime thread owning this context */
} Ctx;

extern VU g_vu0;              /* VU0 is hardware state shared by all threads */

typedef void (*recomp_fn)(Ctx *);

/* ---- guest memory -------------------------------------------------------
 * The whole 32-bit guest address space is reserved at a fixed host base, so
 * a guest address maps to host = MEMBASE + addr. RAM mirrors are aliased by
 * the runtime; hardware registers (0x10000000-0x13FFFFFF) go through hw_*. */
#define MEMBASE 0x200000000ull
#define HPTR(a) ((u8 *)(MEMBASE + (u64)(u32)(a)))
/* Cheap filter for hardware-register addresses, in kuseg (0x1xxxxxxx) or
 * kseg1 (0xBxxxxxxx). It also catches a few RAM mirrors (0x3xxxxxxx); the
 * hw_* functions sort those out and fall back to plain memory. */
#define IS_HW(a) (((u32)(a) & 0x5C000000u) == 0x10000000u)

u64  hw_read(Ctx *c, u32 a, int size);
void hw_write(Ctx *c, u32 a, u64 v, int size);
void hw_write128(Ctx *c, u32 a, const r128 *v);
void hw_read128(Ctx *c, u32 a, r128 *v);

#define DEF_RD(name, T, hw) \
static inline T name(Ctx *c, u32 a) { \
    if (UNLIKELY(IS_HW(a))) return (T)hw; \
    T v; memcpy(&v, HPTR(a), sizeof v); return v; }
#define DEF_WR(name, T, hw) \
static inline void name(Ctx *c, u32 a, T v) { \
    if (UNLIKELY(IS_HW(a))) { hw; return; } \
    memcpy(HPTR(a), &v, sizeof v); }
DEF_RD(mr8,  u8,  hw_read(c, a, 1))
DEF_RD(mr16, u16, hw_read(c, a, 2))
DEF_RD(mr32, u32, hw_read(c, a, 4))
DEF_RD(mr64, u64, hw_read(c, a, 8))
DEF_WR(mw8,  u8,  hw_write(c, a, v, 1))
DEF_WR(mw16, u16, hw_write(c, a, v, 2))
DEF_WR(mw32, u32, hw_write(c, a, v, 4))
DEF_WR(mw64, u64, hw_write(c, a, v, 8))
static inline void mr128(Ctx *c, u32 a, r128 *v) {
    a &= ~15u;
    if (UNLIKELY(IS_HW(a))) { hw_read128(c, a, v); return; }
    memcpy(v, HPTR(a), 16);
}
static inline void mw128(Ctx *c, u32 a, const r128 *v) {
    a &= ~15u;
    if (UNLIKELY(IS_HW(a))) { hw_write128(c, a, v); return; }
    memcpy(HPTR(a), v, 16);
}

/* ---- function-entry hook (widescreen, rt/wide.c) --------------------------
 * gen.py puts TRACE_FN(entry) at the top of every generated function. The
 * address is a constant, so the test folds away everywhere but in the hooked
 * function. A hook that returns non-zero has performed the whole call. */
#ifndef TRACE_FN
struct Ctx;
int wide_hook(struct Ctx *c, u32 entry);
#define TRACE_FN(a) do { if ((a) == 0x228b80u && wide_hook(c, (a))) return; } while (0)
#endif

/* ---- runtime services called from generated code ------------------------ */
void rt_syscall(Ctx *c);
void rt_break(Ctx *c, u32 pc);
void rt_trap(Ctx *c, u32 pc);
void rt_invalid(Ctx *c, u32 pc);            /* undecodable / non-code address */
void rt_tick(Ctx *c);                       /* cycles expired: events, irqs   */
recomp_fn rt_lookup(Ctx *c, u32 addr);      /* never NULL: returns a stub that
                                               leaves c->pc for the unwinder  */
u32  rt_mfc0(Ctx *c, int reg);
void rt_mtc0(Ctx *c, int reg, u32 v);
void rt_ei(Ctx *c);
void rt_di(Ctx *c);
u32  rt_eret(Ctx *c, u32 pc);                /* returns the new pc */
int  rt_cpcond0(Ctx *c);
void rt_trace_call(Ctx *c, u32 from, u32 target);
void vu0_callms(Ctx *c, u32 addr);

/* ---- GPR access ---------------------------------------------------------- */
#define GD(n)  (c->r[n].u64[0])
#define GS(n)  (c->r[n].s64[0])
#define GU(n)  (c->r[n].u32[0])
#define GW(n)  (c->r[n].s32[0])
#define SD(n, v) do { u64 v__ = (u64)(v); if (n) c->r[n].u64[0] = v__; } while (0)
#define SW(n, v) SD(n, (s64)(s32)(v))
#define SX32(v) ((u64)(s64)(s32)(v))

#define TICK(n) do { if (UNLIKELY((c->cycles -= (n)) < 0)) rt_tick(c); } while (0)
/* The branch back in a loop that only polls the COP0 Status register (gen.py, spin_loop; the game's idle thread):
 * every iteration costs n cycles and changes nothing until the counter runs out, so all the iterations before the
 * one in which it does are taken at once. The counter and everything else are then exactly what running them one
 * by one gives. */
#define TICK_SPIN(n) do { if (c->cycles >= (n)) c->cycles %= (n); TICK(n); } while (0)

/* ---- unaligned loads/stores (little-endian) ------------------------------ */
static inline void op_lwl(Ctx *c, int rt, u32 a) {
    u32 sh = (a & 3) * 8, m = mr32(c, a & ~3u);
    u32 v = (GU(rt) & (0x00FFFFFFu >> sh)) | (m << (24 - sh));
    SW(rt, v);
}
static inline void op_lwr(Ctx *c, int rt, u32 a) {
    u32 sh = (a & 3) * 8, m = mr32(c, a & ~3u);
    if (sh == 0) { SW(rt, m); }   /* full word: sign-extends */
    else if (rt) c->r[rt].u32[0] = (GU(rt) & (0xFFFFFF00u << (24 - sh))) | (m >> sh);
}
static inline void op_swl(Ctx *c, int rt, u32 a) {
    u32 sh = (a & 3) * 8, m = mr32(c, a & ~3u);
    mw32(c, a & ~3u, (GU(rt) >> (24 - sh)) | (m & (0xFFFFFF00u << sh)));
}
static inline void op_swr(Ctx *c, int rt, u32 a) {
    u32 sh = (a & 3) * 8, m = mr32(c, a & ~3u);
    mw32(c, a & ~3u, (GU(rt) << sh) | (m & (0x00FFFFFFu >> (24 - sh))));
}
static inline void op_ldl(Ctx *c, int rt, u32 a) {
    u32 sh = (a & 7) * 8; u64 m = mr64(c, a & ~7u);
    SD(rt, (GD(rt) & (0x00FFFFFFFFFFFFFFull >> sh)) | (m << (56 - sh)));
}
static inline void op_ldr(Ctx *c, int rt, u32 a) {
    u32 sh = (a & 7) * 8; u64 m = mr64(c, a & ~7u);
    SD(rt, sh == 0 ? m : ((GD(rt) & (0xFFFFFFFFFFFFFF00ull << (56 - sh))) | (m >> sh)));
}
static inline void op_sdl(Ctx *c, int rt, u32 a) {
    u32 sh = (a & 7) * 8; u64 m = mr64(c, a & ~7u);
    mw64(c, a & ~7u, (GD(rt) >> (56 - sh)) | (m & (0xFFFFFFFFFFFFFF00ull << sh)));
}
static inline void op_sdr(Ctx *c, int rt, u32 a) {
    u32 sh = (a & 7) * 8; u64 m = mr64(c, a & ~7u);
    mw64(c, a & ~7u, sh == 0 ? GD(rt) : ((GD(rt) << sh) | (m & (0x00FFFFFFFFFFFFFFull >> (56 - sh)))));
}

/* ---- multiply / divide (n = 0: HI/LO, n = 1: HI1/LO1) -------------------- */
static inline void op_mult(Ctx *c, int n, int rd, int rs, int rt) {
    s64 t = (s64)GW(rs) * (s64)GW(rt);
    c->lo.u64[n] = SX32(t); c->hi.u64[n] = SX32(t >> 32);
    SD(rd, c->lo.u64[n]);
}
static inline void op_multu(Ctx *c, int n, int rd, int rs, int rt) {
    u64 t = (u64)GU(rs) * (u64)GU(rt);
    c->lo.u64[n] = SX32(t); c->hi.u64[n] = SX32(t >> 32);
    SD(rd, c->lo.u64[n]);
}
static inline void op_madd(Ctx *c, int n, int rd, int rs, int rt) {
    s64 acc = (s64)(((u64)c->hi.u32[n * 2] << 32) | c->lo.u32[n * 2]);
    s64 t = acc + (s64)GW(rs) * (s64)GW(rt);
    c->lo.u64[n] = SX32(t); c->hi.u64[n] = SX32(t >> 32);
    SD(rd, c->lo.u64[n]);
}
static inline void op_maddu(Ctx *c, int n, int rd, int rs, int rt) {
    u64 acc = ((u64)c->hi.u32[n * 2] << 32) | c->lo.u32[n * 2];
    u64 t = acc + (u64)GU(rs) * (u64)GU(rt);
    c->lo.u64[n] = SX32(t); c->hi.u64[n] = SX32(t >> 32);
    SD(rd, c->lo.u64[n]);
}
static inline void op_div(Ctx *c, int n, int rs, int rt) {
    s32 a = GW(rs), b = GW(rt);
    if (b == 0) { c->lo.u64[n] = (a < 0) ? 1 : (u64)-1; c->hi.u64[n] = SX32(a); }
    else if (a == INT32_MIN && b == -1) { c->lo.u64[n] = SX32(INT32_MIN); c->hi.u64[n] = 0; }
    else { c->lo.u64[n] = SX32(a / b); c->hi.u64[n] = SX32(a % b); }
}
static inline void op_divu(Ctx *c, int n, int rs, int rt) {
    u32 a = GU(rs), b = GU(rt);
    if (b == 0) { c->lo.u64[n] = (u64)-1; c->hi.u64[n] = SX32(a); }
    else { c->lo.u64[n] = SX32(a / b); c->hi.u64[n] = SX32(a % b); }
}

/* ---- MMI (128-bit parallel) ---------------------------------------------- */
#define WR128(rd, o) do { if (rd) c->r[rd] = (o); } while (0)
#define MMI_ELEM(name, fld, n, expr) \
static inline void mmi_##name(Ctx *c, int rd, int rs, int rt) { \
    r128 a = c->r[rs], b = c->r[rt], o; (void)a; (void)b; \
    for (int i = 0; i < n; i++) o.fld[i] = (expr); \
    WR128(rd, o); }
static inline s32 sat_s32(s64 v) { return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (s32)v; }
static inline s16 sat_s16(s32 v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (s16)v; }
static inline s8  sat_s8(s32 v)  { return v > 127 ? 127 : v < -128 ? -128 : (s8)v; }
MMI_ELEM(paddw, u32, 4, a.u32[i] + b.u32[i])
MMI_ELEM(psubw, u32, 4, a.u32[i] - b.u32[i])
MMI_ELEM(pcgtw, u32, 4, a.s32[i] > b.s32[i] ? 0xFFFFFFFFu : 0)
MMI_ELEM(pceqw, u32, 4, a.u32[i] == b.u32[i] ? 0xFFFFFFFFu : 0)
MMI_ELEM(pmaxw, s32, 4, a.s32[i] > b.s32[i] ? a.s32[i] : b.s32[i])
MMI_ELEM(pminw, s32, 4, a.s32[i] < b.s32[i] ? a.s32[i] : b.s32[i])
MMI_ELEM(paddh, u16, 8, (u16)(a.u16[i] + b.u16[i]))
MMI_ELEM(psubh, u16, 8, (u16)(a.u16[i] - b.u16[i]))
MMI_ELEM(pcgth, u16, 8, a.s16[i] > b.s16[i] ? 0xFFFF : 0)
MMI_ELEM(pceqh, u16, 8, a.u16[i] == b.u16[i] ? 0xFFFF : 0)
MMI_ELEM(pmaxh, s16, 8, a.s16[i] > b.s16[i] ? a.s16[i] : b.s16[i])
MMI_ELEM(pminh, s16, 8, a.s16[i] < b.s16[i] ? a.s16[i] : b.s16[i])
MMI_ELEM(paddb, u8, 16, (u8)(a.u8[i] + b.u8[i]))
MMI_ELEM(psubb, u8, 16, (u8)(a.u8[i] - b.u8[i]))
MMI_ELEM(pcgtb, u8, 16, a.s8[i] > b.s8[i] ? 0xFF : 0)
MMI_ELEM(pceqb, u8, 16, a.u8[i] == b.u8[i] ? 0xFF : 0)
MMI_ELEM(paddsw, s32, 4, sat_s32((s64)a.s32[i] + b.s32[i]))
MMI_ELEM(psubsw, s32, 4, sat_s32((s64)a.s32[i] - b.s32[i]))
MMI_ELEM(paddsh, s16, 8, sat_s16((s32)a.s16[i] + b.s16[i]))
MMI_ELEM(psubsh, s16, 8, sat_s16((s32)a.s16[i] - b.s16[i]))
MMI_ELEM(paddsb, s8, 16, sat_s8((s32)a.s8[i] + b.s8[i]))
MMI_ELEM(psubsb, s8, 16, sat_s8((s32)a.s8[i] - b.s8[i]))
MMI_ELEM(padduw, u32, 4, (u64)a.u32[i] + b.u32[i] > 0xFFFFFFFFull ? 0xFFFFFFFFu : a.u32[i] + b.u32[i])
MMI_ELEM(psubuw, u32, 4, a.u32[i] < b.u32[i] ? 0 : a.u32[i] - b.u32[i])
MMI_ELEM(padduh, u16, 8, a.u16[i] + b.u16[i] > 0xFFFF ? 0xFFFF : (u16)(a.u16[i] + b.u16[i]))
MMI_ELEM(psubuh, u16, 8, a.u16[i] < b.u16[i] ? 0 : (u16)(a.u16[i] - b.u16[i]))
MMI_ELEM(paddub, u8, 16, a.u8[i] + b.u8[i] > 0xFF ? 0xFF : (u8)(a.u8[i] + b.u8[i]))
MMI_ELEM(psubub, u8, 16, a.u8[i] < b.u8[i] ? 0 : (u8)(a.u8[i] - b.u8[i]))
MMI_ELEM(pabsw, s32, 4, b.s32[i] == INT32_MIN ? INT32_MAX : b.s32[i] < 0 ? -b.s32[i] : b.s32[i])
MMI_ELEM(pabsh, s16, 8, b.s16[i] == -32768 ? 32767 : b.s16[i] < 0 ? -b.s16[i] : b.s16[i])
MMI_ELEM(padsbh, u16, 8, i < 4 ? (u16)(a.u16[i] - b.u16[i]) : (u16)(a.u16[i] + b.u16[i]))
MMI_ELEM(pand, u64, 2, a.u64[i] & b.u64[i])
MMI_ELEM(por,  u64, 2, a.u64[i] | b.u64[i])
MMI_ELEM(pxor, u64, 2, a.u64[i] ^ b.u64[i])
MMI_ELEM(pnor, u64, 2, ~(a.u64[i] | b.u64[i]))
MMI_ELEM(pextlw, u32, 4, (i & 1) ? a.u32[i >> 1] : b.u32[i >> 1])
MMI_ELEM(pextuw, u32, 4, (i & 1) ? a.u32[2 + (i >> 1)] : b.u32[2 + (i >> 1)])
MMI_ELEM(pextlh, u16, 8, (i & 1) ? a.u16[i >> 1] : b.u16[i >> 1])
MMI_ELEM(pextuh, u16, 8, (i & 1) ? a.u16[4 + (i >> 1)] : b.u16[4 + (i >> 1)])
MMI_ELEM(pextlb, u8, 16, (i & 1) ? a.u8[i >> 1] : b.u8[i >> 1])
MMI_ELEM(pextub, u8, 16, (i & 1) ? a.u8[8 + (i >> 1)] : b.u8[8 + (i >> 1)])
MMI_ELEM(ppacw, u32, 4, i < 2 ? b.u32[i * 2] : a.u32[(i - 2) * 2])
MMI_ELEM(ppach, u16, 8, i < 4 ? b.u16[i * 2] : a.u16[(i - 4) * 2])
MMI_ELEM(ppacb, u8, 16, i < 8 ? b.u8[i * 2] : a.u8[(i - 8) * 2])
MMI_ELEM(pext5, u32, 4, ((b.u32[i] & 0x1F) << 3) | ((b.u32[i] & 0x3E0) << 6) | ((b.u32[i] & 0x7C00) << 9) | ((b.u32[i] & 0x8000) << 16))
MMI_ELEM(ppac5, u32, 4, ((b.u32[i] >> 3) & 0x1F) | ((b.u32[i] >> 6) & 0x3E0) | ((b.u32[i] >> 9) & 0x7C00) | ((b.u32[i] >> 16) & 0x8000))
MMI_ELEM(pcpyld, u64, 2, i == 0 ? b.u64[0] : a.u64[0])
MMI_ELEM(pcpyud, u64, 2, i == 0 ? a.u64[1] : b.u64[1])
MMI_ELEM(pcpyh, u16, 8, i < 4 ? b.u16[0] : b.u16[4])
MMI_ELEM(pinth, u16, 8, (i & 1) ? a.u16[4 + (i >> 1)] : b.u16[i >> 1])
MMI_ELEM(pinteh, u16, 8, (i & 1) ? a.u16[i - 1] : b.u16[i])
MMI_ELEM(pexeh, u16, 8, b.u16[(i & 4) | ((i & 3) == 0 ? 2 : (i & 3) == 2 ? 0 : (i & 3))])
MMI_ELEM(prevh, u16, 8, b.u16[(i & 4) | (3 - (i & 3))])
MMI_ELEM(pexch, u16, 8, b.u16[(i & 4) | ((i & 3) == 1 ? 2 : (i & 3) == 2 ? 1 : (i & 3))])
MMI_ELEM(pexew, u32, 4, b.u32[i == 0 ? 2 : i == 2 ? 0 : i])
MMI_ELEM(pexcw, u32, 4, b.u32[i == 1 ? 2 : i == 2 ? 1 : i])
MMI_ELEM(prot3w, u32, 4, b.u32[i == 0 ? 1 : i == 1 ? 2 : i == 2 ? 0 : 3])
MMI_ELEM(psllvw, u64, 2, SX32(b.u32[i * 2] << (a.u32[i * 2] & 31)))
MMI_ELEM(psrlvw, u64, 2, SX32(b.u32[i * 2] >> (a.u32[i * 2] & 31)))
MMI_ELEM(psravw, u64, 2, SX32(b.s32[i * 2] >> (a.u32[i * 2] & 31)))
#define MMI_SHIFT(name, fld, n, expr) \
static inline void mmi_##name(Ctx *c, int rd, int rt, int sa) { \
    r128 b = c->r[rt], o; for (int i = 0; i < n; i++) o.fld[i] = (expr); WR128(rd, o); }
MMI_SHIFT(psllh, u16, 8, (u16)(b.u16[i] << (sa & 15)))
MMI_SHIFT(psrlh, u16, 8, (u16)(b.u16[i] >> (sa & 15)))
MMI_SHIFT(psrah, s16, 8, (s16)(b.s16[i] >> (sa & 15)))
MMI_SHIFT(psllw, u32, 4, b.u32[i] << sa)
MMI_SHIFT(psrlw, u32, 4, b.u32[i] >> sa)
MMI_SHIFT(psraw, s32, 4, b.s32[i] >> sa)
static inline u32 plzc32(u32 v) {     /* leading sign bits, minus one */
    if (v & 0x80000000u) v = ~v;
    return (v == 0 ? 32 : (u32)__builtin_clz(v)) - 1;
}
static inline void mmi_plzcw(Ctx *c, int rd, int rs) {
    if (rd) { u32 a = plzc32(c->r[rs].u32[0]), b = plzc32(c->r[rs].u32[1]);
              c->r[rd].u32[0] = a; c->r[rd].u32[1] = b; }
}
static inline void mmi_qfsrv(Ctx *c, int rd, int rs, int rt) {
    r128 a = c->r[rs], b = c->r[rt], o; u32 sh = (c->sa & 15) * 8;
    if (sh == 0) o = b;
    else if (sh < 64)  { o.u64[0] = (b.u64[0] >> sh) | (b.u64[1] << (64 - sh));
                         o.u64[1] = (b.u64[1] >> sh) | (a.u64[0] << (64 - sh)); }
    else if (sh == 64) { o.u64[0] = b.u64[1]; o.u64[1] = a.u64[0]; }
    else { o.u64[0] = (b.u64[1] >> (sh - 64)) | (a.u64[0] << (128 - sh));
           o.u64[1] = (a.u64[0] >> (sh - 64)) | (a.u64[1] << (128 - sh)); }
    WR128(rd, o);
}

/* ---- COP1 (FPU). The R5900 FPU has no NaN/Inf/denormals: results saturate
 * to +-FLT_MAX and tiny values flush to zero (host runs with FTZ/DAZ). ---- */
#define FCR_C 0x00800000u
#define FCR_I 0x00020000u
#define FCR_D 0x00010000u
#define FCR_O 0x00008000u
#define FCR_U 0x00004000u
#define FCR_SI 0x40u
#define FCR_SD 0x20u
#define FCR_SO 0x10u
#define FCR_SU 0x08u
static inline float fclamp(float x) {
    fpr_t v; v.f = x;
    if ((v.u & 0x7F800000u) == 0x7F800000u) v.u = (v.u & 0x80000000u) | 0x7F7FFFFFu;
    return v.f;
}
#define FS_ (fclamp(c->fpr[fs].f))
#define FT_ (fclamp(c->fpr[ft].f))
#define FACC_ (fclamp(c->facc.f))
static inline float fres(Ctx *c, float x) {      /* arithmetic result fixup */
    fpr_t v; v.f = x;
    if (UNLIKELY((v.u & 0x7F800000u) == 0x7F800000u)) {
        v.u = (v.u & 0x80000000u) | 0x7F7FFFFFu;
        c->fcr31 |= FCR_O | FCR_SO;
    }
    return v.f;
}
static inline void fpu_div(Ctx *c, int fd, int fs, int ft) {
    fpr_t a = c->fpr[fs], b = c->fpr[ft];
    c->fcr31 &= ~(FCR_I | FCR_D);
    if ((b.u & 0x7F800000u) == 0) {
        if ((a.u & 0x7F800000u) == 0) c->fcr31 |= FCR_I | FCR_SI;
        else c->fcr31 |= FCR_D | FCR_SD;
        c->fpr[fd].u = ((a.u ^ b.u) & 0x80000000u) | 0x7F7FFFFFu;
    } else c->fpr[fd].f = fres(c, fclamp(a.f) / fclamp(b.f));
}
static inline void fpu_sqrt(Ctx *c, int fd, int ft) {
    fpr_t b = c->fpr[ft];
    c->fcr31 &= ~(FCR_I | FCR_D);
    if ((b.u & 0x7F800000u) == 0) { c->fpr[fd].u = b.u & 0x80000000u; return; }
    if (b.u & 0x80000000u) c->fcr31 |= FCR_I | FCR_SI;
    c->fpr[fd].f = sqrtf(fabsf(fclamp(b.f)));
}
static inline void fpu_rsqrt(Ctx *c, int fd, int fs, int ft) {
    fpr_t a = c->fpr[fs], b = c->fpr[ft];
    c->fcr31 &= ~(FCR_I | FCR_D);
    if ((b.u & 0x7F800000u) == 0) {
        c->fcr31 |= FCR_D | FCR_SD;
        c->fpr[fd].u = ((a.u ^ b.u) & 0x80000000u) | 0x7F7FFFFFu;
        return;
    }
    if (b.u & 0x80000000u) c->fcr31 |= FCR_I | FCR_SI;
    c->fpr[fd].f = fres(c, fclamp(a.f) / sqrtf(fabsf(fclamp(b.f))));
}
static inline s32 fpu_cvtw(fpr_t a) {
    if ((a.u & 0x7F800000u) <= 0x4E800000u) return (s32)a.f;   /* |x| < 2^31 */
    return (a.u & 0x80000000u) ? INT32_MIN : INT32_MAX;
}
#define FCMP(cond) do { if (cond) c->fcr31 |= FCR_C; else c->fcr31 &= ~FCR_C; } while (0)

#include "vu_ops.h"

#endif
