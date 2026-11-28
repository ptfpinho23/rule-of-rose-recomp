/* hw.c — EE hardware registers (physical 0x10000000-0x13FFFFFF, reachable
 * through kuseg and kseg1). */
#include "rt.h"
#include "gs.h"
#include "vu.h"

/* Plain storage for registers with no modelled side effects: a read
 * returns the last value written. */
static u32 regstore[0x10000 / 4];
#define REG(a) regstore[((a) & 0xFFFF) >> 2]
static u8 seen[0x4000];

static void note(const char *what, u32 a, u64 v) {
    u32 k = (a ^ (a >> 14)) & 0x3FFF;
    if (seen[k]) return;
    seen[k] = 1;
    rt_log("[hw] unhandled %s 0x%08x (value 0x%llx) pc=0x%x\n", what, a, (unsigned long long)v,
           kernel_cur_ctx()->pc);
}

/* Returns 1 and the physical address if `a` is a hardware register. */
static inline int hw_phys(u32 *a) {
    u32 top = *a >> 28;
    if (top == 0x1) return 1;
    if (top == 0xB) { *a -= 0xA0000000u; return 1; }
    return 0;                                 /* a RAM mirror: plain memory */
}

/* ---- timers T0..T3 -------------------------------------------------------
 * 16-bit counters clocked from the bus clock (147.456 MHz, optionally /16 or
 * /256) or from H-blank. Counts are derived from the virtual clock. */
#define TICKS_PER_SEC (TICKS_PER_FRAME * VIDEO_HZ_NUM / VIDEO_HZ_DEN)
typedef struct { u32 mode, comp, hold, base_count; u64 base_time; } Timer;
static Timer tm[4];
#define TM_CUE 0x80u
#define TM_ZRET 0x40u
#define TM_CMPE 0x100u
#define TM_OVFE 0x200u
#define TM_EQUF 0x400u
#define TM_OVFF 0x800u

static void tm_rate(const Timer *t, u64 *num, u64 *den) {
    switch (t->mode & 3) {
    case 0: *num = 147456000ull; *den = TICKS_PER_SEC; break;
    case 1: *num = 147456000ull / 16; *den = TICKS_PER_SEC; break;
    case 2: *num = 147456000ull / 256; *den = TICKS_PER_SEC; break;
    default: *num = 1; *den = TICKS_PER_HSYNC; break;
    }
}
static u64 tm_clocks(const Timer *t, u64 now) {
    if (!(t->mode & TM_CUE) || now <= t->base_time) return 0;
    u64 num, den; tm_rate(t, &num, &den);
    return (now - t->base_time) * num / den;
}
static u32 tm_count(const Timer *t, u64 now) { return (u32)((t->base_count + tm_clocks(t, now)) & 0xFFFF); }
static void tm_rebase(Timer *t, u64 now) { t->base_count = tm_count(t, now); t->base_time = now; }
/* Source clocks from the base until the next compare match / overflow. */
static u64 tm_next_clocks(const Timer *t, int *is_cmp) {
    u64 ovf = 0x10000u - t->base_count;
    u64 cmp = ((t->comp & 0xFFFF) - t->base_count) & 0xFFFF;
    if (cmp == 0) cmp = 0x10000;
    int want_cmp = (t->mode & (TM_CMPE | TM_ZRET)) != 0;
    int want_ovf = (t->mode & TM_OVFE) != 0;
    if (want_cmp && (!want_ovf || cmp <= ovf)) { *is_cmp = 1; return cmp; }
    if (want_ovf) { *is_cmp = 0; return ovf; }
    *is_cmp = -1; return 0;
}
static u64 tm_event_time(const Timer *t) {
    if (!(t->mode & TM_CUE)) return ~0ull;
    int is_cmp; u64 n = tm_next_clocks(t, &is_cmp);
    if (is_cmp < 0) return ~0ull;
    u64 num, den; tm_rate(t, &num, &den);
    return t->base_time + (n * den + num - 1) / num;
}
u64 hw_next_event(void) {
    u64 best = ~0ull;
    for (int i = 0; i < 4; i++) { u64 e = tm_event_time(&tm[i]); if (e < best) best = e; }
    return best;
}
void hw_fire(u64 now) {
    for (int i = 0; i < 4; i++) {
        Timer *t = &tm[i];
        for (int guard = 0; guard < 64 && tm_event_time(t) <= now; guard++) {
            int is_cmp; u64 n = tm_next_clocks(t, &is_cmp);
            u64 when = tm_event_time(t);
            t->base_count = (u32)((t->base_count + n) & 0xFFFF);
            t->base_time = when;
            if (is_cmp) {
                if (t->mode & TM_ZRET) t->base_count = 0;
                if ((t->mode & TM_CMPE) && !(t->mode & TM_EQUF)) intc_raise(INTC_TIM0 + i);
                if (t->mode & TM_CMPE) t->mode |= TM_EQUF;
            } else {
                if (!(t->mode & TM_OVFF)) intc_raise(INTC_TIM0 + i);
                t->mode |= TM_OVFF;
            }
        }
    }
}
static int tm_reg(u32 a, int *idx, int *reg) {
    if (a < 0x10000000u || a >= 0x10002000u || (a & 0x7CF)) return 0;
    *idx = (int)((a >> 11) & 3); *reg = (int)((a >> 4) & 3);
    return 1;
}

