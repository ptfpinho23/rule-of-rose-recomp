/* iop_rxsnd.c — the game's sound driver service (RXSNDDRV.IRX, RPC 0xD000).
 *
 * Protocol-correct, silent re-implementation. The full specification, with
 * the driver/EE addresses each rule was read from, is recomp/docs/rxsnddrv.md;
 * "IRX 0x..." below are module-relative addresses in RXSNDDRV.IRX.
 *
 * Wire format: fn = command, send = argument block, recv = { s32 result,
 * out data }. Command 512 is a batch of { cmd, out_size, in_size, in[] }.
 *
 * Model. The real driver has a worker thread that runs once per vblank and
 * applies the requests recorded by the RPC handlers; the EE only ever sees
 * state through polled status commands. Here the worker is `driver_tick`,
 * run lazily: every RPC first runs the ticks that are due in virtual time
 * (`catch_up`). Below the driver sits a silent model of the SPU2: 2 MiB of
 * sound RAM (filled by the same transfers as on hardware) and 48 voices
 * that walk their ADPCM block flags at the programmed pitch, so that sample
 * ends, loops, stream positions (NAX) and "voice finished" (ENVX == 0)
 * happen when they would on hardware. Nothing is decoded or mixed.
 *
 * Adding audio later: everything a mixer needs is in `spu_ram`, `g_voice[]`
 * (SSA/LSAX/pitch/ADSR/volumes/mix bits/key state), `g_core[]`, `g_fx[]`
 * and `g_pcm[]` (block ring + volumes); `spu_run_frame` is the single place
 * where SPU time advances (960 output samples per call).
 *
 * Not implemented (stops with a clear message instead of guessing): BGM
 * playback (cmd 112; the game has no caller for it), XVAB/XSEQ container
 * version >= 0x200 (not on the disc), SE sequence opcodes other than
 * end/nop/note-on/key-off/kill (not on the disc). */
#include <math.h>
#include "iop.h"

#define DRV_TICK        TICKS_PER_FRAME      /* driver worker period: one vblank */
#define SPU_RAM_SIZE    0x200000u
#define NVOICE          48
#define SAMPLES_PER_TICK 960                 /* 48 kHz / 50 Hz */

typedef struct In { const u8 *p; u32 n; } In;       /* argument block */
typedef struct Out { u8 *p; u32 n; } Out;           /* out-data area */

static Ctx *cur_ctx;
#define FATAL(...) rt_fatal(cur_ctx, "rxsnd: " __VA_ARGS__)

static u32 in32(In in, u32 off) {
    if (off + 4 > in.n) FATAL("argument block too short (need %u, have %u)", off + 4, in.n);
    return rd32(in.p + off);
}
static s16 in16(In in, u32 off) {
    if (off + 2 > in.n) FATAL("argument block too short (need %u, have %u)", off + 2, in.n);
    s16 v; memcpy(&v, in.p + off, 2); return v;
}
static u8 in8(In in, u32 off) {
    if (off + 1 > in.n) FATAL("argument block too short (need %u, have %u)", off + 1, in.n);
    return in.p[off];
}
static void need_out(Out out, u32 n, u32 cmd) {
    if (out.n < n) FATAL("cmd %u: out area %u bytes, needs %u", cmd, out.n, n);
}
static u8 *iop_ptr(u32 addr, u32 size, const char *what) {
    if (addr >= IOP_RAM_SIZE || size > IOP_RAM_SIZE - addr) FATAL("%s: IOP range 0x%x+0x%x invalid", what, addr, size);
    return g_iop_ram + addr;
}
static u32 iop32(u32 addr) { return rd32(iop_ptr(addr, 4, "read")); }
static u16 iop16(u32 addr) { u16 v; memcpy(&v, iop_ptr(addr, 2, "read"), 2); return v; }
static u8  iop8(u32 addr)  { return *iop_ptr(addr, 1, "read"); }

/* ======================================================================
 * Driver globals: worker thread, config (cmd 16/17/18/48)
 * ====================================================================== */
static struct {
    int inited;
    int thread_running;      /* worker thread exists (cmd 16 / module start) */
    int terminate_req;       /* cmd 17 */
    int terminated;          /* value cmd 17 returns */
    u64 next_tick;
    u64 pcm_origin;          /* when the PCM block transfer started (module start) */
    /* cmd 48 */
    u32 debug_mask;          /* code 0 */
    u32 video_rate;          /* code 1: 50 or 60, default 60 */
    u32 stereo;              /* code 2 */
    u8  spdif[3];            /* codes 3..5 */
    struct { u32 mask, v[3]; u8 b[3]; } cfg_req;
} D;

/* ======================================================================
 * IOP heap (cmd 32/33/34/35). AllocSysMemory semantics: 256-byte blocks,
 * flags 0 = lowest address, 1 = highest address, 2 = at address.
 * The arena is one block taken from the shared service allocator.
 * ====================================================================== */
#define HEAP_ARENA  0x00080000u
#define HEAP_MAXBLK 1024
static struct { u32 base, size; int n; struct { u32 addr, size; int used; } b[HEAP_MAXBLK]; } H;

static void heap_init(void) {
    u32 a = iop_alloc(HEAP_ARENA + 0x100);
    H.base = (a + 0xFF) & ~0xFFu; H.size = HEAP_ARENA;
    H.n = 1; H.b[0].addr = H.base; H.b[0].size = H.size; H.b[0].used = 0;
}
static void heap_split(int i, u32 off, u32 size) {      /* carve [off, off+size) out of free block i */
    u32 a = H.b[i].addr, s = H.b[i].size;
    int extra = (off ? 1 : 0) + (off + size < s ? 1 : 0);
    if (H.n + extra > HEAP_MAXBLK) FATAL("IOP heap: too many blocks");
    memmove(&H.b[i + extra + 1], &H.b[i + 1], (size_t)(H.n - i - 1) * sizeof H.b[0]);
    H.n += extra;
    if (off) { H.b[i].addr = a; H.b[i].size = off; H.b[i].used = 0; i++; }
    H.b[i].addr = a + off; H.b[i].size = size; H.b[i].used = 1;
    if (off + size < s) { H.b[i + 1].addr = a + off + size; H.b[i + 1].size = s - off - size; H.b[i + 1].used = 0; }
}
static u32 heap_alloc(u32 flags, u32 size, u32 addr) {
    size = (size + 0xFF) & ~0xFFu;
    if (!size) return 0;
    if (flags == 0) {
        for (int i = 0; i < H.n; i++)
            if (!H.b[i].used && H.b[i].size >= size) { u32 a = H.b[i].addr; heap_split(i, 0, size); return a; }
    } else if (flags == 1) {
        for (int i = H.n - 1; i >= 0; i--)
            if (!H.b[i].used && H.b[i].size >= size) { u32 off = H.b[i].size - size, a = H.b[i].addr + off; heap_split(i, off, size); return a; }
    } else if (flags == 2) {
        addr &= ~0xFFu;
        for (int i = 0; i < H.n; i++)
            if (!H.b[i].used && addr >= H.b[i].addr && addr - H.b[i].addr + size <= H.b[i].size) { heap_split(i, addr - H.b[i].addr, size); return addr; }
    } else FATAL("AllocSysMemory: unknown flags %u", flags);
    return 0;
}
static void heap_free(u32 addr) {
    for (int i = 0; i < H.n; i++) {
        if (H.b[i].addr != addr || !H.b[i].used) continue;
        H.b[i].used = 0;
        if (i + 1 < H.n && !H.b[i + 1].used) {
            H.b[i].size += H.b[i + 1].size;
            memmove(&H.b[i + 1], &H.b[i + 2], (size_t)(H.n - i - 2) * sizeof H.b[0]); H.n--;
        }
        if (i > 0 && !H.b[i - 1].used) {
            H.b[i - 1].size += H.b[i].size;
            memmove(&H.b[i], &H.b[i + 1], (size_t)(H.n - i - 1) * sizeof H.b[0]); H.n--;
        }
        return;
    }
    rt_log("[rxsnd] FreeSysMemory(0x%x): not an allocated block\n", addr);
}
static u32 heap_query(u32 what) {
    u32 max = 0, total = 0;
    for (int i = 0; i < H.n; i++) if (!H.b[i].used) { total += H.b[i].size; if (H.b[i].size > max) max = H.b[i].size; }
    return what == 0 ? IOP_RAM_SIZE : what == 1 ? max : what == 2 ? total : 0;
}

/* ======================================================================
 * SPU2 model: sound RAM and 48 voices (0..23 core 0, 24..47 core 1)
 * ====================================================================== */
static u8 spu_ram[SPU_RAM_SIZE];

typedef void (*VoiceCb)(int voice, int reason, void *arg);
typedef struct Voice {
    /* driver voice manager state (IRX 0x1aa84, 56 bytes per voice) */
    s32 prio; u32 age;
    u8  flags;               /* 1 allocated, 2 owned, 4 keyed on, 8 fading out, 0x10 free when ENVX stays 0 */
    u8  zero_ticks;          /* consecutive ticks with ENVX == 0 */
    s32 scale;               /* fade-out scale, 128 = unity */
    u8  mix;                 /* 1 dry L/R, 2 effect L/R, 4 noise, 8 pitch modulation */
    s32 pitch;               /* SPU pitch, 0x1000 = 48 kHz */
    u32 ssa, lsax; int lsax_set;
    u16 adsr1, adsr2;
    s32 vol_l, vol_r;
    VoiceCb cb; void *cb_arg;
    u32 enable;              /* cmd 145 */
    u32 owner;               /* exclusive-owner id, 0 = none */
    int kon, koff;           /* pending key on / key off for the end of this tick */
    /* SPU side (the mixer) */
    int env_phase;           /* 0 off, 1 attack, 2 decay, 3 sustain, 4 release */
    s32 env_level;           /* 0..0x7fff, what ENVX reads */
    u32 env_count;
    u32 nax;                 /* address of the block being played */
    u32 loop;                /* loop address latched by the SPU */
    s16 block[28]; int si;   /* decoded current block, next sample index */
    u8  block_flags;
    s32 dec1, dec2;          /* ADPCM predictor history */
    s32 hist[4];             /* last four source samples, for interpolation */
    u32 frac;                /* 12-bit pitch counter */
} Voice;
static Voice g_voice[NVOICE];
static u32 g_voice_cursor[3];
static struct Core { u32 mmix; s16 mvol_l, mvol_r; u32 req; u32 req_mmix; s16 req_l, req_r; } g_core[2];

