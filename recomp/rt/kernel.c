/* kernel.c — high-level implementation of the PS2 EE kernel services the
 * game uses through `syscall`: threads, semaphores, interrupt and DMA
 * handlers, alarms, and the virtual clock that drives them.
 *
 * Each EE thread owns a Ctx and a host stack (a plat.h coroutine). Scheduling
 * follows the real kernel: strict priorities (lower value wins), FIFO within
 * a priority, a thread runs until it blocks or a higher-priority thread
 * becomes ready. Interrupt handlers run on a dedicated context at safe
 * points (syscalls and tick boundaries). */
#include <xmmintrin.h>
#include "rt.h"
#include "plat.h"
#include "iop.h"
#include "gs.h"

#define MAX_THREADS 256
#define MAX_SEMAS   256
#define MAX_HANDLERS 64
#define MAX_ALARMS  64
#define HOST_STACK  (8u << 20)

enum { THS_RUN = 0x01, THS_READY = 0x02, THS_WAIT = 0x04, THS_SUSPEND = 0x08,
       THS_WAITSUSPEND = 0x0C, THS_DORMANT = 0x10 };
enum { TSW_NONE = 0, TSW_SLEEP = 1, TSW_SEMA = 2 };

typedef struct Thread {
    int used, status, wait_type, wait_id, wakeup_count;
    u32 func, stack, stack_size, gp, heap_base;
    int init_prio, prio;
    u32 attr, option;
    u64 seq;                 /* FIFO order inside a priority */
    Ctx ctx;
    PlatCo *co;              /* host stack and saved host context */
    int started;
} Thread;

typedef struct { int used, count, max_count, init_count, wait_threads; u32 attr, option; } Sema;
typedef struct { int used, cause; u32 func, arg, gp; u64 order; } Handler;
typedef struct { int used; u64 deadline; u32 func, arg, gp; u16 time; } Alarm;

static Thread threads[MAX_THREADS];
static Sema semas[MAX_SEMAS];
static Handler intc_handlers[MAX_HANDLERS], dmac_handlers[MAX_HANDLERS];
static Alarm alarms[MAX_ALARMS];
#define MAX_TIMED 64
static struct { int used; u64 when; void (*fn)(void *); void *arg; } timed[MAX_TIMED];
static int cur = 1;            /* thread 0 is the kernel's idle thread; main is 1 */
static u64 seq_counter = 1, handler_order = 1;
static PlatCo *sched_co;

static Ctx irq_ctx;
static int in_irq, irq_pending_resched;
static int int_enabled = 1;          /* COP0 Status EIE (EI / DI) */
static int status_ie = 1;            /* COP0 Status IE with EXL and ERL clear */
#define INT_ON() (int_enabled && status_ie)
static int dispatch_disabled;
u32 g_intc_stat, g_intc_mask, g_dmac_stat;
static u32 intc_pending;
u32 g_dmac_mask;
#define dmac_mask g_dmac_mask
u64 g_now;
static s64 g_slice;
static u64 next_vblank_start, next_vblank_end;
static u32 heap_end;
static u32 vsync_flag_ptr, vsync_csr_ptr;
/* OSD config word: 4:3 screen, English, console version 2
 * (japLanguage bit 4, version bits 13-15, language bits 16-20). */
static u32 osd_config = 0x00014010u;
static int kmode;                    /* running a game-installed syscall handler */

/* The kernel's syscall table, visible to the game in kernel memory: SDK
 * start-up code locates it by scanning kseg0 for handler addresses it has
 * just registered, then reads entries from it. Built-in services get
 * distinct placeholder addresses. */
#define SYSCALL_TABLE   0x80010000u
#define SYSCALL_TABLE_N 512
#define SYSCALL_STUBS   0x80070000u
static u32 custom_syscall[SYSCALL_TABLE_N];
static u8 kpatch_syscall[SYSCALL_TABLE_N];

#define IRQ_STACK_TOP 0x000FF000u    /* kernel area below the game image */

Ctx *kernel_cur_ctx(void) { return in_irq ? &irq_ctx : &threads[cur].ctx; }

/* ---- clock --------------------------------------------------------------- */
static void clock_sync(Ctx *c) {
    if (c == &irq_ctx) return;
    s64 used = g_slice - c->cycles;
    if (used > 0) { g_now += (u64)used; c->total_cycles += (u64)used; }
    g_slice = c->cycles;
}
static u64 next_event(void) {
    u64 t = next_vblank_start < next_vblank_end ? next_vblank_start : next_vblank_end;
    for (int i = 0; i < MAX_ALARMS; i++)
        if (alarms[i].used && alarms[i].deadline < t && (INT_ON() || alarms[i].deadline > g_now)) t = alarms[i].deadline;
    for (int i = 0; i < MAX_TIMED; i++)
        if (timed[i].used && timed[i].when < t) t = timed[i].when;
    u64 h = hw_next_event();
    if (h < t) t = h;
    return t;
}
void kernel_schedule(u64 when, void (*fn)(void *), void *arg) {
    for (int i = 0; i < MAX_TIMED; i++) if (!timed[i].used) {
        timed[i].used = 1; timed[i].when = when; timed[i].fn = fn; timed[i].arg = arg;
        return;
    }
    rt_fatal(0, "too many timed events");
}
static void clock_arm(Ctx *c) {
    u64 t = next_event();
    s64 d = t > g_now ? (s64)(t - g_now) : 1;
    if (d > MAX_SLICE) d = MAX_SLICE;
    g_slice = d;
    c->cycles = d;
}