u64  ipu_read(u32 a);
void ipu_write(u32 a, u64 v);
void ipu_fifo_write(const u8 *qw);
int  ipu_out_pull(u8 *qw);
void ipu_init(void);
u32  dmac_read(u32 a);
void dmac_write(u32 a, u32 v);
void video_present(void);

void hw_init(void) { gs_init(); vu_init(); ipu_init(); vif_reset(0); vif_reset(1); gif_reset(); }
void hw_vblank(int start) {
    gs_vblank(start);
    if (start) video_present();
}

/* VU memories are visible at 0x11000000: VU0 micro, VU0 data, VU1 micro, VU1 data. */
static u8 *vu_mem_ptr(u32 a) {
    if (a < 0x11000000u || a >= 0x11010000u) return 0;
    u32 o = a - 0x11000000u;
    if (o < 0x4000) return g_vu0.micro + (o & 0xFFF);
    if (o < 0x8000) return g_vu0.mem + (o & 0xFFF);
    if (o < 0xC000) { vu_micro_changed(1); return g_vu1.micro + (o & 0x3FFF); }   /* recheck the generated-code image */
    { extern int g_vi_rec; void vi_note(int what) __attribute__((weak)); if (UNLIKELY(g_vi_rec)) vi_note(3); }   /* rt/vinterp.c: VU1 data touched outside VIF1 */
    return g_vu1.mem + (o & 0x3FFF);
}