static u32 spu_envx(int v) { return (u32)g_voice[v].env_level; }

/* Decode the 16-byte ADPCM block at nax into v->block. */
static void spu_decode_block(Voice *v) {
    static const s32 f0[5] = { 0, 60, 115, 98, 122 }, f1[5] = { 0, 0, -52, -55, -60 };
    if (v->nax > SPU_RAM_SIZE - 16) { v->env_phase = 0; v->env_level = 0; return; }
    const u8 *b = spu_ram + v->nax;
    int shift = b[0] & 15, filt = b[0] >> 4;
    if (filt > 4) filt = 4;
    if (shift > 12) shift = 9;               /* invalid shifts behave like 9 on hardware */
    v->block_flags = b[1];
    if (b[1] & 4) v->loop = v->nax;
    s32 s1 = v->dec1, s2 = v->dec2;
    for (int i = 0; i < 28; i++) {
        s32 n = (b[2 + (i >> 1)] >> ((i & 1) * 4)) & 15;
        n = (s32)((u32)n << 28) >> 28;
        s32 x = ((n << 12) >> shift) + ((s1 * f0[filt] + s2 * f1[filt] + 32) >> 6);
        if (x > 32767) x = 32767;
        if (x < -32768) x = -32768;
        v->block[i] = (s16)x; s2 = s1; s1 = x;
    }
    v->dec1 = s1; v->dec2 = s2; v->si = 0;
}
/* Advance to the next source sample (into hist[3]). */
static void spu_next_sample(Voice *v) {
    if (v->si >= 28) {
        if (v->block_flags & 1) {
            v->nax = v->loop;
            if (!(v->block_flags & 2)) { v->env_phase = 0; v->env_level = 0; return; }   /* end without repeat: muted */
        } else v->nax += 16;
        spu_decode_block(v);
        if (!v->env_phase) return;
    }
    v->hist[0] = v->hist[1]; v->hist[1] = v->hist[2]; v->hist[2] = v->hist[3];
    v->hist[3] = v->block[v->si++];
}
/* One ADSR step per output sample (SPU2 ADSR1/ADSR2 semantics). */
static void spu_env_step(Voice *v) {
    u32 a1 = v->adsr1, a2 = v->adsr2;
    int exp, dec, shift, step;
    switch (v->env_phase) {
    case 1: exp = (int)(a1 >> 15); dec = 0; shift = (a1 >> 10) & 31; step = (a1 >> 8) & 3; break;
    case 2: exp = 1; dec = 1; shift = (a1 >> 4) & 15; step = 0; break;
    case 3: exp = (int)(a2 >> 15); dec = (a2 >> 14) & 1; shift = (a2 >> 8) & 31; step = (a2 >> 6) & 3; break;
    case 4: exp = (a2 >> 5) & 1; dec = 1; shift = a2 & 31; step = 0; break;
    default: return;
    }
    u32 cycles = 1u << (shift > 11 ? shift - 11 : 0);
    s32 sv = dec ? -8 + step : 7 - step;
    sv <<= (shift < 11 ? 11 - shift : 0);
    if (exp && !dec && v->env_level > 0x6000) cycles *= 4;
    if (exp && dec) sv = (sv * v->env_level) >> 15;
    if ((v->env_phase == 1 || v->env_phase == 3) && shift == 31 && step == 3) cycles = 0;   /* rate 0x7f: never */
    if (cycles && ++v->env_count >= cycles) {
        v->env_count = 0;
        v->env_level += sv;
        if (v->env_level > 0x7FFF) v->env_level = 0x7FFF;
        if (v->env_level < 0) v->env_level = 0;
    }
    if (v->env_phase == 1 && v->env_level >= 0x7FFF) { v->env_phase = 2; v->env_count = 0; }
    else if (v->env_phase == 2 && v->env_level <= (s32)(((a1 & 15) + 1) * 0x800)) { v->env_phase = 3; v->env_count = 0; }
    else if (v->env_phase == 4 && v->env_level <= 0) { v->env_phase = 0; v->env_level = 0; }
}
static void spu_key_on(Voice *v) {
    v->env_phase = 1; v->env_level = 0; v->env_count = 0;
    v->nax = v->ssa; v->loop = v->lsax_set ? v->lsax : v->ssa;
    v->dec1 = v->dec2 = 0; v->frac = 0;
    memset(v->hist, 0, sizeof v->hist);
    spu_decode_block(v);
}
static void spu_key_off(Voice *v) { if (v->env_phase) { v->env_phase = 4; v->env_count = 0; } }

/* The mixer: render `frames` stereo frames at 48 kHz. Dry path only: the
 * SPU2 reverb is not implemented (voices routed to the effect bus are still
 * heard dry), nor is the noise generator or pitch modulation. */
static void spu_render(s16 *out, int frames) {
    for (int n = 0; n < frames; n++) {
        s32 acc[2][2] = { { 0, 0 }, { 0, 0 } };
        for (int i = 0; i < NVOICE; i++) {
            Voice *v = &g_voice[i];
            if (!v->env_phase) continue;
            v->frac += (u32)(v->pitch < 0 ? 0 : v->pitch > 0x3FFF ? 0x3FFF : v->pitch);
            while (v->frac >= 0x1000 && v->env_phase) { v->frac -= 0x1000; spu_next_sample(v); }
            if (!v->env_phase) continue;
            /* 4-point Catmull-Rom between hist[1] and hist[2] */
            s32 t = (s32)v->frac, h0 = v->hist[0], h1 = v->hist[1], h2 = v->hist[2], h3 = v->hist[3];
            s32 c1 = (h2 - h0) / 2, c2 = h0 - (5 * h1) / 2 + 2 * h2 - h3 / 2, c3 = (h3 - h0) / 2 + (3 * (h1 - h2)) / 2;
            s32 x = h1 + (s32)(((s64)c1 * t + (((s64)c2 * t + (((s64)c3 * t) >> 12) * t) >> 12)) >> 12);
            if (x > 32767) x = 32767;
            if (x < -32768) x = -32768;
            spu_env_step(v);
            x = (x * v->env_level) >> 15;
            if (!(v->mix & 1)) continue;                        /* not routed to the dry mix */
            s32 l = (v->vol_l * v->scale) >> 7, r = (v->vol_r * v->scale) >> 7;
            if (l > 0x3FFF) l = 0x3FFF;
            if (l < -0x3FFF) l = -0x3FFF;
            if (r > 0x3FFF) r = 0x3FFF;
            if (r < -0x3FFF) r = -0x3FFF;
            if (!D.stereo) { l = r = ((l < 0 ? -l : l) + (r < 0 ? -r : r)) / 2; }
            acc[i >= 24][0] += (x * l) >> 14; acc[i >= 24][1] += (x * r) >> 14;
        }
        for (int ch = 0; ch < 2; ch++) {
            s32 m0 = ch ? g_core[0].mvol_r : g_core[0].mvol_l, m1 = ch ? g_core[1].mvol_r : g_core[1].mvol_l;
            s32 o = (s32)(((s64)acc[0][ch] * m0) >> 14) + (s32)(((s64)acc[1][ch] * m1) >> 14);
            if (o > 32767) o = 32767;
            if (o < -32768) o = -32768;
            out[n * 2 + ch] = (s16)o;
        }
    }
}

/* Output FIFO: the driver tick renders 960 frames, the host pulls them. */
#define FIFO_FRAMES (SAMPLES_PER_TICK * 8)
static s16 g_fifo[FIFO_FRAMES * 2];
static u32 g_fifo_rd, g_fifo_n;

/* Advance the SPU by one tick (960 output samples). */
static void spu_run_frame(void) {
    s16 buf[SAMPLES_PER_TICK * 2];
    spu_render(buf, SAMPLES_PER_TICK);
    if (g_fifo_n + SAMPLES_PER_TICK > FIFO_FRAMES) {            /* nobody is listening: drop the oldest tick */
        g_fifo_rd = (g_fifo_rd + SAMPLES_PER_TICK) % FIFO_FRAMES; g_fifo_n -= SAMPLES_PER_TICK;
    }
    u32 wr = (g_fifo_rd + g_fifo_n) % FIFO_FRAMES;
    for (int i = 0; i < SAMPLES_PER_TICK; i++) {
        g_fifo[wr * 2] = buf[i * 2]; g_fifo[wr * 2 + 1] = buf[i * 2 + 1];
        wr = (wr + 1) % FIFO_FRAMES;
    }
    g_fifo_n += SAMPLES_PER_TICK;
}