u64 kernel_now(Ctx *c) { clock_sync(c); return g_now; }
void kernel_rearm(Ctx *c) { if (c != &irq_ctx) { clock_sync(c); clock_arm(c); } }

/* ---- scheduler ----------------------------------------------------------- */
static int pick(void) {
    int best = -1;
    for (int i = 0; i < MAX_THREADS; i++) {
        Thread *t = &threads[i];
        if (!t->used || !(t->status == THS_RUN || t->status == THS_READY)) continue;
        if (best < 0 || t->prio < threads[best].prio ||
            (t->prio == threads[best].prio && t->seq < threads[best].seq)) best = i;
    }
    return best;
}
static void make_ready(Thread *t) { t->status = THS_READY; t->seq = seq_counter++; }

static void thread_trampoline(void);

static void fire_events(void);

/* Switch to the best runnable thread; idles (advancing the clock) while no
 * thread is runnable. Must be called on a thread's host stack. */
static void reschedule(void) {
    if (in_irq) { irq_pending_resched = 1; return; }
    for (;;) {
        int n = pick();
        if (n < 0) {                     /* idle: jump to the next event */
            u64 t = next_event();
            if (t > g_now) g_now = t;
            fire_events();
            continue;
        }
        if (dispatch_disabled && (threads[cur].status == THS_RUN)) n = cur;
        if (n == cur && threads[cur].status == THS_RUN) break;
        int prev = cur;
        if (threads[prev].used && threads[prev].status == THS_RUN) threads[prev].status = THS_READY;
        cur = n;
        threads[n].status = THS_RUN;
        if (n != prev) plat_co_switch(threads[prev].co, threads[n].co);
        break;
    }
    clock_arm(&threads[cur].ctx);
}

static void thread_exit_current(int delete) {
    Thread *t = &threads[cur];
    t->status = THS_DORMANT;
    if (delete) t->used = 0;
    t->started = 0;
    /* never returns into this thread: its host stack is abandoned */
    for (;;) {
        int n = pick();
        if (n < 0) { u64 e = next_event(); if (e > g_now) g_now = e; fire_events(); continue; }
        cur = n; threads[n].status = THS_RUN;
        clock_arm(&threads[n].ctx);
        plat_co_jump(threads[n].co);
    }
}

static void thread_trampoline(void) {
    _mm_setcsr(_mm_getcsr() | 0x8040);       /* FTZ | DAZ */
    Thread *t = &threads[cur];
    clock_arm(&t->ctx);
    run_guest(&t->ctx, t->func);
    clock_sync(&t->ctx);
    thread_exit_current(0);
}

static void thread_prepare(Thread *t, u32 arg) {
    Ctx *c = &t->ctx;
    memset(c, 0, sizeof *c);
    c->thread = t;
    c->r[29].u64[0] = SX32((t->stack + t->stack_size - 0x2A0) & ~15u);
    c->r[28].u64[0] = SX32(t->gp);
    c->r[4].u64[0] = SX32(arg);
    c->r[31].u64[0] = 0;                     /* returning from func ends the thread */
    c->fcr31 = 0x01000001u;
    if (!t->co) t->co = plat_co_new(HOST_STACK);
    plat_co_start(t->co, thread_trampoline);
    t->started = 1;
}

/* ---- interrupts ---------------------------------------------------------- */
static void run_handlers(Handler *tab, int cause) {
    /* Handlers run in registration order (the `next` argument of
     * AddIntcHandler is ignored). */
    u64 last = 0;
    in_irq++;
    for (;;) {
        Handler *h = 0;
        for (int i = 0; i < MAX_HANDLERS; i++)
            if (tab[i].used && tab[i].cause == cause && tab[i].order > last &&
                (!h || tab[i].order < h->order)) h = &tab[i];
        if (!h) break;
        last = h->order;
        irq_ctx.r[28].u64[0] = SX32(h->gp);
        call_guest(&irq_ctx, h->func, (u32)cause, h->arg, 0, 0);
    }
    in_irq--;
}

