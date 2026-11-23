/* vubench.c — replay captured VU1 program starts (ROR_VUDUMP files) or a
 * captured VIF1 stream (ROR_VIFDUMP file) through recomp/rt/vu.c + vif.c and
 * (a) print a hash of everything observable: every XGKICK packet, every GIF
 * path-2 quadword, final registers and data memory; (b) time the replay.
 *   vubench starts DIR [reps]       one line "name hash" per dump, then timing
 *   vubench stream FILE [reps]      one hash for the whole stream, then timing
 *   vubench gen IMAGE [ENTRIES]     write recomp/rt/vu1_fast.inc (to stdout) for a micro memory image;
 *                                   ENTRIES lists extra pcs (hex, one per line) that JR/JALR reach
 * Hash lines go to stdout, timing to stderr. Build with recomp/tools/vubench.sh
 * (VUSRC=dir selects another copy of the runtime sources, e.g. a reference). */
#include "vu.c"
#include "vif.c"
#include <stdarg.h>
#include <time.h>
#include <xmmintrin.h>

VU g_vu0 = { .vf = { [0] = { .f = { 0.0f, 0.0f, 0.0f, 1.0f } } } };
u64 g_gs_frames, g_gs_prims;
void rt_fatal(Ctx *c, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); exit(1);
}
void rt_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); }

static u64 H; static int hashing = 1; static u64 n_kick_qw, n_p2_qw;
static inline void hq(const u8 *p, u32 bytes) {
    for (u32 i = 0; i + 8 <= bytes; i += 8) { u64 w; memcpy(&w, p + i, 8); H = (H ^ w) * 0x9E3779B97F4A7C15ull; H ^= H >> 29; }
}
void gif_write(int path, const u8 *data, u32 bytes) {
    n_p2_qw += bytes / 16;
    if (hashing) for (u32 i = 0; i + 16 <= bytes; i += 16) { u64 t = 0x1000 + (u64)path; hq((const u8 *)&t, 8); hq(data + i, 16); }   /* per quadword: independent of how callers batch */
}
/* same walk as gif.c: tags and their data until the end-of-packet tag is complete */
void gif_write_path1(const u8 *mem, u32 addr, u32 mask) {
    u32 nloop = 0, eop = 0, flg = 0, nreg = 0, reg_idx = 0;
    if (hashing) { u64 t = 0x2000; hq((const u8 *)&t, 8); }
    for (u32 n = 0; n <= mask / 16 + 1; n++, addr += 16) {
        const u8 *qw = mem + (addr & mask & ~15u);
        n_kick_qw++;
        if (hashing) hq(qw, 16);
        if (nloop == 0) {
            u64 lo; memcpy(&lo, qw, 8);
            nloop = lo & 0x7FFF; eop = (lo >> 15) & 1; flg = (lo >> 58) & 3; nreg = (lo >> 60) & 15;
            if (!nreg) nreg = 16;
            reg_idx = 0;
        } else if (flg == 0) { if (++reg_idx == nreg) { reg_idx = 0; nloop--; } }
        else if (flg == 1) { for (int h = 0; h < 2 && nloop; h++) if (++reg_idx == nreg) { reg_idx = 0; nloop--; } }
        else nloop--;
        if (nloop == 0 && eop) return;
    }
}
static void hash_state(void) {
    VU *v = &g_vu1;
    hq((const u8 *)v->vf, 512); hq((const u8 *)v->vi, 64); hq((const u8 *)&v->acc, 16);
    u32 ext[8] = { v->q.u, v->p.u, v->i.u, v->r, v->status, v->mac, v->clip, v->pc };
    hq((const u8 *)ext, 32); hq(v->mem, 0x4000);
}
/* timespec_get: C11, present on Linux and in the Windows UCRT (no clock_gettime there without winpthreads) */
u64 plat_time_ns(void) { struct timespec t; timespec_get(&t, TIME_UTC); return (u64)t.tv_sec * 1000000000ull + (u64)t.tv_nsec; }
static double now(void) { struct timespec t; timespec_get(&t, TIME_UTC); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }

typedef struct { char name[64]; u32 hdr[4]; r128 vf[32]; u32 vi[16]; r128 acc; u32 ext[8]; u8 mem[0x4000], micro[0x4000]; } Start;
static void load_start(const Start *s, int full) {
    VU *v = &g_vu1;
    memcpy(v->vf, s->vf, 512); memcpy(v->vi, s->vi, 64); v->acc = s->acc;
    v->q.u = s->ext[0]; v->p.u = s->ext[1]; v->i.u = s->ext[2]; v->r = s->ext[3]; v->status = s->ext[4]; v->mac = s->ext[5]; v->clip = s->ext[6];
    v->vi_top = s->hdr[1]; v->vi_itop = s->hdr[2];
    memcpy(v->mem, s->mem, 0x4000);
    if (full || memcmp(v->micro, s->micro, 0x4000) != 0) { memcpy(v->micro, s->micro, 0x4000); vu_micro_changed(1); }
}

static int run_starts(const char *dir, int reps) {
    Start *st = 0; size_t n = 0, cap = 0;
    for (int k = 0;; k++) {                       /* vu1_00000.bin, vu1_00001.bin, ... until one is missing */
        char name[64], path[1024]; snprintf(name, sizeof name, "vu1_%05d.bin", k); snprintf(path, sizeof path, "%s/%s", dir, name);
        FILE *f = fopen(path, "rb");
        if (!f) break;
        if (n == cap) { cap = cap ? cap * 2 : 256; st = realloc(st, cap * sizeof *st); }
        Start *s = &st[n];
        snprintf(s->name, sizeof s->name, "%s", name);
        if (fread(s->hdr, 4, 4, f) == 4 && fread(s->vf, 16, 32, f) == 32 && fread(s->vi, 4, 16, f) == 16 && fread(&s->acc, 16, 1, f) == 1 &&
            fread(s->ext, 4, 8, f) == 8 && fread(s->mem, 1, 0x4000, f) == 0x4000 && fread(s->micro, 1, 0x4000, f) == 0x4000) n++;
        fclose(f);
    }
    for (size_t i = 0; i < n; i++) {
        load_start(&st[i], 1);
        H = 0x1234567; vu_exec(1, st[i].hdr[0]); hash_state();
        printf("%s %016llx\n", st[i].name, (unsigned long long)H);
    }
    hashing = 0;
    /* time only the vu_exec calls: TSC ticks summed per call, converted with a calibration against the clock */
    u64 ticks = 0, best = ~0ull; double c0 = now(); u64 k0 = __builtin_ia32_rdtsc();
    for (int r = 0; r < reps; r++) {
        u64 pass = 0;
        for (size_t i = 0; i < n; i++) {
            load_start(&st[i], 0);
            u64 t0 = __builtin_ia32_rdtsc(); vu_exec(1, st[i].hdr[0]); pass += __builtin_ia32_rdtsc() - t0;
        }
        ticks += pass; if (pass < best) best = pass;
    }
    if (getenv("VUBENCH_PERSTART")) {             /* best time of each start, for regressions against instruction counts */
        for (size_t i = 0; i < n; i++) {
            u64 b = ~0ull;
            for (int r = 0; r < 5; r++) { load_start(&st[i], 0); u64 t0 = __builtin_ia32_rdtsc(); vu_exec(1, st[i].hdr[0]); u64 t = __builtin_ia32_rdtsc() - t0; if (t < b) b = t; }
#ifdef VU_STATS
            u64 ic = g_vu_icount; load_start(&st[i], 0); vu_exec(1, st[i].hdr[0]); ic = g_vu_icount - ic;
#else
            u64 ic = 0;
#endif
            fprintf(stderr, "PS %zu %llu %llu\n", i, (unsigned long long)b, (unsigned long long)ic);
        }
    }
    double ns_per_tick = (now() - c0) * 1e9 / (double)(__builtin_ia32_rdtsc() - k0);
    if (reps) fprintf(stderr, "%zu starts: vu_exec %.3f ms per pass avg, %.3f ms best (%.2f us per start)\n",
                      n, (double)ticks * ns_per_tick * 1e-6 / reps, (double)best * ns_per_tick * 1e-6, (double)best * ns_per_tick * 1e-3 / (double)n);
    return 0;
}