/* --- voice manager API used by the note layer and the ADPCM streams --- */
static void voice_key_off(int i) {                              /* IRX 0x429c */
    Voice *v = &g_voice[i];
    if (!(v->flags & 4)) return;
    v->flags &= ~4; v->koff = 1; v->kon = 0;
}
static void voice_stop(int i, int reason, int keep) {           /* IRX 0x3fcc */
    Voice *v = &g_voice[i];
    if ((v->flags & 3) != 3) return;
    voice_key_off(i);
    if (!keep) v->flags |= 8;
    if (v->cb) { VoiceCb cb = v->cb; void *arg = v->cb_arg; v->cb = 0; v->cb_arg = 0; cb(i, reason, arg); }
    v->flags &= ~2;
}
static int voice_group_ok(int i, int group) {
    return group == 0 || (group == 1 ? i < 24 : i >= 24);
}
/* IRX 0x3b80: returns the voice index or -1. */
static int voice_alloc(int want, s32 prio, int auto_free, int group, u32 owner, VoiceCb cb, void *arg) {
    if (group < 0) group = 0;
    if (group > 2) group = 2;
    if ((s32)owner > 0)
        for (int i = 0; i < NVOICE; i++)
            if ((g_voice[i].flags & 2) && g_voice[i].owner == owner) voice_stop(i, 1, 0);
    int pick = -1;
    if (want >= 0 && want < NVOICE) pick = want;
    else {
        u32 c = g_voice_cursor[group];
        for (int n = 0; n < NVOICE && pick < 0; n++) {
            int i = (int)((c + (u32)n) % NVOICE);
            if (g_voice[i].enable && voice_group_ok(i, group) && !(g_voice[i].flags & 2)) {
                pick = i; g_voice_cursor[group] = (u32)(i + 1) % NVOICE;
            }
        }
        if (pick < 0) {                                         /* steal: lowest priority, then oldest */
            for (int i = 0; i < NVOICE; i++) {
                if (!g_voice[i].enable || !voice_group_ok(i, group)) continue;
                if (pick < 0 || g_voice[i].prio < g_voice[pick].prio ||
                    (g_voice[i].prio == g_voice[pick].prio && g_voice[i].age > g_voice[pick].age)) pick = i;
            }
            if (pick < 0) return -1;
        }
    }
    Voice *v = &g_voice[pick];
    if (v->flags & 2) voice_stop(pick, 1, 0);
    v->prio = prio; v->age = 0; v->flags = auto_free ? 0x13 : 3; v->zero_ticks = 0; v->scale = 128;
    v->cb = cb; v->cb_arg = arg; v->owner = owner; v->lsax_set = 0; v->kon = v->koff = 0;
    return pick;
}
static void voice_set_sample(int i, u32 ssa, u16 adsr1, u16 adsr2) { g_voice[i].ssa = ssa; g_voice[i].adsr1 = adsr1; g_voice[i].adsr2 = adsr2; }
static void voice_set_loop(int i, u32 lsax) { g_voice[i].lsax = lsax; g_voice[i].lsax_set = 1; }
static void voice_key_on(int i) { g_voice[i].flags |= 4; g_voice[i].kon = 1; g_voice[i].koff = 0; }   /* IRX 0x4208 */

/* sceSdNote2Pitch: 0x1000 at the centre note; notes in semitones, fine in 1/128. */
static s32 note2pitch(int center, int cfine, int note, int fine) {
    double semis = (double)(note - center) + (double)(fine - cfine) / 128.0;
    double p = 4096.0 * pow(2.0, semis / 12.0);
    return p > 16383.0 ? 0x3FFF : (s32)p;
}

static void voice_tick(void) {                                  /* IRX 0x35c0 */
    for (int i = 0; i < NVOICE; i++) {
        Voice *v = &g_voice[i];
        if (!(v->flags & 1)) continue;
        if (v->flags & 8) { v->scale -= 16; if (v->scale < 0) v->scale = 0; }
        if ((v->flags & 0x10) && (v->flags & 2)) {
            if (spu_envx(i) == 0) {
                if (++v->zero_ticks >= 9) { voice_stop(i, 0, 1); v->flags = 0; v->koff = 1; }
            } else v->zero_ticks = 0;
        }
        if ((v->flags & 8) && v->scale == 0) { v->flags = 0; v->koff = 1; v->kon = 0; }
        else if (v->flags) v->age++;
    }
    for (int i = 0; i < NVOICE; i++) {                          /* KON / KOFF switches */
        Voice *v = &g_voice[i];
        if (v->kon) spu_key_on(v);
        else if (v->koff) spu_key_off(v);
        v->kon = v->koff = 0;
    }
}

/* ======================================================================
 * SPU RAM transfers (cmd 64/65). They complete inside the call, so
 * cmd 65 always answers "done".
 * ====================================================================== */
static u32 g_xfer_id;
static u8 *spu_ptr(u32 addr, u32 size, const char *what) {
    if (addr >= SPU_RAM_SIZE || size > SPU_RAM_SIZE - addr) FATAL("%s: SPU range 0x%x+0x%x invalid", what, addr, size);
    return spu_ram + addr;
}
static u8 *ee_ptr(u32 addr, u32 size, const char *what) {
    addr &= 0x1FFFFFFFu;
    if (addr >= RAM_SIZE || size > RAM_SIZE - addr) FATAL("%s: EE range 0x%x+0x%x invalid", what, addr, size);
    return gptr(addr);
}
static u32 xfer(u32 type, u32 a, u32 b, u32 size) {
    switch (type) {
    case 1: memcpy(spu_ptr(a, size, "xfer IOP->SPU"), iop_ptr(b, size, "xfer IOP->SPU"), size); break;
    case 2: memcpy(iop_ptr(a, size, "xfer SPU->IOP"), spu_ptr(b, size, "xfer SPU->IOP"), size); break;
    case 3: memcpy(spu_ptr(a, size, "xfer EE->SPU"), ee_ptr(b, size, "xfer EE->SPU"), size); break;
    case 4: memcpy(ee_ptr(a, size, "xfer SPU->EE"), spu_ptr(b, size, "xfer SPU->EE"), size); break;
    case 5: memset(spu_ptr(a, size, "xfer clear"), 0, size); break;
    default:
        /* the real queue accepts the entry and the worker then drops it */
        rt_log("[rxsnd] transfer type %u ignored\n", type);
    }
    return g_xfer_id++;
}

/* ======================================================================
 * Core settings (cmd 49) and effects (cmd 50): state only.
 * ====================================================================== */
static struct Fx {
    u32 type, eea; s16 depth_l, depth_r, target_l, target_r, feedback, delay;
    u32 req, req_type, req_eea; s16 req_l, req_r, req_fb, req_delay;
} g_fx[2];

static void core_tick(void) {                                   /* IRX 0x19e0 */
    for (int c = 0; c < 2; c++) {
        struct Core *k = &g_core[c];
        if (k->req & 1) k->mmix = k->req_mmix & 0xFFF;
        if (k->req & 2) { k->mvol_l = k->req_l; k->mvol_r = k->req_r; }
        k->req = 0;
    }
}
static void fx_tick(void) {                                     /* IRX 0x1ca0 */
    static const u32 work_size[10] = { 0x80, 0x26C0, 0x1F40, 0x4840, 0x6FE0, 0xADE0, 0xF6C0, 0x18040, 0x18040, 0x3C00 };
    for (int c = 0; c < 2; c++) {
        struct Fx *f = &g_fx[c];
        int reinit = 0;
        if (f->req & 2) { f->type = f->req_type; reinit = 1; }
        if (f->req & 1) { f->eea = f->req_eea; reinit = 1; }
        if (f->req & 4) { f->target_l = f->req_l; f->target_r = f->req_r; }
        if (f->req & 0x10) f->feedback = f->req_fb;
        if (f->req & 8) f->delay = f->req_delay;
        f->req = 0;
        if (reinit) {                                           /* clear the work area, depth restarts at 0 */
            u32 sz = (work_size[f->type] + 63) & ~63u;
            if (f->eea < SPU_RAM_SIZE && f->eea + 1 >= sz) memset(spu_ram + f->eea + 1 - sz, 0, sz);
            f->depth_l = f->depth_r = 0;
        }
        s16 *cur[2] = { &f->depth_l, &f->depth_r }, tgt[2] = { f->target_l, f->target_r };
        for (int k = 0; k < 2; k++) {
            int d = tgt[k] - *cur[k];
            if (d > 8) d = 8;
            if (d < -8) d = -8;
            *cur[k] = (s16)(*cur[k] + d);
        }
    }
}

/* ======================================================================
 * XVAB sample banks (cmd 80/81). Header in IOP RAM, ADPCM body in SPU RAM.
 * The real driver rewrites the header's file-relative offsets in place; we
 * leave the header untouched and resolve offsets against its start.
 * ====================================================================== */
#define NXVAB 128
typedef struct Xvab { u32 hdr; u32 spu; s32 refs; u32 tgif, seif, velt, pant; } Xvab;
static Xvab g_xvab[NXVAB];

static s32 xvab_open(s32 id, u32 hdr, u32 spu) {                /* IRX 0x2330 */
    if (id < 0) { id = 0; while (id < NXVAB && g_xvab[id].hdr) id++; if (id == NXVAB) return -1; }
    else if (id >= NXVAB) FATAL("XVAB id %d outside the driver's 128 slots (real driver corrupts memory)", id);
    else if (g_xvab[id].hdr) return -1;
    iop_ptr(hdr, 0x30, "XVAB header");
    if (memcmp(g_iop_ram + hdr, "XVAB", 4)) return -1;
    u32 ver = iop32(hdr + 4), hsize = iop32(hdr + 8);
    if (ver >= 0x200) FATAL("XVAB version 0x%x: chunk layout not implemented", ver);
    iop_ptr(hdr, hsize, "XVAB header");
    u32 head = 0;
    for (u32 o = 12; o + 8 <= hsize; o += 8 + iop32(hdr + o + 4))
        if (!memcmp(g_iop_ram + hdr + o, "HEAD", 4)) { head = o + 8; break; }
    if (!head) FATAL("XVAB without HEAD chunk: not implemented");
    Xvab *x = &g_xvab[id];
    x->hdr = hdr; x->spu = spu; x->refs = 0;
    x->tgif = iop32(hdr + head + 8); x->seif = iop32(hdr + head + 12);
    x->velt = iop32(hdr + head + 16); x->pant = iop32(hdr + head + 20);
    if (x->tgif >= hsize || x->seif >= hsize) FATAL("XVAB %d: TGIF/SEIF offsets outside the header", id);
    if (D.debug_mask & 4) rt_log("[rxsnd] open XVAB %d: header IOP 0x%x (%u bytes), body SPU 0x%x\n", id, hdr, hsize, spu);
    return id;
}
static s32 xvab_close(u32 id) {                                 /* IRX 0x28b8 */
    if (id >= NXVAB) FATAL("XVAB close: id %u out of range", id);
    if (!g_xvab[id].hdr || g_xvab[id].refs > 0) return 1;
    memset(&g_xvab[id], 0, sizeof g_xvab[id]);
    return 0;
}

/* ======================================================================
 * Note layer (IRX 0x8f10...): one note = every tone of a program whose key
 * range contains the key, one voice per tone.
 * ====================================================================== */
