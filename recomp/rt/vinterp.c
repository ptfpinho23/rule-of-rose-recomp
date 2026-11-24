/* vinterp.c — frame interpolation from the inputs of the game's VU1 program.
 *
 * The game draws 3D through one VU1 micro program, a command interpreter:
 * MSCAL 0x08 executes the command list at TOP, MSCAL 0x40 one command. A
 * command is a quadword whose x is a handler address / 8: 0x190 (x, end,
 * start) copies the rows that follow to the globals start..end-1 (0x3d2..0x3ff);
 * 0x11 (x, n) sends the n quadwords that follow to the GS (XGKICK); 0x15
 * transforms the position palette; 0x39 / 0xc8 light; 0x3dc / 0x3de draw
 * indexed primitives; 0x0a ends the list. An object starts with the block
 * (0x190, 0x3e1, 0x3d2) + 15 rows: model -> world, its inverse, two 3x3 normal
 * matrices, node pointers in w of rows 12 and 14. A camera is the block
 * (0x190, 0x3ec, 0x3e2) + 10 rows: world -> screen, world -> clip, scales.
 *
 * What this file does with that:
 *  1. Recording (game thread). Every VIF1 command of a game frame is kept
 *     (vif.c calls vi_rec), grouped into runs (one per micro program start)
 *     and objects (from one object block to the next), together with every
 *     vertex the runs send to the GS (gs.c calls vi_vtx).
 *  2. Matching, at the frame boundary. A run is identified by the structure
 *     of its commands and by the addresses its data is read from, for data
 *     that both frames read from the same place (model memory; the display
 *     list buffers alternate, so their addresses never repeat). Objects are
 *     matched by their node pointers and their runs' identities, runs inside
 *     a matched pair of objects in order. A matched run that sent the same
 *     sequence of vertices (count, primitive types, PRIM writes) gives every
 *     vertex of the new frame its exact previous position: vertex i of the
 *     run was vertex i of the run before. gs.c uses that in place of pairing
 *     primitives by position in the draw list (the fallback for everything
 *     this file does not vouch for).
 *  3. Key times (worker threads, optional). The frame's commands are run
 *     again on a private VIF state and VU1 instance with inputs interpolated
 *     between the two frames (camera: rotation slerp, eye lerp; object
 *     blocks: rotation slerp, scale and translation lerp, inverse and normal
 *     matrices rebuilt; position palettes and other floats linear; integers
 *     and GS packets from the new frame). The vertices of such a pass are a
 *     third position between the two real ones for every run that kept its
 *     structure.
 * Nothing here runs unless vi_start was called (GPU renderer with --fps or a
 * temporal upscaler, --interp auto|inputs). */
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <xmmintrin.h>
#define VI_IMPL
#include "vinterp.h"
#include "plat.h"

typedef struct { u32 code, words, dest, tops, mask, src, off; u8 cl, wl, mode, rel; int run; } VCmd;      /* off: [code, data...] in the arena, ~0 = not kept */
typedef struct { int c0, c1, obj, v0, nv, nstatic, camc, camrow, camid; u32 entry; u64 key, vhash; } VRun;   /* commands c0..c1-1 (the last one starts the program) */
typedef struct { int r0, r1, setup; u64 key, skey; u32 p1, p2; float t[3]; } VObj;      /* key: node pointers + runs; skey: runs only */
typedef struct { const u32 *d; int count; } VCam;
#define VI_BITS (RAM_SIZE / 4 / 8)
#define VI_MAXCMD 600000              /* per recorded frame (the game's frames have about 30,000 commands and 45,000 vertices): beyond, the frame is not used */
#define VI_MAXVTX 2000000
typedef struct VFrame_ {
    VCmd *c; int nc, cc; VRun *r; int nr, cr; VObj *o; int no, co; u32 *arena; size_t an, ac; VVtx *v; int nv, cv;
    u8 *bits;                       /* one bit per word of guest RAM: a command's data was read from there */
    VCam cam[16]; int ncam, maincam;
    u8 *vif, *vu; int have_snap, full, bad, used; u64 serial; atomic_int hold;
} VFrame;
static VFrame vf_ring[4]; static VFrame *vf_cur, *vf_prev;
static int vi_mode = 1, vi_on, vi_nkeys, vi_threads_want, vi_debug;
static u64 vi_serial;
u64 g_vi_stat[VIS_N];
static VResult vi_res; static int *vi_delta, *vi_v0, vi_dcap;
static int *vi_objm, *vi_runm, vi_mcap;                 /* per object / run of the new frame: its partner in the previous one or -1 */
static u64 run_hash; static VRun *run_cur;

void vi_set_mode(int mode) { vi_mode = mode; }
int  vi_get_mode(void) { return vi_mode; }
int  vi_keys_wanted(void) { return vi_on ? vi_nkeys : 0; }
void vi_start(void) {
    if (vi_on || vi_mode == 0) return;
    const char *e = getenv("ROR_INTERP_KEYS"); vi_nkeys = e ? atoi(e) : 0;
    if (vi_nkeys < 0) vi_nkeys = 0; if (vi_nkeys > 7) vi_nkeys = 7;
    e = getenv("ROR_INTERP_THREADS"); vi_threads_want = e ? atoi(e) : 0;
    vi_debug = getenv("ROR_INTERP_DEBUG") ? atoi(getenv("ROR_INTERP_DEBUG")) : 0;
    vi_on = 1; g_vi_rec = 1;
    rt_log("[interp] in-between pictures from the VU1 program's inputs (%s), %d key time%s per game frame\n", vi_mode == 2 ? "--interp inputs" : "--interp auto", vi_nkeys, vi_nkeys == 1 ? "" : "s");
}