/* stream file: "VIFS", u32 sizeof(Vif), Vif[2], VU1 snapshot (same layout as a start dump), then records
 * { u32 kind (0 = vif_write, 1 = vif_write_reg), u32 n, u32 a, u32 b } [+ a bytes of data for kind 0] */
static int run_stream(const char *path, int reps) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    u8 *buf = malloc((size_t)size);
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) return 1;
    fclose(f);
    u32 vsz; memcpy(&vsz, buf + 4, 4);
    if (memcmp(buf, "VIFS", 4) != 0 || vsz != sizeof(Vif)) { fprintf(stderr, "bad stream header (Vif size %u, expected %zu)\n", vsz, sizeof(Vif)); return 1; }
    static Start s0; const u8 *q = buf + 8 + 2 * sizeof(Vif);
    memcpy(s0.hdr, q, 16); memcpy(s0.vf, q + 16, 512); memcpy(s0.vi, q + 528, 64); memcpy(&s0.acc, q + 592, 16); memcpy(s0.ext, q + 608, 32);
    memcpy(s0.mem, q + 640, 0x4000); memcpy(s0.micro, q + 640 + 0x4000, 0x4000);
    const u8 *recs = q + 640 + 0x8000, *end = buf + size;
    double best = 1e9, tot = 0;
    for (int r = -1; r < reps; r++) {            /* pass -1: hashing; then timed passes */
        memcpy(vif, buf + 8, 2 * sizeof(Vif));
        load_start(&s0, 1); g_vu1.pc = s0.hdr[0];
        hashing = (r < 0); H = 0x1234567; n_kick_qw = n_p2_qw = 0;
        u64 nrec = 0, nbytes = 0;
        double t0 = now();
        for (const u8 *p = recs; p + 16 <= end;) {
            u32 h[4]; memcpy(h, p, 16); p += 16;
            if (h[0] == 0) { if (p + h[2] > end) break; if (h[1] == 1) { vif_write(1, p, h[2]); nbytes += h[2]; nrec++; } p += h[2]; }
            else if (h[1] == 1) vif_write_reg(1, h[2], h[3]);
        }
        double t1 = now();
        if (r < 0) {
            hash_state();
            {   /* the VIF1 state that matters: registers, and whatever is partly collected */
                Vif *w = &vif[1]; u32 t[40] = { 0 }; u32 k = 0;
                memcpy(t, w->r, 16); memcpy(t + 4, w->c, 16); k = 8;
                t[k++] = w->cl; t[k++] = w->wl; t[k++] = w->mask; t[k++] = w->mode; t[k++] = w->itop; t[k++] = w->itops; t[k++] = w->top; t[k++] = w->tops;
                t[k++] = w->base; t[k++] = w->ofst; t[k++] = w->dbf; t[k++] = w->mark; t[k++] = w->code; t[k++] = w->err;
                t[k++] = w->have; t[k++] = w->have ? w->need : 0; t[k++] = w->direct_left; t[k++] = w->qw_n;
                for (u32 j = 0; j < w->qw_n && j < 4; j++) t[k++] = w->qw[j];
                hq((const u8 *)t, sizeof t);
                if (w->have) hq((const u8 *)w->buf, (w->have * 4 + 7) & ~7u);
            }
            printf("stream %016llx\n", (unsigned long long)H);
            fprintf(stderr, "%llu VIF1 writes, %llu bytes, %llu XGKICK qwords, %llu path-2 qwords\n", (unsigned long long)nrec,
                    (unsigned long long)nbytes, (unsigned long long)n_kick_qw, (unsigned long long)n_p2_qw);
        } else { tot += t1 - t0; if (t1 - t0 < best) best = t1 - t0; }
    }
    if (reps) fprintf(stderr, "stream replay (VIF1 + VU1, GIF output discarded): %.3f ms avg, %.3f ms best\n", tot * 1e3 / reps, best * 1e3);
    return 0;
}