#define NNOTE 64
#define NOTE_VOICES 16
typedef struct Note {
    int used; int alive; int xvab; int nvoice;
    s32 vol_l, vol_r;        /* 128 = unity */
    u8  vel, pan;            /* 0..127, pan centre 64 */
    struct NoteVoice { struct Note *note; int voice; u32 tone; } v[NOTE_VOICES];
} Note;
static Note g_note[NNOTE];

static void note_voice_ended(int voice, int reason, void *arg) {    /* IRX 0x9e50 */
    struct NoteVoice *nv = arg; Note *n = nv->note;
    (void)voice;
    nv->voice = -1;
    int left = 0;
    for (int i = 0; i < n->nvoice; i++) if (n->v[i].voice >= 0) left++;
    if (!left) n->alive = 0;
    if (reason == 1)
        for (int i = 0; i < n->nvoice; i++) if (n->v[i].voice >= 0) voice_stop(n->v[i].voice, 0, 0);
}
static void note_key_off(int h) {                               /* IRX 0x9974 */
    Note *n = &g_note[h];
    for (int i = 0; i < n->nvoice; i++) if (n->v[i].voice >= 0) voice_key_off(n->v[i].voice);
}
static void note_kill(int h) {                                  /* IRX 0x99c8 */
    Note *n = &g_note[h];
    if (!n->used) return;
    for (int i = 0; i < n->nvoice; i++) if (n->v[i].voice >= 0) voice_stop(n->v[i].voice, 0, 0);
    g_xvab[n->xvab].refs--;
    n->used = 0;
}
/* IRX 0x9508-0x96f4 (without the LFOs): voice volume from note volume, tone
 * volume, velocity curve and pan curve. */
static void note_set_volume(int h, s32 l, s32 r) {
    Note *n = &g_note[h];
    Xvab *x = &g_xvab[n->xvab];
    n->vol_l = l; n->vol_r = r;
    for (int i = 0; i < n->nvoice; i++) {
        if (n->v[i].voice < 0) continue;
        const u8 *T = iop_ptr(n->v[i].tone, 36, "XVAB tone");
        const u8 *velt = iop_ptr(x->hdr + x->velt + 4 + 128u * T[8], 128, "XVAB VELT");
        const u8 *pant = iop_ptr(x->hdr + x->pant + 4 + 256u * T[9], 256, "XVAB PANT");
        s32 vl = ((l * (s8)T[10] * velt[n->vel]) >> 7) * pant[n->pan] >> 7;
        s32 vr = ((r * (s8)T[11] * velt[n->vel]) >> 7) * pant[128 + n->pan] >> 7;
        g_voice[n->v[i].voice].vol_l = vl; g_voice[n->v[i].voice].vol_r = vr;
    }
}
static int note_on(u32 xvab, u32 prog, u32 key, int cls, int group, s32 pitch_scale, u32 vel, s32 vol_l, s32 vol_r) {
    if (xvab >= NXVAB || !g_xvab[xvab].hdr) { rt_log("[rxsnd] note on: XVAB %u not open\n", xvab); return -1; }
    Xvab *x = &g_xvab[xvab];
    u32 tg = x->hdr + x->tgif;
    u32 po = prog < iop16(tg) ? iop32(tg + 4 + 4 * prog) : 0;
    if (!po) { rt_log("[rxsnd] note on: program %u not in XVAB %u\n", prog, xvab); return -1; }
    int h;
    h = 0; while (h < NNOTE && g_note[h].used) h++;
    if (h == NNOTE) { rt_log("[rxsnd] no free note slot\n"); return -1; }
    Note *n = &g_note[h];
    memset(n, 0, sizeof *n);
    n->used = 1; n->alive = 1; n->xvab = (int)xvab; n->vel = (u8)(vel > 127 ? 127 : vel); n->pan = 64;
    x->refs++;
    u32 ntone = iop16(x->hdr + po + 2), to = x->hdr + iop32(x->hdr + po + 4);
    int failed = 0;
    for (u32 t = 0; t < ntone && !failed; t++) {
        const u8 *T = iop_ptr(to + 36 * t, 36, "XVAB tone");
        if (key < T[4] || key > T[5]) continue;
        if (n->nvoice == NOTE_VOICES) FATAL("note with more than %d layered tones", NOTE_VOICES);
        struct NoteVoice *nv = &n->v[n->nvoice++];
        nv->note = n; nv->tone = to + 36 * t;
        int c = cls >= 0 ? cls : T[3];
        u32 owner = group > 0 ? (u32)group : T[2] ? (xvab << 16 | T[2]) : 0;
        nv->voice = voice_alloc(-1, T[1], 1, c, owner, note_voice_ended, nv);
        if (nv->voice < 0) { failed = 1; break; }
        Voice *v = &g_voice[nv->voice];
        u16 a1, a2; s16 tscale; u32 soff;
        memcpy(&tscale, T + 12, 2); memcpy(&a1, T + 16, 2); memcpy(&a2, T + 18, 2); memcpy(&soff, T + 20, 4);
        voice_set_sample(nv->voice, x->spu + soff, a1, a2);
        s32 base = (T[0] & 0x40) ? 0x1000 : note2pitch(T[6], T[7], (int)key, 0);
        v->pitch = (s32)(((s64)base * ((pitch_scale * tscale) >> 12)) >> 12);
        v->mix = (u8)((T[0] & 3) | ((T[0] & 4) ? 4 : 0));
        v->vol_l = v->vol_r = 0;
        voice_key_on(nv->voice);
    }
    if (failed || n->nvoice == 0) { note_kill(h); return -1; }
    note_set_volume(h, vol_l, vol_r);
    return h;
}

/* ======================================================================
 * SE (cmd 128..131)
 * ====================================================================== */
#define NSE 64
#define SE_NOTES 32
#define SE_CHANS 16
typedef struct Se {
    u32 flags;               /* 1 allocated, 2 active, 8 paused, 0x10000 started, 0x20000 stop requested */
    u8  seq_mode;            /* 1 = cmd 128 (sequence), 0 = cmd 129 (single note) */
    u8  xvab, a, b;          /* cmd 128: group, index; cmd 129: program, key */
    u8  ended;
    u32 tempo; s32 timer;    /* 20.12 */
    u32 seq;                 /* IOP address of the next sequence byte */
    s16 vol_l, vol_r, pitch, tempo_scale; s32 cls, group; u8 fx;
    s32 def_l, def_r;        /* SE definition volume (SERQ), 128 = unity */
    int nnote; struct { u32 chan, id; int h; } note[SE_NOTES];
    int nchan; u32 chan[SE_CHANS];
    u32 req; u8 req_stop, req_pause;
} Se;
static Se g_se[NSE];
static int g_se_list[NSE], g_se_count;                          /* active list, oldest first */