/* ---- recording ---------------------------------------------------------------------------- */
static VFrame *frame_begin(void) {
    VFrame *f = 0;
    for (int i = 0; i < 4 && !f; i++) if (&vf_ring[i] != vf_prev && atomic_load(&vf_ring[i].hold) == 0) f = &vf_ring[i];
    if (!f) return 0;
    if (!f->bits) { f->bits = calloc(VI_BITS, 1); f->vif = malloc(vif_state_size()); f->vu = malloc(vu1_snap_size()); }
    else if (f->used) memset(f->bits, 0, VI_BITS);
    f->nc = f->nr = f->no = f->nv = f->ncam = 0; f->an = 0; f->bad = 0; f->used = 1; f->maincam = -1;
    f->full = vi_nkeys > 0; f->have_snap = 0; f->serial = ++vi_serial;
    if (f->full) { vif1_state_save(f->vif); vu1_snap_save(f->vu); f->have_snap = 1; }
    return f;
}
static int rec_off;              /* this frame cannot be recorded (no free record) */
void vi_rec(const VRec *q) {
    VFrame *f = vf_cur;
    if (!f) { if (rec_off) return; f = vf_cur = frame_begin(); if (!f) { rec_off = 1; return; } }
    u32 cmd = (q->code >> 24) & 0x7F;
    if (cmd == 0x00 || cmd == 0x06 || cmd == 0x07 || cmd == 0x10 || cmd == 0x11 || cmd == 0x13) return;
    if (cmd == 0x4A) { f->bad |= 2; return; }
    if (f->nc >= VI_MAXCMD || f->nv >= VI_MAXVTX) { f->bad |= 2; return; }      /* (a frame that never ends: no flip for a long time) */
    if (f->nc == f->cc) { f->cc = f->cc ? f->cc * 2 : 32768; f->c = realloc(f->c, sizeof(VCmd) * (size_t)f->cc); }
    VCmd *c = &f->c[f->nc++];
    c->code = q->code; c->words = q->words; c->dest = q->dest; c->tops = q->tops; c->mask = q->mask; c->cl = q->cl; c->wl = q->wl; c->mode = q->mode; c->rel = q->rel; c->run = f->nr;
    uintptr_t g = (uintptr_t)(q->w + 4) - (uintptr_t)HPTR(0);
    c->src = (g < RAM_SIZE && q->words) ? (u32)g : 0;
    if (c->src) f->bits[c->src >> 5] |= (u8)(1u << ((c->src >> 2) & 7));
    c->off = ~0u;
    if (f->full || ((q->code >> 24) & 0x6F) == 0x6C) {
        if (f->an + q->words + 1 > f->ac) { f->ac = f->ac ? f->ac * 2 : (1u << 20); while (f->an + q->words + 1 > f->ac) f->ac *= 2; f->arena = realloc(f->arena, f->ac * 4); }
        c->off = (u32)f->an; memcpy(f->arena + f->an, q->w, ((size_t)q->words + 1) * 4); f->an += q->words + 1;
    }
}
void vi_note(int what) { if (vf_cur) vf_cur->bad |= 1 << what; else if (vi_on && !rec_off) { vf_cur = frame_begin(); if (vf_cur) vf_cur->bad |= 1 << what; } }
void vi_run_begin(u32 entry) {
    VFrame *f = vf_cur; if (!f) return;
    if (f->nc >= VI_MAXCMD || f->nv >= VI_MAXVTX) { f->bad |= 2; return; }
    if (f->nr == f->cr) { f->cr = f->cr ? f->cr * 2 : 8192; f->r = realloc(f->r, sizeof(VRun) * (size_t)f->cr); }
    VRun *r = &f->r[f->nr]; memset(r, 0, sizeof *r);
    r->c0 = f->nr ? f->r[f->nr - 1].c1 : 0; r->c1 = f->nc; r->entry = entry; r->v0 = f->nv; r->camc = -1; r->camid = -1;
    run_cur = r; run_hash = 1469598103934665603ull; g_vi_run = f->nr;
}
void vi_run_end(void) {
    VFrame *f = vf_cur; if (!f || !run_cur) return;
    run_cur->nv = f->nv - run_cur->v0; run_cur->vhash = run_hash; run_cur = 0; f->nr++; g_vi_run = -1;
}
int vi_vtx(s32 x, s32 y, u32 z, u32 fog, u32 rgba, float q, float s, float t, u32 u, u32 v, int kind) {
    VFrame *f = vf_cur;
    if (f->nv == f->cv) { f->cv = f->cv ? f->cv * 2 : 65536; f->v = realloc(f->v, sizeof(VVtx) * (size_t)f->cv); }
    VVtx *o = &f->v[f->nv];
    o->x = x; o->y = y; o->z = z; o->rgba = rgba; o->q = q; o->s = s; o->t = t; o->u = (u16)u; o->v = (u16)v; o->f = (u8)fog; o->kind = (u8)kind; o->pad[0] = o->pad[1] = 0;
    run_hash = (run_hash ^ (u64)((kind & 7) + 1)) * 1099511628211ull;      /* (not the kick flag: a triangle left out in one frame still has its vertices in both) */
    return f->nv++;
}
void vi_prim_mark(void) { run_hash = (run_hash ^ 0x55u) * 1099511628211ull; }

/* ---- what a run's parameter block holds ---------------------------------------------------- */
static inline const u32 *cmd_data(const VFrame *f, const VCmd *c) { return f->arena + c->off + 1; }
static int is_objblock(const VFrame *f, const VCmd *c) {
    if ((c->code >> 24) != 0x6C || c->words < 64 || c->off == ~0u) return 0;
    const u32 *d = cmd_data(f, c);
    return d[0] == 0x190 && d[1] == 0x3e1 && d[2] == 0x3d2;
}
/* The command list of a run that starts at TOP: where a 0x190 copy puts the globals 0x3e2.. (the camera).
 * Returns the row of the first camera row inside command c's data, or -1. */
static int find_camera(const VFrame *f, const VCmd *c) {
    if ((c->code >> 24) != 0x6C || !c->rel || c->dest != c->tops || c->off == ~0u) return -1;
    const u32 *d = cmd_data(f, c); u32 rows = c->words / 4;
    for (u32 r = 0; r < rows;) {
        u32 x = d[4 * r];
        if (x == 0x11) { r += 1 + d[4 * r + 1]; continue; }
        if (x != 0x190) break;
        u32 end = d[4 * r + 1], start = d[4 * r + 2];
        if (end <= start || end > 0x400) break;
        if (start <= 0x3e2 && end >= 0x3ea && r + 1 + (0x3e2 - start) + 8 <= rows) return (int)(r + 1 + (0x3e2 - start));
        r += 1 + (end - start);
    }
    return -1;
}
static void frame_finish(VFrame *f) {
    f->no = 0; f->ncam = 0; f->maincam = -1;
    for (int i = 0; i < f->nr; i++) {
        VRun *r = &f->r[i]; int setup = -1;
        for (int k = r->c0; k < r->c1; k++) {
            const VCmd *c = &f->c[k];
            if (r->entry == 0x40) { if (setup < 0 && is_objblock(f, c)) setup = k; }
            else if (r->camc < 0) { int row = find_camera(f, c); if (row >= 0) { r->camc = k; r->camrow = row; } }
        }
        if (r->camc >= 0) {
            const u32 *d = cmd_data(f, &f->c[r->camc]) + 4 * r->camrow; int j;
            for (j = 0; j < f->ncam; j++) if (!memcmp(f->cam[j].d, d, 128)) break;
            if (j == f->ncam && f->ncam < 16) { f->cam[j].d = d; f->cam[j].count = 0; f->ncam++; }
            if (j < f->ncam) { f->cam[j].count++; r->camid = j; if (f->maincam < 0 || f->cam[j].count > f->cam[f->maincam].count) f->maincam = j; }
        }
        if (setup >= 0 || f->no == 0) {
            if (f->no == f->co) { f->co = f->co ? f->co * 2 : 1024; f->o = realloc(f->o, sizeof(VObj) * (size_t)f->co); }
            VObj *o = &f->o[f->no++]; memset(o, 0, sizeof *o); o->r0 = i; o->setup = setup;
            if (setup >= 0) { const u32 *d = cmd_data(f, &f->c[setup]); o->p1 = d[4 * 12 + 3]; o->p2 = d[4 * 14 + 3]; memcpy(o->t, d + 16, 12); }
        }
        r->obj = f->no - 1; f->o[f->no - 1].r1 = i + 1;
    }
}
static inline u64 mix(u64 h, u64 x) { h ^= x; h *= 0x9E3779B97F4A7C15ull; return h ^ (h >> 29); }
/* Identities of the runs and objects of f, as seen together with frame g: a source address counts when both read from it. */
static void frame_keys(VFrame *f, const VFrame *g) {
    for (int i = 0; i < f->nr; i++) {
        VRun *r = &f->r[i]; u64 h = 0x1234567ull + r->entry; int ns = 0;
        for (int k = r->c0; k < r->c1; k++) {
            const VCmd *c = &f->c[k]; u32 cmd = (c->code >> 24) & 0x7F, src = 0;
            if (c->src && (g->bits[c->src >> 5] >> ((c->src >> 2) & 7)) & 1) { src = c->src; ns++; }
            h = mix(h, (u64)(c->code & 0xFFFF0000u) | ((u64)(c->rel ? c->dest - c->tops : c->dest) << 32) | ((u64)c->rel << 48));
            h = mix(h, (u64)src | ((u64)(cmd >= 0x70 ? c->mask : 0) << 32));
            if (cmd >= 0x60) h = mix(h, (u64)c->cl | ((u64)c->wl << 8) | ((u64)c->mode << 16) | ((u64)c->words << 32));
        }
        r->key = h; r->nstatic = ns;
    }
    for (int i = 0; i < f->no; i++) {
        VObj *o = &f->o[i]; u64 h = 0x51ed27ull;
        for (int k = o->r0; k < o->r1; k++) h = mix(h, f->r[k].key);
        o->skey = h; o->key = mix(h, mix(o->p1, o->p2));
    }
}
/* Objects first: same node pointers and the same runs (among several, the nearest); then same pointers and most
 * runs shared; then most runs shared, whatever the pointers. Runs of a matched pair of objects pair by identity in
 * order of occurrence. The first two passes look candidates up by hash (a scene can have thousands of objects); the
 * third compares everything left with everything left and is given up when that would be too much work. */