static void service_irqs(void) {
    if (in_irq || !INT_ON()) return;
    for (;;) {
        u32 pend = intc_pending & g_intc_mask;
        u32 dpend = g_dmac_stat & dmac_mask;
        if (!pend && !dpend) break;
        if (dpend) {
            int ch = __builtin_ctz(dpend);
            g_dmac_stat &= ~(1u << ch);
            run_handlers(dmac_handlers, ch);
        } else {
            int cause = __builtin_ctz(pend);
            intc_pending &= ~(1u << cause);
            run_handlers(intc_handlers, cause);
        }
    }
}
/* I_STAT as the game reads it stays latched until the game writes the bit
 * back (SDK code such as sceGsSyncV polls it for VBlank even while a VBlank
 * handler is installed). Handler dispatch is driven by a separate pending
 * set so each event runs the handlers exactly once. */
void intc_raise(int cause) { g_intc_stat |= 1u << cause; intc_pending |= 1u << cause; }
void intc_ack(u32 bits) { g_intc_stat &= ~bits; intc_pending &= ~bits; }
void dmac_raise(int ch) { g_dmac_stat |= 1u << ch; }

static void fire_events(void) {
    hw_fire(g_now);
    for (int i = 0; i < MAX_TIMED; i++)
        if (timed[i].used && timed[i].when <= g_now) { timed[i].used = 0; timed[i].fn(timed[i].arg); }
    while (g_now >= next_vblank_start) {
        next_vblank_start += TICKS_PER_FRAME;
        hw_vblank(1);
        intc_raise(INTC_VBLANK_S);
    }
    while (g_now >= next_vblank_end) {
        next_vblank_end += TICKS_PER_FRAME;
        hw_vblank(0);
        intc_raise(INTC_VBLANK_E);
    }
    for (int i = 0; i < MAX_ALARMS; i++) {
        Alarm *a = &alarms[i];
        /* alarms are delivered by the timer interrupt: held while interrupts are off */
        if (!a->used || a->deadline > g_now || in_irq || !INT_ON()) continue;
        a->used = 0;
        in_irq++;
        irq_ctx.r[28].u64[0] = SX32(a->gp);
        call_guest(&irq_ctx, a->func, (u32)i, a->time, a->arg, 0);
        in_irq--;
    }
    service_irqs();
}

/* Called from generated code when the slice expires, and by syscalls. */
void kernel_events(Ctx *c) {
    clock_sync(c);
    if (c == &irq_ctx) return;
    fire_events();
    if (irq_pending_resched) irq_pending_resched = 0;
    reschedule();
}
void rt_tick(Ctx *c) {
    if (c == &irq_ctx) { c->cycles = INT64_MAX / 2; return; }
    if (kmode) { clock_sync(c); g_slice = c->cycles = MAX_SLICE; return; }
    kernel_events(c);
}

/* ---- syscalls ------------------------------------------------------------ */
#define A0 (c->r[4].u32[0])
#define A1 (c->r[5].u32[0])
#define A2 (c->r[6].u32[0])
#define A3 (c->r[7].u32[0])
#define T0 (c->r[8].u32[0])
#define RET(v) do { c->r[2].u64[0] = SX32(v); } while (0)

static Thread *thread_by_id(u32 id) {
    if (id == 0 || id >= MAX_THREADS || !threads[id].used) return 0;
    return &threads[id];
}
static void wake_thread(Thread *t) {
    if (t->status == THS_WAIT) make_ready(t);
    else if (t->status == THS_WAITSUSPEND) t->status = THS_SUSPEND;
    t->wait_type = TSW_NONE;
}
static void block_current(int type, int id) {
    Thread *t = &threads[cur];
    t->status = THS_WAIT; t->wait_type = type; t->wait_id = id;
}
static int add_handler(Handler *tab, int cause, u32 func, u32 arg, u32 gp) {
    for (int i = 0; i < MAX_HANDLERS; i++) if (!tab[i].used) {
        tab[i] = (Handler){ 1, cause, func, arg, gp, handler_order++ };
        return i;
    }
    return -1;
}
static void signal_sema(Ctx *c, u32 id) {
    if (id >= MAX_SEMAS || !semas[id].used) { RET(-1); return; }
    Sema *s = &semas[id];
    if (s->wait_threads > 0) {
        Thread *best = 0;
        for (int i = 0; i < MAX_THREADS; i++) {
            Thread *t = &threads[i];
            if (t->used && (t->status & THS_WAIT) && t->wait_type == TSW_SEMA && t->wait_id == (int)id &&
                (!best || t->seq < best->seq)) best = t;
        }
        if (best) { s->wait_threads--; wake_thread(best); RET(id); return; }
        s->wait_threads = 0;
    }
    if (s->count >= s->max_count) { RET(-1); return; }
    s->count++;
    RET(id);
}

static const char *sc_name(int n);