static void se_kill_notes(Se *s) { for (int i = 0; i < s->nnote; i++) if (s->note[i].h >= 0) note_kill(s->note[i].h); s->nnote = 0; }
static void se_keyoff_notes(Se *s) { for (int i = 0; i < s->nnote; i++) if (s->note[i].h >= 0) note_key_off(s->note[i].h); }
static s32 se_vol(Se *s, int right) { return (((right ? s->def_r : s->def_l) * (right ? s->vol_r : s->vol_l)) >> 7) * 128 >> 7; }
static void se_note_on(Se *s, u32 id, u32 chan, u32 prog, u32 key, u32 vel) {    /* IRX 0x8a5c */
    if (s->nnote == SE_NOTES) { rt_log("[rxsnd] SE: note list full\n"); return; }
    int c;
    c = 0; while (c < s->nchan && s->chan[c] != chan) c++;
    if (c == s->nchan) { if (s->nchan == SE_CHANS) FATAL("SE slot with more than %d channels", SE_CHANS); s->chan[s->nchan++] = chan; }
    int h = note_on(s->xvab, prog, key, s->cls, s->group, s->pitch, vel, se_vol(s, 0), se_vol(s, 1));
    if (h < 0) return;
    s->note[s->nnote].chan = chan; s->note[s->nnote].id = id; s->note[s->nnote].h = h; s->nnote++;
}
static void se_note_off(Se *s, u32 id, int release) {           /* IRX 0x8b48: first note with that id */
    for (int i = 0; i < s->nnote; i++) {
        if (s->note[i].id != id) continue;
        if (s->note[i].h >= 0) { if (release) note_key_off(s->note[i].h); else { note_kill(s->note[i].h); s->note[i].h = -1; } }
        return;
    }
}
static u32 se_varlen(Se *s) {
    u32 v = 0; u8 b;
    do { b = iop8(s->seq++); v = v << 7 | (b & 0x7F); } while (b & 0x80);
    return v;
}
static void se_start(Se *s, int slot) {                         /* IRX 0x6e04, request bit 0 */
    s->flags |= 0x10000;
    if (!s->seq_mode) { se_note_on(s, 0, 0, s->a, s->b, 128); return; }
    Xvab *x = s->xvab < NXVAB ? &g_xvab[s->xvab] : 0;
    u32 e = 0;
    if (x && x->hdr) {
        u32 se = x->hdr + x->seif;
        u32 go = s->a < iop16(se) ? iop32(se + 4 + 4 * s->a) : 0;
        if (go) e = s->b < iop16(x->hdr + go) ? iop32(x->hdr + go + 4 + 4 * s->b) : 0;
    }
    if (!e) {
        /* the real driver would run the sequencer on a null pointer here */
        rt_log("[rxsnd] SE slot %d: SE (%u, %u, %u) not registered, ending at once\n", slot, s->xvab, s->a, s->b);
        s->ended = 1; return;
    }
    e += x->hdr;
    u32 fl = iop16(e + 2);
    s->tempo = (fl & 0x3FF) ? (fl & 0x3FF) : 60;
    s->def_l = iop8(e); s->def_r = iop8(e + 1);
    if (fl & 0x1000) s->def_l = -s->def_l;
    if (fl & 0x2000) s->def_r = -s->def_r;
    if ((fl & 0xC000) == 0xC000) s->cls = 0; else if (fl & 0x4000) s->cls = 1; else if (fl & 0x8000) s->cls = 2;
    s->seq = x->hdr + iop32(e + 4);
}
static void se_sequencer(Se *s) {                               /* IRX 0x7260 */
    if (!s->seq_mode || s->ended || !s->seq) return;
    s->timer -= (s32)(s->tempo * 4096 / D.video_rate);
    while (s->timer < 0 && !s->ended) {
        u8 op = iop8(s->seq++);
        int bc = op & 0x40;
        switch (op & 0x3F) {
        case 0: s->ended = 1; continue;
        case 1: case 11: break;
        case 4: {
            u32 id = se_varlen(s), ch = se_varlen(s);
            u32 prog = iop8(s->seq), key = iop8(s->seq + 1);    /* third byte: velocity */
            s->seq += 3;
            se_note_on(s, id, ch, prog, key, iop8(s->seq - 1));
            break;
        }
        case 5: case 6: {
            u32 id = se_varlen(s);
            int rel = (op & 0x3F) == 5;
            if (!bc) { se_note_off(s, id, rel); break; }
            /* broadcast: every active slot that has a note whose handle, or a channel whose id, equals the operand */
            for (int k = 0; k < g_se_count; k++) {
                Se *t = &g_se[g_se_list[k]];
                int hit = 0;
                for (int i = 0; i < t->nnote; i++) if (t->note[i].h == (int)id) hit = 1;
                for (int i = 0; i < t->nchan; i++) if (t->chan[i] == id) hit = 1;
                if (hit) se_note_off(t, id, rel);
            }
            break;
        }
        default:
            FATAL("SE sequence opcode 0x%02x at IOP 0x%x not implemented", op, s->seq - 1);
        }
        s->timer += (s32)(se_varlen(s) << 12);
    }
}
static void se_tick(void) {                                     /* IRX 0x8878 */
    for (int k = 0; k < g_se_count; k++) {                      /* apply requests */
        int slot = g_se_list[k]; Se *s = &g_se[slot];
        if (s->req & 1) se_start(s, slot);
        if (s->req & 2) { s->flags |= 0x20000; if (s->req_stop == 1) se_keyoff_notes(s); else se_kill_notes(s); }
        if (s->req & 4) {
            if (s->req_pause == 0) s->flags &= ~8u;
            else { s->flags |= 8; if (s->req_pause == 1) se_keyoff_notes(s); else se_kill_notes(s); }
        }
        s->req = 0;
    }
    for (int k = 0; k < g_se_count; ) {                         /* release stopped slots */
        Se *s = &g_se[g_se_list[k]];
        if ((s->flags & 0x30000) == 0x30000 && s->nnote == 0) {
            s->flags = 0;
            memmove(&g_se_list[k], &g_se_list[k + 1], (size_t)(g_se_count - k - 1) * sizeof g_se_list[0]); g_se_count--;
        } else k++;
    }
    for (int k = 0; k < g_se_count; k++) {
        Se *s = &g_se[g_se_list[k]];
        if (!(s->flags & 0x10000)) continue;
        se_sequencer(s);
        int n = 0;
        for (int i = 0; i < s->nnote; i++) {                    /* drop finished notes */
            int h = s->note[i].h;
            if (h >= 0 && g_note[h].alive) { note_set_volume(h, se_vol(s, 0), se_vol(s, 1)); s->note[n++] = s->note[i]; continue; }
            if (h >= 0) note_kill(h);
        }
        s->nnote = n;
        if (s->ended && s->nnote == 0) s->flags &= ~2u;
    }
}
static s32 se_play(In in, int seq_mode) {                       /* IRX 0x84b0 / 0x8658 */
    u32 slot = in8(in, 0);
    if (slot >= NSE) FATAL("SE slot %u out of range", slot);
    Se *s = &g_se[slot];
    if (s->flags & 1) return 0;
    memset(s, 0, sizeof *s);
    s->flags = 3; s->seq_mode = (u8)seq_mode; s->xvab = in8(in, 1); s->a = in8(in, 2); s->b = in8(in, 3);
    s->ended = seq_mode ? 0 : 1;
    s->tempo = 60; s->vol_l = s->vol_r = 128; s->def_l = s->def_r = 128; s->pitch = 4096; s->tempo_scale = 4096; s->cls = -1; s->group = -1;
    s->req = 1;
    g_se_list[g_se_count++] = (int)slot;
    return 1;
}
static void se_control(In in) {                                 /* IRX 0x6bcc */
    u32 slot = in32(in, 0), code = in32(in, 4);
    if (slot >= NSE) FATAL("SE control: slot %u out of range", slot);
    Se *s = &g_se[slot];
    if (!(s->flags & 1) || code >= 13) return;
    switch (code) {
    case 0: s->req |= 2; s->req_stop = in8(in, 8); break;
    case 1: s->req |= 4; s->req_pause = in8(in, 8); break;
    /* The rest only changes how notes sound; the values are applied at once
     * (the real driver applies them at the next tick). */
    case 2: s->fx = in8(in, 8); break;
    case 3: s->vol_l = in16(in, 8); s->vol_r = in16(in, 10); break;
    case 4: s->pitch = in16(in, 8); break;      /* used for notes started from now on */
    case 5: s->tempo_scale = in16(in, 8); break;
    case 6: case 7: case 8: case 9: case 10: break;             /* single-note channel volume/pan/bend/LFO */
    case 11: s->cls = in8(in, 8) > 2 ? 2 : in8(in, 8); break;
    case 12: s->group = in16(in, 8); break;
    }
}
static u32 se_status(Out out) {                                 /* IRX 0x8800 */
    need_out(out, 260, 131);
    for (int k = 0; k < g_se_count; k++) { out.p[4 * k] = (u8)g_se_list[k]; out.p[4 * k + 1] = (u8)g_se[g_se_list[k]].flags; }
    wr32(out.p + 256, (u32)g_se_count);
    return (u32)g_se_count;
}

/* ======================================================================
 * XSEQ (cmd 96/97/99) and BGM (cmd 112..116). The game never opens a
 * sequence or starts a BGM, so only the slot bookkeeping exists.
 * ====================================================================== */
#define NXSEQ 32
static struct { u32 data; s16 bank[8]; } g_xseq[NXSEQ];

static s32 xseq_open(s32 id, u32 addr) {                        /* IRX 0x2e00 */
    if (id < 0) { id = 0; while (id < NXSEQ && g_xseq[id].data) id++; if (id == NXSEQ) return -1; }
    else if (id >= NXSEQ) FATAL("XSEQ id %d outside the driver's 32 slots", id);
    else if (g_xseq[id].data) return -1;
    iop_ptr(addr, 16, "XSEQ");
    if (memcmp(g_iop_ram + addr, "XSEQ", 4)) return -1;
    u32 ver = iop32(addr + 4), size = iop32(addr + 8);
    if (ver >= 0x200) FATAL("XSEQ version 0x%x: chunk layout not implemented", ver);
    iop_ptr(addr, size, "XSEQ");
    int found = 0;
    for (u32 o = 12; o + 8 <= size; o += 8 + iop32(addr + o + 4))
        if (!memcmp(g_iop_ram + addr + o, "HEAD", 4) || !memcmp(g_iop_ram + addr + o, "MIDI", 4)) { found = 1; break; }
    if (!found) return -1;
    g_xseq[id].data = addr;
    for (int i = 0; i < 8; i++) g_xseq[id].bank[i] = -1;
    return id;
}

/* ======================================================================
 * ADPCM streams (cmd 160..164): EE fills an IOP ring, the driver moves it
 * to a ring in SPU RAM that looping voices play (IRX 0x9eb0-0xb490).
 * ====================================================================== */
#define NADPCM 48
#define ADPCM_CH 8
typedef struct AdpcmCh {
    struct Adpcm *slot;
    u32 iop_buf, spu_buf;
    int voice;               /* -1 = none */
    s32 want_voice;          /* preferred voice, -1 = any (cmd 164 code 5) */
    s16 vol_l, vol_r;
    u32 req; s16 req_l, req_r; s32 req_voice;
} AdpcmCh;
typedef struct Adpcm {
    u32 flags;               /* 1 open, 2 playing, 4 finished, 8 paused, 0x10000 started, 0x20000 close requested */
    s32 prio;                /* open h4; < 0: a stolen voice does not mark the stream finished */
    u8  fx, end;             /* effect switch; end = last data delivered (cmd 161 b1) */
    s16 vol_l, vol_r, pitch, pitch_scale;
    s32 group, owner;        /* control codes 6, 7 */
    s32 threshold;           /* bytes that must be buffered to start / keep playing */
    s32 iop_size, iop_wr, iop_rd, iop_fill;                     /* EE-fed ring in IOP RAM */
    s32 spu_size, spu_wr, spu_play, spu_fill;                   /* ring in SPU RAM */
    u32 played_hi, played_lo;                                   /* bytes played; both -1 when finished */
    int nch; AdpcmCh ch[ADPCM_CH];
    u32 serial;              /* open counter, for debug dumps */
    u32 req; u8 req_pause, req_fx; s16 req_l, req_r, req_scale, req_group, req_owner;
} Adpcm;
static Adpcm g_adpcm[NADPCM];
static int g_adpcm_list[NADPCM], g_adpcm_count;                 /* open slots, newest first */