static int common_runs(const VFrame *A, const VObj *a, const VFrame *B, const VObj *b) {
    int common = 0;
    for (int x = b->r0; x < b->r1; x++) for (int y = a->r0; y < a->r1; y++) if (B->r[x].key == A->r[y].key) { common++; break; }
    return common;
}
static void match_frames(const VFrame *A, const VFrame *B, int *objm, int *runm, int *n_match) {
    static u8 *used, *rused; static int ucap, rcap, *head, *nextk, *nextp, hcap, ncap;
    if (ucap < A->no + 1) { ucap = A->no + 1024; used = realloc(used, (size_t)ucap); }
    if (rcap < A->nr + 1) { rcap = A->nr + 8192; rused = realloc(rused, (size_t)rcap); }
    memset(used, 0, (size_t)A->no + 1); memset(rused, 0, (size_t)A->nr + 1);
    for (int i = 0; i < B->no; i++) objm[i] = -1;
    for (int i = 0; i < B->nr; i++) runm[i] = -1;
    int hn = 256; while (hn < A->no * 2) hn *= 2;
    if (hcap < hn * 2) { hcap = hn * 2; head = realloc(head, sizeof(int) * (size_t)hcap); }
    if (ncap < A->no + 1) { ncap = A->no + 1024; nextk = realloc(nextk, sizeof(int) * (size_t)ncap); nextp = realloc(nextp, sizeof(int) * (size_t)ncap); }
    int *hk = head, *hp = head + hn;                     /* chains by identity and by node pointers, in list order */
    for (int k = 0; k < hn * 2; k++) head[k] = -1;
    #define HK(o) ((int)(((o)->key * 0x9E3779B97F4A7C15ull) >> 40) & (hn - 1))
    #define HP(o) ((int)((mix((o)->p1, (o)->p2) * 0x9E3779B97F4A7C15ull) >> 40) & (hn - 1))
    for (int k = A->no - 1; k >= 0; k--) { const VObj *a = &A->o[k]; int s1 = HK(a), s2 = HP(a); nextk[k] = hk[s1]; hk[s1] = k; nextp[k] = hp[s2]; hp[s2] = k; }
    long budget = 2000000; int left = 0;
    for (int i = 0; i < B->no; i++) {                    /* same identity: the nearest */
        const VObj *b = &B->o[i]; int best = -1; double bd = 1e30;
        for (int k = hk[HK(b)]; k >= 0; k = nextk[k]) {
            const VObj *a = &A->o[k];
            if (used[k] || a->key != b->key || a->p1 != b->p1 || a->p2 != b->p2 || (a->setup < 0) != (b->setup < 0)) continue;
            double d = 0; for (int q = 0; q < 3; q++) d += ((double)a->t[q] - b->t[q]) * ((double)a->t[q] - b->t[q]);
            if (d < bd) { bd = d; best = k; }
        }
        if (best >= 0) { objm[i] = best; used[best] = 1; }
    }
    for (int i = 0; i < B->no; i++) {                    /* same node pointers: the one sharing most runs, more than half */
        if (objm[i] >= 0) continue;
        const VObj *b = &B->o[i]; int best = -1, bc = 0;
        for (int k = hp[HP(b)]; k >= 0 && budget > 0; k = nextp[k]) {
            const VObj *a = &A->o[k];
            if (used[k] || a->p1 != b->p1 || a->p2 != b->p2 || (a->setup < 0) != (b->setup < 0)) continue;
            budget -= (long)(b->r1 - b->r0) * (a->r1 - a->r0);
            int c = common_runs(A, a, B, b);
            if (c * 2 > (b->r1 - b->r0) && c > bc) { bc = c; best = k; }
        }
        if (best >= 0) { objm[i] = best; used[best] = 1; } else left++;
    }
    if (left) {
        /* The same runs under other "node pointers": seen in the third area, 715 objects of six runs each whose two
         * words are not pointers but values that change every frame. Objects made of the same runs pair in order of
         * occurrence; whether a pair of runs is then really the same thing is decided per run, as always (structure of
         * the vertices sent, and texture coordinates where no data address vouches for it). */
        for (int k = 0; k < hn; k++) hk[k] = -1;
        #define HS(o) ((int)(((o)->skey * 0x9E3779B97F4A7C15ull) >> 40) & (hn - 1))
        for (int k = A->no - 1; k >= 0; k--) if (!used[k]) { int s1 = HS(&A->o[k]); nextk[k] = hk[s1]; hk[s1] = k; }
        left = 0;
        for (int i = 0; i < B->no; i++) {
            if (objm[i] >= 0) continue;
            const VObj *b = &B->o[i]; int *link = &hk[HS(b)];
            for (; *link >= 0; link = &nextk[*link]) if (A->o[*link].skey == b->skey && (A->o[*link].setup < 0) == (b->setup < 0)) break;
            if (*link >= 0) { objm[i] = *link; used[*link] = 1; *link = nextk[*link]; } else left++;
        }
        #undef HS
    }
    if (left) for (int i = 0; i < B->no && budget > 0; i++) {     /* anything sharing most runs */
        if (objm[i] >= 0) continue;
        const VObj *b = &B->o[i]; int best = -1, bc = 0;
        for (int k = 0; k < A->no && budget > 0; k++) {
            const VObj *a = &A->o[k];
            if (used[k] || (a->setup < 0) != (b->setup < 0)) continue;
            budget -= (long)(b->r1 - b->r0) * (a->r1 - a->r0) + 1;
            int c = common_runs(A, a, B, b);
            if (c * 2 > (b->r1 - b->r0) && c > bc) { bc = c; best = k; }
        }
        if (best >= 0) { objm[i] = best; used[best] = 1; }
    }
    #undef HK
    #undef HP
    int nm = 0;
    for (int i = 0; i < B->no; i++) {
        if (objm[i] < 0) continue;
        const VObj *b = &B->o[i], *a = &A->o[objm[i]]; int y0 = a->r0;
        for (int x = b->r0; x < b->r1; x++) {
            int hit = -1, lim = 4096;                    /* (the usual case is the very next run; an object of thousands of runs must not turn this quadratic) */
            for (int y = y0; y < a->r1 && lim-- > 0; y++) if (!rused[y] && A->r[y].key == B->r[x].key) { hit = y; break; }
            if (hit < 0) for (int y = a->r0; y < y0 && lim-- > 0; y++) if (!rused[y] && A->r[y].key == B->r[x].key) { hit = y; break; }
            if (hit >= 0) { runm[x] = hit; rused[hit] = 1; y0 = hit + 1; nm++; }
        }
    }
    *n_match = nm;
}

