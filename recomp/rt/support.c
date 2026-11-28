/* support.c — services called from generated code: indirect-jump lookup,
 * guest entry, COP0, diagnostics, call tracing. */
#include <stdarg.h>
#include "rt.h"
#include "plat.h"
#include "funcs.h"

/* the edition this program is: recomp/games.py -> gen.py -> funcs.h */
const RorGame ror_game = { ROR_GAME_KEY, ROR_GAME_SERIAL, ROR_GAME_ELF, ROR_GAME_REGION, ROR_GAME_NTSC, ROR_FN_CAMERA, ROR_GUARD_BAND };

int g_use_interp, g_verbose;
VU g_vu0 = { .vf = { [0] = { .f = { 0.0f, 0.0f, 0.0f, 1.0f } } } };
static u32 g_last_miss;

/* ---- diagnostics --------------------------------------------------------- */
void rt_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}
void dump_ctx(Ctx *c) {
    static const char *const n[32] = { "zero","at","v0","v1","a0","a1","a2","a3","t0","t1","t2","t3",
        "t4","t5","t6","t7","s0","s1","s2","s3","s4","s5","s6","s7","t8","t9","k0","k1","gp","sp","fp","ra" };
    fprintf(stderr, "  pc=%08x  now=%llu\n", c->pc, (unsigned long long)g_now);
    for (int i = 0; i < 32; i++)
        fprintf(stderr, "  %-4s %016llx%s", n[i], (unsigned long long)c->r[i].u64[0], (i & 3) == 3 ? "\n" : "");
}
void rt_fatal(Ctx *c, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "\n[FATAL] "); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n");
    va_end(ap);
    if (c) dump_ctx(c);
    exit(70);
}
void rt_invalid(Ctx *c, u32 pc) { rt_fatal(c, "invalid instruction or non-code address reached at 0x%08x (target 0x%08x)", pc, c->pc); }
void rt_break(Ctx *c, u32 pc)   { rt_fatal(c, "BREAK executed at 0x%08x", pc); }
void rt_trap(Ctx *c, u32 pc)    { rt_fatal(c, "TRAP executed at 0x%08x", pc); }

/* Host fault (SIGSEGV and friends, or a Windows exception): report it with
 * the guest address when the access was inside the guest address space. */
static void on_fault(const char *what, void *addr, int has_addr) {
    u64 a = (u64)addr;
    Ctx *c = kernel_cur_ctx();
    fprintf(stderr, "\n[FATAL] %s", what);
    if (has_addr) fprintf(stderr, ", host addr %p", addr);
    if (has_addr && a >= MEMBASE && a < MEMBASE + 0x100000000ull)
        fprintf(stderr, " = guest address 0x%08x (unmapped)", (u32)(a - MEMBASE));
    fprintf(stderr, "\n");
    dump_ctx(c);
    plat_exit_now(71);
}
static void on_int(const char *what) {
    fprintf(stderr, "\n[%s] current guest state:\n", what);
    dump_ctx(kernel_cur_ctx());
    kernel_dump();
    { void ipu_dump(void); void dmac_dump(void); void sif_dump(void); ipu_dump(); dmac_dump(); sif_dump(); }
    plat_exit_now(72);
}
void install_signal_handlers(void) { plat_install_handlers(on_fault, on_int); }
/* ROR_CRASHTEST=1: touch an unmapped guest address once the guest space
 * exists, to check that the fault report works (expected exit status 71). */
void crash_test(void) { if (getenv("ROR_CRASHTEST")) *(volatile u32 *)HPTR(0x40000000u) = 1; }

/* ---- indirect control flow ---------------------------------------------- */
static void f_missing(Ctx *c) { g_last_miss = c->pc; }   /* leaves c->pc for the unwinder */

recomp_fn rt_lookup(Ctx *c, u32 addr) {
    (void)c;
    if (LIKELY(addr >= ROR_CODE_LO && addr < ROR_CODE_HI && !(addr & 3))) {
        recomp_fn f = ror_fn_table[(addr - ROR_CODE_LO) >> 2];
        if (LIKELY(f != 0)) return f;
    }
    return f_missing;
}