static s32 adpcm_bytes_per_frame(void) { return (s32)(27428 / D.video_rate); }     /* IRX 0xb1ec */
static void adpcm_threshold(Adpcm *a) {
    s32 v = ((a->pitch * a->pitch_scale) >> 12) * adpcm_bytes_per_frame() * 3;
    a->threshold = (((v + (s32)((u32)v >> 31)) >> 13) + 63) & ~63;
}
static void adpcm_stop_voices(Adpcm *a) {                       /* IRX 0xb354 */
    for (int i = 0; i < a->nch; i++) if (a->ch[i].voice >= 0) voice_stop(a->ch[i].voice, 0, 0);
    a->flags &= ~2u;
}
static void adpcm_voice_lost(int voice, int reason, void *arg) {    /* IRX 0xb3c4 */
    AdpcmCh *c = arg; Adpcm *a = c->slot;
    (void)voice;
    c->voice = -1;
    if (reason != 1) return;
    adpcm_stop_voices(a);
    if (a->prio >= 0) a->flags |= 4;
}
static s32 adpcm_open(In in) {                                  /* IRX 0x9eb0 */
    u32 slot = in8(in, 0), nch = in8(in, 2);
    if (slot >= NADPCM) FATAL("ADPCM slot %u out of range", slot);
    if (nch > ADPCM_CH) FATAL("ADPCM stream with %u channels (model supports %d)", nch, ADPCM_CH);
    Adpcm *a = &g_adpcm[slot];
    if (a->flags & 1) return 0;
    memset(a, 0, sizeof *a);
    static u32 serial;
    a->serial = serial++;
    a->flags = 1; a->prio = in16(in, 4); a->pitch = in16(in, 6); a->pitch_scale = 4096;
    a->vol_l = a->vol_r = 128;
    a->iop_size = (s32)in32(in, 8); a->spu_size = (s32)in32(in, 12);
    if (a->iop_size <= 0 || a->spu_size <= 0) FATAL("ADPCM open: ring sizes %d / %d", a->iop_size, a->spu_size);
    adpcm_threshold(a);
    a->nch = (int)nch;
    for (u32 i = 0; i < nch; i++) {
        AdpcmCh *c = &a->ch[i];
        c->slot = a; c->iop_buf = in32(in, 16 + 8 * i); c->spu_buf = in32(in, 20 + 8 * i);
        c->voice = -1; c->want_voice = -1; c->vol_l = c->vol_r = 128;
        iop_ptr(c->iop_buf, (u32)a->iop_size, "ADPCM IOP ring"); spu_ptr(c->spu_buf, (u32)a->spu_size, "ADPCM SPU ring");
    }
    a->req = 1;
    memmove(&g_adpcm_list[1], &g_adpcm_list[0], (size_t)g_adpcm_count * sizeof g_adpcm_list[0]);
    g_adpcm_list[0] = (int)slot; g_adpcm_count++;
    return 1;
}
static s32 adpcm_feed(In in) {                                  /* IRX 0xad94 */
    u32 slot = in8(in, 0); s32 want = (s32)in32(in, 4);
    if (slot >= NADPCM) FATAL("ADPCM feed: slot %u out of range", slot);
    Adpcm *a = &g_adpcm[slot];
    if (!(a->flags & 1)) return want;
    s32 n = want, room = a->iop_size - a->iop_fill;
    if (n >= room) n = room;
    static const char *dump; static int dump_checked;
    if (!dump_checked) { dump = getenv("ROR_SNDDUMP"); dump_checked = 1; }
    if (dump && n > 0)                       /* debug: raw ADPCM as fed, one file per slot/open/channel */
        for (int i = 0; i < a->nch; i++) {
            char path[256];
            snprintf(path, sizeof path, "%s_slot%u_open%u_ch%d.adp", dump, slot, a->serial, i);
            FILE *f = fopen(path, "ab");
            if (!f) break;
            s32 first = n < a->iop_size - a->iop_wr ? n : a->iop_size - a->iop_wr;
            fwrite(g_iop_ram + a->ch[i].iop_buf + a->iop_wr, 1, (size_t)first, f);
            fwrite(g_iop_ram + a->ch[i].iop_buf, 1, (size_t)(n - first), f);
            fclose(f);
        }
    a->iop_fill += n;
    a->iop_wr += n; if (a->iop_wr >= a->iop_size) a->iop_wr -= a->iop_size;
    a->end = in8(in, 1);
    return want;
}
static void adpcm_control(In in) {                              /* IRX 0xaea0 */
    u32 slot = in32(in, 0), code = in32(in, 4);
    if (slot >= NADPCM) FATAL("ADPCM control: slot %u out of range", slot);
    Adpcm *a = &g_adpcm[slot];
    if (!(a->flags & 1)) return;
    switch (code) {
    case 0: a->req |= 2; break;                                         /* close */
    case 1: a->req |= 4; a->req_pause = in8(in, 8); break;              /* pause on/off */
    case 2: a->req |= 8; a->req_fx = in8(in, 8); break;                 /* effect send on/off */
    case 3: a->req |= 0x10; a->req_l = in16(in, 8); a->req_r = in16(in, 10); break;
    case 4: a->req |= 0x20; a->req_scale = in16(in, 8); break;          /* pitch scale, 4096 = 1.0 */
    case 6: a->req |= 0x40; a->req_group = in16(in, 8); break;          /* voice group 0..2 */
    case 7: a->req |= 0x80; a->req_owner = in16(in, 8); break;          /* exclusive-owner id */
    default: break;                                                     /* 5 and >= 8: no effect in the driver */
    }
}
static void adpcm_ch_control(In in) {                           /* IRX 0xb01c */
    s32 slot = in16(in, 0), chn = in16(in, 2); u32 code = in32(in, 4);
    if (slot < 0 || slot >= NADPCM) FATAL("ADPCM channel control: slot %d out of range", slot);
    Adpcm *a = &g_adpcm[slot];
    if (!(a->flags & 1) || chn < 0 || chn >= a->nch) return;
    AdpcmCh *c = &a->ch[chn];
    if (code == 3) { c->req |= 2; c->req_l = in16(in, 8); c->req_r = in16(in, 10); }
    else if (code == 5) { c->req |= 8; c->req_voice = (s8)in8(in, 8); }
}
static u32 adpcm_status(Out out) {                              /* IRX 0xb160 */
    need_out(out, 772, 163);
    for (int k = 0; k < g_adpcm_count; k++) {
        Adpcm *a = &g_adpcm[g_adpcm_list[k]]; u8 *r = out.p + 16 * k;
        r[0] = (u8)g_adpcm_list[k]; r[1] = (u8)a->flags;
        wr32(r + 4, (u32)(a->iop_size - a->iop_fill));          /* free bytes in the IOP ring */
        wr32(r + 8, a->played_hi); wr32(r + 12, a->played_lo);
        { static u32 n; if (getenv("ROR_SNDDBG") && (n++ % 100) == 0)
            rt_log("[rxsnd] adpcm slot %d flags=%x iop_fill=%d/%d played=%x:%x\n", g_adpcm_list[k], a->flags, (int)a->iop_fill, (int)a->iop_size, a->played_hi, a->played_lo); }
    }
    wr32(out.p + 768, (u32)g_adpcm_count);
    return (u32)g_adpcm_count;
}
static void adpcm_try_start(Adpcm *a) {                         /* IRX 0xa7cc */
    if (!a->end && a->nch && a->spu_fill < a->threshold) { adpcm_stop_voices(a); return; }
    for (int i = 0; i < a->nch; i++) {
        AdpcmCh *c = &a->ch[i];
        u32 owner = a->owner > 0 ? ((u32)a->owner | 0x2000000u) : 0;
        c->voice = voice_alloc(c->want_voice, a->prio, 0, (i == 0 && a->group > 0) ? a->group : 0, owner, adpcm_voice_lost, c);
        if (c->voice < 0) { adpcm_stop_voices(a); return; }
    }
    int no_wrap = a->end == 1 && a->spu_play + a->spu_fill + a->iop_fill < a->spu_size;
    for (int i = 0; i < a->nch; i++) {
        AdpcmCh *c = &a->ch[i];
        voice_set_sample(c->voice, c->spu_buf + (u32)a->spu_play, 0x8000, 0x8026);
        if (!no_wrap) voice_set_loop(c->voice, c->spu_buf);
        g_voice[c->voice].pitch = (a->pitch * a->pitch_scale) >> 12;
        g_voice[c->voice].mix = a->fx ? 3 : 1;
        g_voice[c->voice].vol_l = (a->vol_l * c->vol_l) >> 7 << 7; g_voice[c->voice].vol_r = (a->vol_r * c->vol_r) >> 7 << 7;
        voice_key_on(c->voice);
    }
    a->flags |= 2;
}
static void adpcm_track(Adpcm *a) {                             /* IRX 0xa63c */
    if (!a->nch || a->ch[0].voice < 0) { adpcm_stop_voices(a); return; }
    s32 pos = (s32)(g_voice[a->ch[0].voice].nax - a->ch[0].spu_buf) & ~63;
    if (pos < 0 || pos > a->spu_size) { adpcm_stop_voices(a); return; }
    if (!a->end && a->spu_fill < a->threshold) { adpcm_stop_voices(a); return; }       /* underrun */
    s32 d = pos - a->spu_play;
    if (d < 0) d += a->spu_size;
    a->spu_fill -= d;
    a->spu_play += d; if (a->spu_play >= a->spu_size) a->spu_play -= a->spu_size;
    u32 lo = a->played_lo + (u32)d;
    if (lo < a->played_lo) a->played_hi++;
    a->played_lo = lo;
}
static void adpcm_tick(void) {                                  /* IRX 0xb1dc */
    for (int k = 0; k < g_adpcm_count; k++) {                   /* apply requests (0xa140) */
        Adpcm *a = &g_adpcm[g_adpcm_list[k]];
        if (a->req & 1) a->flags |= 0x10000;
        if (!(a->flags & 0x10000)) continue;
        if (a->req & 2) a->flags |= 0x20000;
        if (a->req & 4) { if (a->req_pause) a->flags |= 8; else a->flags &= ~8u; }
        if (a->req & 8) a->fx = a->req_fx;
        if (a->req & 0x10) { a->vol_l = a->req_l; a->vol_r = a->req_r; }
        if (a->req & 0x20) { a->pitch_scale = a->req_scale; adpcm_threshold(a); }
        if (a->req & 0x40) a->group = a->req_group;
        if (a->req & 0x80) a->owner = a->req_owner;
        for (int i = 0; i < a->nch; i++) {
            AdpcmCh *c = &a->ch[i];
            if (c->req & 8) c->want_voice = c->req_voice;
            if (c->req & 2) { c->vol_l = c->req_l; c->vol_r = c->req_r; }
            c->req = 0;
        }
        a->req = 0;
    }
    for (int k = 0; k < g_adpcm_count; ) {                      /* close (0xb250) */
        Adpcm *a = &g_adpcm[g_adpcm_list[k]];
        if ((a->flags & 0x30000) == 0x30000) {
            for (int i = 0; i < a->nch; i++) if (a->ch[i].voice >= 0) voice_stop(a->ch[i].voice, 0, 0);
            a->flags = 0;
            memmove(&g_adpcm_list[k], &g_adpcm_list[k + 1], (size_t)(g_adpcm_count - k - 1) * sizeof g_adpcm_list[0]); g_adpcm_count--;
        } else k++;
    }
    for (int k = 0; k < g_adpcm_count; k++) {                   /* state machine (0xa3ec) */
        Adpcm *a = &g_adpcm[g_adpcm_list[k]];
        if (!(a->flags & 0x10000)) continue;
        if (a->end == 1 && !(a->flags & 4))
            for (int i = 0; i < a->nch; i++)
                if (a->ch[i].voice >= 0 && spu_envx(a->ch[i].voice) == 0) {     /* played into the end block */
                    adpcm_stop_voices(a);
                    a->played_hi = a->played_lo = 0xFFFFFFFFu;
                    a->flags |= 4;
                    break;
                }
        if (a->flags & 4) continue;
        if (a->flags & 8) adpcm_stop_voices(a);
        else if (a->flags & 2) adpcm_track(a);
        else adpcm_try_start(a);
        if (a->flags & 2)                                       /* live parameters */
            for (int i = 0; i < a->nch; i++) if (a->ch[i].voice >= 0) {
                Voice *v = &g_voice[a->ch[i].voice];
                v->pitch = (a->pitch * a->pitch_scale) >> 12; v->mix = a->fx ? 3 : 1;
                v->vol_l = (a->vol_l * a->ch[i].vol_l) >> 7 << 7; v->vol_r = (a->vol_r * a->ch[i].vol_r) >> 7 << 7;
            }
    }
    for (int k = 0; k < g_adpcm_count; k++) {                   /* IOP ring -> SPU ring (0xa9e0) */
        Adpcm *a = &g_adpcm[g_adpcm_list[k]];
        if (!(a->flags & 0x10000)) continue;
        for (;;) {
            s32 n = a->iop_fill;
            if (n > a->spu_size - a->spu_fill) n = a->spu_size - a->spu_fill;
            if (n > a->iop_size - a->iop_rd) n = a->iop_size - a->iop_rd;
            if (n > a->spu_size - a->spu_wr) n = a->spu_size - a->spu_wr;
            if (n <= 0) break;
            for (int i = 0; i < a->nch; i++)
                memcpy(spu_ram + a->ch[i].spu_buf + a->spu_wr, g_iop_ram + a->ch[i].iop_buf + a->iop_rd, (size_t)n);
            a->iop_fill -= n; a->iop_rd += n; if (a->iop_rd >= a->iop_size) a->iop_rd -= a->iop_size;
            a->spu_fill += n; a->spu_wr += n; if (a->spu_wr >= a->spu_size) a->spu_wr -= a->spu_size;
        }
    }
}