/* ---- matrices ------------------------------------------------------------------------------- */
typedef double M3[3][3];
static inline float wf(u32 x) { float f; memcpy(&f, &x, 4); return f; }
static inline u32 fw(double d) { float f = (float)d; u32 x; memcpy(&x, &f, 4); return x; }
static void m2q(M3 R, double q[4]) {                 /* rotation matrix -> quaternion (w, x, y, z) */
    double tr = R[0][0] + R[1][1] + R[2][2];
    if (tr > 0) { double s = sqrt(tr + 1.0) * 2; q[0] = 0.25 * s; q[1] = (R[2][1] - R[1][2]) / s; q[2] = (R[0][2] - R[2][0]) / s; q[3] = (R[1][0] - R[0][1]) / s; }
    else if (R[0][0] > R[1][1] && R[0][0] > R[2][2]) { double s = sqrt(1.0 + R[0][0] - R[1][1] - R[2][2]) * 2; q[0] = (R[2][1] - R[1][2]) / s; q[1] = 0.25 * s; q[2] = (R[0][1] + R[1][0]) / s; q[3] = (R[0][2] + R[2][0]) / s; }
    else if (R[1][1] > R[2][2]) { double s = sqrt(1.0 + R[1][1] - R[0][0] - R[2][2]) * 2; q[0] = (R[0][2] - R[2][0]) / s; q[1] = (R[0][1] + R[1][0]) / s; q[2] = 0.25 * s; q[3] = (R[1][2] + R[2][1]) / s; }
    else { double s = sqrt(1.0 + R[2][2] - R[0][0] - R[1][1]) * 2; q[0] = (R[1][0] - R[0][1]) / s; q[1] = (R[0][2] + R[2][0]) / s; q[2] = (R[1][2] + R[2][1]) / s; q[3] = 0.25 * s; }
}
static void q2m(const double q[4], M3 R) {
    double w = q[0], x = q[1], y = q[2], z = q[3];
    R[0][0] = 1 - 2 * (y * y + z * z); R[0][1] = 2 * (x * y - w * z); R[0][2] = 2 * (x * z + w * y);
    R[1][0] = 2 * (x * y + w * z); R[1][1] = 1 - 2 * (x * x + z * z); R[1][2] = 2 * (y * z - w * x);
    R[2][0] = 2 * (x * z - w * y); R[2][1] = 2 * (y * z + w * x); R[2][2] = 1 - 2 * (x * x + y * y);
}
/* out = A turned towards B by t; returns the whole angle in degrees */
static double rot_slerp(M3 A, M3 B, double t, M3 out) {
    double qa[4], qb[4], q[4]; m2q(A, qa); m2q(B, qb);
    double d = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
    if (d < 0) { d = -d; for (int i = 0; i < 4; i++) qb[i] = -qb[i]; }
    if (d > 1) d = 1;
    double th = acos(d), wa, wb;
    if (th < 1e-6) { wa = 1 - t; wb = t; } else { wa = sin((1 - t) * th) / sin(th); wb = sin(t * th) / sin(th); }
    double n = 0; for (int i = 0; i < 4; i++) { q[i] = wa * qa[i] + wb * qb[i]; n += q[i] * q[i]; }
    n = sqrt(n); for (int i = 0; i < 4; i++) q[i] /= n;
    if (out) q2m(q, out);
    return 2 * th * 57.29577951308232;
}
static int norm3(double *v) { double n = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (n < 1e-12) return 0; v[0] /= n; v[1] /= n; v[2] /= n; return 1; }
/* Camera: 8 rows, world -> screen then world -> clip (row vectors: out = x * row0 + y * row1 + z * row2 + row3);
 * both have the view depth as their w column. Decomposed into a rotation Q (columns: right, up, forward), the eye
 * and, per matrix column, what remains in camera space. */
typedef struct { M3 Q; double e[3], k[8][3], p[8]; int ok; } CamDec;
static void cam_decompose(const u32 *d, CamDec *c) {
    double col[8][3], tr[8];
    for (int m = 0; m < 2; m++) for (int j = 0; j < 4; j++) { for (int r = 0; r < 3; r++) col[m * 4 + j][r] = wf(d[m * 16 + r * 4 + j]); tr[m * 4 + j] = wf(d[m * 16 + 12 + j]); }
    double f[3] = { col[7][0], col[7][1], col[7][2] }, e0[3], e1[3];
    c->ok = norm3(f);
    double dp = col[4][0] * f[0] + col[4][1] * f[1] + col[4][2] * f[2];
    for (int r = 0; r < 3; r++) e0[r] = col[4][r] - dp * f[r];
    c->ok &= norm3(e0);
    e1[0] = f[1] * e0[2] - f[2] * e0[1]; e1[1] = f[2] * e0[0] - f[0] * e0[2]; e1[2] = f[0] * e0[1] - f[1] * e0[0];
    for (int r = 0; r < 3; r++) { c->Q[r][0] = e0[r]; c->Q[r][1] = e1[r]; c->Q[r][2] = f[r]; }
    double a[3][3], b[3]; int js[3] = { 4, 5, 7 };      /* eye: clip x, y and w are linear in (world - eye) */
    for (int i = 0; i < 3; i++) { for (int r = 0; r < 3; r++) a[i][r] = col[js[i]][r]; b[i] = -tr[js[i]]; }
    double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (!(fabs(det) > 1e-12)) { c->ok = 0; return; }
    for (int k = 0; k < 3; k++) {
        double m[3][3]; memcpy(m, a, sizeof m); for (int i = 0; i < 3; i++) m[i][k] = b[i];
        c->e[k] = (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0])) / det;
    }
    for (int j = 0; j < 8; j++) {
        for (int q = 0; q < 3; q++) c->k[j][q] = c->Q[0][q] * col[j][0] + c->Q[1][q] * col[j][1] + c->Q[2][q] * col[j][2];
        c->p[j] = tr[j] + col[j][0] * c->e[0] + col[j][1] * c->e[1] + col[j][2] * c->e[2];
    }
    for (int i = 0; i < 3; i++) if (!isfinite(c->e[i])) c->ok = 0;
}
static void cam_interp(const u32 *da, const u32 *db, double t, u32 *out) {
    CamDec A, B; cam_decompose(da, &A); cam_decompose(db, &B);
    if (!A.ok || !B.ok) { for (int i = 0; i < 32; i++) out[i] = fw(wf(da[i]) + ((double)wf(db[i]) - wf(da[i])) * t); return; }
    M3 Q; rot_slerp(A.Q, B.Q, t, Q);
    double e[3]; for (int r = 0; r < 3; r++) e[r] = A.e[r] + (B.e[r] - A.e[r]) * t;
    for (int j = 0; j < 8; j++) {
        double k[3], col[3], p = A.p[j] + (B.p[j] - A.p[j]) * t;
        for (int q = 0; q < 3; q++) k[q] = A.k[j][q] + (B.k[j][q] - A.k[j][q]) * t;
        for (int r = 0; r < 3; r++) col[r] = Q[r][0] * k[0] + Q[r][1] * k[1] + Q[r][2] * k[2];
        int m = j / 4, c = j & 3;
        for (int r = 0; r < 3; r++) out[m * 16 + r * 4 + c] = fw(col[r]);
        out[m * 16 + 12 + c] = fw(p - (col[0] * e[0] + col[1] * e[1] + col[2] * e[2]));
    }
}
static int rows_frame(const double L[3][3], M3 Q) {       /* orthonormal rows from the rows of L */
    double q0[3] = { L[0][0], L[0][1], L[0][2] }, q1[3], q2[3];
    if (!norm3(q0)) return 0;
    double dp = L[1][0] * q0[0] + L[1][1] * q0[1] + L[1][2] * q0[2];
    for (int r = 0; r < 3; r++) q1[r] = L[1][r] - dp * q0[r];
    if (!norm3(q1)) return 0;
    q2[0] = q0[1] * q1[2] - q0[2] * q1[1]; q2[1] = q0[2] * q1[0] - q0[0] * q1[2]; q2[2] = q0[0] * q1[1] - q0[1] * q1[0];
    for (int r = 0; r < 3; r++) { Q[0][r] = q0[r]; Q[1][r] = q1[r]; Q[2][r] = q2[r]; }
    return 1;
}
/* Object block, the 15 rows after the command row: model -> world (4 rows), its inverse (4), two 3x3 (rows 8-10
 * like the matrix, 11-13 like the inverse; their w is not a float), one more row. */
