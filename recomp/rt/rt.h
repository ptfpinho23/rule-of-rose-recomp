/* rt.h — runtime internals shared between the runtime modules. */
#ifndef ROR_RT_H
#define ROR_RT_H
#include <stdio.h>
#include <stdlib.h>
#include "cpu.h"

#define RAM_SIZE 0x02000000u

/* PAL timing in runtime ticks (1 tick ~ 1 executed-instruction unit). */
#define TICKS_PER_FRAME 2000000ull
#define HSYNC_PER_FRAME 312u
#define TICKS_PER_HSYNC (TICKS_PER_FRAME / HSYNC_PER_FRAME)
#define MAX_SLICE 20000

/* INTC causes / DMAC channels */
enum { INTC_GS, INTC_SBUS, INTC_VBLANK_S, INTC_VBLANK_E, INTC_VIF0, INTC_VIF1,
       INTC_VU0, INTC_VU1, INTC_IPU, INTC_TIM0, INTC_TIM1, INTC_TIM2,
       INTC_TIM3, INTC_SFIFO, INTC_VU0WD };
enum { DMAC_VIF0, DMAC_VIF1, DMAC_GIF, DMAC_FROM_IPU, DMAC_TO_IPU, DMAC_SIF0,
       DMAC_SIF1, DMAC_SIF2, DMAC_FROM_SPR, DMAC_TO_SPR };

extern int g_use_interp;
extern int g_verbose;
extern u64 g_now;                 /* virtual time in ticks */
extern const recomp_fn ror_fn_table[];

/* main.c / support.c */
void rt_fatal(Ctx *c, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void run_guest(Ctx *c, u32 pc);   /* run from pc until the context returns to 0 */
u32  call_guest(Ctx *c, u32 fn, u32 a0, u32 a1, u32 a2, u32 a3);
void dump_ctx(Ctx *c);

/* mem.c */
void mem_init(void);
void mem_load_elf(const char *path, u32 *entry);
void mem_load_elf_buf(const u8 *b, size_t n, const char *what, u32 *entry);
static inline void *gptr(u32 a) { return HPTR(a); }
static inline u32 g32(u32 a) { u32 v; memcpy(&v, HPTR(a), 4); return v; }
static inline void p32(u32 a, u32 v) { memcpy(HPTR(a), &v, 4); }

/* interp.c */
void interp_init(const char *insn_bin);
void interp_run(Ctx *c);

/* kernel.c */
void kernel_init(u32 entry);
void kernel_run(void) __attribute__((noreturn));
void kernel_events(Ctx *c);       /* advance clock, fire due events, resched */
void intc_raise(int cause);
void intc_ack(u32 bits);
void dmac_raise(int channel);
Ctx *kernel_cur_ctx(void);
void kernel_dump(void);
int  kernel_int_enabled(void);
void kernel_set_status(Ctx *c, u32 status);   /* COP0 Status written */
extern u32 g_intc_stat, g_intc_mask, g_dmac_stat;

void kernel_schedule(u64 when, void (*fn)(void *), void *arg);  /* one-shot timed callback */
u64  kernel_now(Ctx *c);          /* virtual time, including the running slice */
void kernel_rearm(Ctx *c);        /* an event deadline changed: recompute slice */

/* hw.c */
void hw_init(void);
u64  hw_next_event(void);
void hw_fire(u64 now);
void hw_vblank(int start);

#endif