u64 hw_read(Ctx *c, u32 a, int size) {
    if (!hw_phys(&a)) { u64 v = 0; memcpy(&v, HPTR(a), (size_t)size); return v; }
    int ti, tr;
    if (tm_reg(a, &ti, &tr)) {
        Timer *t = &tm[ti];
        switch (tr) {
        case 0: return tm_count(t, kernel_now(c));
        case 1: return t->mode;
        case 2: return t->comp;
        default: return t->hold;
        }
    }
    if ((a & 0xFF000000u) == 0x12000000u) return gs_read_priv(a);
    u8 *vm = vu_mem_ptr(a);
    if (vm) { u64 v = 0; memcpy(&v, vm, (size_t)size); return v; }
    if (a >= 0x10002000u && a < 0x10002040u) return ipu_read(a & ~0xFu);
    if (a >= 0x10003800u && a < 0x10003C00u) return vif_read_reg(0, a & 0x3FF);
    if (a >= 0x10003C00u && a < 0x10004000u) return vif_read_reg(1, a & 0x3FF);
    if (a >= 0x10003000u && a < 0x10003800u) return 0;                 /* GIF: idle, FIFO empty */
    if ((a >= 0x10008000u && a < 0x1000F000u) || a == 0x1000F520u) return dmac_read(a);
    switch (a) {
    case 0x1000F000: return g_intc_stat;
    case 0x1000F010: return g_intc_mask;
    }
    if ((a & 0xFFFF0000u) == 0x10000000u) { note("read", a, REG(a)); return REG(a); }
    note("read", a, 0);
    return 0;
}
void hw_write(Ctx *c, u32 a, u64 v, int size) {
    if (!hw_phys(&a)) { memcpy(HPTR(a), &v, (size_t)size); return; }
    int ti, tr;
    if (tm_reg(a, &ti, &tr)) {
        Timer *t = &tm[ti];
        u64 now = kernel_now(c);
        tm_rebase(t, now);
        switch (tr) {
        case 0: t->base_count = (u32)v & 0xFFFF; break;
        case 1: t->mode = ((u32)v & 0x3FF) | (t->mode & ~(u32)v & (TM_EQUF | TM_OVFF)); break;
        case 2: t->comp = (u32)v & 0xFFFF; break;
        default: t->hold = (u32)v & 0xFFFF; break;
        }
        kernel_rearm(c);
        return;
    }
    if ((a & 0xFF000000u) == 0x12000000u) { gs_write_priv(a, v); return; }
    u8 *vm = vu_mem_ptr(a);
    if (vm) { memcpy(vm, &v, (size_t)size); return; }
    if (a >= 0x10002000u && a < 0x10002040u) { ipu_write(a & ~0xFu, v); kernel_rearm(c); return; }
    if (a >= 0x10003800u && a < 0x10003C00u) { vif_write_reg(0, a & 0x3FF, (u32)v); return; }
    if (a >= 0x10003C00u && a < 0x10004000u) { vif_write_reg(1, a & 0x3FF, (u32)v); return; }
    if (a >= 0x10003000u && a < 0x10003800u) { if (a == 0x10003000u && (v & 1)) gif_reset(); return; }
    if ((a >= 0x10008000u && a < 0x1000F000u) || a == 0x1000F590u) { dmac_write(a, (u32)v); kernel_rearm(c); return; }
    switch (a) {
    case 0x1000F000: intc_ack((u32)v); return;
    case 0x1000F010: g_intc_mask ^= (u32)v; return;
    case 0x1000F180: fputc((int)(v & 0xFF), stdout); return;           /* SIO transmit: debug console */
    }
    if ((a & 0xFFFF0000u) == 0x10000000u) { REG(a) = (u32)v; note("write", a, v); return; }
    note("write", a, v);
}
void hw_read128(Ctx *c, u32 a, r128 *v) {
    (void)c;
    if (!hw_phys(&a)) { memcpy(v, HPTR(a), 16); return; }
    u8 *vm = vu_mem_ptr(a);
    if (vm) { memcpy(v, vm, 16); return; }
    if (a == 0x10007000u) { memset(v, 0, sizeof *v); ipu_out_pull((u8 *)v); return; }   /* IPU output FIFO */
    memset(v, 0, sizeof *v); note("read128", a, 0);
}
void hw_write128(Ctx *c, u32 a, const r128 *v) {
    (void)c;
    if (!hw_phys(&a)) { memcpy(HPTR(a), v, 16); return; }
    u8 *vm = vu_mem_ptr(a);
    if (vm) { memcpy(vm, v, 16); return; }
    switch (a) {
    case 0x10004000: vif_write(0, (const u8 *)v, 16); return;          /* VIF0 FIFO */
    case 0x10005000: vif_write(1, (const u8 *)v, 16); return;          /* VIF1 FIFO */
    case 0x10006000: gif_write(GIF_PATH3, (const u8 *)v, 16); return;  /* GIF FIFO */
    case 0x10007010: ipu_fifo_write((const u8 *)v); return;            /* IPU input FIFO */
    }
    note("write128", a, v->u64[0]);
}