static void obj_interp(const u32 *da, const u32 *db, double t, u32 *out) {
    double LA[3][3], LB[3][3]; M3 QA, QB, Q;
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) { LA[r][c] = wf(da[r * 4 + c]); LB[r][c] = wf(db[r * 4 + c]); }
    memcpy(out, db, 60 * 4);
    if (!rows_frame(LA, QA) || !rows_frame(LB, QB)) {
        for (int i = 0; i < 56; i++) if ((i & 3) != 3) out[i] = fw(wf(da[i]) + ((double)wf(db[i]) - wf(da[i])) * t);
        return;
    }
    rot_slerp(QA, QB, t, Q);
    for (int blk = 0; blk < 4; blk++) {                    /* 0: matrix, 1: inverse, 2: rows 8-10, 3: rows 11-13 */
        int base = blk == 0 ? 0 : blk == 1 ? 16 : blk == 2 ? 32 : 44;
        double XA[3][3], XB[3][3], KA[3][3], KB[3][3], K[3][3];
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) { XA[r][c] = wf(da[base + r * 4 + c]); XB[r][c] = wf(db[base + r * 4 + c]); }
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) {
            KA[r][c] = KB[r][c] = 0;
            for (int k = 0; k < 3; k++) {
                if (!(blk & 1)) { KA[r][c] += XA[r][k] * QA[c][k]; KB[r][c] += XB[r][k] * QB[c][k]; }      /* K = X Q^T */
                else { KA[r][c] += QA[r][k] * XA[k][c]; KB[r][c] += QB[r][k] * XB[k][c]; }                  /* K = Q X  */
            }
        }
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) K[r][c] = KA[r][c] + (KB[r][c] - KA[r][c]) * t;
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) {
            double x = 0;
            for (int k = 0; k < 3; k++) x += !(blk & 1) ? K[r][k] * Q[k][c] : Q[k][r] * K[k][c];              /* X = K Q  /  Q^T K */
            out[base + r * 4 + c] = fw(x);
        }
    }
    for (int i = 3; i < 44; i += 4) out[i] = fw(wf(da[i]) + ((double)wf(db[i]) - wf(da[i])) * t);      /* the w column of the first 11 rows */
    double T[3], Ti[3], err = 0;
    for (int c = 0; c < 3; c++) { T[c] = wf(da[12 + c]) + ((double)wf(db[12 + c]) - wf(da[12 + c])) * t; out[12 + c] = fw(T[c]); }
    /* inverse translation = -T * Linv, if that is what the game sends (checked on the new frame's own data) */
    for (int c = 0; c < 3; c++) { double chk = 0; for (int k = 0; k < 3; k++) chk -= (double)wf(db[12 + k]) * wf(db[16 + k * 4 + c]); err = fmax(err, fabs(chk - wf(db[28 + c])) / (1.0 + fabs(chk))); }
    for (int c = 0; c < 3; c++) {
        if (err < 1e-2) { Ti[c] = 0; for (int k = 0; k < 3; k++) Ti[c] -= T[k] * wf(out[16 + k * 4 + c]); }
        else Ti[c] = wf(da[28 + c]) + ((double)wf(db[28 + c]) - wf(da[28 + c])) * t;
        out[28 + c] = fw(Ti[c]);
    }
}
static inline int plausible(u32 x) { u32 e = (x >> 23) & 0xFF; return (x & 0x7FFFFFFFu) == 0 || (e >= 0x58 && e <= 0x9D); }   /* a float between about 1e-12 and 1e9, or zero */

