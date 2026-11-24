/* vinterp.h — frame interpolation from the inputs of the game's VU1 program (see vinterp.c). */
#ifndef ROR_VINTERP_H
#define ROR_VINTERP_H
#include "rt.h"
/* The hooks are weak references everywhere but in vinterp.c itself: the tools that include vif.c, vu.c or gs.c without
 * linking vinterp.c (vubench, gsreplay) never set the flags that lead to a call. */
#ifdef VI_IMPL
#define VI_WEAK
#else
#define VI_WEAK __attribute__((weak))
#endif

/* One vertex as VU1 sent it to the GS (XYZ2 / XYZF2 / XYZ3 / XYZF3 with the vertex registers of that moment). */
typedef struct { s32 x, y; u32 z; u32 rgba; float q, s, t; u16 u, v; u8 f, kind, pad[2]; } VVtx;   /* x, y: 1/16 pixels (the game thread's streams: XYOFFSET taken off; a pass's: as sent); kind: primitive type | drawing kick << 3 */

/* One recorded VIF1 command (vif.c fills this in for every complete command while g_vi_rec is set). */
typedef struct { const u8 *w; u32 code, words, dest, tops, mask; u8 cl, wl, mode, rel; } VRec;

extern int g_vi_rec;          /* vif.c: VIF1 commands are being recorded */
extern int g_vi_run;          /* gs.c: >= 0 while a recorded VU1 program runs (its number in the frame), else -1 */

void vi_set_mode(int mode);   /* --interp: 0 pairs, 1 auto, 2 inputs (before the first frame) */
int  vi_get_mode(void);
void vi_start(void) VI_WEAK;          /* the GPU renderer retains draw lists from now on: start recording (unless the mode is pairs) */

/* vif.c -> vinterp.c (only called while g_vi_rec is set) */
void vi_rec(const VRec *r) VI_WEAK;
void vi_run_begin(u32 entry) VI_WEAK;       /* a micro program is about to start; entry ~0 = continue */
void vi_run_end(void) VI_WEAK;
void vi_note(int what) VI_WEAK;             /* something the recording does not cover happened: 1 VIF1 reset, 2 micro program upload, 3 EE write into VU1 memory */

/* gs.c -> vinterp.c */
int  vi_vtx(s32 x, s32 y, u32 z, u32 fog, u32 rgba, float q, float s, float t, u32 u, u32 v, int kind) VI_WEAK;   /* returns the vertex's index in the frame's stream */
void vi_prim_mark(void) VI_WEAK;            /* PRIM written by path 1 */

/* Result of a frame boundary. Run numbers and vertex indices are those handed out while the frame was recorded. */
typedef struct {
    int usable;               /* 0: nothing can be taken from the inputs for this frame (reason in why) */
    int why;                  /* 1 no previous frame, 2 other micro program, 3 recording incomplete, 4 too few programs matched, 5 camera cut */
    int cut;                  /* the camera jumped: show real frames only */
    int nrun, nvtx, pnvtx;
    const int *delta;         /* per run of the new frame: previous index = index + delta, or VI_NONE */
    const int *v0;            /* per run: its first vertex */
    const int *prun; int pnrun;   /* per run: the previous frame's run it was paired with, or -1; runs in the previous frame */
    const VVtx *cur, *prev;   /* the vertex streams of the new and the previous frame */
    u64 serial;               /* number of this frame (for results delivered later) */
    float cam_turn, cam_move;
} VResult;
#define VI_NONE (-0x40000000)
const VResult *vi_boundary(void) VI_WEAK;

/* Key times between the two frames: vertex streams from re-running the VU1 program with interpolated inputs.
 * vi_keys_wanted: how many (0 = none). vi_keys_submit: start the passes for the frame of the last vi_boundary;
 * done(user, serial, k, t, stream, valid) is called on a worker thread for each key when its stream is there
 * (valid[run] = the run sent the same vertices structure as in the real frame), then once with stream = 0. */
typedef void (*vi_key_fn)(void *user, u64 serial, int k, float t, const VVtx *stream, const u8 *valid);
int  vi_keys_wanted(void) VI_WEAK;
int  vi_keys_submit(vi_key_fn done, void *user) VI_WEAK;
void vi_keys_release(u64 serial) VI_WEAK;   /* the caller is done with the streams of that frame */

/* statistics for ROR_PERFLOG (video.c reads and clears) */
enum { VIS_FRAMES, VIS_USED, VIS_CUT, VIS_FALLBACK, VIS_RUNS, VIS_RUNS_OK, VIS_RUNS_NOPARTNER, VIS_RUNS_STRUCT, VIS_RUNS_UV, VIS_RUNS_JUMP,
       VIS_PRIMS, VIS_PRIMS_OK, VIS_PRIMS_NEW, VIS_PRIMS_CLIP, VIS_PASSES, VIS_PASS_NS, VIS_PASS_DROPPED, VIS_KEYRUNS, VIS_KEYRUNS_OK, VIS_MATCH_NS, VIS_UNKNOWN, VIS_N };
extern u64 g_vi_stat[VIS_N] VI_WEAK;

/* vif.c / vu.c services for the worker passes */
size_t vif_state_size(void);
void   vif1_state_save(void *dst);
typedef struct VUW VUW;
void   vif_w_command(void *state, VUW *w, const u8 *cmd);        /* one recorded command on a private VIF state and VU1 instance */
VUW   *vu_w_new(void *user);
size_t vu1_snap_size(void);
void   vu1_snap_save(void *dst);                                 /* game thread: registers and data memory of VU1 */
void   vu_w_load(VUW *w, const void *snap);
VU    *vu_w_vu(VUW *w);
void   vu_w_exec(VUW *w, u32 addr);                              /* ~0 = continue */
int    vu1_program_known(void);                                  /* micro memory holds the program the build translated */
u64    vu1_micro_gen(void);                                      /* changes whenever micro memory may have changed */
void   vi_w_kick(void *user, const u8 *mem, u32 addr, u32 mask) VI_WEAK; /* XGKICK of a worker instance */
#endif