#ifdef VU_HAS_PREDECODE      /* (older copies of vu.c, used as a reference, have neither records nor generated code) */
/* ---- generator for recomp/rt/vu1_fast.inc ------------------------------------------ */
static const char *UKN[] = { "U_NOP", "U_ADD", "U_SUB", "U_MUL", "U_MADD", "U_MSUB", "U_MAX", "U_MINI", "U_ITOF", "U_FTOI", "U_ABS", "U_CLIP", "U_OPMULA", "U_OPMSUB", "U_BAD" };
static const char *SRCN[] = { "SRC_VF", "SRC_BC", "SRC_Q", "SRC_I" };
static const char *LKN[] = { "L_SLOW", "L_SLOWF", "L_NOP", "L_LQ", "L_SQ", "L_ILW", "L_ISW", "L_IADDIU", "L_ISUBIU", "L_FCSET", "L_FCEQ", "L_FCAND", "L_FCOR", "L_FCGET",
    "L_FSAND", "L_FMAND", "L_B", "L_BAL", "L_JR", "L_JALR", "L_IBEQ", "L_IBNE", "L_IBLTZ", "L_IBGTZ", "L_IBLEZ", "L_IBGEZ",
    "L_IADD", "L_ISUB", "L_IADDI", "L_IAND", "L_IOR", "L_MOVE", "L_MR32", "L_LQI", "L_SQI", "L_LQD", "L_SQD",
    "L_MTIR", "L_MFIR", "L_ILWR", "L_ISWR", "L_XTOP", "L_XITOP", "L_XGKICK" };
static Rec G[0x800];
static u8 g_target[0x800];        /* something jumps here: static branch target, return address, or listed in the entries file */
static u8 g_cj[0x800];            /* "chain join": the straight-line code is entered here from elsewhere than the instruction before */
static int g_hole(u32 i) { return G[i].raw == 0; }
static int g_branch(u32 i) { return !(G[i].fl & F_IBIT) && G[i].lk >= L_B && G[i].lk <= L_IBGEZ; }
static int g_uncond(u32 i) { return G[i].lk == L_B || G[i].lk == L_BAL || G[i].lk == L_JR || G[i].lk == L_JALR; }
static int g_static(u32 i) { return G[i].lk != L_JR && G[i].lk != L_JALR; }
/* instruction i can be the second half of a pair (delay slot / after an E bit) */
static int g_second_ok(u32 i) { return i < 0x800 && !g_hole(i) && !g_branch(i) && !(G[i].fl & F_EBIT) && G[i].uk != U_BAD; }
/* how the generated code enters instruction i: 0 no label, 1 bail, 2 plain, 3 branch pair, 4 E-bit pair */
static int g_kind(u32 i) {
    if (i >= 0x800 || g_hole(i)) return 0;
    if (G[i].uk == U_BAD) return 1;
    if (g_branch(i)) return (!(G[i].fl & F_EBIT) && g_second_ok(i + 1)) ? 3 : 1;
    if (G[i].fl & F_EBIT) return g_second_ok(i + 1) ? 4 : 1;
    return 2;
}
/* i is only ever run by the pair code of i - 1 when reached in sequence; its own label is for jumps to it */
static int g_is_second(u32 i) { return i >= 1 && (g_kind(i - 1) == 3 || g_kind(i - 1) == 4); }
/* the straight-line code runs into i from the instruction(s) before it */
static int g_fallthrough(u32 i) {
    if (i == 0) return 0;
    if (g_is_second(i - 1)) return g_kind(i - 2) == 3 && !g_uncond(i - 2);  /* after a pair: only a conditional branch not taken */
    return g_kind(i - 1) == 2;
}
/* kind of flag result instruction i puts into the pipeline */
static const char *g_evkind(u32 i) {
    int uk = G[i].uk;
    if ((uk >= U_ADD && uk <= U_MSUB) || uk == U_OPMULA || uk == U_OPMSUB) return "EV_MACV";
    if (uk == U_CLIP) return "EV_CLIP";
    return "0";
}
/* What stage 0 of the flag pipeline holds when an instance of instruction i starts, given that the three
 * instructions before it in the chain were prev3 .. prev1 = i-3 .. i-1 (for a label) — known only if the
 * chain cannot have been entered at any of them or at i itself. */