/* Run guest code starting at pc on context c until it returns to address 0
 * (the sentinel return address given to thread entry points and handlers). */
void run_guest(Ctx *c, u32 pc) {
    c->pc = pc;
    if (g_use_interp) { interp_run(c); return; }
    while (c->pc != 0) {
        u32 at = c->pc;
        recomp_fn f = rt_lookup(c, at);
        if (f == f_missing)
            rt_fatal(c, "control reached 0x%08x, which is not a known code entry (ra=0x%08x)",
                     at, c->r[31].u32[0]);
        f(c);
    }
}

/* ---- COP0 ---------------------------------------------------------------- */
/* COP0 is per-CPU, not per-thread. Status.EIE (bit 16) mirrors the
 * kernel's interrupt-enable flag (the EI/DI instructions). */
static u32 cop0[32] = { [12] = 0x70000C11u, [15] = 0x00002E20u /* PRId: R5900 */ };
u32 rt_mfc0(Ctx *c, int reg) {
    (void)c;
    switch (reg) {
    case 9:  return (u32)(g_now * 2);                 /* Count */
    case 12: return (cop0[12] & ~0x10000u) | (kernel_int_enabled() ? 0x10000u : 0);
    }
    return cop0[reg];
}
void rt_mtc0(Ctx *c, int reg, u32 v) {
    cop0[reg] = v;
    if (reg == 12) kernel_set_status(c, v);
}
/* ERET: return to ErrorEPC if Status.ERL is set, else to EPC. */
u32 rt_eret(Ctx *c, u32 pc) {
    (void)pc;
    u32 ret;
    if (cop0[12] & 4) { cop0[12] &= ~4u; ret = cop0[30]; }
    else { cop0[12] &= ~2u; ret = cop0[14]; }
    kernel_set_status(c, rt_mfc0(c, 12));
    return ret;
}
int rt_cpcond0(Ctx *c) { (void)c; return 1; }         /* no DMA transfer pending */

/* ---- call trace (for interpreter-vs-recompiled comparison) -------------- */
static u64 tr_count, tr_hash = 1469598103934665603ull, tr_every, tr_from = ~0ull, tr_to;
static int tr_on;
void trace_init(void) {
    const char *e = getenv("ROR_TRACE");
    if (!e) return;
    tr_on = 1; tr_every = strtoull(e, 0, 0); if (!tr_every) tr_every = 1000000;
    if ((e = getenv("ROR_TRACE_FROM"))) tr_from = strtoull(e, 0, 0);
    if ((e = getenv("ROR_TRACE_TO"))) tr_to = strtoull(e, 0, 0);
}
void rt_trace_call(Ctx *c, u32 from, u32 target) {
    if (!tr_on) return;
    u64 h = tr_hash;
    h = (h ^ from) * 1099511628211ull; h = (h ^ target) * 1099511628211ull;
    for (int i = 1; i < 32; i++) h = (h ^ c->r[i].u64[0]) * 1099511628211ull;
    tr_hash = h; tr_count++;
    if (tr_count >= tr_from && tr_count <= tr_to)
        fprintf(stderr, "[trace %llu] %08x -> %08x v0=%llx a0=%llx a1=%llx a2=%llx sp=%llx h=%016llx\n",
                (unsigned long long)tr_count, from, target, (unsigned long long)c->r[2].u64[0],
                (unsigned long long)c->r[4].u64[0], (unsigned long long)c->r[5].u64[0],
                (unsigned long long)c->r[6].u64[0], (unsigned long long)c->r[29].u64[0], (unsigned long long)h);
    if (tr_count % tr_every == 0)
        fprintf(stderr, "[trace] calls=%llu hash=%016llx now=%llu\n",
                (unsigned long long)tr_count, (unsigned long long)h, (unsigned long long)g_now);
}