/* ---- the frame boundary --------------------------------------------------------------------- */
static void keys_prepare(VFrame *A, VFrame *B);
const VResult *vi_boundary(void) {
    VResult *R = &vi_res; VFrame *B = vf_cur, *A = vf_prev;
    memset(R, 0, sizeof *R);
    rec_off = 0; vf_cur = 0; g_vi_run = -1; run_cur = 0;
    if (!vi_on) return R;
    if (!B) { vf_prev = 0; return R; }                       /* a frame without VU1 work */
    u64 t0 = plat_time_ns();
    frame_finish(B);
    R->nrun = B->nr; R->nvtx = B->nv; R->cur = B->v; R->serial = B->serial;
    g_vi_stat[VIS_FRAMES]++;
    if (!B->nr) R->why = 0;
    else if (!A || !A->nr) R->why = 1;
    else if (!vu1_program_known()) { R->why = 2; g_vi_stat[VIS_UNKNOWN]++; }
    else if ((A->bad | B->bad) & 6) R->why = 3;
    else {
        if (vi_dcap < B->nr + 1) { vi_dcap = B->nr + 8192; vi_delta = realloc(vi_delta, sizeof(int) * (size_t)vi_dcap); vi_v0 = realloc(vi_v0, sizeof(int) * (size_t)vi_dcap); }
        if (vi_mcap < B->nr + B->no + 2) { vi_mcap = B->nr + B->no + 8192; vi_objm = realloc(vi_objm, sizeof(int) * (size_t)vi_mcap); vi_runm = realloc(vi_runm, sizeof(int) * (size_t)vi_mcap); }
        frame_keys(A, B); frame_keys(B, A);
        int nm = 0; match_frames(A, B, vi_objm, vi_runm, &nm);
        int ok = 0, nopart = 0, nstruct = 0, nuv = 0;
        for (int x = 0; x < B->nr; x++) {
            const VRun *b = &B->r[x]; vi_v0[x] = b->v0; vi_delta[x] = VI_NONE;
            if (vi_runm[x] < 0) { nopart++; continue; }
            const VRun *a = &A->r[vi_runm[x]];
            if (a->nv != b->nv || a->vhash != b->vhash) { nstruct++; continue; }
            if (!b->nstatic) {          /* nothing but its place in the list says it is the same mesh: the texture coordinates must agree */
                const VVtx *va = A->v + a->v0, *vb = B->v + b->v0; int bad = 0;
                for (int i = 0; i < b->nv && !bad; i++) bad = fabsf(va[i].s * vb[i].q - vb[i].s * va[i].q) > 0.004f * fabsf(va[i].q * vb[i].q) || fabsf(va[i].t * vb[i].q - vb[i].t * va[i].q) > 0.004f * fabsf(va[i].q * vb[i].q) || va[i].u != vb[i].u || va[i].v != vb[i].v;
                if (bad) { nuv++; continue; }
            }
            vi_delta[x] = a->v0 - b->v0; ok++;
        }
        g_vi_stat[VIS_RUNS] += (u64)B->nr; g_vi_stat[VIS_RUNS_OK] += (u64)ok; g_vi_stat[VIS_RUNS_NOPARTNER] += (u64)nopart; g_vi_stat[VIS_RUNS_STRUCT] += (u64)nstruct; g_vi_stat[VIS_RUNS_UV] += (u64)nuv;
        R->delta = vi_delta; R->v0 = vi_v0; R->prev = A->v; R->pnvtx = A->nv; R->prun = vi_runm; R->pnrun = A->nr; R->usable = 1;
        /* camera cut: the camera turns or moves more than any continuous motion does, or most runs have no partner */
        if (A->maincam >= 0 && B->maincam >= 0) {
            CamDec ca, cb; cam_decompose(A->cam[A->maincam].d, &ca); cam_decompose(B->cam[B->maincam].d, &cb);
            if (ca.ok && cb.ok) {
                double mv = 0; for (int i = 0; i < 3; i++) mv += (cb.e[i] - ca.e[i]) * (cb.e[i] - ca.e[i]);
                R->cam_turn = (float)rot_slerp(ca.Q, cb.Q, 0.5, 0); R->cam_move = (float)sqrt(mv);
                if (R->cam_turn > 5.0f || R->cam_move > 100.0f) R->cut = 1;
            }
        }
        if (nm * 10 < B->nr * 7) { R->cut = 1; R->why = 4; } else if (R->cut) R->why = 5;
        if (R->cut) g_vi_stat[VIS_CUT]++; else g_vi_stat[VIS_USED]++;
        if (vi_debug) rt_log("[interp] frame %llu: %d runs (%d before), %d objects; runs matched %d, usable %d, no partner %d, other structure %d, other texture coordinates %d; camera turned %.2f deg, moved %.1f%s\n",
                             (unsigned long long)B->serial, B->nr, A->nr, B->no, nm, ok, nopart, nstruct, nuv, R->cam_turn, R->cam_move, R->cut ? " -> cut" : "");
        if (vi_debug > 1) {
            int ou = 0, ru_o = 0, ru_i = 0, big = 0, nos = 0; long dupk = 0;
            for (int i = 0; i < B->no; i++) { const VObj *o = &B->o[i]; if (o->r1 - o->r0 > big) big = o->r1 - o->r0; nos += o->setup < 0; if (vi_objm[i] < 0) { ou++; ru_o += o->r1 - o->r0; } else for (int x = o->r0; x < o->r1; x++) ru_i += vi_runm[x] < 0; }
            for (int i = 1; i < B->no; i++) dupk += B->o[i].p1 == B->o[i - 1].p1 && B->o[i].p2 == B->o[i - 1].p2;
            rt_log("[interp]   objects without a partner %d (their runs %d), runs without a partner inside matched objects %d; largest object %d runs, objects without a block %d, consecutive objects with the same node pointers %ld\n", ou, ru_o, ru_i, big, nos, dupk);
            for (int i = 0, shown = 0; i < B->no && shown < 6; i++) if (vi_objm[i] < 0) { const VObj *o = &B->o[i]; shown++; rt_log("[interp]     object %d: runs %d, node pointers %08x %08x, position %.1f %.1f %.1f, key %016llx\n", i, o->r1 - o->r0, o->p1, o->p2, o->t[0], o->t[1], o->t[2], (unsigned long long)o->key); }
        }
        if (!R->cut && vi_nkeys > 0) keys_prepare(A, B);
    }
    if (B->nr && !R->usable) g_vi_stat[VIS_FALLBACK]++;
    g_vi_stat[VIS_MATCH_NS] += plat_time_ns() - t0;
    vf_prev = B;
    return R;
}

/* ---- key times: the frame's commands again, with inputs between the two frames --------------- */
typedef struct {
    VFrame *A, *B; int *runm; int nkeys; atomic_int left; vi_key_fn done; void *user; u64 gen;
    const u32 *cama[16];            /* per distinct camera of B: what it was in A, or 0 */
} VJob;
typedef struct {
    pthread_t th; VUW *vu; u8 *vif; u32 *sub; size_t sub_n, sub_cap; const u32 **tab; int tab_cap;
    VVtx *out; int nout, cout; u64 hash; int run_nv0; u8 *valid; int vcap;
    u32 prim, rgba, uu, vv, fog; float q, s, t;        /* the GS vertex registers as this pass leaves them */
} VWorker;
static VWorker vw[8]; static int vw_n;
static struct { VJob *job; int k; } vq_items[64]; static int vq_head, vq_tail;
static pthread_mutex_t vq_mx = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t vq_cv = PTHREAD_COND_INITIALIZER;
static VJob *job_pending;