void rt_syscall(Ctx *c) {
    s32 raw = (s32)c->r[3].u32[0];
    int n = raw < 0 ? -raw : raw;
    int resched = 0;
    clock_sync(c);
    if (g_verbose > 1) rt_log("[syscall] %s(0x%x, 0x%x, 0x%x, 0x%x) pc=0x%x thr=%d%s\n",
                              sc_name(n), A0, A1, A2, A3, c->pc, cur, c == &irq_ctx ? " irq" : "");
    switch (n) {
    case 0x02: /* SetGsCrt(interlace, mode, field) */
        rt_log("[gs] SetGsCrt interlace=%d mode=0x%x field=%d\n", A0, A1, A2);
        break;
    case 0x04: /* Exit */
        rt_log("[kernel] Exit(%d)\n", (s32)A0);
        exit((int)A0);
    case 0x0D: case 0x0E: case 0x0F:
        /* SetVTLBRefillHandler / SetVCommonHandler / SetVInterruptHandler:
         * the game's own CPU-exception handlers (crash screens). Faults are
         * reported by the runtime instead, so they are only recorded. */
        rt_log("[kernel] exception handler (syscall 0x%x) cause=%d handler=0x%08x\n", n, (s32)A0, A1);
        RET(0); break;
    case 0x10: /* AddIntcHandler(cause, handler, next, arg) */
        RET(add_handler(intc_handlers, (int)A0, A1, A3, c->r[28].u32[0])); break;
    case 0x11: /* RemoveIntcHandler(cause, id) */
        if (A1 < MAX_HANDLERS && intc_handlers[A1].used && intc_handlers[A1].cause == (int)A0) {
            intc_handlers[A1].used = 0; RET(0);
        } else RET(-1);
        break;
    case 0x12: /* AddDmacHandler(channel, handler, next, arg) */
        RET(add_handler(dmac_handlers, (int)A0, A1, A3, c->r[28].u32[0])); break;
    case 0x13: /* RemoveDmacHandler */
        if (A1 < MAX_HANDLERS && dmac_handlers[A1].used && dmac_handlers[A1].cause == (int)A0) {
            dmac_handlers[A1].used = 0; RET(0);
        } else RET(-1);
        break;
    case 0x14: case 0x1A: /* _EnableIntc(cause) */
        RET((g_intc_mask >> A0) & 1 ? 0 : 1); g_intc_mask |= 1u << A0; resched = 1; break;
    case 0x15: case 0x1B: /* _DisableIntc */
        RET((g_intc_mask >> A0) & 1); g_intc_mask &= ~(1u << A0); break;
    case 0x16: case 0x1C: /* _EnableDmac */
        RET((dmac_mask >> A0) & 1 ? 0 : 1); dmac_mask |= 1u << A0; resched = 1; break;
    case 0x17: case 0x1D: /* _DisableDmac */
        RET((dmac_mask >> A0) & 1); dmac_mask &= ~(1u << A0); break;
    case 0x18: case 0x1E: case 0xFC: case 0xFE: { /* SetAlarm(hsyncs, handler, arg) */
        int id = -1;
        for (int i = 0; i < MAX_ALARMS; i++) if (!alarms[i].used) { id = i; break; }
        /* alarms count H-sync edges, which fall on a fixed grid independent
         * of when the alarm is set */
        if (id >= 0) alarms[id] = (Alarm){ 1, (g_now / TICKS_PER_HSYNC + (u64)(u16)A0) * TICKS_PER_HSYNC, A1, A2,
                                           c->r[28].u32[0], (u16)A0 };
        RET(id); break;
    }
    case 0x19: case 0x1F: case 0xFD: case 0xFF: /* ReleaseAlarm(id) */
        if (A0 < MAX_ALARMS && alarms[A0].used) { alarms[A0].used = 0; RET(0); } else RET(-1);
        break;
    case 0x20: { /* CreateThread(ee_thread_t *) */
        int id = -1;
        for (int i = 2; i < MAX_THREADS; i++) if (!threads[i].used) { id = i; break; }
        if (id < 0) { RET(-1); break; }
        Thread *t = &threads[id];
        PlatCo *co = t->co;
        memset(t, 0, sizeof *t);
        t->co = co;
        t->used = 1; t->status = THS_DORMANT;
        t->func = g32(A0 + 4); t->stack = g32(A0 + 8); t->stack_size = g32(A0 + 12);
        t->gp = g32(A0 + 16); t->init_prio = t->prio = (int)g32(A0 + 20);
        t->attr = g32(A0 + 28); t->option = g32(A0 + 32);
        RET(id); break;
    }
    case 0x21: { /* DeleteThread */
        Thread *t = thread_by_id(A0);
        if (!t || t == &threads[cur] || t->status != THS_DORMANT) { RET(-1); break; }
        t->used = 0; RET(A0); break;
    }
    case 0x22: { /* StartThread(id, arg) */
        Thread *t = thread_by_id(A0);
        if (!t || t == &threads[cur] || t->status != THS_DORMANT) { RET(-1); break; }
        t->prio = t->init_prio; t->wakeup_count = 0;
        thread_prepare(t, A1);
        make_ready(t);
        RET(A0); resched = 1; break;
    }
    case 0x23: /* ExitThread */
        if (c == &irq_ctx) rt_fatal(c, "ExitThread in interrupt context");
        thread_exit_current(0);
    case 0x24: /* ExitDeleteThread */
        if (c == &irq_ctx) rt_fatal(c, "ExitDeleteThread in interrupt context");
        thread_exit_current(1);
    case 0x25: case 0x26: { /* TerminateThread */
        Thread *t = thread_by_id(A0);
        if (!t || t == &threads[cur] || t->status == THS_DORMANT) { RET(-1); break; }
        if ((t->status & THS_WAIT) && t->wait_type == TSW_SEMA) semas[t->wait_id].wait_threads--;
        t->status = THS_DORMANT; t->started = 0; RET(A0); break;
    }
    case 0x27: dispatch_disabled = 1; RET(0); break;
    case 0x28: dispatch_disabled = 0; RET(0); resched = 1; break;
    case 0x29: case 0x2A: { /* ChangeThreadPriority(id, prio) */
        Thread *t = thread_by_id(A0);
        if (!t || t->status == THS_DORMANT) { RET(-1); break; }
        int old = t->prio; t->prio = (int)A1;
        if (t->status == THS_READY || t->status == THS_RUN) t->seq = seq_counter++;
        RET(old); resched = 1; break;
    }
    case 0x2B: case 0x2C: { /* RotateThreadReadyQueue(prio) */
        Thread *first = 0;
        for (int i = 0; i < MAX_THREADS; i++) {
            Thread *t = &threads[i];
            if (t->used && (t->status == THS_READY || t->status == THS_RUN) && t->prio == (int)A0 &&
                (!first || t->seq < first->seq)) first = t;
        }
        if (first) first->seq = seq_counter++;
        RET(A0); resched = 1; break;
    }
    case 0x2D: case 0x2E: { /* ReleaseWaitThread */
        Thread *t = thread_by_id(A0);
        if (!t || !(t->status & THS_WAIT)) { RET(-1); break; }
        if (t->wait_type == TSW_SEMA) semas[t->wait_id].wait_threads--;
        t->ctx.r[2].u64[0] = (u64)-1;
        wake_thread(t); RET(A0); resched = 1; break;
    }
    case 0x2F: RET(cur); break; /* GetThreadId */
    case 0x30: case 0x31: { /* ReferThreadStatus(id, ee_thread_status_t *) */
        Thread *t = thread_by_id(A0);
        if (!t) { RET(-1); break; }
        if (A1) {
            u32 p = A1;
            p32(p, t->status); p32(p + 4, t->func); p32(p + 8, t->stack); p32(p + 12, t->stack_size);
            p32(p + 16, t->gp); p32(p + 20, t->init_prio); p32(p + 24, t->prio);
            p32(p + 28, t->attr); p32(p + 32, t->option); p32(p + 36, t->wait_type);
            p32(p + 40, t->wait_id); p32(p + 44, t->wakeup_count);
        }
        RET(t->status); break;
    }
    case 0x32: /* SleepThread */
        if (c == &irq_ctx) rt_fatal(c, "SleepThread in interrupt context");
        RET(cur);
        if (threads[cur].wakeup_count > 0) threads[cur].wakeup_count--;
        else { block_current(TSW_SLEEP, 0); resched = 1; }
        break;
    case 0x33: case 0x34: { /* WakeupThread */
        Thread *t = thread_by_id(A0);
        if (!t || t == &threads[cur] || t->status == THS_DORMANT) { RET(-1); break; }
        if ((t->status & THS_WAIT) && t->wait_type == TSW_SLEEP) wake_thread(t);
        else t->wakeup_count++;
        RET(A0); resched = 1; break;
    }
    case 0x35: case 0x36: { /* CancelWakeupThread */
        Thread *t = thread_by_id(A0);
        if (!t) { RET(-1); break; }
        RET(t->wakeup_count); t->wakeup_count = 0; break;
    }
    case 0x37: case 0x38: { /* SuspendThread */
        Thread *t = thread_by_id(A0);
        if (!t || t->status == THS_DORMANT || (t->status & THS_SUSPEND)) { RET(-1); break; }
        t->status = (t->status & THS_WAIT) ? THS_WAITSUSPEND : THS_SUSPEND;
        RET(A0); resched = 1; break;
    }
    case 0x39: case 0x3A: { /* ResumeThread */
        Thread *t = thread_by_id(A0);
        if (!t || !(t->status & THS_SUSPEND)) { RET(-1); break; }
        if (t->status == THS_WAITSUSPEND) t->status = THS_WAIT; else make_ready(t);
        RET(A0); resched = 1; break;
    }
    case 0x3C: { /* SetupThread(gp, stack, stack_size, args, root_func) */
        Thread *t = &threads[1];
        u32 stack = A1, size = A2;
        if (stack == 0xFFFFFFFFu) stack = RAM_SIZE - 0x1000 - size;
        t->stack = stack; t->stack_size = size; t->gp = A0;
        RET((stack + size - 0x2A0) & ~15u);
        break;
    }
    case 0x3D: { /* SetupHeap(heap_start, heap_size) */
        heap_end = (A1 == 0xFFFFFFFFu) ? threads[1].stack : A0 + A1;
        RET(heap_end); break;
    }
    case 0x3E: RET(heap_end); break; /* EndOfHeap */
    case 0x40: { /* CreateSema(ee_sema_t *) */
        int id = -1;
        for (int i = 0; i < MAX_SEMAS; i++) if (!semas[i].used) { id = i; break; }
        if (id < 0) { RET(-1); break; }
        semas[id] = (Sema){ 1, (int)g32(A0 + 8), (int)g32(A0 + 4), (int)g32(A0 + 8), 0,
                            g32(A0 + 16), g32(A0 + 20) };
        RET(id); break;
    }
    case 0x41: { /* DeleteSema */
        if (A0 >= MAX_SEMAS || !semas[A0].used) { RET(-1); break; }
        for (int i = 0; i < MAX_THREADS; i++) {
            Thread *t = &threads[i];
            if (t->used && (t->status & THS_WAIT) && t->wait_type == TSW_SEMA && t->wait_id == (int)A0) {
                t->ctx.r[2].u64[0] = (u64)-1; wake_thread(t);
            }
        }
        semas[A0].used = 0; RET(A0); resched = 1; break;
    }
    case 0x42: case 0x43: signal_sema(c, A0); resched = 1; break;
    case 0x44: { /* WaitSema */
        if (A0 >= MAX_SEMAS || !semas[A0].used) { RET(-1); break; }
        if (c == &irq_ctx) rt_fatal(c, "WaitSema in interrupt context");
        RET(A0);
        if (semas[A0].count > 0) semas[A0].count--;
        else { semas[A0].wait_threads++; block_current(TSW_SEMA, (int)A0); resched = 1; }
        break;
    }
    case 0x45: case 0x46: /* PollSema */
        if (A0 >= MAX_SEMAS || !semas[A0].used || semas[A0].count <= 0) { RET(-1); break; }
        semas[A0].count--; RET(A0); break;
    case 0x47: case 0x48: { /* ReferSemaStatus(id, ee_sema_t *) */
        if (A0 >= MAX_SEMAS || !semas[A0].used) { RET(-1); break; }
        Sema *s = &semas[A0];
        p32(A1, s->count); p32(A1 + 4, s->max_count); p32(A1 + 8, s->init_count);
        p32(A1 + 12, s->wait_threads); p32(A1 + 16, s->attr); p32(A1 + 20, s->option);
        RET(A0); break;
    }
    case 0x4A: osd_config = g32(A0); RET(0); break;      /* SetOsdConfigParam */
    case 0x4B: p32(A0, osd_config); RET(0); break;       /* GetOsdConfigParam */
    case 0x6E: RET(0); break;                            /* SetOsdConfigParam2 */
    case 0x6F: memset(gptr(A0), 0, A1 < 64 ? A1 : 64); RET(0); break; /* GetOsdConfigParam2 */
    case 0x64: case 0x68: RET(0); break;       /* FlushCache / iFlushCache */
    case 0x70: RET(g_gs_imr); break;           /* GsGetIMR */
    case 0x71: { u32 old = g_gs_imr; g_gs_imr = A0; RET(old); break; } /* GsPutIMR */
    case 0x7C: /* Deci2Call(code, param): debugger channel */
        if (A0 == 0x10) {                      /* kputs: param[0] = string */
            const char *s = gptr(g32(A1));
            fputs("[game] ", stdout); fputs(s, stdout);
            if (!*s || s[strlen(s) - 1] != '\n') fputc('\n', stdout);
            fflush(stdout);
        }
        RET(0); break;
    case 0x6B: RET(0); break;                  /* SifStopDma */
    case 0x76: RET(-1); break;                 /* SifDmaStat: transfer complete */
    case 0x77: RET(sif_set_dma(c, A0, A1)); resched = 1; break; /* SifSetDma */
    case 0x78: RET(0); break;                  /* SifSetDChain */
    case 0x79: sif_set_reg(A0, A1); RET(0); break; /* SifSetReg */
    case 0x7A: RET(sif_get_reg(A0)); break;    /* SifGetReg */
    case 0x72: RET(0); break;                  /* SetPgifHandler */
    case 0x73: vsync_flag_ptr = A0; vsync_csr_ptr = A1; RET(0); break; /* SetVSyncFlag */
    case 0x74: /* SetSyscall(number, handler) */
        /* Handlers inside the game image are run as guest code. Handlers in
         * kernel memory belong to SDK kernel patches (code the SDK copies
         * into the kernel); those services are implemented natively here,
         * so the patch code itself is never executed. */
        if (A0 < SYSCALL_TABLE_N) {
            p32(SYSCALL_TABLE + A0 * 4, A1);
            custom_syscall[A0] = (A1 < 0x80000000u) ? A1 : 0;
            kpatch_syscall[A0] = (A1 >= 0x80000000u);
            if (kpatch_syscall[A0]) rt_log("[kernel] SetSyscall(0x%x) -> kernel patch 0x%08x (native)\n", A0, A1);
        }
        RET(0); break;
    case 0x7E: RET(0); break;                  /* MachineType */
    case 0x7F: RET(RAM_SIZE); break;           /* GetMemorySize */
    default:
        if (n < SYSCALL_TABLE_N && custom_syscall[n]) {
            /* handler installed by the game with SetSyscall: runs as guest
             * code in "kernel mode" (not preemptible), on the caller's context */
            u64 ra = c->r[31].u64[0];
            u32 pc = c->pc;
            kmode++;
            c->r[31].u64[0] = 0;
            run_guest(c, custom_syscall[n]);
            kmode--;
            c->r[31].u64[0] = ra;
            c->pc = pc;
            break;
        }
        if (n < SYSCALL_TABLE_N && kpatch_syscall[n]) {
            /* The only non-service entry SDK patches register is their
             * "GetEntryAddress(syscall)" helper: answer with the placeholder
             * address of that service. */
            rt_log("[kernel] patch helper syscall 0x%x(0x%x) -> entry address\n", n, A0);
            RET(SYSCALL_STUBS + (A0 & 0x1FF) * 8);
            break;
        }
        rt_fatal(c, "unimplemented EE syscall 0x%x %s (a0=0x%x a1=0x%x a2=0x%x a3=0x%x)",
                 n, sc_name(n), A0, A1, A2, A3);
    }
    if (c == &irq_ctx) { if (resched) irq_pending_resched = 1; return; }
    if (kmode) return;
    fire_events();
    if (resched || irq_pending_resched || threads[cur].status != THS_RUN) {
        irq_pending_resched = 0;
        reschedule();
    } else clock_arm(c);
}