/* ======================================================================
 * PCM streams (cmd 176..179): 16-bit PCM blocks in an IOP ring, mixed in
 * software into a looping 8192-byte sceSdBlockTrans buffer, one block per
 * stream per 1024 output samples (IRX 0xb4a0-0xce60). Not used by the game.
 * ====================================================================== */
#define NPCM 32
#define PCM_TAGS 256
typedef struct Pcm {
    u32 flags;               /* 1 open, 2 playing, 4 finished, 8 paused, 0x10000 started, 0x20000 close req, 0x40000 end req */
    u32 mode;                /* 0 mono, 1 stereo planes (2048-byte blocks); 2, 3 interleaved stereo (4096-byte blocks) */
    u32 buf; s32 nblocks;
    s32 rd, fill;            /* next block to mix, blocks queued */
    s16 vol_l, vol_r; u32 xor_mask;
    s32 half_tag[2];         /* tag of the block mixed into each half of the output buffer, -1 = none */
    s32 cur_tag, cur_off, last_tag;
    u32 tags[PCM_TAGS]; int tag_rd, tag_n;
    u32 req; u32 req_pause, req_xor; s16 req_l, req_r;
} Pcm;
static Pcm g_pcm[NPCM];
static int g_pcm_list[NPCM], g_pcm_count;                       /* open slots, newest first */
static int g_pcm_fill_half;

static s32 pcm_open(In in) {                                    /* IRX 0xc944 */
    u32 slot = in8(in, 0);
    if (slot >= NPCM) FATAL("PCM slot %u out of range", slot);
    Pcm *p = &g_pcm[slot];
    if (p->flags & 1) return 0;
    memset(p, 0, sizeof *p);
    p->flags = 1; p->mode = in8(in, 1); p->buf = in32(in, 4); p->nblocks = (s32)in32(in, 8);
    if (p->nblocks <= 0) FATAL("PCM open: %d blocks", p->nblocks);
    p->vol_l = p->vol_r = 128; p->half_tag[0] = p->half_tag[1] = -1;
    p->req = 1;
    memmove(&g_pcm_list[1], &g_pcm_list[0], (size_t)g_pcm_count * sizeof g_pcm_list[0]);
    g_pcm_list[0] = (int)slot; g_pcm_count++;
    return 1;
}
static void pcm_feed(In in) {                                   /* IRX 0xcabc */
    u32 slot = in8(in, 0); s32 count = in8(in, 1);
    if (slot >= NPCM) FATAL("PCM feed: slot %u out of range", slot);
    Pcm *p = &g_pcm[slot];
    if (!(p->flags & 1)) return;
    p->fill += count;
    if (p->fill > p->nblocks) p->fill = p->nblocks;
    if (p->fill < 0) p->fill = 0;
    for (s32 i = 0; i < count; i++) {
        u32 tag = in32(in, 4 + 4 * (u32)i);
        if (p->tag_n < PCM_TAGS) { p->tags[(p->tag_rd + p->tag_n) % PCM_TAGS] = tag; p->tag_n++; }
        p->last_tag = (s32)tag;
    }
}
static void pcm_control(In in) {                                /* IRX 0xcc28 */
    u32 slot = in32(in, 0), code = in32(in, 4);
    if (slot >= NPCM) FATAL("PCM control: slot %u out of range", slot);
    Pcm *p = &g_pcm[slot];
    if (!(p->flags & 1)) return;
    switch (code) {
    case 0: p->req |= 2; break;                                         /* close */
    case 1: p->req |= 4; p->req_pause = in32(in, 8); break;             /* 0 run, 1 or 2 pause */
    case 2: p->req |= 8; p->req_xor = in32(in, 8); break;               /* sample XOR masks (mode 3) */
    case 4: p->req |= 0x10; p->req_l = in16(in, 8); p->req_r = in16(in, 10); break;
    case 5: p->req |= 0x20; break;                                      /* no more data: finish at the last tag */
    default: break;                                                     /* 3 and >= 6: no effect */
    }
}
static u32 pcm_status(Out out) {                                /* IRX 0xcd58 */
    need_out(out, 388, 179);
    for (int k = 0; k < g_pcm_count; k++) {
        Pcm *p = &g_pcm[g_pcm_list[k]]; u8 *r = out.p + 12 * k;
        s16 fill = (s16)p->fill;
        r[0] = (u8)g_pcm_list[k]; r[1] = (u8)p->flags; memcpy(r + 2, &fill, 2);
        wr32(r + 4, (u32)p->cur_tag); wr32(r + 8, (u32)p->cur_off);
    }
    wr32(out.p + 384, (u32)g_pcm_count);
    return (u32)g_pcm_count;
}
static void pcm_tick(u64 now) {                                 /* IRX 0xb5ac */
    for (int k = 0; k < g_pcm_count; k++) {
        Pcm *p = &g_pcm[g_pcm_list[k]];
        if (p->req & 1) p->flags |= 0x10000;
        if (!(p->flags & 0x10000)) continue;
        if (p->req & 2) p->flags |= 0x20000;
        if (p->req & 4) { if (p->req_pause == 0) p->flags &= ~8u; else if (p->req_pause < 3) p->flags |= 8; }
        if (p->req & 8) p->xor_mask = p->req_xor;
        if (p->req & 0x10) { p->vol_l = p->req_l; p->vol_r = p->req_r; }
        if (p->req & 0x20) p->flags |= 0x40000;
        p->req = 0;
    }
    for (int k = 0; k < g_pcm_count; ) {
        Pcm *p = &g_pcm[g_pcm_list[k]];
        if ((p->flags & 0x30000) == 0x30000) {
            p->flags = 0;
            memmove(&g_pcm_list[k], &g_pcm_list[k + 1], (size_t)(g_pcm_count - k - 1) * sizeof g_pcm_list[0]); g_pcm_count--;
        } else k++;
    }
    /* position of the SPU in the 8192-byte output buffer: 192000 bytes per second */
    u32 pos = (u32)(((now - D.pcm_origin) * 192000u / 100000000u) & 0x1FFF);
    int half = (int)(pos >> 12);
    for (int k = 0; k < g_pcm_count; k++) {
        Pcm *p = &g_pcm[g_pcm_list[k]];
        if (!(p->flags & 0x10000)) continue;
        if (p->half_tag[half] >= 0) { p->flags |= 2; p->cur_tag = p->half_tag[half]; p->cur_off = (s32)((pos & 0xFFF) >> 1); }
        else p->flags &= ~2u;
        if ((p->flags & 0x40000) && p->cur_tag >= p->last_tag) p->flags |= 4;
    }
    if (half == g_pcm_fill_half) return;
    for (int k = 0; k < g_pcm_count; k++) {                     /* "mix" the half the SPU is not playing */
        Pcm *p = &g_pcm[g_pcm_list[k]];
        if ((p->flags & 0x10008) != 0x10000 || p->fill <= 0) { p->half_tag[g_pcm_fill_half] = -1; continue; }
        if (p->tag_n) { p->half_tag[g_pcm_fill_half] = (s32)p->tags[p->tag_rd]; p->tag_rd = (p->tag_rd + 1) % PCM_TAGS; p->tag_n--; }
        p->rd = (p->rd + 1) % p->nblocks;
        p->fill--;
    }
    g_pcm_fill_half ^= 1;
}

/* ======================================================================
 * Worker thread
 * ====================================================================== */