static void *sub_alloc(VWorker *w, u32 words) {
    if (w->sub_n + words > w->sub_cap) return 0;
    u32 *p = w->sub + w->sub_n; w->sub_n += words; return p;
}
/* Which camera of the previous frame each camera of the new frame was: from the matched runs, else the unused one most alike. */
static void cams_pair(VJob *J) {
    VFrame *A = J->A, *B = J->B; int taken[16] = { 0 };
    for (int j = 0; j < 16; j++) J->cama[j] = 0;
    for (int x = 0; x < B->nr; x++) {
        const VRun *b = &B->r[x]; if (b->camid < 0 || J->runm[x] < 0 || J->cama[b->camid]) continue;
        const VRun *a = &A->r[J->runm[x]]; if (a->camid < 0) continue;
        J->cama[b->camid] = A->cam[a->camid].d; taken[a->camid] = 1;
    }
    for (int j = 0; j < B->ncam; j++) {
        if (J->cama[j]) continue;
        const u32 *d = B->cam[j].d; int best = -1; double bs = 1e30;
        for (int k = 0; k < A->ncam; k++) {
            if (taken[k]) continue;
            const u32 *e = A->cam[k].d;
            double dot = (double)wf(d[3]) * wf(e[3]) + (double)wf(d[7]) * wf(e[7]) + (double)wf(d[11]) * wf(e[11]), n0 = 0, n1 = 0;
            for (int i = 0; i < 3; i++) { n0 += (double)wf(d[16 + 4 * i]) * wf(d[16 + 4 * i]); n1 += (double)wf(e[16 + 4 * i]) * wf(e[16 + 4 * i]); }
            double sc = fabs(sqrt(n0 / (n1 > 0 ? n1 : 1)) - 1.0), sco = (1.0 - dot) + sc;
            if (dot > 0.94 && sc < 0.2 && sco < bs) { bs = sco; best = k; }
        }
        if (best >= 0) { J->cama[j] = A->cam[best].d; taken[best] = 1; }
    }
}
/* Replacement data for the commands of frame B at time t (0 = as in A, 1 = as in B): tab[i] = [code, data...] or 0. */
static int build_subst(VWorker *w, const VJob *J, double t) {
    const VFrame *A = J->A, *B = J->B;
    if (w->tab_cap < B->nc) { w->tab_cap = B->nc + 4096; w->tab = realloc(w->tab, sizeof(u32 *) * (size_t)w->tab_cap); }
    if (!w->sub) { w->sub_cap = 1u << 21; w->sub = malloc(w->sub_cap * 4); }
    memset(w->tab, 0, sizeof(u32 *) * (size_t)B->nc); w->sub_n = 0;
    for (int x = 0; x < B->nr; x++) {
        const VRun *rb = &B->r[x], *ra = J->runm[x] >= 0 ? &A->r[J->runm[x]] : 0;
        if (ra && ra->c1 - ra->c0 != rb->c1 - rb->c0) ra = 0;
        for (int k = 0; k < rb->c1 - rb->c0; k++) {
            const VCmd *cb = &B->c[rb->c0 + k], *ca = ra ? &A->c[ra->c0 + k] : 0;
            u32 cmd = (cb->code >> 24) & 0x7F, fmt = cmd & 15;
            if (cmd < 0x60 || !cb->words || cb->off == ~0u) continue;
            if (ca && (ca->code != cb->code || ca->words != cb->words || ca->off == ~0u)) ca = 0;
            const u32 *db = cmd_data(B, cb), *da = ca ? cmd_data(A, ca) : 0; u32 *out = 0;
            int cam0 = -1, obj = 0;
            #define NEED_OUT() do { if (!out) { u32 *p_ = sub_alloc(w, cb->words + 1); if (!p_) return 0; p_[0] = cb->code; out = p_ + 1; memcpy(out, db, (size_t)cb->words * 4); } } while (0)
            if (rb->camc == rb->c0 + k && rb->camid >= 0 && J->cama[rb->camid]) {     /* the camera, wherever it is sent */
                NEED_OUT(); cam0 = rb->camrow * 4;
                cam_interp(J->cama[rb->camid], db + cam0, t, out + cam0);
            }
            if (!da || cb->words > 4096 || !memcmp(da, db, (size_t)cb->words * 4)) { w->tab[rb->c0 + k] = out ? out - 1 : 0; continue; }
            if (rb->entry == 0x40 && is_objblock(B, cb) && is_objblock(A, ca)) { NEED_OUT(); obj_interp(da + 4, db + 4, t, out + 4); obj = 1; }
            if (fmt == 0xC || fmt == 0x8 || fmt == 0x4) {                       /* 32-bit vectors: floats that differ move linearly */
                u32 rows = cb->words / 4, skip_to = 0;
                for (u32 q = 0; q < cb->words; q++) {
                    if (fmt == 0xC && (q & 3) == 0 && q / 4 >= skip_to && db[q + 1] == 0x10000000u && db[q + 2] == 0x0000000Eu && db[q + 3] == 0 && q / 4 < rows) skip_to = q / 4 + 1 + (db[q] & 0x7FFF);   /* a GS packet of register writes */
                    if (da[q] == db[q] || (fmt == 0xC && q / 4 < skip_to)) continue;
                    if (obj && q >= 4 && q < 64) continue;
                    if (cam0 >= 0 && q >= (u32)cam0 && q < (u32)cam0 + 32) continue;
                    if (!plausible(da[q]) || !plausible(db[q])) continue;
                    NEED_OUT();
                    out[q] = fw(wf(da[q]) + ((double)wf(db[q]) - wf(da[q])) * t);
                }
            }
            w->tab[rb->c0 + k] = out ? out - 1 : 0;
            #undef NEED_OUT
        }
    }
    return 1;
}
/* XGKICK of a pass: the vertices of the packet, as gif.c and gs.c would take them. */
void vi_w_kick(void *user, const u8 *mem, u32 addr, u32 mask) {
    VWorker *w = user; u32 nloop = 0, nreg = 0, flg = 0, reg_idx = 0, eop = 0, stq = 0x3F800000u; u64 regs = 0;
    for (u32 n = 0; n <= mask / 16 + 1; n++, addr += 16) {
        u64 lo, hi; const u8 *qw = mem + (addr & mask & ~15u);
        memcpy(&lo, qw, 8); memcpy(&hi, qw + 8, 8);
        if (nloop == 0) {
            nloop = lo & 0x7FFF; eop = (lo >> 15) & 1; flg = (lo >> 58) & 3; nreg = (lo >> 60) & 15; if (!nreg) nreg = 16;
            regs = hi; reg_idx = 0; stq = 0x3F800000u;
            if (flg == 0 && ((lo >> 46) & 1)) { w->prim = (u32)(lo >> 47) & 0x7FF; w->hash = (w->hash ^ 0x55u) * 1099511628211ull; }
            if (nloop == 0 && eop) return;
            continue;
        }
        for (int h = 0; h < (flg == 1 ? 2 : 1) && nloop; h++) {
            u32 desc = flg == 2 ? 0xF : (u32)(regs >> (reg_idx * 4)) & 15; u64 d = lo; int kick = -1; u32 x = 0, y = 0, z = 0, f = w->fog;
            if (flg == 0) {
                switch (desc) {
                case 0x0: desc = 0x100; break;
                case 0x1: w->rgba = (u32)((lo & 0xFF) | ((lo >> 24) & 0xFF00) | ((hi & 0xFF) << 16) | (((hi >> 32) & 0xFF) << 24)); memcpy(&w->q, &stq, 4); desc = 0xF; break;
                case 0x2: stq = (u32)hi; { u32 a = (u32)lo, b = (u32)(lo >> 32); memcpy(&w->s, &a, 4); memcpy(&w->t, &b, 4); } desc = 0xF; break;
                case 0x3: w->uu = (u32)lo & 0x3FFF; w->vv = (u32)(lo >> 32) & 0x3FFF; desc = 0xF; break;
                case 0x4: kick = !((hi >> 47) & 1); x = (u32)lo & 0xFFFF; y = (u32)(lo >> 32) & 0xFFFF; z = (u32)(hi >> 4) & 0xFFFFFF; f = (u32)(hi >> 36) & 0xFF; desc = 0xF; break;
                case 0x5: kick = !((hi >> 47) & 1); x = (u32)lo & 0xFFFF; y = (u32)(lo >> 32) & 0xFFFF; z = (u32)hi; desc = 0xF; break;
                case 0xA: w->fog = (u32)(hi >> 36) & 0xFF; desc = 0xF; break;
                case 0xE: desc = 0x100 + (u32)(hi & 0xFF); break;
                case 0xF: break;
                default: desc += 0x100; break;
                }
            } else if (flg == 1) { d = h ? hi : lo; if (desc == 2) stq = 0; desc = desc < 0xE ? desc + 0x100 : 0xF; }
            if (desc >= 0x100) switch (desc - 0x100) {          /* a register write */
                case 0x00: w->prim = (u32)d & 0x7FF; w->hash = (w->hash ^ 0x55u) * 1099511628211ull; break;
                case 0x01: w->rgba = (u32)d; { u32 q = (u32)(d >> 32); memcpy(&w->q, &q, 4); } break;
                case 0x02: { u32 a = (u32)d, b = (u32)(d >> 32); memcpy(&w->s, &a, 4); memcpy(&w->t, &b, 4); break; }
                case 0x03: w->uu = (u32)d & 0x3FFF; w->vv = (u32)(d >> 16) & 0x3FFF; break;
                case 0x04: kick = 1; x = (u32)d & 0xFFFF; y = (u32)(d >> 16) & 0xFFFF; z = (u32)(d >> 32) & 0xFFFFFF; f = (u32)(d >> 56); break;
                case 0x05: kick = 1; x = (u32)d & 0xFFFF; y = (u32)(d >> 16) & 0xFFFF; z = (u32)(d >> 32); break;
                case 0x0A: w->fog = (u32)(d >> 56); break;
                case 0x0C: kick = 0; x = (u32)d & 0xFFFF; y = (u32)(d >> 16) & 0xFFFF; z = (u32)(d >> 32) & 0xFFFFFF; f = (u32)(d >> 56); break;
                case 0x0D: kick = 0; x = (u32)d & 0xFFFF; y = (u32)(d >> 16) & 0xFFFF; z = (u32)(d >> 32); break;
                default: break;
            }
            if (kick >= 0) {
                if (w->nout == w->cout) { w->cout = w->cout ? w->cout * 2 : 65536; w->out = realloc(w->out, sizeof(VVtx) * (size_t)w->cout); }
                VVtx *o = &w->out[w->nout++]; int kind = (int)(w->prim & 7) | (kick << 3);
                o->x = (s32)x; o->y = (s32)y; o->z = z; o->rgba = w->rgba; o->q = w->q; o->s = w->s; o->t = w->t; o->u = (u16)w->uu; o->v = (u16)w->vv; o->f = (u8)f; o->kind = (u8)kind; o->pad[0] = o->pad[1] = 0;
                w->hash = (w->hash ^ (u64)((kind & 7) + 1)) * 1099511628211ull;
            }
            if (flg == 2) { nloop--; break; }
            if (++reg_idx == nreg) { reg_idx = 0; nloop--; }
        }
        if (nloop == 0 && eop) return;
    }
}
static void run_pass(VWorker *w, VJob *J, int k) {
    VFrame *B = J->B; float t = (float)(k + 1) / (float)(J->nkeys + 1); u64 t0 = plat_time_ns();
    int ok = J->gen == vu1_micro_gen() && build_subst(w, J, t);
    if (w->vcap < B->nr + 1) { w->vcap = B->nr + 8192; w->valid = realloc(w->valid, (size_t)w->vcap); }
    memset(w->valid, 0, (size_t)B->nr + 1);
    if (ok) {
        memcpy(w->vif, B->vif, vif_state_size()); vu_w_load(w->vu, B->vu);
        w->nout = 0; w->prim = 0; w->rgba = 0; w->uu = w->vv = w->fog = 0; w->q = 1.0f; w->s = w->t = 0.0f;
        /* Every run sends its vertices to the place the real run's vertices have in the frame's stream, so that
         * an index means the same vertex in both; a run that sent another number of them is not used. */
        for (int x = 0; x < B->nr && ok; x++) {
            const VRun *r = &B->r[x];
            for (int i = r->c0; i < r->c1 - 1; i++) {
                const VCmd *c = &B->c[i]; if (c->off == ~0u) { ok = 0; break; }
                vif_w_command(w->vif, w->vu, (const u8 *)(w->tab[i] ? w->tab[i] : B->arena + c->off));
            }
            if (!ok) break;
            if (w->cout < r->v0 + r->nv + 4096) { w->cout = r->v0 + r->nv + 65536; w->out = realloc(w->out, sizeof(VVtx) * (size_t)w->cout); }
            w->nout = r->v0; w->hash = 1469598103934665603ull;
            { const VCmd *c = &B->c[r->c1 - 1]; if (c->off == ~0u) { ok = 0; break; } vif_w_command(w->vif, w->vu, (const u8 *)(B->arena + c->off)); }
            w->valid[x] = w->nout - r->v0 == r->nv && w->hash == r->vhash;
        }
        if (J->gen != vu1_micro_gen()) ok = 0;
    }
    if (!ok) memset(w->valid, 0, (size_t)B->nr + 1);
    int nok = 0; for (int x = 0; x < B->nr; x++) nok += w->valid[x];
    __atomic_fetch_add(&g_vi_stat[VIS_PASSES], 1, __ATOMIC_RELAXED); __atomic_fetch_add(&g_vi_stat[VIS_PASS_NS], plat_time_ns() - t0, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_vi_stat[VIS_KEYRUNS], (u64)B->nr, __ATOMIC_RELAXED); __atomic_fetch_add(&g_vi_stat[VIS_KEYRUNS_OK], (u64)nok, __ATOMIC_RELAXED);
    J->done(J->user, B->serial, k, t, w->out, w->valid);
}
static void *worker_main(void *arg) {
    VWorker *w = arg;
    _mm_setcsr(_mm_getcsr() | 0x8040);
    plat_thread_role(2 + (int)(w - vw));
    for (;;) {
        pthread_mutex_lock(&vq_mx);
        while (vq_head == vq_tail) pthread_cond_wait(&vq_cv, &vq_mx);
        VJob *J = vq_items[vq_head & 63].job; int k = vq_items[vq_head & 63].k; vq_head++;
        pthread_mutex_unlock(&vq_mx);
        run_pass(w, J, k);
        if (atomic_fetch_sub(&J->left, 1) == 1) {
            J->done(J->user, J->B->serial, -1, 1.0f, 0, 0);
            atomic_fetch_sub(&J->A->hold, 1); atomic_fetch_sub(&J->B->hold, 1);
            free(J->runm); free(J);
        }
    }
    return 0;
}
static void keys_prepare(VFrame *A, VFrame *B) {
    if (job_pending) { free(job_pending->runm); free(job_pending); job_pending = 0; }
    if (!A->full || !B->full || !A->have_snap || !B->have_snap || ((A->bad | B->bad) & ~1)) return;
    VJob *J = calloc(1, sizeof *J);
    J->A = A; J->B = B; J->nkeys = vi_nkeys; J->gen = vu1_micro_gen();
    J->runm = malloc(sizeof(int) * (size_t)(B->nr + 1)); memcpy(J->runm, vi_runm, sizeof(int) * (size_t)B->nr);
    cams_pair(J);
    job_pending = J;
}
int vi_keys_submit(vi_key_fn done, void *user) {
    VJob *J = job_pending; job_pending = 0;
    if (!J) return 0;
    if (!vw_n) {
        int n = vi_threads_want > 0 ? vi_threads_want : vi_nkeys; if (n > 8) n = 8; if (n < 1) n = 1;
        for (int i = 0; i < n; i++) {
            vw[i].vu = vu_w_new(&vw[i]); vw[i].vif = malloc(vif_state_size());
            if (pthread_create(&vw[i].th, 0, worker_main, &vw[i]) != 0) break;
            vw_n++;
        }
        if (!vw_n) { vi_nkeys = 0; free(J->runm); free(J); return 0; }
    }
    pthread_mutex_lock(&vq_mx);
    if (vq_tail - vq_head + J->nkeys > 32) {          /* the workers are not keeping up: this frame goes without key times */
        pthread_mutex_unlock(&vq_mx);
        g_vi_stat[VIS_PASS_DROPPED] += (u64)J->nkeys; free(J->runm); free(J); return 0;
    }
    J->done = done; J->user = user; atomic_store(&J->left, J->nkeys);
    atomic_fetch_add(&J->A->hold, 1); atomic_fetch_add(&J->B->hold, 1);
    /* the middle first: if only some passes are done in time, they are the most useful ones */
    for (int i = 0; i < J->nkeys; i++) { int k = (J->nkeys / 2 + (i & 1 ? (i + 1) / 2 : -(i / 2)) + J->nkeys) % J->nkeys; vq_items[vq_tail & 63].job = J; vq_items[vq_tail & 63].k = k; vq_tail++; }
    pthread_cond_broadcast(&vq_cv);
    pthread_mutex_unlock(&vq_mx);
    return J->nkeys;
}