/* Interrupts just became enabled: take whatever is pending right away, as
 * the CPU would on the next instruction. */
static void irq_window(Ctx *c) {
    if (c == &irq_ctx || kmode || in_irq || !INT_ON()) return;
    clock_sync(c);
    fire_events();
    if (irq_pending_resched || threads[cur].status != THS_RUN) { irq_pending_resched = 0; reschedule(); }
    else clock_arm(c);
}
void rt_ei(Ctx *c) { int was = INT_ON(); int_enabled = 1; if (!was) irq_window(c); }
void rt_di(Ctx *c) { (void)c; int_enabled = 0; }
int  kernel_int_enabled(void) { return int_enabled; }
void kernel_set_status(Ctx *c, u32 status) {
    int was = INT_ON();
    int_enabled = (status >> 16) & 1;
    status_ie = (status & 1) && !(status & 6);
    if (!was) irq_window(c);
}

static const char *sc_name(int n) {
    static const char *const names[0x81] = {
        [0x02] = "SetGsCrt", [0x04] = "Exit", [0x06] = "LoadExecPS2", [0x07] = "ExecPS2",
        [0x10] = "AddIntcHandler", [0x11] = "RemoveIntcHandler", [0x12] = "AddDmacHandler",
        [0x13] = "RemoveDmacHandler", [0x14] = "_EnableIntc", [0x15] = "_DisableIntc",
        [0x16] = "_EnableDmac", [0x17] = "_DisableDmac", [0x18] = "SetAlarm", [0x19] = "ReleaseAlarm",
        [0x1A] = "_iEnableIntc", [0x1B] = "_iDisableIntc", [0x1C] = "_iEnableDmac",
        [0x1D] = "_iDisableDmac", [0x1E] = "iSetAlarm", [0x1F] = "iReleaseAlarm",
        [0x20] = "CreateThread", [0x21] = "DeleteThread", [0x22] = "StartThread",
        [0x23] = "ExitThread", [0x24] = "ExitDeleteThread", [0x25] = "TerminateThread",
        [0x26] = "iTerminateThread", [0x27] = "DisableDispatchThread", [0x28] = "EnableDispatchThread",
        [0x29] = "ChangeThreadPriority", [0x2A] = "iChangeThreadPriority",
        [0x2B] = "RotateThreadReadyQueue", [0x2C] = "iRotateThreadReadyQueue",
        [0x2D] = "ReleaseWaitThread", [0x2E] = "iReleaseWaitThread", [0x2F] = "GetThreadId",
        [0x30] = "ReferThreadStatus", [0x31] = "iReferThreadStatus", [0x32] = "SleepThread",
        [0x33] = "WakeupThread", [0x34] = "iWakeupThread", [0x35] = "CancelWakeupThread",
        [0x36] = "iCancelWakeupThread", [0x37] = "SuspendThread", [0x38] = "iSuspendThread",
        [0x39] = "ResumeThread", [0x3A] = "iResumeThread", [0x3B] = "JoinThread",
        [0x3C] = "SetupThread", [0x3D] = "SetupHeap", [0x3E] = "EndOfHeap",
        [0x40] = "CreateSema", [0x41] = "DeleteSema", [0x42] = "SignalSema", [0x43] = "iSignalSema",
        [0x44] = "WaitSema", [0x45] = "PollSema", [0x46] = "iPollSema", [0x47] = "ReferSemaStatus",
        [0x48] = "iReferSemaStatus", [0x4A] = "SetOsdConfigParam", [0x4B] = "GetOsdConfigParam",
        [0x4C] = "GetGsHParam", [0x4D] = "GetGsVParam", [0x4E] = "SetGsHParam", [0x4F] = "SetGsVParam",
        [0x5C] = "EnableIntcHandler", [0x5D] = "DisableIntcHandler", [0x5E] = "EnableDmacHandler",
        [0x5F] = "DisableDmacHandler", [0x64] = "FlushCache", [0x68] = "iFlushCache",
        [0x70] = "GsGetIMR", [0x71] = "GsPutIMR", [0x72] = "SetPgifHandler", [0x73] = "SetVSyncFlag",
        [0x74] = "SetSyscall", [0x75] = "_print", [0x76] = "SifDmaStat", [0x77] = "SifSetDma",
        [0x78] = "SifSetDChain", [0x79] = "SifSetReg", [0x7A] = "SifGetReg", [0x7B] = "ExecOSD",
        [0x7C] = "Deci2Call", [0x7D] = "PSMode", [0x7E] = "MachineType", [0x7F] = "GetMemorySize",
    };
    return (n >= 0 && n <= 0x80 && names[n]) ? names[n] : "?";
}