static const char *g_pop(u32 i, int inline_second) {
    if (i < 4) return "POP_DYN";
    for (u32 j = i - 3; j <= i; j++) if (g_cj[j] && !(inline_second && j == i)) return "POP_DYN";
    return g_evkind(i - 4);
}
/* A loop the generated code runs in a copy of its own: instructions T .. B-1 plain, B a conditional branch
 * back to T, B+1 its delay slot, at least four instructions in all. From the second iteration on, the
 * four instructions before any instruction of the body are known (the body itself, cyclically), so the
 * copy needs no run-time look at the flag pipeline stages. Returns T, or -1. */
static int g_loop(u32 b) {
    if (g_kind(b) != 3 || !g_static(b) || g_uncond(b)) return -1;
    u32 t = (G[b].ltarget & 0x3FFF) >> 3;
    if (t > b || b + 2 - t < 4 || g_is_second(t)) return -1;
    for (u32 j = t; j < b; j++) if (g_kind(j) != 2) return -1;
    return (int)t;
}
static void g_step(u32 i, int rec, const char *pop) {
    const Rec *r = &G[i]; char fl[96] = "0";
    if (r->fl) { fl[0] = 0;
        if (r->fl & F_IBIT) strcat(fl, "|F_IBIT"); if (r->fl & F_EBIT) strcat(fl, "|F_EBIT"); if (r->fl & F_SAVE) strcat(fl, "|F_SAVE");
        if (r->fl & F_STALLQ) strcat(fl, "|F_STALLQ"); if (r->fl & F_STALLP) strcat(fl, "|F_STALLP");
        memmove(fl, fl + 1, strlen(fl)); }
    printf("S(0x%04x, 0x%016llxull, %s, %s, %d, %d, %d, %d, %d, %d, 0x%04x, %af, %s, %d, %d, %d, %d, %s, %d, 0x%x, %d, %s);", i * 8, (unsigned long long)r->raw,
           UKN[r->uk], SRCN[r->usrc], r->ufs, r->uft, r->ubc, r->udst, r->udest, r->wr, r->mask16, (double)r->scale,
           LKN[r->lk], r->lit, r->lis, r->lid, r->ldest, fl, r->limm, r->ltarget, rec, pop);
}
static void g_goto(u32 i) {                       /* continue at instruction i */
    if (g_kind(i & 0x7FF)) printf(" goto I_%04x;", (i & 0x7FF) * 8); else printf(" { pc = 0x%04x; goto bail; }", (i & 0x7FF) * 8);
}
static int run_gen(const char *path, const char *entries) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    fseek(f, 0, SEEK_END); long size = ftell(f);
    static u8 micro[0x4000];
    fseek(f, size - 0x4000, SEEK_SET);            /* a raw 16K image, or a start dump (micro memory is its tail) */
    if (size < 0x4000 || fread(micro, 1, 0x4000, f) != 0x4000) { fprintf(stderr, "short image\n"); return 1; }
    fclose(f);
    u64 h = 0x1234567;
    for (u32 i = 0; i < 0x800; i++) { u64 w; memcpy(&w, micro + i * 8, 8); predecode(&G[i], (u32)w, (u32)(w >> 32), i * 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    /* jump targets */
    for (u32 i = 0; i < 0x800; i++) if (g_kind(i) == 3) {
        if (g_static(i)) g_target[(G[i].ltarget & 0x3FFF) >> 3] = 1;
        if ((G[i].lk == L_BAL || G[i].lk == L_JALR) && i + 2 < 0x800) g_target[i + 2] = 1;
    }
    /* Addresses JR/JALR reach that cannot be read off the program (a constant return address, the handlers of
     * a jump table). They only decide where the generated code has labels; a jump to an address without one
     * leaves the fast path for the interpreter. For the one program this port is built for (SLES-54218) they
     * are known (found once with: EXTRA=-DVU_STATS recomp/tools/vubench.sh starts <dump dir> 0): the command
     * loop's return address and the six plane handlers of the clipper. An entries file overrides them. */
    if (!entries && h == 0x208dc456c8f9e587ull) {
        static const u16 known[] = { 0x0028, 0x2a50, 0x2ab8, 0x2b28, 0x2b90, 0x2c00, 0x2c68 };
        for (unsigned k = 0; k < sizeof known / sizeof *known; k++) g_target[known[k] >> 3] = 1;
    }
    if (entries && (f = fopen(entries, "r"))) {   /* addresses JR/JALR were seen to reach (one hex pc per line) */
        unsigned a; char line[128];
        while (fgets(line, sizeof line, f)) if (sscanf(line, "%x", &a) == 1 && a < 0x4000) g_target[a >> 3] = 1;
        fclose(f);
    }
    for (u32 i = 0; i < 0x800; i++) if (g_kind(i)) {
        if (g_is_second(i)) { if (g_target[i] && i + 1 < 0x800) g_target[i + 1] = 1; }    /* its label rejoins through the dispatcher */
    }
    for (u32 i = 0; i < 0x800; i++) g_cj[i] = g_kind(i) && !g_is_second(i) && (g_target[i] || !g_fallthrough(i));
    printf("/* vu1_fast.inc — GENERATED by `recomp/tools/vubench.sh gen <micro image> [entries]`; do not edit.\n"
           " * VU1 micro program %016llx as straight-line code: see \"generated code\" in vu.c. */\n", (unsigned long long)h);
    printf("static const u64 vu1_fast_image[0x800] = {");
    for (u32 i = 0; i < 0x800; i++) printf("%s0x%016llxull,", i % 4 ? " " : "\n    ", (unsigned long long)G[i].raw);
    printf("\n};\n");
    printf("static int vu1_fast(u32 *ppc, u64 *pguard) {\n"
           "    VU *const v = &g_vu1; Pipe *const p = &vu_pipes[1];\n"
           "    u32 pc = *ppc, tg = 0, tg0 = 0; int tk = 0, tk0 = 0;\n"
           "    FlagRegs f = p->fr;                          /* the flag pipeline lives in locals here */\n"
           "#define S(PC, RAW, uk, usrc, ufs, uft, ubc, udst, udest, wr, mask16, scale, lk, lit, lis, lid, ldest, fl, limm, ltarget, rec, popk) \\\n"
           "    VU_STEP(1, v, p, &v->acc, &f, popk, vu1_mem, 0x3FFFu, 4, PC, RAW, uk, usrc, ufs, uft, ubc, udst, udest, wr, mask16, scale, lk, lit, lis, lid, ldest, fl, limm, ltarget, rec, tk, tg)\n"
           "#define G() if (UNLIKELY(++*pguard > VU_GUARD_MAX)) rt_fatal(0, \"VU1 micro program does not terminate (pc 0x%%x)\", pc)\n"
           "dispatch:\n    G();\n    switch (pc >> 3) {\n");
    for (u32 i = 0; i < 0x800; i++) if (g_kind(i) >= 2 && (g_cj[i] || g_is_second(i))) printf("    case 0x%03x: goto I_%04x;\n", i, i * 8);
    printf("    default: goto bail;\n    }\n");
    for (u32 i = 0; i < 0x800; i++) {
        int k = g_kind(i);
        if (!k) continue;
        printf("I_%04x: ", i * 8);
        if (k == 1) { printf("pc = 0x%04x; goto bail;\n", i * 8); continue; }
        if (g_is_second(i)) {                     /* jumped to directly: run it alone, then go through the dispatcher */
            g_step(i, 1, "POP_DYN");
            printf(" pc = 0x%04x; goto dispatch;\n", ((i + 1) & 0x7FF) * 8);
            continue;
        }
        if (k == 2) {
            g_step(i, i + 1 < 0x800 && g_branch(i + 1), g_pop(i, 0));
            if (!g_kind((i + 1) & 0x7FF) || i + 1 == 0x800) g_goto(i + 1);
            printf("\n");
        } else if (k == 3) {
            g_step(i, 1, g_pop(i, 0));
            printf("\n        tk0 = tk; tg0 = tg; ");
            g_step(i + 1, 1, g_pop(i + 1, 1));
            printf("\n        if (tk0) {");
            if (g_loop(i) >= 0) printf(" G(); goto L_%04x_%04x;", g_loop(i) * 8, i * 8);
            else if (g_static(i)) { printf(" G();"); g_goto((G[i].ltarget & 0x3FFF) >> 3); }
            else printf(" pc = tg0 & 0x3FF8; goto dispatch;");
            printf(" }");
            g_goto(i + 2);
            printf("\n");
        } else {
            g_step(i, 1, g_pop(i, 0));
            printf("\n        ");
            g_step(i + 1, 1, g_pop(i + 1, 1));
            printf(" pc = 0x%04x; goto done;\n", ((i + 2) & 0x7FF) * 8);
        }
    }
    for (u32 b = 0; b < 0x800; b++) {             /* loop copies */
        int t = g_loop(b);
        if (t < 0) continue;
        u32 len = b + 2 - (u32)t;                 /* T .. B, delay slot */
        printf("L_%04x_%04x:\n", t * 8, b * 8);
        for (u32 k = 0; k < len; k++) {
            u32 i = (u32)t + k;
            if (i == b) printf("        ");
            else if (i == b + 1) printf("\n        tk0 = tk; tg0 = tg; ");
            else printf("        ");
            g_step(i, i >= b || g_branch(i + 1), g_evkind((u32)t + (k + len - 4) % len));
            if (i < b) printf("\n");
        }
        printf("\n        if (tk0) { G(); goto L_%04x_%04x; }", t * 8, b * 8);
        g_goto(b + 2);
        printf("\n");
    }
    printf("done:     p->fr = f; *ppc = pc; return 0;\n"
           "bail:     p->fr = f; *ppc = pc; return 1;\n"
           "#undef S\n#undef G\n}\n");
    return 0;
}

#endif

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: vubench starts DIR [reps] | vubench stream FILE [reps]\n"); return 1; }
    _mm_setcsr(_mm_getcsr() | 0x8040);       /* FTZ | DAZ, as in the runtime's main() */
    vu_init();
#ifdef VU_HAS_PREDECODE
    if (strcmp(argv[1], "gen") == 0) return run_gen(argv[2], argc > 3 ? argv[3] : 0);
#endif
    vu_dump_frame = ~0ull;
    int reps = argc > 3 ? atoi(argv[3]) : 20;
    int rc = strcmp(argv[1], "stream") == 0 ? run_stream(argv[2], reps) : run_starts(argv[2], reps);
#ifdef VU_STATS
    fprintf(stderr, "instructions (all passes): %llu\n upper:", (unsigned long long)g_vu_icount);
    for (int i = 0; i < 16; i++) fprintf(stderr, " %d:%.1f%%", i, 100.0 * (double)g_vu_ucount[i] / (double)g_vu_icount);
    fprintf(stderr, "\n lower:");
    for (int i = 0; i < 64; i++) if (g_vu_lcount[i]) fprintf(stderr, " %d:%.1f%%", i, 100.0 * (double)g_vu_lcount[i] / (double)g_vu_icount);
    fprintf(stderr, "\n");
#ifdef VU_HAVE_FAST
    fprintf(stderr, " generated code handed back to the interpreter at (pc:count):");
    for (int i = 0; i < 0x800; i++) if (g_vu_bail[i]) fprintf(stderr, " %04x:%llu", i * 8, (unsigned long long)g_vu_bail[i]);
    fprintf(stderr, "\n");
#endif
#endif
    return rc;
}