static void cfg_tick(void) {                                    /* IRX 0x1680 */
    if (D.cfg_req.mask & 1) D.debug_mask = D.cfg_req.v[0];
    if (D.cfg_req.mask & 2) D.video_rate = D.cfg_req.v[1];
    if (D.cfg_req.mask & 4) D.stereo = D.cfg_req.v[2];
    for (int i = 0; i < 3; i++) if (D.cfg_req.mask & (8u << i)) D.spdif[i] = D.cfg_req.b[i];
    D.cfg_req.mask = 0;
    if (D.video_rate == 0) FATAL("config: video rate set to 0 (the real driver would divide by zero)");
}
static void driver_tick(u64 now) {                              /* IRX 0x2c0-0x338 */
    spu_run_frame();                                            /* the SPU ran since the previous tick */
    cfg_tick(); core_tick(); fx_tick();
    /* bgm_tick: no BGM slot can exist */
    se_tick();
    adpcm_tick();
    voice_tick();
    pcm_tick(now);
}
static void start_thread(u64 now) {                             /* IRX 0x190 */
    D.thread_running = 1; D.terminate_req = 0; D.terminated = 0;
    D.next_tick = now + DRV_TICK;
}
static void driver_init(u64 now) {                              /* module start, IRX 0x0-0x18c */
    D.inited = 1;
    heap_init();
    D.debug_mask = 7; D.video_rate = 60; D.stereo = 1; D.spdif[0] = D.spdif[1] = D.spdif[2] = 1;
    for (int c = 0; c < 2; c++) { g_core[c].mmix = 0xFCC; g_core[c].mvol_l = g_core[c].mvol_r = 0x3FFF; }
    g_fx[0].eea = 0x1FFFFF; g_fx[1].eea = 0x1DFFFF;
    for (int i = 0; i < NVOICE; i++) { g_voice[i].enable = 1; g_voice[i].scale = 128; }
    D.pcm_origin = now;
    start_thread(now);
}
static void catch_up(u64 now) {
    while (D.thread_running && now >= D.next_tick) {
        if (D.terminate_req) { D.thread_running = 0; D.terminated = 1; break; }
        driver_tick(D.next_tick);
        D.next_tick += DRV_TICK;
    }
}

/* ======================================================================
 * Dispatcher (IRX 0x360)
 * ====================================================================== */
static s32 dispatch(u32 cmd, Out out, In in, int depth) {
    switch (cmd) {
    case 16: if (!D.thread_running) start_thread(D.next_tick > DRV_TICK ? D.next_tick - DRV_TICK : 0); return 1;
    case 17: { s32 r = D.terminated; D.terminate_req = 1; return r; }
    case 18: return 0x101;
    case 32: return (s32)heap_alloc(in32(in, 0), in32(in, 4), in32(in, 8));
    case 33: heap_free(in32(in, 0)); return 0;
    case 34: return (s32)heap_query(in32(in, 0));
    case 35: { u32 n = in32(in, 8); memmove(iop_ptr(in32(in, 0), n, "memmove dst"), iop_ptr(in32(in, 4), n, "memmove src"), n); return 0; }
    case 48: {
        u32 code = in32(in, 0);
        if (code < 3) { D.cfg_req.mask |= 1u << code; D.cfg_req.v[code] = in32(in, 4); }
        else if (code < 6) { D.cfg_req.mask |= 1u << code; D.cfg_req.b[code - 3] = in8(in, 4); }
        return 0;
    }
    case 49: {
        u32 core = in32(in, 0), sub = in32(in, 4);
        if (core > 1) FATAL("cmd 49: core %u", core);
        if (sub == 0) { g_core[core].req |= 1; g_core[core].req_mmix = in32(in, 8); }
        else if (sub == 1) { g_core[core].req |= 2; g_core[core].req_l = in16(in, 8); g_core[core].req_r = in16(in, 10); }
        return 0;
    }
    case 50: {
        u32 core = in32(in, 0), sub = in32(in, 4);
        if (core > 1) FATAL("cmd 50: core %u", core);
        struct Fx *f = &g_fx[core];
        switch (sub) {
        case 0: if (in32(in, 8) > 9) FATAL("cmd 50: effect type %u", in32(in, 8)); f->req |= 2; f->req_type = in32(in, 8); break;
        case 1: f->req |= 1; f->req_eea = in32(in, 8); break;
        case 2: f->req |= 4; f->req_l = in16(in, 8); f->req_r = in16(in, 10); break;
        case 3: f->req |= 0x10; f->req_fb = in16(in, 8); break;
        case 4: f->req |= 8; f->req_delay = in16(in, 8); break;
        }
        return 0;
    }
    case 64: return (s32)xfer(in8(in, 0), in32(in, 4), in32(in, 8), in32(in, 12));
    case 65: (void)in32(in, 0); return 0;
    case 80: return xvab_open((s32)in32(in, 0), in32(in, 4), in32(in, 8));
    case 81: return xvab_close(in32(in, 0));
    case 96: return xseq_open((s32)in32(in, 0), in32(in, 4));
    case 97: {
        u32 id = in32(in, 0);
        if (id >= NXSEQ) FATAL("XSEQ close: id %u out of range", id);
        if (!g_xseq[id].data) return 1;
        g_xseq[id].data = 0; return 0;
    }
    case 99: {
        u32 id = in32(in, 0), idx = in32(in, 4);
        if (id >= NXSEQ) FATAL("cmd 99: XSEQ id %u out of range", id);
        if (g_xseq[id].data && idx < 8) g_xseq[id].bank[idx] = in16(in, 8);
        return 0;
    }
    case 112:
        FATAL("BGM play (cmd 112: slot %u, XSEQ %u): the MIDI sequencer is not implemented", in32(in, 0), in32(in, 4));
    case 113: case 115: case 116:
        return 0;                                               /* ignored by the driver unless the BGM slot is in use */
    case 114: need_out(out, 260, cmd); wr32(out.p + 256, 0); return 0;
    case 128: return se_play(in, 1);
    case 129: return se_play(in, 0);
    case 130: se_control(in); return 0;
    case 131: se_status(out); return 0;      /* the count goes to out[256]; the result stays 0 */
    case 144: case 146: case 147: {
        u32 sel = cmd == 144 ? 0 : cmd == 146 ? 6 : in32(in, 0);
        if (cmd == 147 && sel >= 6) return 0;
        need_out(out, 192, cmd);
        for (int i = 0; i < NVOICE; i++) {
            Voice *v = &g_voice[i]; u32 w = 0;
            switch (sel) {
            case 0: w = spu_envx(i); break;
            case 1: w = (u32)(v->vol_l >> 7) << 16 | ((u32)(v->vol_r >> 7) & 0xFFFF); break;
            case 2: w = (u32)(s32)(s16)v->pitch; break;
            case 3: w = v->mix; break;
            case 4: w = (u32)v->prio; break;
            case 5: w = (u32)v->adsr1 << 16 | v->adsr2; break;
            case 6: w = v->flags; break;
            }
            wr32(out.p + 4 * i, w);
        }
        return 0;
    }
    case 145: {
        u32 voice = in32(in, 0);
        if (voice >= NVOICE) FATAL("cmd 145: voice %u out of range", voice);
        g_voice[voice].enable = in32(in, 4);
        return 0;
    }
    case 160: return adpcm_open(in);
    case 161: return adpcm_feed(in);
    case 162: adpcm_control(in); return 0;
    case 163: adpcm_status(out); return 0;
    case 164: adpcm_ch_control(in); return 0;
    case 176: return pcm_open(in);
    case 177: pcm_feed(in); return 0;
    case 178: pcm_control(in); return 0;
    case 179: pcm_status(out); return 0;
    case 512: {
        if (depth) FATAL("nested batch command");
        u32 ipos = 0, opos = 0;
        for (;;) {
            u32 sub = in32(in, ipos);
            if (!sub) break;
            u32 osz = in32(in, ipos + 4), isz = in32(in, ipos + 8);
            if (isz > in.n - ipos - 12) FATAL("batch: record at %u overruns the argument block", ipos);
            if (osz > out.n - opos) FATAL("batch: out data overruns the reply (%u + %u > %u)", opos, osz, out.n);
            if (g_verbose) rt_log("[rxsnd]   batch cmd %u in=%u out=%u\n", sub, isz, osz);
            dispatch(sub, (Out){ out.p + opos, osz }, (In){ in.p + ipos + 12, isz }, 1);
            ipos += 12 + isz; opos += osz;
        }
        return 0;
    }
    default: {
        /* IRX 0xac4: unknown numbers are ignored (the EE does queue 180, which this driver build lacks) */
        static u32 warned;
        if (warned++ < 4) rt_log("[rxsnd] command %u is not handled by the driver (ignored, as on hardware)\n", cmd);
        return 0;
    }
    }
}

void rxsnd_call(RpcCall *rc) {
    cur_ctx = rc->ctx;
    u64 now = rc->ctx ? kernel_now(rc->ctx) : g_now;
    if (!D.inited) driver_init(now);
    catch_up(now);
    if (rc->recv_size < 4) FATAL("cmd %u: reply buffer of %u bytes", rc->fno, rc->recv_size);
    s32 r = dispatch(rc->fno, (Out){ rc->recv + 4, rc->recv_size - 4 }, (In){ rc->send, rc->send_size }, 0);
    wr32(rc->recv, (u32)r);
}

/* Host audio pull: `frames` stereo s16 frames at 48 kHz. SPU time is owned
 * by the driver tick (960 frames per 20 ms of virtual time); this first runs
 * the ticks that are due and then drains what they rendered. If the driver
 * has not produced enough (not started yet, or a phase slip), the rest is
 * silence. Call it once per video frame with 960. */
void rxsnd_mix(s16 *out, int frames) {
    cur_ctx = 0;
    if (D.inited) catch_up(g_now);
    int i = 0;
    for (; i < frames && g_fifo_n; i++) {
        out[i * 2] = g_fifo[g_fifo_rd * 2]; out[i * 2 + 1] = g_fifo[g_fifo_rd * 2 + 1];
        g_fifo_rd = (g_fifo_rd + 1) % FIFO_FRAMES; g_fifo_n--;
    }
    if (i < frames && D.inited && !D.thread_running) spu_render(out + i * 2, frames - i);   /* worker stopped: SPU still runs */
    else if (i < frames) memset(out + i * 2, 0, (size_t)(frames - i) * 4);
}