void kernel_dump(void) {
    fprintf(stderr, "  threads (cur=%d, in_irq=%d, int_enabled=%d, intc stat/mask %x/%x, dmac %x/%x):\n",
            cur, in_irq, int_enabled, g_intc_stat, g_intc_mask, g_dmac_stat, dmac_mask);
    for (int i = 0; i < MAX_THREADS; i++) {
        Thread *t = &threads[i];
        if (!t->used) continue;
        fprintf(stderr, "    #%d status=%02x prio=%d wait=%d/%d func=%08x pc=%08x ra=%08x sp=%08x\n",
                i, t->status, t->prio, t->wait_type, t->wait_id, t->func, t->ctx.pc,
                t->ctx.r[31].u32[0], t->ctx.r[29].u32[0]);
        /* code addresses found on the guest stack: a rough call chain */
        u32 sp = t->ctx.r[29].u32[0] & 0x01FFFFF8u;
        fprintf(stderr, "       stack:");
        for (u32 o = 0, n = 0; o < 0x400 && n < 14 && sp + o < RAM_SIZE - 8; o += 8) {
            u32 w = g32(sp + o);
            if (w >= 0x100000 && w < 0x297eb8 && !(w & 3) && g32(sp + o + 4) == 0) { fprintf(stderr, " %06x", w); n++; }
        }
        fprintf(stderr, "\n");
    }
}

/* ---- boot ---------------------------------------------------------------- */
void kernel_init(u32 entry) {
    memset(&irq_ctx, 0, sizeof irq_ctx);
    irq_ctx.cycles = INT64_MAX / 2;
    irq_ctx.fcr31 = 0x01000001u;
    next_vblank_start = TICKS_PER_FRAME;
    next_vblank_end = TICKS_PER_FRAME + TICKS_PER_FRAME / 12;
    for (u32 i = 0; i < SYSCALL_TABLE_N; i++) p32(SYSCALL_TABLE + i * 4, SYSCALL_STUBS + i * 8);
    Thread *t = &threads[1];
    t->used = 1; t->status = THS_RUN; t->prio = t->init_prio = 0;
    t->func = entry; t->stack = RAM_SIZE - 0x100000; t->stack_size = 0x80000;
    t->seq = seq_counter++;
    thread_prepare(t, 0);
    cur = 1;
}

void kernel_run(void) {
    sched_co = plat_co_new(0);
    plat_co_switch(sched_co, threads[1].co);
    rt_log("[kernel] all threads ended\n");
    exit(0);
}

u32 call_guest(Ctx *c, u32 fn, u32 a0, u32 a1, u32 a2, u32 a3) {
    if (c == &irq_ctx) c->r[29].u64[0] = SX32(IRQ_STACK_TOP);
    c->r[4].u64[0] = SX32(a0); c->r[5].u64[0] = SX32(a1);
    c->r[6].u64[0] = SX32(a2); c->r[7].u64[0] = SX32(a3);
    c->r[31].u64[0] = 0;
    run_guest(c, fn);
    return c->r[2].u32[0];
}
