/* gs.c — Graphics Synthesizer, software implementation.
 *
 * Models the GS as the game sees it: 4 MiB of local memory in the real
 * block/column layout (so textures, frame buffers and CLUTs written in one
 * format read back correctly in another), the general registers written
 * through GIF, host->local and local->local image transfers, and a
 * rasterizer for points, lines, triangles and sprites with texturing,
 * fog, alpha/destination-alpha/depth tests and alpha blending.
 *
 * Rendering is deferred and band-parallel (see "render queue" below):
 * primitives are queued together with an immutable snapshot of their draw
 * state, and worker threads each shade one horizontal band of the render
 * target for all queued primitives, in submission order, while the caller
 * carries on. Everything that looks at local memory from outside the
 * renderer first waits for the queue (gs_sync). ROR_GS_THREADS=n sets the
 * number of workers; 0 renders every primitive at once on the caller. */
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <sched.h>
#include <emmintrin.h>
#include "gs.h"

#define ALWAYS_INLINE static inline __attribute__((always_inline))

#define VRAM_WORDS (1u << 20)
static u32 vram[VRAM_WORDS];
#define VRAM16 ((u16 *)vram)
#define VRAM8  ((u8 *)vram)

u32 g_gs_imr = 0xFF00;
u64 g_gs_prims, g_gs_frames;
u64 g_gs_tex_rebuilt;           /* texels decoded into the texture cache */
u64 g_gs_stat[6];              /* bounding-box pixels and primitive counts: small / medium / large */

enum { PSMCT32 = 0x00, PSMCT24 = 0x01, PSMCT16 = 0x02, PSMCT16S = 0x0A, PSMT8 = 0x13,
       PSMT4 = 0x14, PSMT8H = 0x1B, PSMT4HL = 0x24, PSMT4HH = 0x2C,
       PSMZ32 = 0x30, PSMZ24 = 0x31, PSMZ16 = 0x32, PSMZ16S = 0x3A };

/* ---- local memory layout -------------------------------------------------- */
static const u8 blk32[4][8] = {
    { 0, 1, 4, 5, 16, 17, 20, 21 }, { 2, 3, 6, 7, 18, 19, 22, 23 },
    { 8, 9, 12, 13, 24, 25, 28, 29 }, { 10, 11, 14, 15, 26, 27, 30, 31 } };
static const u8 blk32z[4][8] = {
    { 24, 25, 28, 29, 8, 9, 12, 13 }, { 26, 27, 30, 31, 10, 11, 14, 15 },
    { 16, 17, 20, 21, 0, 1, 4, 5 }, { 18, 19, 22, 23, 2, 3, 6, 7 } };
static const u8 blk16[8][4] = {
    { 0, 2, 8, 10 }, { 1, 3, 9, 11 }, { 4, 6, 12, 14 }, { 5, 7, 13, 15 },
    { 16, 18, 24, 26 }, { 17, 19, 25, 27 }, { 20, 22, 28, 30 }, { 21, 23, 29, 31 } };
static const u8 blk16s[8][4] = {
    { 0, 2, 16, 18 }, { 1, 3, 17, 19 }, { 8, 10, 24, 26 }, { 9, 11, 25, 27 },
    { 4, 6, 20, 22 }, { 5, 7, 21, 23 }, { 12, 14, 28, 30 }, { 13, 15, 29, 31 } };
static const u8 blk16z[8][4] = {
    { 24, 26, 16, 18 }, { 25, 27, 17, 19 }, { 28, 30, 20, 22 }, { 29, 31, 21, 23 },
    { 8, 10, 0, 2 }, { 9, 11, 1, 3 }, { 12, 14, 4, 6 }, { 13, 15, 5, 7 } };
static const u8 blk16sz[8][4] = {
    { 24, 26, 8, 10 }, { 25, 27, 9, 11 }, { 16, 18, 0, 2 }, { 17, 19, 1, 3 },
    { 28, 30, 12, 14 }, { 29, 31, 13, 15 }, { 20, 22, 4, 6 }, { 21, 23, 5, 7 } };
#define blk8 blk32
#define blk4 blk16
/* One "column" (64 bytes) of each format; the block tables are built from
 * these: columns stack down the block, and for 8- and 4-bit formats odd
 * columns have their two halves swapped. */
static const u8 colp32[2][8] = { { 0, 1, 4, 5, 8, 9, 12, 13 }, { 2, 3, 6, 7, 10, 11, 14, 15 } };
static const u8 colp16[2][16] = {
    { 0, 2, 8, 10, 16, 18, 24, 26, 1, 3, 9, 11, 17, 19, 25, 27 },
    { 4, 6, 12, 14, 20, 22, 28, 30, 5, 7, 13, 15, 21, 23, 29, 31 } };
static const u8 colp8[4][16] = {
    { 0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54 },
    { 8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62 },
    { 33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23 },
    { 41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31 } };
static const u8 colp4[4][32] = {
    { 0, 8, 32, 40, 64, 72, 96, 104, 2, 10, 34, 42, 66, 74, 98, 106,
      4, 12, 36, 44, 68, 76, 100, 108, 6, 14, 38, 46, 70, 78, 102, 110 },
    { 16, 24, 48, 56, 80, 88, 112, 120, 18, 26, 50, 58, 82, 90, 114, 122,
      20, 28, 52, 60, 84, 92, 116, 124, 22, 30, 54, 62, 86, 94, 118, 126 },
    { 65, 73, 97, 105, 1, 9, 33, 41, 67, 75, 99, 107, 3, 11, 35, 43,
      69, 77, 101, 109, 5, 13, 37, 45, 71, 79, 103, 111, 7, 15, 39, 47 },
    { 81, 89, 113, 121, 17, 25, 49, 57, 83, 91, 115, 123, 19, 27, 51, 59,
      85, 93, 117, 125, 21, 29, 53, 61, 87, 95, 119, 127, 23, 31, 55, 63 } };
static u8 col32[8][8], col16[8][16], col8[16][16];
static u16 col4[16][32];
/* Swizzle tables: offset of pixel (x, y) inside its page, for every layout.
 * A pixel's address is  bp * (units per block) + page_offset * (units per
 * page) + table[y][x]  with the page offset computed from the buffer width. */
static u16 swz32[32][64], swz32z[32][64];                               /* words (2048 per page) */
static u16 swz16[64][64], swz16s[64][64], swz16z[64][64], swz16sz[64][64];   /* u16s (4096 per page) */
static u16 swz8[64][128];                                               /* bytes (8192 per page) */
static u16 swz4[128][128];                                              /* nibbles (16384 per page) */

static void build_tables(void) {
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) col32[y][x] = (u8)((y >> 1) * 16 + colp32[y & 1][x]);
    for (int y = 0; y < 8; y++) for (int x = 0; x < 16; x++) col16[y][x] = (u8)((y >> 1) * 32 + colp16[y & 1][x]);
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
        int c = y >> 2;
        col8[y][x] = (u8)(c * 64 + (colp8[y & 3][x] ^ ((c & 1) ? 32 : 0)));
    }
    for (int y = 0; y < 16; y++) for (int x = 0; x < 32; x++) {
        int c = y >> 2;
        col4[y][x] = (u16)(c * 128 + (colp4[y & 3][x] ^ ((c & 1) ? 64 : 0)));
    }
    for (int y = 0; y < 32; y++) for (int x = 0; x < 64; x++) {
        swz32[y][x] = (u16)(blk32[(y >> 3) & 3][(x >> 3) & 7] * 64 + col32[y & 7][x & 7]);
        swz32z[y][x] = (u16)(blk32z[(y >> 3) & 3][(x >> 3) & 7] * 64 + col32[y & 7][x & 7]);
    }
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
        swz16[y][x] = (u16)(blk16[(y >> 3) & 7][(x >> 4) & 3] * 128 + col16[y & 7][x & 15]);
        swz16s[y][x] = (u16)(blk16s[(y >> 3) & 7][(x >> 4) & 3] * 128 + col16[y & 7][x & 15]);
        swz16z[y][x] = (u16)(blk16z[(y >> 3) & 7][(x >> 4) & 3] * 128 + col16[y & 7][x & 15]);
        swz16sz[y][x] = (u16)(blk16sz[(y >> 3) & 7][(x >> 4) & 3] * 128 + col16[y & 7][x & 15]);
    }
    for (int y = 0; y < 64; y++) for (int x = 0; x < 128; x++)
        swz8[y][x] = (u16)(blk8[(y >> 4) & 3][(x >> 4) & 7] * 256 + col8[y & 15][x & 15]);
    for (int y = 0; y < 128; y++) for (int x = 0; x < 128; x++)
        swz4[y][x] = (u16)(blk4[(y >> 4) & 7][(x >> 5) & 3] * 512 + col4[y & 15][x & 31]);
}

ALWAYS_INLINE u32 a32(const u16 (*t)[64], u32 bp, u32 bw, u32 x, u32 y) {
    return (bp * 64 + ((y >> 5) * bw + (x >> 6)) * 2048 + t[y & 31][x & 63]) & (VRAM_WORDS - 1);
}
ALWAYS_INLINE u32 a16(const u16 (*t)[64], u32 bp, u32 bw, u32 x, u32 y) {
    return (bp * 128 + ((y >> 6) * bw + (x >> 6)) * 4096 + t[y & 63][x & 63]) & (VRAM_WORDS * 2 - 1);
}
ALWAYS_INLINE u32 a8(u32 bp, u32 bw, u32 x, u32 y) {
    return (bp * 256 + ((y >> 6) * (bw >> 1) + (x >> 7)) * 8192 + swz8[y & 63][x & 127]) & (VRAM_WORDS * 4 - 1);
}
ALWAYS_INLINE u32 a4(u32 bp, u32 bw, u32 x, u32 y) {     /* nibble index */
    return (bp * 512 + ((y >> 7) * (bw >> 1) + (x >> 7)) * 16384 + swz4[y & 127][x & 127]) & (VRAM_WORDS * 8 - 1);
}

/* Raw pixel access in any format. */
static u32 rd_px(u32 psm, u32 bp, u32 bw, u32 x, u32 y) {
    switch (psm) {
    case PSMCT32: return vram[a32(swz32, bp, bw, x, y)];
    case PSMCT24: return vram[a32(swz32, bp, bw, x, y)] & 0xFFFFFF;
    case PSMZ32:  return vram[a32(swz32z, bp, bw, x, y)];
    case PSMZ24:  return vram[a32(swz32z, bp, bw, x, y)] & 0xFFFFFF;
    case PSMCT16: return VRAM16[a16(swz16, bp, bw, x, y)];
    case PSMCT16S: return VRAM16[a16(swz16s, bp, bw, x, y)];
    case PSMZ16:  return VRAM16[a16(swz16z, bp, bw, x, y)];
    case PSMZ16S: return VRAM16[a16(swz16sz, bp, bw, x, y)];
    case PSMT8:   return VRAM8[a8(bp, bw, x, y)];
    case PSMT4: { u32 n = a4(bp, bw, x, y); return (VRAM8[n >> 1] >> ((n & 1) * 4)) & 15; }
    case PSMT8H:  return vram[a32(swz32, bp, bw, x, y)] >> 24;
    case PSMT4HL: return (vram[a32(swz32, bp, bw, x, y)] >> 24) & 15;
    case PSMT4HH: return vram[a32(swz32, bp, bw, x, y)] >> 28;
    }
    return 0;
}
/* Write without any bookkeeping (the renderer accounts for the pages a
 * primitive can touch when it is queued). */
static void wr_raw(u32 psm, u32 bp, u32 bw, u32 x, u32 y, u32 v) {
    u32 *p;
    switch (psm) {
    case PSMCT32: vram[a32(swz32, bp, bw, x, y)] = v; break;
    case PSMCT24: p = &vram[a32(swz32, bp, bw, x, y)]; *p = (*p & 0xFF000000u) | (v & 0xFFFFFF); break;
    case PSMZ32:  vram[a32(swz32z, bp, bw, x, y)] = v; break;
    case PSMZ24:  p = &vram[a32(swz32z, bp, bw, x, y)]; *p = (*p & 0xFF000000u) | (v & 0xFFFFFF); break;
    case PSMCT16: VRAM16[a16(swz16, bp, bw, x, y)] = (u16)v; break;
    case PSMCT16S: VRAM16[a16(swz16s, bp, bw, x, y)] = (u16)v; break;
    case PSMZ16:  VRAM16[a16(swz16z, bp, bw, x, y)] = (u16)v; break;
    case PSMZ16S: VRAM16[a16(swz16sz, bp, bw, x, y)] = (u16)v; break;
    case PSMT8:   VRAM8[a8(bp, bw, x, y)] = (u8)v; break;
    case PSMT4: { u32 n = a4(bp, bw, x, y); u8 *b = &VRAM8[n >> 1]; int s = (n & 1) * 4;
                  *b = (u8)((*b & ~(15 << s)) | ((v & 15) << s)); break; }
    case PSMT8H:  p = &vram[a32(swz32, bp, bw, x, y)]; *p = (*p & 0x00FFFFFFu) | (v << 24); break;
    case PSMT4HL: p = &vram[a32(swz32, bp, bw, x, y)]; *p = (*p & 0xF0FFFFFFu) | ((v & 15) << 24); break;
    case PSMT4HH: p = &vram[a32(swz32, bp, bw, x, y)]; *p = (*p & 0x0FFFFFFFu) | ((v & 15) << 28); break;
    }
}

/* ---- page generations ---------------------------------------------------------
 * Local memory is 512 pages of 8 KiB. page_gen[p] is bumped (by the calling
 * thread only) whenever page p may have been modified: by an image transfer,
 * or by a primitive whose bounding box touches it, at the time the primitive
 * is queued. The texture cache uses the generations as a cheap "nothing can
 * have changed" test and a hash of the page contents as the precise one. */
static u64 page_gen[512], page_hgen[512], page_hash[512];
/* Page that pixel (x, y) of a buffer falls into, relative to bp >> 5. */
ALWAYS_INLINE u32 page_off(u32 psm, u32 bw, u32 x, u32 y) {
    switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return (y >> 6) * bw + (x >> 6);
    case PSMT8: return (y >> 6) * (bw >> 1) + (x >> 7);
    case PSMT4: return (y >> 7) * (bw >> 1) + (x >> 7);
    default: return (y >> 5) * bw + (x >> 6);
    }
}
/* Tracked write, for the calling thread only (transfers, serial primitives). */
static void wr_px(u32 psm, u32 bp, u32 bw, u32 x, u32 y, u32 v) {
    u32 pg = (bp >> 5) + page_off(psm, bw, x, y);
    page_gen[pg & 511]++;
    if (bp & 31) page_gen[(pg + 1) & 511]++;               /* not page-aligned: may spill into the next page */
    wr_raw(psm, bp, bw, x, y, v);
}
static inline u32 c16to32(u32 c) {        /* RGBA5551 -> RGBA8888, A = bit 15 in bit 31 */
    return ((c & 0x1F) << 3) | ((c & 0x3E0) << 6) | ((c & 0x7C00) << 9) | ((c & 0x8000) << 16);
}
static inline u32 c32to16(u32 c) {
    return ((c >> 3) & 0x1F) | ((c >> 6) & 0x3E0) | ((c >> 9) & 0x7C00) | ((c >> 16) & 0x8000);
}

/* 512-bit page sets */
typedef struct { u64 w[8]; } PageSet;
ALWAYS_INLINE void ps_set(PageSet *s, u32 p) { s->w[(p >> 6) & 7] |= 1ull << (p & 63); }
ALWAYS_INLINE int ps_test(const PageSet *s, u32 p) { return (int)((s->w[(p >> 6) & 7] >> (p & 63)) & 1); }
ALWAYS_INLINE int ps_meet(const PageSet *a, const PageSet *b) {
    u64 r = 0;
    for (int i = 0; i < 8; i++) r |= a->w[i] & b->w[i];
    return r != 0;
}
ALWAYS_INLINE void ps_clear(PageSet *a) { memset(a, 0, sizeof *a); }

/* ---- register state ------------------------------------------------------- */
typedef struct {
    u64 tex0, tex1, clamp, xyoffset, scissor, alpha, test, fba, frame, zbuf;
    s32 ofx, ofy, sx0, sx1, sy0, sy1;
    u32 fbp, fbw, fpsm, fbmsk, zbp, zpsm, zmsk;
    u32 tbp, tbw, tpsm, tw, th, tcc, tfx, cbp, cpsm, csm, csa;
    u32 wms, wmt, minu, maxu, minv, maxv, mmag;
} Ctxt;
typedef struct { s32 x, y; u32 z; u8 r, g, b, a, f; float q, s, t; u32 u, v; } Vtx;

static Ctxt ctxt[2];
static u64 r_prim, r_prmode, r_prmodecont = 1, r_texclut, r_texa, r_fogcol, r_pabe, r_colclamp = 1,
           r_dthe, r_bitblt, r_trxpos, r_trxreg, r_trxdir;
static u32 r_rgbaq; static float r_q = 1.0f, r_s, r_t; static u32 r_u, r_v, r_fog;
static Vtx vq[3]; static int vcount;
static u32 clut[256]; static int clut16;
static struct { u32 x, y, active; } tx;

/* privileged */
static u64 p_pmode, p_smode2, p_dispfb[2], p_display[2], p_bgcolor, p_siglblid;
static u32 p_csr = 0x551B0000u | 0x4000u;
static u32 frame_out[1024 * 1024];

static u64 gs_dump_frame;

/* ---- draw state snapshots and the render queue: types ---------------------------
 * (the machinery is further down, after the rasterizer) */
enum { ZK_32, ZK_24, ZK_16 };
/* What the fast pixel paths need, flattened so that a rasterizer can copy it
 * into locals: nothing in the inner loops reads state through a pointer that
 * a frame-buffer write could alias. */
typedef struct {
    u32 fpg0, fbw, fbmsk;                                 /* frame buffer (PSMCT32): first page, width in pages */
    u32 zpg0, zysh, zymask, zmax; const u16 (*ztab)[64];  /* depth buffer */
    int zte, ztst, zwr;
    int tme, mmag, tfx, tcc;
    const u32 *tex; u32 tw; s32 w, h; u32 wms, wmt; s32 minu, maxu, minv, maxv;
    int fge, fogr, fogg, fogb;
    int ate, atst, aref, afail;
    int need_fb, date; u32 datm;
    int abe, pabe, colclamp, fba; u32 aa, ab, ac, ad; int fix;
} FS;
typedef struct DState {
    Ctxt c;                  /* copy of the GS context the primitive is drawn with */
    u64 pr;                  /* effective PRIM */
    int tme, fge, abe, iip, fst;
    u32 aa, ab, ac, ad, fix;
    int ate, atst, aref, afail, date, datm, zte, ztst;
    int pabe, colclamp, fba, need_fb;
    const u32 *tex;          /* decoded texture (immutable while any queued primitive refers to it) */
    u32 ta0, ta1, aem;
    u32 zmax;
    u8 fogr, fogg, fogb;
    u8 zkind;                /* ZK_* (valid when `fast`) */
    u8 fast;                 /* the specialised CT32 paths apply */
    u8 serial;               /* must be drawn alone, in order, by the calling thread */
    u8 touch_z;              /* reads or writes the depth buffer */
    u8 never;                /* depth test NEVER: draws nothing */
    u64 tkey;                /* render target identity (frame + depth buffer layout) */
    PageSet texmask;         /* pages the texture occupies */
    FS fs;
    void (*tri_fn)(void), (*sprite_fn)(void);   /* TriFn / SpriteFn: the rasterizer instances for this state */
    int hw_kind, hw_tex;     /* hardware renderer: 0 no texture, 1 texture id, 2 render target index */
} DState;

typedef struct { u32 tbp, tbw, tpsm, tw, th, csa, ta0, ta1, aem; } TexDesc;
typedef struct { u32 cbp, cpsm, csm, csa, n4; u64 texclut; } ClutDesc;
typedef struct { ClutDesc d; u32 seq; u32 val[256]; } ClutJob;
enum { PT_POINT, PT_LINE, PT_TRI, PT_SPRITE, PT_BARRIER, PT_DECODE, PT_CLUT };
typedef struct {
    const DState *st;
    s32 px0, px1, py0, py1;          /* pixels the primitive may touch (inclusive), already scissored */
    u8 type;
    union {
        Vtx v[3];
        struct { TexDesc d; u32 *px; } dec;
        ClutJob *clut;
    };
} Prim;

static int nthreads;                 /* render workers; 0 = draw immediately on the caller */
static int gs_force;                 /* ROR_GS_FORCE (testing): 1 = generic pixel path only, 2 = serial drawing only, 3 = no specialised instances */
static PageSet pend_dirty;           /* pages queued primitives may write (cleared by gs_sync) */
static int state_dirty = 1;          /* registers or texture memory changed since cur_st was built */
static u64 sync_gen;                 /* number of completed gs_sync()s that had work to wait for */
enum { SY_QFULL, SY_STATES, SY_SERIAL, SY_TEX, SY_CLUT, SY_XFER, SY_L2L, SY_FRAME, SY_SNAP, SY_N };
static void gs_sync(int why);
static void tx_flush(void);
static int  q_pending(void);
static void q_barrier(void);
static void q_decode(const TexDesc *d, u32 *px);
static ClutJob *q_clut(const ClutDesc *d, u32 seq);
#ifdef GS_HW
/* Hardware renderer glue (see "hardware renderer" near the end of the file). */
static int hw_on, hw_scale, hw_shblend;
static int hw_interp_n, hw_fps_on, hwi_uploads, hwi_texgone; static float hwi_snap_px;
static int hw_fsr3;                       /* --filter fsr3 and the FidelityFX upscaler is usable (see "FSR 3.1") */
struct HwState_; struct HwVertex_;
static void hwi_record(const struct HwState_ *h, const struct HwVertex_ *v, int n);
static u64 hw_state_gen, hwi_state_key;   /* draw states built so far; identity of the current one for frame matching */
static void hwg_tex(DState *s);
static void hwg_prim(const DState *st, const Prim *p);
static void hwg_sync(void);
static int  hwg_clut(const ClutDesc *d, u32 seq);
static void hwg_xfer_begin(void);
static void hwg_xfer_data(void);
static void hwg_cpu_needs(const PageSet *pages);
static void hwg_cpu_wrote_all(void);
static int  hwg_frame(u32 fbp, u32 fbw, u32 dbx, u32 dby, u32 ww, u32 hh, u32 **pixels, int *w, int *h);
static void hwg_reset(void);
struct HRt_; static struct HRt_ *hwg_display_rt(u32 fbp, u32 fbw, u32 dbx, u32 dby, u32 ww, u32 hh);
static void hwg_apply_cluts(void);
struct Pkg_; static struct Pkg_ *hwp_publish(const int *partner, int ok, int pic, int sx, int sy, int sw, int sh);
u64  plat_time_ns(void);                 /* plat.h (the replay tool supplies its own) */
void plat_sleep_until(u64 t_ns);
#include "hw/gs_hw.h"
#endif

/* ---- CLUT ------------------------------------------------------------------
 * A CLUT load copies 16 or 256 pixels of local memory into the CLUT buffer
 * when TEX0/TEX2 is written. If those pixels are in pages that queued
 * primitives may still write (the game regularly "loads" a CLUT from the top
 * of the frame buffer when it resets TEX0), the read is queued as a job
 * instead of waiting for the renderer: the affected entries are then marked
 * pending and filled in by gs_sync, which anything that needs their values
 * calls first. Entries are tracked in groups of 16 (one PSMT4 palette) so a
 * later load into the same group supersedes a pending one. */
static u64 clut_sig; static int clut_sig_valid;      /* hash of the CLUT buffer contents */
static u16 clut_pending;                              /* groups a queued job will fill */
static u32 clut_seq[16], clut_load_seq;               /* which load last wrote each group */
static ClutJob *clut_jobs[64]; static int clut_njobs;

static void clut_read(const ClutDesc *d, u32 *out) {
    int count = d->n4 ? 16 : 256;
    u32 base = d->n4 ? (d->csa & 15) * 16 : 0;
    if (d->csm == 0) {                      /* CSM1: 16x16 (or 8x2) grid, bw = 1 */
        for (int i = 0; i < count; i++) {
            int j = d->n4 ? i : ((i & 0xE7) | ((i & 8) << 1) | ((i & 16) >> 1));
            u32 x = d->n4 ? (u32)(i & 7) : (u32)(j & 15), y = d->n4 ? (u32)(i >> 3) : (u32)(j >> 4);
            out[(base + (u32)i) & 255] = rd_px(d->cpsm, d->cbp, 1, x, y);
        }
    } else {                                /* CSM2: linear strip described by TEXCLUT */
        u32 cbw = d->texclut & 63, cou = ((d->texclut >> 6) & 63) * 16, cov = (d->texclut >> 12) & 1023;
        for (int i = 0; i < count; i++)
            out[(base + (u32)i) & 255] = rd_px(PSMCT16, d->cbp, cbw, cou + (u32)i, cov);
    }
}
/* Returns 1 if the CLUT buffer (or what is known of it) changed. */
static int load_clut(const Ctxt *c) {
    int n4 = (c->tpsm == PSMT4 || c->tpsm == PSMT4HL || c->tpsm == PSMT4HH);
    int n8 = (c->tpsm == PSMT8 || c->tpsm == PSMT8H);
    if (!n4 && !n8) return 0;
    ClutDesc d = { c->cbp, c->cpsm, c->csm, c->csa, (u32)n4, r_texclut };
    u16 groups = n4 ? (u16)(1u << (c->csa & 15)) : 0xFFFF;
    int is16 = c->csm ? 1 : (c->cpsm != PSMCT32), changed = (is16 != clut16);
    u32 seq = ++clut_load_seq;
    for (int g = 0; g < 16; g++) if (groups & (1u << g)) clut_seq[g] = seq;
    clut16 = is16;
    u32 pg = c->cbp >> 5;           /* CSM1 data sits in the first page (or spills into the next) */
#ifdef GS_HW
    if (hw_on && hwg_clut(&d, seq)) { clut_pending |= groups; clut_sig_valid = 0; return 1; }
#endif
    if (q_pending() && (c->csm != 0 || ps_test(&pend_dirty, pg & 511) || ps_test(&pend_dirty, (pg + 1) & 511))) {
        if (clut_njobs == 64) gs_sync(SY_CLUT);
        else {
            ClutJob *j = q_clut(&d, seq);
            clut_jobs[clut_njobs++] = j; clut_pending |= groups; clut_sig_valid = 0;
            return 1;
        }
    }
    u32 tmp[256];
    clut_read(&d, tmp);
    u32 base = n4 ? (c->csa & 15) * 16 : 0;
    if (clut_pending & groups) changed = 1;
    for (u32 i = base; i < base + (n4 ? 16u : 256u); i++) if (clut[i] != tmp[i]) { clut[i] = tmp[i]; changed = 1; }
    clut_pending &= (u16)~groups;
    if (changed) clut_sig_valid = 0;
    return changed;
}
/* gs_sync: the queued loads have run; take over the groups each one still owns. */
static void clut_apply_jobs(void) {
    for (int k = 0; k < clut_njobs; k++) {
        ClutJob *j = clut_jobs[k];
        u16 groups = j->d.n4 ? (u16)(1u << (j->d.csa & 15)) : 0xFFFF;
        for (int g = 0; g < 16; g++) if ((groups & (1u << g)) && clut_seq[g] == j->seq) memcpy(&clut[g * 16], &j->val[g * 16], 64);
        free(j);
    }
    clut_njobs = 0; clut_pending = 0; clut_sig_valid = 0;
}
static u64 clut_full_sig(void) {    /* callers make sure nothing is pending */
    if (!clut_sig_valid) {
        u64 h = 1469598103934665603ull;
        for (int i = 0; i < 256; i++) h = (h ^ clut[i]) * 1099511628211ull;
        clut_sig = h ^ (u64)clut16; clut_sig_valid = 1;
    }
    return clut_sig;
}

static int set_tex0(Ctxt *c, u64 v, int tex2) {
    if (tex2) v = (c->tex0 & ~0x03F00000ull & ~0xFFFFFFE000000000ull) | (v & 0x03F00000ull) | (v & 0xFFFFFFE000000000ull);
    c->tex0 = v;
    c->tbp = v & 0x3FFF; c->tbw = (v >> 14) & 63; c->tpsm = (v >> 20) & 63;
    c->tw = (v >> 26) & 15; c->th = (v >> 30) & 15; c->tcc = (v >> 34) & 1; c->tfx = (v >> 35) & 3;
    c->cbp = (v >> 37) & 0x3FFF; c->cpsm = (v >> 51) & 15; c->csm = (v >> 55) & 1; c->csa = (v >> 56) & 31;
    if (c->tw > 10) c->tw = 10;
    if (c->th > 10) c->th = 10;
    if (gs_dump_frame && g_gs_frames == gs_dump_frame)
        rt_log("[gsdump] %s tbp=%x psm=%x cbp=%x cpsm=%x csm=%u csa=%u cld=%u texclut=%llx\n", tex2 ? "TEX2" : "TEX0", c->tbp, c->tpsm,
               c->cbp, c->cpsm, c->csm, c->csa, (u32)((v >> 61) & 7), (unsigned long long)r_texclut);
    return ((v >> 61) & 7) ? load_clut(c) : 0;
}

/* ---- texels ------------------------------------------------------------------ */
ALWAYS_INLINE u32 expand16(u32 col, u32 ta0, u32 ta1, u32 aem) {
    u32 rgb = c16to32(col) & 0xFFFFFF;
    u32 a = (col & 0x8000) ? ta1 : ((aem && !(col & 0x7FFF)) ? 0 : ta0);
    return rgb | (a << 24);
}
static int tex_is_pal4(u32 psm) { return psm == PSMT4 || psm == PSMT4HL || psm == PSMT4HH; }
static int tex_is_direct(u32 psm) { return psm == PSMCT32 || psm == PSMCT24 || psm == PSMCT16 || psm == PSMCT16S; }

/* Fetch and expand one texel straight from local memory (u, v already wrapped).
 * Calling thread only: uses the live CLUT. */
static u32 texel_fetch(const DState *st, s32 u, s32 v) {
    const Ctxt *c = &st->c;
    u32 raw = rd_px(c->tpsm, c->tbp, c->tbw, (u32)u, (u32)v), col;
    int is16 = 0;
    switch (c->tpsm) {
    case PSMCT32: return raw;
    case PSMCT24:
        return raw | ((st->aem && !raw) ? 0 : st->ta0 << 24);
    case PSMCT16: case PSMCT16S: col = raw; is16 = 1; break;
    case PSMT4: case PSMT4HL: case PSMT4HH:
        col = clut[((c->csa & 15) * 16 + raw) & 255]; is16 = clut16; break;
    default:
        col = clut[raw & 255]; is16 = clut16; break;
    }
    if (!is16) return col;
    return expand16(col, st->ta0, st->ta1, st->aem);
}

/* Decode rows [y0, y1) of a texture into a linear RGBA array. `pal` is the
 * expanded palette for indexed formats (unused otherwise), so this function
 * reads nothing but local memory and its arguments and may run on workers. */
static void tex_decode_rows(const TexDesc *t, const u32 *pal, u32 *px, u32 y0, u32 y1) {
    u32 w = 1u << t->tw, bp = t->tbp, bw = t->tbw;
    switch (t->tpsm) {
    case PSMCT32:
        for (u32 y = y0; y < y1; y++) { u32 *o = px + ((size_t)y << t->tw);
            for (u32 x = 0; x < w; x++) o[x] = vram[a32(swz32, bp, bw, x, y)]; }
        break;
    case PSMCT24:
        for (u32 y = y0; y < y1; y++) { u32 *o = px + ((size_t)y << t->tw);
            for (u32 x = 0; x < w; x++) { u32 raw = vram[a32(swz32, bp, bw, x, y)] & 0xFFFFFF; o[x] = raw | ((t->aem && !raw) ? 0 : t->ta0 << 24); } }
        break;
    case PSMCT16: case PSMCT16S: {
        const u16 (*tab)[64] = t->tpsm == PSMCT16 ? swz16 : swz16s;
        for (u32 y = y0; y < y1; y++) { u32 *o = px + ((size_t)y << t->tw);
            for (u32 x = 0; x < w; x++) o[x] = expand16(VRAM16[a16(tab, bp, bw, x, y)], t->ta0, t->ta1, t->aem); }
        break;
    }
    case PSMT8:
        for (u32 y = y0; y < y1; y++) { u32 *o = px + ((size_t)y << t->tw);
            for (u32 x = 0; x < w; x++) o[x] = pal[VRAM8[a8(bp, bw, x, y)]]; }
        break;
    case PSMT4:
        for (u32 y = y0; y < y1; y++) { u32 *o = px + ((size_t)y << t->tw);
            for (u32 x = 0; x < w; x++) { u32 n = a4(bp, bw, x, y); o[x] = pal[(VRAM8[n >> 1] >> ((n & 1) * 4)) & 15]; } }
        break;
    case PSMT8H:
        for (u32 y = y0; y < y1; y++) { u32 *o = px + ((size_t)y << t->tw);
            for (u32 x = 0; x < w; x++) o[x] = pal[vram[a32(swz32, bp, bw, x, y)] >> 24]; }
        break;
    case PSMT4HL:
        for (u32 y = y0; y < y1; y++) { u32 *o = px + ((size_t)y << t->tw);
            for (u32 x = 0; x < w; x++) o[x] = pal[(vram[a32(swz32, bp, bw, x, y)] >> 24) & 15]; }
        break;
    case PSMT4HH:
        for (u32 y = y0; y < y1; y++) { u32 *o = px + ((size_t)y << t->tw);
            for (u32 x = 0; x < w; x++) o[x] = pal[vram[a32(swz32, bp, bw, x, y)] >> 28]; }
        break;
    default:            /* depth formats and undefined codes: whatever rd_px makes of them, through the CLUT */
        for (u32 y = y0; y < y1; y++) { u32 *o = px + ((size_t)y << t->tw);
            for (u32 x = 0; x < w; x++) o[x] = pal[rd_px(t->tpsm, bp, bw, x, y) & 255]; }
        break;
    }
}

/* ---- decoded-texture cache --------------------------------------------------
 * A texture is expanded once into a linear RGBA array. An entry is identified
 * by the texture's layout, its palette and TEXA, and validated in two steps:
 *  1. the generations of the pages it occupies are unchanged: nothing can have
 *     written there, the entry is current;
 *  2. otherwise the *contents* of those pages are hashed (per page, cached per
 *     generation) and compared. The game re-uploads its textures every frame,
 *     often several different ones through the same area in turn, so a
 *     "was written" test alone would decode some 2 M texels per frame; with
 *     the content test each distinct texture is decoded once and kept.
 * When the pages are still being rendered to by queued primitives (a frame
 * buffer or other render target used as a texture) their contents are not
 * known yet: the decode itself is queued, between two barriers, and the
 * workers fill the array at the right point of the draw order. Either way a
 * primitive always samples a snapshot taken at the moment it was submitted,
 * which is also what makes a primitive that draws into its own texture
 * well-defined and safe to shade in parallel.
 * Buffers are immutable while queued primitives may refer to them: replacing
 * one that was handed out since the last gs_sync defers the free to the next. */
typedef struct { u64 id, cs, gsig, csig, stamp, used_gen, frame; u32 texa, cap; u8 csig_known; u32 *px; int hwtex; u8 hw_valid; } TexEnt;
static TexEnt *tex_last;                 /* entry the last texcache_get returned */
static void tex_hw_forget(TexEnt *e);
#define TEX_SETS 512
#define TEX_WAYS 4
#define TEX_BUDGET (256ull << 20)
#define TEX_MAX_AGE 1500                 /* frames an unused texture is kept (30 s) */
static TexEnt texcache[TEX_SETS][TEX_WAYS];
static u64 tex_clock, tex_bytes;
static u32 **tex_defer; static int tex_ndefer, tex_cdefer;

static inline u64 rotl64(u64 x, int r) { return (x << r) | (x >> (64 - r)); }
static u64 page_content(u32 p) {                 /* hash of page p; only for pages nothing queued can write */
    if (page_hgen[p] != page_gen[p]) {
        const u64 *q = (const u64 *)(vram + p * 2048);
        const u64 P1 = 0x9E3779B185EBCA87ull, P2 = 0xC2B2AE3D27D4EB4Full;
        u64 a = P1, b = P2, c = 0, d = 0 - P1;
        for (int i = 0; i < 1024; i += 4) {
            a = rotl64(a + q[i] * P2, 31) * P1; b = rotl64(b + q[i + 1] * P2, 31) * P1;
            c = rotl64(c + q[i + 2] * P2, 31) * P1; d = rotl64(d + q[i + 3] * P2, 31) * P1;
        }
        u64 h = rotl64(a, 1) + rotl64(b, 7) + rotl64(c, 12) + rotl64(d, 18);
        h ^= h >> 33; h *= P2; h ^= h >> 29; h *= 0x165667B19E3779F9ull; h ^= h >> 32;
        page_hash[p] = h; page_hgen[p] = page_gen[p];
    }
    return page_hash[p];
}
static void tex_page_range(const Ctxt *c, u32 *first, u32 *last) {
    u32 w = 1u << c->tw, h = 1u << c->th;
    *first = c->tbp >> 5;
    *last = *first + page_off(c->tpsm, c->tbw, w - 1, h - 1);
    if (c->tbp & 31) (*last)++;
    if (*last - *first > 511) *last = *first + 511;
}
static void tex_free_later(u32 *px) {
    if (tex_ndefer == tex_cdefer) { tex_cdefer = tex_cdefer ? tex_cdefer * 2 : 64; tex_defer = realloc(tex_defer, sizeof(u32 *) * (size_t)tex_cdefer); }
    tex_defer[tex_ndefer++] = px;
}
static void tex_drop(TexEnt *e) {                /* release an entry's buffer */
    if (!e->px) return;
    tex_hw_forget(e);
    if (nthreads && e->used_gen == sync_gen && q_pending()) tex_free_later(e->px); else free(e->px);
    tex_bytes -= e->cap; e->px = 0; e->cap = 0;
}
static const u32 *texcache_get(DState *s) {
    const Ctxt *c = &s->c;
    int direct = tex_is_direct(c->tpsm), pal4 = tex_is_pal4(c->tpsm);
    u32 w = 1u << c->tw, h = 1u << c->th;
    u64 id = (u64)c->tbp | ((u64)c->tbw << 14) | ((u64)c->tpsm << 20) | ((u64)c->tw << 26) | ((u64)c->th << 30);
    u64 cs = 0; u32 texa = 0;
    if (!direct) {                               /* the palette is part of the identity */
        if (clut_pending & (pal4 ? (1u << (c->csa & 15)) : 0xFFFFu)) gs_sync(SY_CLUT);
        if (pal4) {                              /* only the 16 entries the texture can reach */
            u64 hh = 1469598103934665603ull; u32 base = (c->csa & 15) * 16;
            for (u32 i = 0; i < 16; i++) hh = (hh ^ clut[(base + i) & 255]) * 1099511628211ull;
            cs = hh ^ (u64)clut16 ^ 0x5bd1e995u;
        } else cs = clut_full_sig();
    }
    if (c->tpsm == PSMCT24) texa = s->ta0 | (s->aem << 16) | 0x1000000u;
    else if (c->tpsm == PSMCT16 || c->tpsm == PSMCT16S || (!direct && clut16)) texa = s->ta0 | (s->ta1 << 8) | (s->aem << 16) | 0x1000000u;
    u32 first, last; tex_page_range(c, &first, &last);
    u64 gsig = 0;
    for (u32 pg = first; pg <= last; pg++) gsig = gsig * 1099511628211ull + page_gen[pg & 511] + 1;
    TexEnt *set = texcache[((id ^ (cs * 0xD6E8FEB86659FD93ull) ^ texa) * 0x9E3779B97F4A7C15ull) >> 55];
    TexEnt *e = 0;
    for (int i = 0; i < TEX_WAYS; i++)
        if (set[i].px && set[i].id == id && set[i].cs == cs && set[i].texa == texa && set[i].gsig == gsig) { e = &set[i]; goto hit; }
    int pending = ps_meet(&s->texmask, &pend_dirty);
    if (pending && !direct) { gs_sync(SY_TEX); pending = 0; }    /* indexed render target: rare, decode it here */
    u64 csig = 0;
    if (!pending) {
        csig = 0x243F6A8885A308D3ull;
        for (u32 pg = first; pg <= last; pg++) { csig = (csig ^ page_content(pg & 511)) * 0x9E3779B185EBCA87ull; csig ^= csig >> 31; }
        for (int i = 0; i < TEX_WAYS; i++)
            if (set[i].px && set[i].id == id && set[i].cs == cs && set[i].texa == texa && set[i].csig_known && set[i].csig == csig) {
                e = &set[i]; e->gsig = gsig; goto hit;
            }
    }
    /* decode: into a free way, else over a stale render-target snapshot of this texture, else the least recently used */
    for (int i = 0; i < TEX_WAYS && !e; i++) if (!set[i].px) e = &set[i];
    for (int i = 0; i < TEX_WAYS && !e; i++) if (set[i].id == id && set[i].cs == cs && set[i].texa == texa && !set[i].csig_known) e = &set[i];
    if (!e) { e = &set[0]; for (int i = 1; i < TEX_WAYS; i++) if (set[i].stamp < e->stamp) e = &set[i]; }
    u32 need = w * h * 4;
    if (e->px && (e->cap < need || (nthreads && e->used_gen == sync_gen && q_pending()))) tex_drop(e);
    tex_hw_forget(e);
    if (!e->px) { e->px = malloc(need); e->cap = need; tex_bytes += need; }
    e->id = id; e->cs = cs; e->texa = texa; e->gsig = gsig; e->csig = csig; e->csig_known = (u8)!pending;
    TexDesc d = { c->tbp, c->tbw, c->tpsm, c->tw, c->th, c->csa, s->ta0, s->ta1, s->aem };
    if (pending) q_decode(&d, e->px);
    else {
        u32 pal[256];
        if (!direct) for (u32 i = 0; i < 256; i++) {
            u32 col = pal4 ? clut[((c->csa & 15) * 16 + i) & 255] : clut[i];
            pal[i] = clut16 ? expand16(col, s->ta0, s->ta1, s->aem) : col;
        }
        tex_decode_rows(&d, pal, e->px, 0, h);
    }
    g_gs_tex_rebuilt += (u64)w * h;
    e->stamp = ++tex_clock;
    while (tex_bytes > TEX_BUDGET) {             /* over budget: drop the least recently used texture */
        TexEnt *old = 0;
        for (int i = 0; i < TEX_SETS; i++) for (int j = 0; j < TEX_WAYS; j++) {
            TexEnt *o = &texcache[i][j];
            if (o->px && o != e && (!old || o->stamp < old->stamp)) old = o;
        }
        if (!old) break;
        tex_drop(old);
    }
hit:
    e->stamp = ++tex_clock; e->used_gen = sync_gen; e->frame = g_gs_frames;
    tex_last = e;
    return e->px;
}
/* Once per frame: look at a few sets and drop textures that have not been
 * used for a long time (palette fades leave a decoded copy per step). */
static void tex_age(void) {
    static u32 next;
    for (int k = 0; k < 8; k++, next = (next + 1) % TEX_SETS)
        for (int j = 0; j < TEX_WAYS; j++) {
            TexEnt *e = &texcache[next][j];
            if (e->px && g_gs_frames - e->frame > TEX_MAX_AGE) tex_drop(e);
        }
}

/* ---- number conversions ----------------------------------------------------------
 * The exact conversions the reference renderer's C casts compile to on x86-64,
 * spelled out so that out-of-range values behave identically everywhere. */
ALWAYS_INLINE s32 f2i(float f) { return _mm_cvttss_si32(_mm_set_ss(f)); }            /* (s32)f */
ALWAYS_INLINE u32 f2u(float f) { return (u32)_mm_cvttss_si64(_mm_set_ss(f)); }       /* (u32)f */
ALWAYS_INLINE s32 d2i(double d) { return _mm_cvttsd_si32(_mm_set_sd(d)); }           /* (s32)d */
ALWAYS_INLINE u32 d2u(double d) { return (u32)_mm_cvttsd_si64(_mm_set_sd(d)); }      /* (u32)d */
ALWAYS_INLINE s32 ifloorf(float f) {                                                 /* (s32)floorf(f) */
    s32 i = f2i(f);
    return i - (((float)i > f) & (i != INT32_MIN));
}
static inline int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* ---- generic pixel pipeline -----------------------------------------------------
 * Handles every state combination and buffer format. `track` selects tracked
 * memory writes (calling thread, serial primitives) or raw ones (workers). */
ALWAYS_INLINE s32 wrap_coord(u32 mode, s32 u, s32 size, s32 mn, s32 mx) {
    switch (mode) {
    case 0: return u & (size - 1);
    case 1: return u < 0 ? 0 : u >= size ? size - 1 : u;
    case 2: return u < mn ? mn : u > mx ? mx : u;
    default: return (u & mn) | mx;
    }
}
static inline u32 texel(const DState *st, s32 u, s32 v) {
    const Ctxt *c = &st->c;
    u = wrap_coord(c->wms, u, 1 << c->tw, (s32)c->minu, (s32)c->maxu);
    v = wrap_coord(c->wmt, v, 1 << c->th, (s32)c->minv, (s32)c->maxv);
    if (st->tex && (u32)u < (1u << c->tw) && (u32)v < (1u << c->th)) return st->tex[((u32)v << c->tw) + (u32)u];
    return texel_fetch(st, u, v);
}
ALWAYS_INLINE u32 bilerp(u32 t00, u32 t10, u32 t01, u32 t11, u32 fu, u32 fv) {
    if (fu <= 256 && fv <= 256) {
        /* two channels per 64-bit multiply; the weights sum to 65536, so each
         * 32-bit lane holds the same exact sum the per-channel form computes */
        u64 wa = (u64)((256 - fu) * (256 - fv)), wb = (u64)(fu * (256 - fv)), wc = (u64)((256 - fu) * fv), wd = (u64)(fu * fv);
#define RB(t) ((u64)((t) & 0xFF) | ((u64)((t) & 0xFF0000) << 16))
#define GA(t) ((u64)(((t) >> 8) & 0xFF) | ((u64)((t) >> 24) << 32))
        u64 rb = RB(t00) * wa + RB(t10) * wb + RB(t01) * wc + RB(t11) * wd;
        u64 ga = GA(t00) * wa + GA(t10) * wb + GA(t01) * wc + GA(t11) * wd;
#undef RB
#undef GA
        return (u32)((rb >> 16) & 0xFF) | (u32)((rb >> 32) & 0xFF0000) | (u32)((ga >> 8) & 0xFF00) | (u32)((ga >> 24) & 0xFF000000u);
    }
    u32 out = 0;                         /* coordinates beyond the integer range: wrap-around arithmetic as written */
    for (int s = 0; s < 32; s += 8) {
        u32 a = (t00 >> s) & 255, b = (t10 >> s) & 255, cc = (t01 >> s) & 255, d = (t11 >> s) & 255;
        u32 top = a * (256 - fu) + b * fu, bot = cc * (256 - fu) + d * fu;
        out |= (((top * (256 - fv) + bot * fv) >> 16) & 255) << s;
    }
    return out;
}
static inline u32 sample(const DState *st, float u, float v) {     /* u,v in texels */
    if (!st->c.mmag) return texel(st, ifloorf(u), ifloorf(v));
    u -= 0.5f; v -= 0.5f;
    s32 iu = ifloorf(u), iv = ifloorf(v);
    u32 fu = f2u((u - (float)iu) * 256.0f), fv = f2u((v - (float)iv) * 256.0f);
    u32 t00 = texel(st, iu, iv), t10 = texel(st, iu + 1, iv), t01 = texel(st, iu, iv + 1), t11 = texel(st, iu + 1, iv + 1);
    return bilerp(t00, t10, t01, t11, fu, fv);
}
ALWAYS_INLINE void tex_func(u32 tfx, u32 tcc, u32 t, int *r, int *g, int *b, int *a) {
    int tr = t & 255, tg = (t >> 8) & 255, tb = (t >> 16) & 255, ta = t >> 24;
    switch (tfx) {
    case 0:   /* modulate */
        *r = clamp8((tr * *r) >> 7); *g = clamp8((tg * *g) >> 7); *b = clamp8((tb * *b) >> 7);
        if (tcc) *a = clamp8((ta * *a) >> 7);
        break;
    case 1:   /* decal */
        *r = tr; *g = tg; *b = tb;
        if (tcc) *a = ta;
        break;
    case 2:   /* highlight */
        *r = clamp8(((tr * *r) >> 7) + *a); *g = clamp8(((tg * *g) >> 7) + *a); *b = clamp8(((tb * *b) >> 7) + *a);
        if (tcc) *a = clamp8(ta + *a);
        break;
    default:  /* highlight 2 */
        *r = clamp8(((tr * *r) >> 7) + *a); *g = clamp8(((tg * *g) >> 7) + *a); *b = clamp8(((tb * *b) >> 7) + *a);
        if (tcc) *a = ta;
        break;
    }
}
ALWAYS_INLINE int alpha_pass(int atst, int a, int aref) {
    switch (atst) {
    case 0: return 0;         case 1: return 1;
    case 2: return a < aref;  case 3: return a <= aref;
    case 4: return a == aref; case 5: return a >= aref;
    case 6: return a > aref;  default: return a != aref;
    }
}
ALWAYS_INLINE int bsel(u32 m, int s, int d) { return m == 0 ? s : m == 1 ? d : 0; }

/* Shade and write one pixel. (r,g,b,a) is the interpolated vertex colour. */
static void px_generic(const DState *st, int track, s32 x, s32 y, u32 z, int r, int g, int b, int a, float tu, float tv, int fog) {
    const Ctxt *c = &st->c;
    if (st->tme) tex_func(c->tfx, c->tcc, sample(st, tu, tv), &r, &g, &b, &a);
    if (st->fge) {
        r = (r * fog + st->fogr * (255 - fog)) >> 8;
        g = (g * fog + st->fogg * (255 - fog)) >> 8;
        b = (b * fog + st->fogb * (255 - fog)) >> 8;
    }
    int wr_rgb = 1, wr_a = 1, wr_z = !c->zmsk;
    if (st->ate && !alpha_pass(st->atst, a, st->aref)) switch (st->afail) {
        case 0: return;
        case 1: wr_z = 0; break;
        case 2: wr_rgb = wr_a = 0; break;
        default: wr_a = 0; wr_z = 0; break;
    }
    u32 fa = 0;                           /* frame pixel as RGBA8888 (read only when something needs it) */
    int f16 = (c->fpsm == PSMCT16 || c->fpsm == PSMCT16S);
    if (st->need_fb || !wr_rgb || !wr_a) {
        u32 fraw = rd_px(c->fpsm, c->fbp, c->fbw, (u32)x, (u32)y);
        if (f16) fa = c16to32(fraw);
        else if (c->fpsm == PSMCT24) fa = fraw | 0x80000000u;
        else fa = fraw;
    }
    if (st->date && c->fpsm != PSMCT24 && ((fa >> 31) != (u32)st->datm)) return;
    if (st->zte) {
        if (z > st->zmax) z = st->zmax;
        if (st->ztst == 0) return;
        if (st->ztst >= 2) {
            u32 zb = rd_px(c->zpsm, c->zbp, c->fbw, (u32)x, (u32)y);
            if (st->ztst == 2 ? z < zb : z <= zb) return;
        }
    } else wr_z = 0;
    if (st->abe && (!st->pabe || (a & 0x80))) {
        int dr = fa & 255, dg = (fa >> 8) & 255, db = (fa >> 16) & 255, da = fa >> 24;
        int cf = st->ac == 0 ? a : st->ac == 1 ? da : (int)st->fix;
        r = (((bsel(st->aa, r, dr) - bsel(st->ab, r, dr)) * cf) >> 7) + bsel(st->ad, r, dr);
        g = (((bsel(st->aa, g, dg) - bsel(st->ab, g, dg)) * cf) >> 7) + bsel(st->ad, g, dg);
        b = (((bsel(st->aa, b, db) - bsel(st->ab, b, db)) * cf) >> 7) + bsel(st->ad, b, db);
        if (st->colclamp) { r = clamp8(r); g = clamp8(g); b = clamp8(b); }
        else { r &= 255; g &= 255; b &= 255; }
    }
    if (wr_rgb || wr_a) {
        if (st->fba) a |= 0x80;
        u32 col = (u32)r | ((u32)g << 8) | ((u32)b << 16) | ((u32)a << 24);
        u32 mask = c->fbmsk;
        if (!wr_rgb) mask |= 0x00FFFFFFu;
        if (!wr_a) mask |= 0xFF000000u;
        col = (col & ~mask) | (fa & mask);
        (track ? wr_px : wr_raw)(c->fpsm, c->fbp, c->fbw, (u32)x, (u32)y, f16 ? c32to16(col) : col);
    }
    if (wr_z) (track ? wr_px : wr_raw)(c->zpsm, c->zbp, c->fbw, (u32)x, (u32)y, z);
}

/* ---- fast pixel pipeline (PSMCT32 frame, decoded texture) --------------------------
 * Same arithmetic as px_generic, with the state in locals (FS), frame and
 * depth pixels addressed through the swizzle tables, and the depth test done
 * first: every test only decides whether to write, so their order is free. */
ALWAYS_INLINE u32 fs_texel(const FS *f, s32 u, s32 v) {
    u = wrap_coord(f->wms, u, f->w, f->minu, f->maxu);
    v = wrap_coord(f->wmt, v, f->h, f->minv, f->maxv);
    return f->tex[((u32)v << f->tw) + (u32)u];
}
ALWAYS_INLINE u32 fs_sample(const FS *f, float u, float v) {
    if (!f->mmag) return fs_texel(f, ifloorf(u), ifloorf(v));
    u -= 0.5f; v -= 0.5f;
    s32 iu = ifloorf(u), iv = ifloorf(v);
    u32 fu = f2u((u - (float)iu) * 256.0f), fv = f2u((v - (float)iv) * 256.0f);
    u32 t00 = fs_texel(f, iu, iv), t10 = fs_texel(f, iu + 1, iv), t01 = fs_texel(f, iu, iv + 1), t11 = fs_texel(f, iu + 1, iv + 1);
    return bilerp(t00, t10, t01, t11, fu, fv);
}
ALWAYS_INLINE u32 z_read(const int ZK, const void *zp) {
    return ZK == ZK_16 ? *(const u16 *)zp : ZK == ZK_24 ? (*(const u32 *)zp & 0xFFFFFF) : *(const u32 *)zp;
}
ALWAYS_INLINE void z_write(const int ZK, void *zp, u32 z) {
    if (ZK == ZK_16) *(u16 *)zp = (u16)z;
    else if (ZK == ZK_24) *(u32 *)zp = (*(u32 *)zp & 0xFF000000u) | (z & 0xFFFFFF);
    else *(u32 *)zp = z;
}
ALWAYS_INLINE void *z_addr(const int ZK, u32 zrow, const u16 *zt, u32 x) {
    u32 pg = (zrow + (x >> 6)) & 511;
    return ZK == ZK_16 ? (void *)(VRAM16 + pg * 4096 + zt[x & 63]) : (void *)(vram + pg * 2048 + zt[x & 63]);
}
/* Depth test; returns 0 when the pixel is rejected. *z is clamped to the buffer's range. */
ALWAYS_INLINE int fs_ztest(const FS *f, const int ZK, const void *zp, u32 *z) {
    if (!f->zte) return 1;
    if (*z > f->zmax) *z = f->zmax;
    if (f->ztst >= 2) {
        u32 zb = z_read(ZK, zp);
        if (f->ztst == 2 ? *z < zb : *z <= zb) return 0;
    }
    return 1;
}
ALWAYS_INLINE void fs_shade(const FS *f, const int ZK, u32 *fp, void *zp, u32 z, int r, int g, int b, int a, float tu, float tv, int fog) {
    if (f->tme) tex_func((u32)f->tfx, (u32)f->tcc, fs_sample(f, tu, tv), &r, &g, &b, &a);
    if (f->fge) {
        r = (r * fog + f->fogr * (255 - fog)) >> 8;
        g = (g * fog + f->fogg * (255 - fog)) >> 8;
        b = (b * fog + f->fogb * (255 - fog)) >> 8;
    }
    int wr_rgb = 1, wr_a = 1, wr_z = f->zwr;
    if (f->ate && !alpha_pass(f->atst, a, f->aref)) switch (f->afail) {
        case 0: return;
        case 1: wr_z = 0; break;
        case 2: wr_rgb = wr_a = 0; break;
        default: wr_a = 0; wr_z = 0; break;
    }
    u32 fa = 0;
    if (f->need_fb || !wr_rgb || !wr_a) fa = *fp;
    if (f->date && (fa >> 31) != f->datm) return;
    if (f->abe && (!f->pabe || (a & 0x80))) {
        int dr = fa & 255, dg = (fa >> 8) & 255, db = (fa >> 16) & 255, da = fa >> 24;
        int cf = f->ac == 0 ? a : f->ac == 1 ? da : f->fix;
        r = (((bsel(f->aa, r, dr) - bsel(f->ab, r, dr)) * cf) >> 7) + bsel(f->ad, r, dr);
        g = (((bsel(f->aa, g, dg) - bsel(f->ab, g, dg)) * cf) >> 7) + bsel(f->ad, g, dg);
        b = (((bsel(f->aa, b, db) - bsel(f->ab, b, db)) * cf) >> 7) + bsel(f->ad, b, db);
        if (f->colclamp) { r = clamp8(r); g = clamp8(g); b = clamp8(b); }
        else { r &= 255; g &= 255; b &= 255; }
    }
    if (wr_rgb || wr_a) {
        if (f->fba) a |= 0x80;
        u32 col = (u32)r | ((u32)g << 8) | ((u32)b << 16) | ((u32)a << 24);
        u32 mask = f->fbmsk;
        if (!wr_rgb) mask |= 0x00FFFFFFu;
        if (!wr_a) mask |= 0xFF000000u;
        *fp = (col & ~mask) | (fa & mask);
    }
    if (wr_z) z_write(ZK, zp, z);
}

/* Specialisation. The fast rasterizers are compiled several times: once
 * reading every setting at run time, and once per common combination with
 * those settings as compile-time constants (fs_assume overwrites the local
 * copy of the state with the values the combination stands for, so the
 * compiler folds the tests away; fs_matches is the condition under which
 * doing so changes nothing). The combinations are the ones the game draws
 * nearly everything with. */
enum { A_BASE = 1,          /* textured, modulate, repeat/repeat, alpha test and blending on, depth test on, no mask/DATE/PABE/FBA, clamped */
       A_BILIN = 2, A_NEAR = 4, A_FOG = 8, A_NOFOG = 16,
       A_B0101 = 32,        /* blend (Cs - Cd) * As + Cd */
       A_FILL = 64 };       /* untextured, no blending, no alpha test: plain fill */
ALWAYS_INLINE void fs_assume(FS *f, const int A) {
    if (A & A_BASE) {
        f->tme = 1; f->tfx = 0; f->wms = 0; f->wmt = 0; f->fbmsk = 0; f->date = 0; f->pabe = 0; f->colclamp = 1; f->fba = 0;
        f->abe = 1; f->ate = 1; f->zte = 1; f->need_fb = 1;
    }
    if (A & A_BILIN) f->mmag = 1;
    if (A & A_NEAR) f->mmag = 0;
    if (A & A_FOG) f->fge = 1;
    if (A & A_NOFOG) f->fge = 0;
    if (A & A_B0101) { f->aa = 0; f->ab = 1; f->ac = 0; f->ad = 1; }
    if (A & A_FILL) { f->tme = 0; f->abe = 0; f->ate = 0; f->date = 0; f->fbmsk = 0; f->fge = 0; f->fba = 0; f->need_fb = 0; f->zte = 1; }
}
static int fs_matches(const FS *f, int A) {
    if ((A & A_BASE) && !(f->tme == 1 && f->tfx == 0 && f->wms == 0 && f->wmt == 0 && f->fbmsk == 0 && f->date == 0 && f->pabe == 0 &&
                          f->colclamp == 1 && f->fba == 0 && f->abe == 1 && f->ate == 1 && f->zte == 1 && f->need_fb == 1)) return 0;
    if ((A & A_BILIN) && f->mmag != 1) return 0;
    if ((A & A_NEAR) && f->mmag != 0) return 0;
    if ((A & A_FOG) && f->fge != 1) return 0;
    if ((A & A_NOFOG) && f->fge != 0) return 0;
    if ((A & A_B0101) && !(f->aa == 0 && f->ab == 1 && f->ac == 0 && f->ad == 1)) return 0;
    if ((A & A_FILL) && !(f->tme == 0 && f->abe == 0 && f->ate == 0 && f->date == 0 && f->fbmsk == 0 && f->fge == 0 && f->fba == 0 &&
                          f->need_fb == 0 && f->zte == 1)) return 0;
    return 1;
}
#define SPEC_LIST(X) X(1, A_BASE | A_BILIN | A_FOG | A_B0101) X(2, A_BASE | A_BILIN | A_NOFOG | A_B0101) X(3, A_BASE | A_BILIN | A_NOFOG) \
                     X(4, A_FILL) X(5, A_BASE | A_NEAR | A_FOG | A_B0101)
#define NSPEC 6             /* 0 = nothing assumed */

/* ---- primitive setup ----------------------------------------------------------------
 * Shared by the submitting thread (culling, extent) and the rasterizers. */
typedef struct { s32 x0, y0, px0, px1, py0, py1; float u0, v0, dudx, dvdy; } SpriteSetup;     /* px1, py1 exclusive */
static int sprite_setup(const DState *st, const Vtx *a, const Vtx *b, SpriteSetup *j) {
    const Ctxt *c = &st->c;
    s32 x0 = a->x, y0 = a->y, x1 = b->x, y1 = b->y;
    float u0, v0, u1, v1;
    if (st->fst) { u0 = (float)a->u / 16.0f; v0 = (float)a->v / 16.0f; u1 = (float)b->u / 16.0f; v1 = (float)b->v / 16.0f; }
    else {
        float q = b->q != 0.0f ? b->q : 1.0f, tw = (float)(1 << c->tw), th = (float)(1 << c->th);
        u0 = a->s / q * tw; v0 = a->t / q * th; u1 = b->s / q * tw; v1 = b->t / q * th;
    }
    if (x0 > x1) { s32 t = x0; x0 = x1; x1 = t; float f = u0; u0 = u1; u1 = f; }
    if (y0 > y1) { s32 t = y0; y0 = y1; y1 = t; float f = v0; v0 = v1; v1 = f; }
    if (x1 == x0 || y1 == y0) return 0;
    j->dudx = (u1 - u0) * 16.0f / (float)(x1 - x0); j->dvdy = (v1 - v0) * 16.0f / (float)(y1 - y0);
    s32 px0 = (x0 + 15) >> 4, px1 = (x1 + 15) >> 4, py0 = (y0 + 15) >> 4, py1 = (y1 + 15) >> 4;   /* [..) */
    if (px0 < c->sx0) px0 = c->sx0;
    if (py0 < c->sy0) py0 = c->sy0;
    if (px1 > c->sx1 + 1) px1 = c->sx1 + 1;
    if (py1 > c->sy1 + 1) py1 = c->sy1 + 1;
    j->x0 = x0; j->y0 = y0; j->px0 = px0; j->px1 = px1; j->py0 = py0; j->py1 = py1; j->u0 = u0; j->v0 = v0;
    return px1 > px0 && py1 > py0;
}

typedef struct {
    s32 px0, px1, py0, py1;                  /* inclusive */
    s64 dx16[3], dy16[3], w0[3], m[3];       /* edge functions at (px0, py0), their steps, and the value needed to be inside */
    double inv;
    double z[3], r[3], g[3], b[3], a[3], q[3], s[3], t[3], u[3], v[3], f[3];
    int fr, fg, fb, fa;                      /* flat colour */
    float tw, th;
} TriSetup;
/* Orientation and extent. Returns 0 when nothing would be drawn. */
static int tri_extent(const DState *st, const Vtx *v0, const Vtx *v1, const Vtx *v2, s32 *px0, s32 *px1, s32 *py0, s32 *py1) {
    const Ctxt *c = &st->c;
    s64 area = (s64)(v1->x - v0->x) * (v2->y - v0->y) - (s64)(v1->y - v0->y) * (v2->x - v0->x);
    if (area == 0) return 0;
    s32 minx = v0->x, maxx = v0->x, miny = v0->y, maxy = v0->y;
    if (v1->x < minx) minx = v1->x; if (v2->x < minx) minx = v2->x;
    if (v1->x > maxx) maxx = v1->x; if (v2->x > maxx) maxx = v2->x;
    if (v1->y < miny) miny = v1->y; if (v2->y < miny) miny = v2->y;
    if (v1->y > maxy) maxy = v1->y; if (v2->y > maxy) maxy = v2->y;
    *px0 = (minx + 15) >> 4; *px1 = maxx >> 4; *py0 = (miny + 15) >> 4; *py1 = maxy >> 4;
    if (*px0 < c->sx0) *px0 = c->sx0;
    if (*py0 < c->sy0) *py0 = c->sy0;
    if (*px1 > c->sx1) *px1 = c->sx1;
    if (*py1 > c->sy1) *py1 = c->sy1;
    return *px0 <= *px1 && *py0 <= *py1;
}
static void tri_setup(const Prim *p, TriSetup *ts) {
    const DState *st = p->st;
    const Vtx *v0 = &p->v[0], *v1 = &p->v[1], *v2 = &p->v[2], *flat = v2;
    s64 area = (s64)(v1->x - v0->x) * (v2->y - v0->y) - (s64)(v1->y - v0->y) * (v2->x - v0->x);
    if (area < 0) { const Vtx *t = v1; v1 = v2; v2 = t; area = -area; }
    ts->px0 = p->px0; ts->px1 = p->px1; ts->py0 = p->py0; ts->py1 = p->py1;
    /* edge i is opposite vertex i; inside when w > 0, or w == 0 on a top/left edge */
    const Vtx *e[3][2] = { { v1, v2 }, { v2, v0 }, { v0, v1 } };
    for (int i = 0; i < 3; i++) {
        s64 dx = e[i][1]->x - e[i][0]->x, dy = e[i][1]->y - e[i][0]->y;
        int tl = (dy < 0) || (dy == 0 && dx > 0);
        ts->w0[i] = dx * ((s64)p->py0 * 16 - e[i][0]->y) - dy * ((s64)p->px0 * 16 - e[i][0]->x);
        ts->dx16[i] = dx * 16; ts->dy16[i] = dy * 16; ts->m[i] = 1 - tl;
    }
    ts->inv = 1.0 / (double)area;
    const Vtx *vv[3] = { v0, v1, v2 };
    for (int i = 0; i < 3; i++) {
        ts->z[i] = (double)vv[i]->z; ts->r[i] = vv[i]->r; ts->g[i] = vv[i]->g; ts->b[i] = vv[i]->b; ts->a[i] = vv[i]->a;
        ts->q[i] = vv[i]->q; ts->s[i] = vv[i]->s; ts->t[i] = vv[i]->t; ts->u[i] = vv[i]->u; ts->v[i] = vv[i]->v; ts->f[i] = vv[i]->f;
    }
    ts->fr = flat->r; ts->fg = flat->g; ts->fb = flat->b; ts->fa = flat->a;
    ts->tw = (float)(1 << st->c.tw); ts->th = (float)(1 << st->c.th);
}
/* Columns [*ka, *kb] (relative to px0) of one row that are inside all three
 * edges, given the edge values at the row's first column. The covered pixels
 * of a row form one run because the triangle is convex; edge i admits column
 * k when w_i - dy16_i * k >= m_i. Returns 0 for an empty row. */
ALWAYS_INLINE int tri_span(const TriSetup *ts, const s64 *wr, s32 *ka, s32 *kb) {
    s64 lo = 0, hi = ts->px1 - ts->px0;
    for (int i = 0; i < 3; i++) {
        s64 A = wr[i] - ts->m[i], d = ts->dy16[i];
        if (d > 0) {                         /* k <= floor(A / d) */
            if (A < 0) return 0;
            s64 k = (s64)((u64)A / (u64)d);
            if (k < hi) hi = k;
        } else if (d < 0) {                  /* k >= ceil(-A / -d) */
            if (A < 0) { s64 k = (s64)(((u64)-A + (u64)-d - 1) / (u64)-d); if (k > lo) lo = k; }
        } else if (A < 0) return 0;
    }
    if (lo > hi) return 0;
    *ka = (s32)lo; *kb = (s32)hi;
    return 1;
}

/* ---- rasterizers --------------------------------------------------------------------------
 * Each draws the rows of one primitive that belong to the caller: the screen
 * is cut into stripes of 8 rows, every stripe has one owner (see the render
 * queue), and `rows` names the stripes of the calling worker. A null table
 * means all rows (calling thread, nothing queued). */
typedef struct { const u8 *own; u8 me; } Rows;
ALWAYS_INLINE int row_mine(const Rows rows, s32 y) { return !rows.own || rows.own[(u32)y >> 3] == rows.me; }
/* The attributes are interpolated two at a time in the halves of an SSE2
 * register. Each half performs exactly the operations of the scalar formula
 * in tri_generic, in the same order, so the results are the same bits. */
#define DOT2(a) _mm_add_pd(_mm_add_pd(_mm_mul_pd(a##0, vb0), _mm_mul_pd(a##1, vb1)), _mm_mul_pd(a##2, vb2))
ALWAYS_INLINE void tri_fast(const DState *st, const TriSetup *ts, const Rows rows, const int ZK, const int A) {
    FS f = st->fs;
    fs_assume(&f, A);
    const int iip = st->iip, fst = st->fst;
    const double inv = ts->inv;
    const double z0 = ts->z[0], z1 = ts->z[1], z2 = ts->z[2];
    const s64 e0 = ts->dy16[0], e1 = ts->dy16[1], e2 = ts->dy16[2];
#define PAIR(name, lo, hi) const __m128d name##0 = _mm_set_pd(ts->hi[0], ts->lo[0]), name##1 = _mm_set_pd(ts->hi[1], ts->lo[1]), name##2 = _mm_set_pd(ts->hi[2], ts->lo[2])
    PAIR(rg, r, g); PAIR(ba, b, a); PAIR(sv, s, t); PAIR(uv, u, v); PAIR(qf, q, f);
#undef PAIR
    const __m128d half = _mm_set1_pd(0.5);
    const __m128 twth = _mm_set_ps(0.0f, 0.0f, ts->th, ts->tw), sixteen = _mm_set1_ps(16.0f);
    for (s32 y = ts->py0; y <= ts->py1; y++) {
        if (!row_mine(rows, y)) continue;
        s64 wr[3]; s32 ka, kb;
        for (int i = 0; i < 3; i++) wr[i] = ts->w0[i] + ts->dx16[i] * (y - ts->py0);
        if (!tri_span(ts, wr, &ka, &kb)) continue;
        s64 w0 = wr[0] - e0 * ka, w1 = wr[1] - e1 * ka, w2 = wr[2] - e2 * ka;
        const u32 frow = f.fpg0 + ((u32)y >> 5) * f.fbw, zrow = f.zpg0 + ((u32)y >> f.zysh) * f.fbw;
        const u16 *ft = swz32[y & 31], *zt = f.ztab[(u32)y & f.zymask];
        for (s32 x = ts->px0 + ka, xe = ts->px0 + kb; x <= xe; x++, w0 -= e0, w1 -= e1, w2 -= e2) {
            double b0 = (double)w0 * inv, b1 = (double)w1 * inv, b2 = (double)w2 * inv;
            u32 z = d2u(z0 * b0 + z1 * b1 + z2 * b2);
            void *zp = z_addr(ZK, zrow, zt, (u32)x);
            if (!fs_ztest(&f, ZK, zp, &z)) continue;
            const __m128d vb0 = _mm_set1_pd(b0), vb1 = _mm_set1_pd(b1), vb2 = _mm_set1_pd(b2);
            int r, g, b, a;
            if (iip) {
                __m128i c0 = _mm_cvttpd_epi32(_mm_add_pd(DOT2(rg), half)), c1 = _mm_cvttpd_epi32(_mm_add_pd(DOT2(ba), half));
                r = _mm_cvtsi128_si32(c0); g = _mm_cvtsi128_si32(_mm_srli_si128(c0, 4));
                b = _mm_cvtsi128_si32(c1); a = _mm_cvtsi128_si32(_mm_srli_si128(c1, 4));
            } else { r = ts->fr; g = ts->fg; b = ts->fb; a = ts->fa; }
            float tu = 0, tv = 0; int fog = 0;
            if ((f.tme && !fst) || f.fge) {
                __m128d qfv = DOT2(qf);                  /* q in the low half, fog in the high half */
                if (f.fge) fog = _mm_cvttsd_si32(_mm_unpackhi_pd(qfv, qfv));
                if (f.tme && !fst) {
                    double q = _mm_cvtsd_f64(qfv);
                    if (q == 0.0) q = 1.0;
                    __m128 t2 = _mm_mul_ps(_mm_cvtpd_ps(_mm_div_pd(DOT2(sv), _mm_set1_pd(q))), twth);
                    tu = _mm_cvtss_f32(t2); tv = _mm_cvtss_f32(_mm_shuffle_ps(t2, t2, 1));
                }
            }
            if (f.tme && fst) {
                __m128 t2 = _mm_div_ps(_mm_cvtpd_ps(DOT2(uv)), sixteen);
                tu = _mm_cvtss_f32(t2); tv = _mm_cvtss_f32(_mm_shuffle_ps(t2, t2, 1));
            }
            u32 *fp = vram + ((frow + ((u32)x >> 6)) & 511) * 2048 + ft[x & 63];
            fs_shade(&f, ZK, fp, zp, z, r, g, b, a, tu, tv, fog);
        }
    }
}
#undef DOT2
typedef void (*TriFn)(const DState *st, const TriSetup *ts, const Rows rows);
static void tri_fast_z16(const DState *st, const TriSetup *ts, const Rows rows) { tri_fast(st, ts, rows, ZK_16, 0); }
static void tri_fast_z24(const DState *st, const TriSetup *ts, const Rows rows) { tri_fast(st, ts, rows, ZK_24, 0); }
static void tri_fast_z32(const DState *st, const TriSetup *ts, const Rows rows) { tri_fast(st, ts, rows, ZK_32, 0); }
#define X(n, a) static void tri_fast_z16_##n(const DState *st, const TriSetup *ts, const Rows rows) { tri_fast(st, ts, rows, ZK_16, a); }
SPEC_LIST(X)
#undef X

static void tri_generic(const DState *st, const TriSetup *ts, const Rows rows, int track) {
    const double inv = ts->inv;
    for (s32 y = ts->py0; y <= ts->py1; y++) {
        if (!row_mine(rows, y)) continue;
        s64 wr[3]; s32 ka, kb;
        for (int i = 0; i < 3; i++) wr[i] = ts->w0[i] + ts->dx16[i] * (y - ts->py0);
        if (!tri_span(ts, wr, &ka, &kb)) continue;
        s64 w0 = wr[0] - ts->dy16[0] * ka, w1 = wr[1] - ts->dy16[1] * ka, w2 = wr[2] - ts->dy16[2] * ka;
        for (s32 x = ts->px0 + ka; x <= ts->px0 + kb; x++, w0 -= ts->dy16[0], w1 -= ts->dy16[1], w2 -= ts->dy16[2]) {
            double b0 = (double)w0 * inv, b1 = (double)w1 * inv, b2 = (double)w2 * inv;
            u32 z = d2u(ts->z[0] * b0 + ts->z[1] * b1 + ts->z[2] * b2);
            int r, g, b, a;
            if (st->iip) {
                r = d2i(ts->r[0] * b0 + ts->r[1] * b1 + ts->r[2] * b2 + 0.5);
                g = d2i(ts->g[0] * b0 + ts->g[1] * b1 + ts->g[2] * b2 + 0.5);
                b = d2i(ts->b[0] * b0 + ts->b[1] * b1 + ts->b[2] * b2 + 0.5);
                a = d2i(ts->a[0] * b0 + ts->a[1] * b1 + ts->a[2] * b2 + 0.5);
            } else { r = ts->fr; g = ts->fg; b = ts->fb; a = ts->fa; }
            float tu = 0, tv = 0;
            if (st->tme) {
                if (st->fst) {
                    tu = (float)(ts->u[0] * b0 + ts->u[1] * b1 + ts->u[2] * b2) / 16.0f;
                    tv = (float)(ts->v[0] * b0 + ts->v[1] * b1 + ts->v[2] * b2) / 16.0f;
                } else {
                    double q = ts->q[0] * b0 + ts->q[1] * b1 + ts->q[2] * b2;
                    if (q == 0.0) q = 1.0;
                    tu = (float)((ts->s[0] * b0 + ts->s[1] * b1 + ts->s[2] * b2) / q) * ts->tw;
                    tv = (float)((ts->t[0] * b0 + ts->t[1] * b1 + ts->t[2] * b2) / q) * ts->th;
                }
            }
            int fog = st->fge ? d2i(ts->f[0] * b0 + ts->f[1] * b1 + ts->f[2] * b2) : 0;
            px_generic(st, track, x, y, z, r, g, b, a, tu, tv, fog);
        }
    }
}

ALWAYS_INLINE void sprite_fast(const DState *st, const SpriteSetup *j, const Vtx *b, const Rows rows, const int ZK, const int A) {
    FS f = st->fs;
    fs_assume(&f, A);
    const int cr = b->r, cg = b->g, cb = b->b, ca = b->a, fog = b->f;
    const float u0 = j->u0, v0 = j->v0, dudx = j->dudx, dvdy = j->dvdy;
    const s32 x0 = j->x0, px0 = j->px0, px1 = j->px1;
    for (s32 y = j->py0; y < j->py1; y++) {
        if (!row_mine(rows, y)) continue;
        float tv = v0 + ((float)(y * 16 - j->y0) / 16.0f) * dvdy;
        const u32 frow = f.fpg0 + ((u32)y >> 5) * f.fbw, zrow = f.zpg0 + ((u32)y >> f.zysh) * f.fbw;
        const u16 *ft = swz32[y & 31], *zt = f.ztab[(u32)y & f.zymask];
        for (s32 x = px0; x < px1; x++) {
            u32 z = b->z;
            void *zp = z_addr(ZK, zrow, zt, (u32)x);
            if (!fs_ztest(&f, ZK, zp, &z)) continue;
            float tu = u0 + ((float)(x * 16 - x0) / 16.0f) * dudx;
            u32 *fp = vram + ((frow + ((u32)x >> 6)) & 511) * 2048 + ft[x & 63];
            fs_shade(&f, ZK, fp, zp, z, cr, cg, cb, ca, tu, tv, fog);
        }
    }
}
typedef void (*SpriteFn)(const DState *st, const SpriteSetup *j, const Vtx *b, const Rows rows);
static void sprite_fast_z16(const DState *st, const SpriteSetup *j, const Vtx *b, const Rows rows) { sprite_fast(st, j, b, rows, ZK_16, 0); }
static void sprite_fast_z24(const DState *st, const SpriteSetup *j, const Vtx *b, const Rows rows) { sprite_fast(st, j, b, rows, ZK_24, 0); }
static void sprite_fast_z32(const DState *st, const SpriteSetup *j, const Vtx *b, const Rows rows) { sprite_fast(st, j, b, rows, ZK_32, 0); }
#define X(n, a) static void sprite_fast_z16_##n(const DState *st, const SpriteSetup *j, const Vtx *b, const Rows rows) { sprite_fast(st, j, b, rows, ZK_16, a); }
SPEC_LIST(X)
#undef X
/* The instance to use for a state (fast states only). */
static void pick_fast(const DState *st, TriFn *tf, SpriteFn *sf) {
    static const struct { int a; TriFn t; SpriteFn s; } z16[] = {
#define X(n, a) { a, tri_fast_z16_##n, sprite_fast_z16_##n },
        SPEC_LIST(X)
#undef X
    };
    *tf = st->zkind == ZK_16 ? tri_fast_z16 : st->zkind == ZK_24 ? tri_fast_z24 : tri_fast_z32;
    *sf = st->zkind == ZK_16 ? sprite_fast_z16 : st->zkind == ZK_24 ? sprite_fast_z24 : sprite_fast_z32;
    if (st->zkind == ZK_16 && gs_force != 3)
        for (unsigned i = 0; i < sizeof z16 / sizeof z16[0]; i++)
            if (fs_matches(&st->fs, z16[i].a)) { *tf = z16[i].t; *sf = z16[i].s; break; }
}
static void sprite_generic(const DState *st, const SpriteSetup *j, const Vtx *b, const Rows rows, int track) {
    for (s32 y = j->py0; y < j->py1; y++) {
        if (!row_mine(rows, y)) continue;
        float tv = j->v0 + ((float)(y * 16 - j->y0) / 16.0f) * j->dvdy;
        for (s32 x = j->px0; x < j->px1; x++) {
            float tu = j->u0 + ((float)(x * 16 - j->x0) / 16.0f) * j->dudx;
            px_generic(st, track, x, y, b->z, b->r, b->g, b->b, b->a, tu, tv, b->f);
        }
    }
}

static void tex_of(const DState *st, const Vtx *v, float *tu, float *tv) {
    const Ctxt *c = &st->c;
    if (st->fst) { *tu = (float)v->u / 16.0f; *tv = (float)v->v / 16.0f; }
    else { float q = v->q != 0.0f ? v->q : 1.0f; *tu = v->s / q * (float)(1 << c->tw); *tv = v->t / q * (float)(1 << c->th); }
}
static void draw_point(const DState *st, const Vtx *v, const Rows rows, int track) {
    const Ctxt *c = &st->c;
    s32 x = (v->x + 8) >> 4, y = (v->y + 8) >> 4;
    if (x < c->sx0 || x > c->sx1 || y < c->sy0 || y > c->sy1 || !row_mine(rows, y)) return;
    float tu, tv; tex_of(st, v, &tu, &tv);
    px_generic(st, track, x, y, v->z, v->r, v->g, v->b, v->a, tu, tv, v->f);
}
static void draw_line(const DState *st, const Vtx *a, const Vtx *b, const Rows rows, int track) {
    const Ctxt *c = &st->c;
    s32 dx = b->x - a->x, dy = b->y - a->y;
    s32 adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    s32 steps = ((adx > ady ? adx : ady) + 15) >> 4;
    if (steps <= 0) { draw_point(st, b, rows, track); return; }
    float au, av, bu, bv; tex_of(st, a, &au, &av); tex_of(st, b, &bu, &bv);
    for (s32 i = 0; i < steps; i++) {             /* last point excluded */
        double t = (double)i / (double)steps;
        s32 x = d2i((a->x + dx * t) + 8) >> 4, y = d2i((a->y + dy * t) + 8) >> 4;
        if (x < c->sx0 || x > c->sx1 || y < c->sy0 || y > c->sy1 || !row_mine(rows, y)) continue;
        int r = b->r, g = b->g, bl = b->b, al = b->a;
        if (st->iip) {
            r = d2i(a->r + (b->r - a->r) * t); g = d2i(a->g + (b->g - a->g) * t);
            bl = d2i(a->b + (b->b - a->b) * t); al = d2i(a->a + (b->a - a->a) * t);
        }
        px_generic(st, track, x, y, d2u(a->z + ((double)b->z - (double)a->z) * t), r, g, bl, al,
                   (float)(au + (bu - au) * t), (float)(av + (bv - av) * t), d2i(a->f + (b->f - a->f) * t));
    }
}

/* Draw the caller's rows of a primitive. `serial` (calling thread only) uses
 * the generic path with tracked writes. */
static void rast_prim(const Prim *p, const Rows rows, int serial) {
    const DState *st = p->st;
    switch (p->type) {
    case PT_POINT: draw_point(st, &p->v[0], rows, serial); break;
    case PT_LINE: draw_line(st, &p->v[0], &p->v[1], rows, serial); break;
    case PT_TRI: {
        TriSetup ts; tri_setup(p, &ts);
        if (serial || !st->fast) tri_generic(st, &ts, rows, serial);
        else ((TriFn)st->tri_fn)(st, &ts, rows);
        break;
    }
    case PT_SPRITE: {
        SpriteSetup j;
        if (!sprite_setup(st, &p->v[0], &p->v[1], &j)) break;
        if (serial || !st->fast) sprite_generic(st, &j, &p->v[1], rows, serial);
        else ((SpriteFn)st->sprite_fn)(st, &j, &p->v[1], rows);
        break;
    }
    }
}

/* ---- draw state snapshot ------------------------------------------------------------ */
static void build_state(DState *s, u64 pr) {
    s->c = ctxt[(pr >> 9) & 1];
    const Ctxt *c = &s->c;
    s->pr = pr;
    s->iip = (pr >> 3) & 1; s->tme = (pr >> 4) & 1; s->fge = (pr >> 5) & 1;
    s->abe = (pr >> 6) & 1; s->fst = (pr >> 8) & 1;
    s->aa = c->alpha & 3; s->ab = (c->alpha >> 2) & 3; s->ac = (c->alpha >> 4) & 3; s->ad = (c->alpha >> 6) & 3;
    s->fix = (c->alpha >> 32) & 255;
    s->ate = c->test & 1; s->atst = (c->test >> 1) & 7; s->aref = (c->test >> 4) & 255; s->afail = (c->test >> 12) & 3;
    s->date = (c->test >> 14) & 1; s->datm = (c->test >> 15) & 1; s->zte = (c->test >> 16) & 1; s->ztst = (c->test >> 17) & 3;
    s->pabe = r_pabe & 1; s->colclamp = r_colclamp & 1; s->fba = c->fba & 1;
    s->ta0 = r_texa & 255; s->aem = (r_texa >> 15) & 1; s->ta1 = (r_texa >> 32) & 255;
    s->zmax = c->zpsm == PSMZ32 ? 0xFFFFFFFFu : c->zpsm == PSMZ24 ? 0xFFFFFFu : 0xFFFFu;
    s->need_fb = s->abe || s->date || c->fbmsk != 0;
    s->fogr = r_fogcol & 255; s->fogg = (r_fogcol >> 8) & 255; s->fogb = (r_fogcol >> 16) & 255;
    s->tex = 0; ps_clear(&s->texmask);
    s->touch_z = (u8)(s->zte && (s->ztst >= 2 || !c->zmsk));
    s->never = (u8)(s->zte && s->ztst == 0);
    s->tkey = (u64)c->fbp | ((u64)c->fbw << 16) | ((u64)c->fpsm << 24) | ((u64)c->zbp << 32) | ((u64)c->zpsm << 48);
    FS *f = &s->fs;
    int zvalid = 1;
    switch (c->zpsm) {
    case PSMZ32: s->zkind = ZK_32; f->ztab = swz32z; f->zysh = 5; break;
    case PSMZ24: s->zkind = ZK_24; f->ztab = swz32z; f->zysh = 5; break;
    case PSMZ16: s->zkind = ZK_16; f->ztab = swz16z; f->zysh = 6; break;
    case PSMZ16S: s->zkind = ZK_16; f->ztab = swz16sz; f->zysh = 6; break;
    default: zvalid = 0; s->zkind = ZK_32; f->ztab = swz32z; f->zysh = 5; break;
    }
    /* Band-parallel shading relies on distinct pixels being distinct memory
     * words, which holds for the real frame and depth formats only. */
    s->serial = (u8)(!zvalid || !(c->fpsm == PSMCT32 || c->fpsm == PSMCT24 || c->fpsm == PSMCT16 || c->fpsm == PSMCT16S));
    if (s->tme) {
        u32 first, last; tex_page_range(c, &first, &last);
        for (u32 pg = first; pg <= last; pg++) ps_set(&s->texmask, pg & 511);
        /* region clamp/repeat can address texels outside the texture, which are
         * read from local memory directly (texel()): calling thread only */
        u32 w = 1u << c->tw, h = 1u << c->th;
        if ((c->wms == 2 && (c->minu >= w || c->maxu >= w)) || (c->wms == 3 && (c->minu | c->maxu) >= w) ||
            (c->wmt == 2 && (c->minv >= h || c->maxv >= h)) || (c->wmt == 3 && (c->minv | c->maxv) >= h)) s->serial = 1;
#ifdef GS_HW
        if (hw_on) hwg_tex(s); else
#endif
        s->tex = texcache_get(s);
    }
    s->fast = (u8)(c->fpsm == PSMCT32 && !s->serial);
    if (gs_force == 1) s->fast = 0;          /* test aids: everything through the generic path ... */
    if (gs_force == 2) s->serial = 1;        /* ... or drawn serially by the calling thread */
    f->fpg0 = c->fbp >> 5; f->fbw = c->fbw; f->fbmsk = c->fbmsk;
    f->zpg0 = c->zbp >> 5; f->zymask = (1u << f->zysh) - 1; f->zmax = s->zmax;
    f->zte = s->zte; f->ztst = s->ztst; f->zwr = s->zte && !c->zmsk;
    f->tme = s->tme; f->mmag = (int)c->mmag; f->tfx = (int)c->tfx; f->tcc = (int)c->tcc;
    f->tex = s->tex; f->tw = c->tw; f->w = 1 << c->tw; f->h = 1 << c->th; f->wms = c->wms; f->wmt = c->wmt;
    f->minu = (s32)c->minu; f->maxu = (s32)c->maxu; f->minv = (s32)c->minv; f->maxv = (s32)c->maxv;
    f->fge = s->fge; f->fogr = s->fogr; f->fogg = s->fogg; f->fogb = s->fogb;
    f->ate = s->ate; f->atst = s->atst; f->aref = s->aref; f->afail = s->afail;
    f->need_fb = s->need_fb; f->date = s->date; f->datm = (u32)s->datm;
    f->abe = s->abe; f->pabe = s->pabe; f->colclamp = s->colclamp; f->fba = s->fba;
    f->aa = s->aa; f->ab = s->ab; f->ac = s->ac; f->ad = s->ad; f->fix = (int)s->fix;
    if (s->fast) { TriFn tf; SpriteFn sf; pick_fast(s, &tf, &sf); s->tri_fn = (void (*)(void))tf; s->sprite_fn = (void (*)(void))sf; }
}

/* ---- render queue -----------------------------------------------------------------------
 * Primitives are appended to `queue` by the calling ("main") thread and
 * consumed by every worker: the screen is cut into stripes of 8 pixel rows,
 * stripe s belongs to worker stripe_owner[s] (dealt round-robin, so that
 * every worker gets a similar share of whatever part of the screen a batch
 * happens to cover), and a worker draws, for each primitive in order, only
 * the rows of its own stripes. The result is the one serial rendering would
 * give, and there are no data races, because:
 *
 *  - A pixel's shading reads and writes only that pixel's own frame and depth
 *    words, plus immutable data (the Prim, its DState, the decoded texture).
 *    All pixels of one row belong to one worker, which processes primitives
 *    in submission order, so every pixel sees exactly the serial history.
 *  - Different pixels must be different memory words. Within a batch (the
 *    primitives between two barriers) all primitives share one frame/depth
 *    layout (DState.tkey), pixel -> word is injective for a layout as long as
 *    x < 64 * FBW and the buffer does not wrap around local memory, and frame
 *    pages are disjoint from depth pages (batch_f / batch_z). A primitive that
 *    would break one of these starts a new batch, or, when it conflicts with
 *    itself, is drawn serially by the main thread after gs_sync.
 *  - At a barrier every worker waits until all workers have arrived, so
 *    batches execute strictly one after another.
 *  - Textures are never read from local memory by workers while it may be
 *    written: primitives sample decoded snapshots, and a snapshot of memory
 *    that queued primitives render to is produced by a PT_DECODE job that sits
 *    between two barriers (each worker decodes a share of the rows).
 *  - The main thread reads or writes local memory only in pages that are not
 *    in pend_dirty (the pages queued work may touch), or after gs_sync().
 *  - Queue entries, DStates and texture snapshots are written before the
 *    entry is published (q_head, release/acquire) and are not modified or
 *    freed until a gs_sync has seen every worker finish.
 * The stripe table is fixed before the workers start. */
#define QCAP 32768                      /* power of two */
#define MAXSTATES 4096
#define MAXT 16
typedef struct {
    _Alignas(64) atomic_ullong pos;     /* queue entries before this index are done */
    int idx;
    u64 t_work, t_wait, t_idle;         /* ROR_GS_TIMING statistics */
} Worker;
static int gs_timing;                   /* ROR_GS_TIMING: per-worker time accounting (printed with the ROR_FPSLOG line) */
static u64 t_sync_wait;
static Worker workers[MAXT];
static Prim *queue; static DState *states; static int nstates;
static const DState *cur_st; static u64 cur_pr;
static _Alignas(64) atomic_ullong q_head;          /* entries below it are published */
static _Alignas(64) u64 q_tail;                    /* next entry to write (main thread) */
static u64 q_base;                                 /* q_tail at the last gs_sync */
static u8 stripe_owner[256];                       /* owner of rows 8s .. 8s+7 */
static PageSet batch_f, batch_z; static u64 batch_key; static int batch_n;
static pthread_mutex_t q_m = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t q_cv = PTHREAD_COND_INITIALIZER;
static atomic_int q_sleepers;
static int idle_spins = 1000;       /* ROR_GS_SPIN: polls before an idle worker sleeps */
static u64 stat_sync[SY_N], stat_barriers, stat_decodes, stat_serial, stat_states, stat_xfer_px;

static int q_pending(void) { return q_tail != q_base; }
static void q_publish(void) {
    if (atomic_load_explicit(&q_head, memory_order_relaxed) == q_tail) return;
    atomic_store(&q_head, q_tail);
    /* Dekker-style handshake with worker_main: a worker increments q_sleepers
     * before re-checking q_head under the mutex, so either it sees the new
     * head or we see it sleeping and wake it. */
    if (atomic_load(&q_sleepers)) {
        pthread_mutex_lock(&q_m);
        pthread_cond_broadcast(&q_cv);
        pthread_mutex_unlock(&q_m);
    }
}
static Prim *q_new(void) { return &queue[q_tail & (QCAP - 1)]; }
static void q_commit(void) {
    q_tail++;
    if (q_tail - atomic_load_explicit(&q_head, memory_order_relaxed) >= 48) q_publish();
}
static void q_barrier(void) {
    if (batch_n) { q_new()->type = PT_BARRIER; q_commit(); stat_barriers++; }
    batch_n = 0; ps_clear(&batch_f); ps_clear(&batch_z);
}
static void q_decode(const TexDesc *d, u32 *px) {
    q_barrier();
    Prim *p = q_new(); p->type = PT_DECODE; p->dec.d = *d; p->dec.px = px; q_commit();
    batch_n = 1; q_barrier();
    /* the job reads the whole texture: keep the main thread out of those pages */
    Ctxt c; c.tbp = d->tbp; c.tbw = d->tbw; c.tpsm = d->tpsm; c.tw = d->tw; c.th = d->th;
    u32 first, last; tex_page_range(&c, &first, &last);
    for (u32 pg = first; pg <= last; pg++) ps_set(&pend_dirty, pg & 511);
    stat_decodes++;
}

static ClutJob *q_clut(const ClutDesc *d, u32 seq) {
    ClutJob *j = malloc(sizeof *j);
    j->d = *d; j->seq = seq;
    /* The job is run by worker 0. Normally it sits between two barriers. The
     * common case needs none: a 32-bit CSM1 CLUT at the very start of the
     * frame buffer the current batch draws to is that buffer's pixels (0..15,
     * 0..15); those rows are stripes 0 and 1, so worker 0 is the only thread that
     * writes them in this batch, and it reaches the job exactly when it has
     * drawn everything submitted before it. (Later primitives for another
     * target start a new batch, i.e. come after a barrier.) */
    u32 fbp = (u32)(batch_key & 0xFFFF), fpsm = (u32)(batch_key >> 24) & 63;
    int inband = batch_n && d->csm == 0 && d->cpsm == PSMCT32 && d->cbp == fbp && (fpsm == PSMCT32 || fpsm == PSMCT24) &&
                 stripe_owner[0] == 0 && stripe_owner[1] == 0 && !ps_test(&batch_z, (d->cbp >> 5) & 511);
    if (!inband) q_barrier();
    else ps_set(&batch_f, (d->cbp >> 5) & 511);       /* so that using the page as depth later in this batch starts a new one */
    Prim *p = q_new(); p->type = PT_CLUT; p->clut = j; q_commit();
    if (!inband) { batch_n = 1; q_barrier(); }
    return j;
}

static unsigned worker_csr;            /* the calling thread's MXCSR (rounding, FTZ/DAZ): workers must compute alike */
static void *worker_main(void *arg) {
    Worker *w = arg; u64 cur = 0;
    _mm_setcsr(worker_csr);
    for (;;) {
        u64 head = atomic_load_explicit(&q_head, memory_order_acquire);
        if (head == cur) {
            /* Idle. Poll for a while before sleeping: during a frame work
             * arrives in bursts microseconds apart, and a sleeping thread
             * takes far longer to wake than a burst takes to draw. */
            int spins = 0; u64 t0 = gs_timing ? __builtin_ia32_rdtsc() : 0;
            while (atomic_load_explicit(&q_head, memory_order_acquire) == cur) {
                if (++spins < idle_spins) { __builtin_ia32_pause(); continue; }
                pthread_mutex_lock(&q_m);
                atomic_fetch_add(&q_sleepers, 1);
                while (atomic_load(&q_head) == cur) pthread_cond_wait(&q_cv, &q_m);
                atomic_fetch_sub(&q_sleepers, 1);
                pthread_mutex_unlock(&q_m);
            }
            if (gs_timing) w->t_idle += __builtin_ia32_rdtsc() - t0;
            continue;
        }
        u64 t1 = gs_timing ? __builtin_ia32_rdtsc() : 0, tw = 0;
        const Rows rows = { stripe_owner, (u8)w->idx };
        for (; cur < head; cur++) {
            const Prim *p = &queue[cur & (QCAP - 1)];
            if (p->type == PT_BARRIER) {
                u64 t2 = gs_timing ? __builtin_ia32_rdtsc() : 0;
                atomic_store_explicit(&w->pos, cur, memory_order_release);
                for (int i = 0; i < nthreads; i++) {
                    int spins = 0;
                    while (atomic_load_explicit(&workers[i].pos, memory_order_acquire) < cur) {
                        if (++spins < 2000) __builtin_ia32_pause(); else { sched_yield(); spins = 0; }
                    }
                }
                if (gs_timing) tw += __builtin_ia32_rdtsc() - t2;
            } else if (p->type == PT_DECODE) {
                u32 h = 1u << p->dec.d.th;
                tex_decode_rows(&p->dec.d, 0, p->dec.px, h * (u32)w->idx / (u32)nthreads, h * ((u32)w->idx + 1) / (u32)nthreads);
            } else if (p->type == PT_CLUT) {
                if (w->idx == 0) clut_read(&p->clut->d, p->clut->val);       /* between barriers, like a decode */
            } else {
                int mine = 0;
                for (s32 sp = p->py0 >> 3; sp <= p->py1 >> 3 && !mine; sp++) mine = stripe_owner[sp] == rows.me;
                if (mine) rast_prim(p, rows, 0);
            }
        }
        if (gs_timing) { w->t_wait += tw; w->t_work += __builtin_ia32_rdtsc() - t1 - tw; }
        atomic_store_explicit(&w->pos, cur, memory_order_release);
    }
    return 0;
}

/* Wait until everything queued has been drawn. Afterwards local memory is
 * exactly what serial rendering would have produced and the main thread may
 * use all of it. */
static void gs_sync(int why) {
#ifdef GS_HW
    if (hw_on) { hwg_sync(); return; }
#endif
    if (!nthreads || q_tail == q_base) return;
    q_publish();
    u64 t0 = gs_timing ? __builtin_ia32_rdtsc() : 0;
    for (int i = 0; i < nthreads; i++) {
        int spins = 0;
        while (atomic_load_explicit(&workers[i].pos, memory_order_acquire) != q_tail) {
            if (++spins < 20000) __builtin_ia32_pause(); else { sched_yield(); spins = 0; }
        }
    }
    if (gs_timing) t_sync_wait += __builtin_ia32_rdtsc() - t0;
    for (int i = 0; i < tex_ndefer; i++) free(tex_defer[i]);
    tex_ndefer = 0;
    clut_apply_jobs();
    ps_clear(&pend_dirty); ps_clear(&batch_f); ps_clear(&batch_z); batch_n = 0;
    nstates = 0; cur_st = 0; state_dirty = 1;       /* the state is rebuilt, which also re-pins its texture */
    q_base = q_tail; sync_gen++; stat_sync[why]++;
}

static void pool_init(void) {
    const char *e = getenv("ROR_GS_THREADS");
    /* Default: a third of the logical CPUs. More is slower in the game: the
     * workers then share physical cores with the thread that runs the game. */
    long n = e ? atol(e) : sysconf(_SC_NPROCESSORS_ONLN) / 3;
    if (!e && n > 12) n = 12;
#ifdef GS_HW
    if (hw_on) n = 0;                   /* the GPU draws; nothing is queued for workers */
#endif
    if (n > MAXT) n = MAXT;
    if (n < 0) n = 0;
    states = malloc(sizeof(DState) * MAXSTATES);
    queue = malloc(sizeof(Prim) * QCAP);
    /* Rows 0..15 go to worker 0 (q_clut relies on it); the others are dealt
     * round-robin, with worker 0 sitting out one round to make up for it. */
    for (int sidx = 0; sidx < 256; sidx++) {
        long k = sidx - 2;
        stripe_owner[sidx] = (u8)(sidx < 2 || n < 2 ? 0 : k < 2 * (n - 1) ? 1 + k % (n - 1) : (k - 2 * (n - 1)) % n);
    }
    nthreads = (int)n;                  /* fixed before any worker starts */
    worker_csr = _mm_getcsr();
    for (long i = 0; i < n; i++) {
        pthread_t t;
        workers[i].idx = (int)i;
        if (pthread_create(&t, 0, worker_main, &workers[i]) != 0) { rt_log("[gs] cannot start render threads\n"); exit(1); }
    }
}

/* Debug: ROR_GSDUMP=<frame> logs that frame's draw calls, grouped by state (ROR_GSDUMP_ALL=1: with every vertex). */
static void dump_prim(const DState *st, int type) {
    static u64 last_key[6]; static u32 run; static s32 bx0, by0, bx1, by1; static u32 zmin, zmax;
    const Ctxt *c = &st->c;
    u64 key[6] = { r_prim, c->tex0, c->test, c->alpha, c->frame, c->zbuf };
    int n = type == 0 ? 1 : (type == 1 || type == 2 || type == 6) ? 2 : 3;
    if (memcmp(key, last_key, sizeof key) != 0) {
        if (st->tme) {                                   /* save the texture once (RGB, then alpha) */
            static u32 saved[64]; static int ns; int seen = 0;
            for (int i = 0; i < ns; i++) if (saved[i] == c->tbp) seen = 1;
            if (!seen && ns < 64) {
                saved[ns++] = c->tbp;
                gs_sync(SY_SNAP);                        /* the snapshot may still be being decoded */
                int w = 1 << c->tw, h = 1 << c->th;
                u32 *px = malloc((size_t)w * 2 * (size_t)h * 4);
                for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                    u32 t = st->tex[((size_t)y << c->tw) + (size_t)x], a8 = t >> 24; a8 = a8 > 127 ? 255 : a8 * 2;
                    px[y * w * 2 + x] = ((t & 0xFF) << 16) | (t & 0xFF00) | ((t >> 16) & 0xFF);
                    px[y * w * 2 + w + x] = a8 * 0x010101u;
                }
                char path[96]; snprintf(path, sizeof path, "build/recomp/shots/tex_%04x.png", c->tbp);
                void save_png(const char *, const u32 *, int, int);
                save_png(path, px, w * 2, h); free(px);
            }
        }
        if (run) rt_log("       x%u  bbox (%d,%d)-(%d,%d) z %x..%x\n", run, bx0 >> 4, by0 >> 4, bx1 >> 4, by1 >> 4, zmin, zmax);
        memcpy(last_key, key, sizeof key); run = 0; bx0 = by0 = 1 << 30; bx1 = by1 = -(1 << 30); zmin = ~0u; zmax = 0;
        rt_log("[gsdump] prim=%d iip=%d tme=%d fge=%d abe=%d fst=%d ctx=%d | tex bp=%x bw=%u psm=%x %ux%u tcc=%u tfx=%u cbp=%x | "
               "test ate=%d atst=%d aref=%x afail=%d date=%d zte=%d ztst=%d | alpha %u%u%u%u fix=%x | fb bp=%x bw=%u psm=%x msk=%x | z bp=%x psm=%x msk=%u\n",
               type, st->iip, st->tme, st->fge, st->abe, st->fst, (int)((r_prim >> 9) & 1), c->tbp, c->tbw, c->tpsm, 1u << c->tw, 1u << c->th,
               c->tcc, c->tfx, c->cbp, st->ate, st->atst, st->aref, st->afail, st->date, st->zte, st->ztst, st->aa, st->ab, st->ac, st->ad, st->fix,
               c->fbp, c->fbw, c->fpsm, c->fbmsk, c->zbp, c->zpsm, c->zmsk);
    }
    static int all = -1; if (all < 0) all = getenv("ROR_GSDUMP_ALL") != 0;      /* every primitive instead of the first two of a group */
    if (run < 2 || all) for (int i = 0; i < n; i++)
        rt_log("       v%d xy=(%.1f,%.1f) z=%x rgba=%02x%02x%02x%02x stq=(%g,%g,%g) uv=(%u,%u)\n", i, vq[i].x / 16.0, vq[i].y / 16.0, vq[i].z,
               vq[i].r, vq[i].g, vq[i].b, vq[i].a, vq[i].s, vq[i].t, vq[i].q, vq[i].u, vq[i].v);
    run++;
    for (int i = 0; i < n; i++) {
        if (vq[i].x < bx0) bx0 = vq[i].x; if (vq[i].x > bx1) bx1 = vq[i].x;
        if (vq[i].y < by0) by0 = vq[i].y; if (vq[i].y > by1) by1 = vq[i].y;
        if (vq[i].z < zmin) zmin = vq[i].z; if (vq[i].z > zmax) zmax = vq[i].z;
    }
}

/* ---- primitive submission (main thread) ---------------------------------------------------- */
static void submit1(int type);
static int film_grain = 100;         /* --film-grain 0..100: strength of the game's film grain, 100 = as the game draws it */
void gs_film_grain(int pct) { film_grain = pct < 0 ? 0 : pct > 100 ? 100 : pct; }
static int want_texfilter = -1;      /* --texture-filter: 0 original, 1 smooth, -1 auto (smooth on a discrete graphics card from --internal 2) */
void gs_texture_filter(int mode) { want_texfilter = mode; }
/* Widescreen (rt/wide.c): while widened 3D is being drawn, everything that
 * does not come from VU1 (PATH2/PATH3: captions, text, HUD, 2D backgrounds)
 * and goes into a display buffer is squeezed around the screen centre by
 * (4/3) / (wide aspect), so that it keeps its proportions when the picture is
 * shown stretched. Primitives that span the whole width (clears, fades,
 * letterbox bars, full-screen feedback and grain passes) are left alone. */
int wide_squeeze(u32 fbp, float *inv);
static void submit(int type) {
    extern int g_gif_path, g_wide_want_p1; void wide_path1(u32 fbp);
    u64 pr = r_prmodecont ? r_prim : ((r_prim & 7) | (r_prmode & ~7ull));
    const Ctxt *c = &ctxt[(pr >> 9) & 1]; float inv;
    /* --film-grain: the game's film grain is a handful of full-screen sprites per
     * frame (PATH2/3) that tile a 64x64 32-bit noise texture over the picture,
     * blended with a constant: (Cs - Cd) * FIX + Cd and (Cd - Cs) * FIX + Cd,
     * FIX = 15 in the first scene. Below 100 the constant is scaled; 0 drops
     * the sprites. Under --wide their texture coordinates are widened (below). */
    if (type == 6 && g_gif_path != GIF_PATH1 && (pr & 0x10) && (pr & 0x40) && (pr & 0x100) &&
        c->tpsm == PSMCT32 && c->tw == 6 && c->th == 6 && ((c->alpha >> 4) & 3) == 2 && ((c->alpha >> 6) & 3) == 1) {
        s32 x0 = vq[0].x < vq[1].x ? vq[0].x : vq[1].x, x1 = vq[0].x < vq[1].x ? vq[1].x : vq[0].x, W = (s32)c->fbw * 64 * 16;
        s32 du = (s32)vq[1].u - (s32)vq[0].u; if (du < 0) du = -du;
        int widen = wide_squeeze(c->fbp, &inv);
        if (x0 <= 16 && x1 >= W - 16 && du > 64 * 16 && (film_grain != 100 || widen)) {      /* full width, and the texture repeats across it */
            if (film_grain != 100) { static int logged; if (!logged) { logged = 1; rt_log("[gs] film grain: %d%% (noise texture at %x, blend constant %u)\n", film_grain, c->tbp, (unsigned)((c->alpha >> 32) & 255)); } }
            if (film_grain <= 0) return;
            u64 keep = c->alpha, fix = (keep >> 32) & 255; u32 keep_u = vq[1].u;
            /* --wide shows the picture stretched: the grain is repeated more often
             * across it instead, so that its specks keep their shape */
            if (widen) vq[1].u = (u32)((s32)vq[0].u + (s32)lrintf((float)((s32)vq[1].u - (s32)vq[0].u) / inv));
            if (film_grain != 100) { ((Ctxt *)c)->alpha = (keep & ~(0xFFull << 32)) | ((u64)((fix * (u64)film_grain + 50) / 100) << 32); state_dirty = 1; }
            submit1(type);
            vq[1].u = keep_u;
            if (film_grain != 100) { ((Ctxt *)c)->alpha = keep; state_dirty = 1; }
            return;
        }
    }
    if (g_gif_path == GIF_PATH1) { if (g_wide_want_p1) wide_path1(c->fbp); }
    else if (wide_squeeze(c->fbp, &inv)) {
        int n = type == 0 ? 1 : (type == 1 || type == 2 || type == 6) ? 2 : 3;
        s32 lo = vq[0].x, hi = vq[0].x, W = (s32)c->fbw * 64 * 16;
        for (int i = 1; i < n; i++) { if (vq[i].x < lo) lo = vq[i].x; if (vq[i].x > hi) hi = vq[i].x; }
        if (!(lo <= 16 && hi >= W - 16)) {
            Vtx sv[3]; memcpy(sv, vq, sizeof sv);              /* strips and fans keep vertices in vq */
            for (int i = 0; i < n; i++) vq[i].x = W / 2 + (s32)lrintf((float)(vq[i].x - W / 2) * inv);
            submit1(type);
            memcpy(vq, sv, sizeof sv);
            return;
        }
    }
    submit1(type);
}
static void submit1(int type) {
    u64 pr = r_prmodecont ? r_prim : ((r_prim & 7) | (r_prmode & ~7ull));
    for (;;) {
        if (nthreads && (q_tail - q_base >= QCAP - 8 || tex_ndefer > 64)) gs_sync(SY_QFULL);
        if (state_dirty || !cur_st || (pr >> 3) != cur_pr) {
            if (nstates == MAXSTATES) { if (nthreads) gs_sync(SY_STATES); nstates = 0; }
            static DState tmp;                    /* built aside: building may gs_sync, which recycles `states` */
            build_state(&tmp, pr);
            DState *s = &states[nstates++];
            *s = tmp; cur_st = s; cur_pr = pr >> 3; state_dirty = 0; stat_states++;
#ifdef GS_HW
            hw_state_gen++;
#endif
        }
        const DState *st = cur_st; const Ctxt *c = &st->c;
        if (gs_dump_frame && g_gs_frames == gs_dump_frame) dump_prim(st, type);
        if (st->never) return;
        if (!cur_st) continue;                    /* the dump synced */
        Prim lp, *p = &lp;                        /* copied into the queue once its place there is settled */
        s64 pixels = -1;
        p->st = st;
#ifdef GS_HW
        if (hw_on) {            /* every primitive goes to the GPU, culled or not: the retained draw list must not depend on culling */
            p->type = type == 0 ? PT_POINT : type < 3 ? PT_LINE : type < 6 ? PT_TRI : PT_SPRITE;
            p->v[0] = vq[0]; p->v[1] = vq[1]; p->v[2] = vq[2];
            hwg_prim(st, p);
            return;
        }
#endif
        switch (type) {
        case 0: {
            s32 x = (vq[0].x + 8) >> 4, y = (vq[0].y + 8) >> 4;
            if (x < c->sx0 || x > c->sx1 || y < c->sy0 || y > c->sy1) return;
            p->type = PT_POINT; p->v[0] = vq[0]; p->px0 = p->px1 = x; p->py0 = p->py1 = y;
            break;
        }
        case 1: case 2: {                         /* generous extent: every pixel the stepping can reach */
            s32 x0 = vq[0].x < vq[1].x ? vq[0].x : vq[1].x, x1 = vq[0].x < vq[1].x ? vq[1].x : vq[0].x;
            s32 y0 = vq[0].y < vq[1].y ? vq[0].y : vq[1].y, y1 = vq[0].y < vq[1].y ? vq[1].y : vq[0].y;
            p->px0 = ((x0 + 8) >> 4) - 1; p->px1 = ((x1 + 8) >> 4) + 1; p->py0 = ((y0 + 8) >> 4) - 1; p->py1 = ((y1 + 8) >> 4) + 1;
            if (p->px0 < c->sx0) p->px0 = c->sx0;
            if (p->py0 < c->sy0) p->py0 = c->sy0;
            if (p->px1 > c->sx1) p->px1 = c->sx1;
            if (p->py1 > c->sy1) p->py1 = c->sy1;
            if (p->px0 > p->px1 || p->py0 > p->py1) return;
            p->type = PT_LINE; p->v[0] = vq[0]; p->v[1] = vq[1];
            break;
        }
        case 3: case 4: case 5:
            if (!tri_extent(st, &vq[0], &vq[1], &vq[2], &p->px0, &p->px1, &p->py0, &p->py1)) return;
            p->type = PT_TRI; p->v[0] = vq[0]; p->v[1] = vq[1]; p->v[2] = vq[2];
            pixels = (s64)(p->px1 - p->px0 + 1) * (p->py1 - p->py0 + 1) / 2;
            break;
        case 6: {
            SpriteSetup j;
            if (!sprite_setup(st, &vq[0], &vq[1], &j)) return;
            p->type = PT_SPRITE; p->v[0] = vq[0]; p->v[1] = vq[1];
            p->px0 = j.px0; p->px1 = j.px1 - 1; p->py0 = j.py0; p->py1 = j.py1 - 1;
            pixels = (s64)(j.px1 - j.px0) * (j.py1 - j.py0);
            break;
        }
        default: return;
        }
        /* Pages the primitive can touch. Frame and depth buffer start on page
         * boundaries; a page holds 64 columns and 32 (32-bit) or 64 (16-bit) rows. */
        const u32 bw = c->fbw, fp0 = c->fbp >> 5, zp0 = c->zbp >> 5;
        const u32 fsh = (c->fpsm == PSMCT16 || c->fpsm == PSMCT16S) ? 6 : 5, zsh = st->fs.zysh;
        const u32 c0 = (u32)p->px0 >> 6, c1 = (u32)p->px1 >> 6;
        const u32 fr0 = (u32)p->py0 >> fsh, fr1 = (u32)p->py1 >> fsh, zr0 = (u32)p->py0 >> zsh, zr1 = (u32)p->py1 >> zsh;
        int serial = st->serial;
        if (c1 >= bw || fp0 + fr1 * bw + c1 > 511) serial = 1;            /* columns or rows alias other pixels */
        if (st->touch_z) {
            if (zp0 + zr1 * bw + c1 > 511) serial = 1;
            else if (fp0 + fr0 * bw + c0 <= zp0 + zr1 * bw + c1 && zp0 + zr0 * bw + c0 <= fp0 + fr1 * bw + c1) serial = 1;   /* frame and depth overlap */
        }
        if (serial) {
            if (q_pending()) { gs_sync(SY_SERIAL); continue; }           /* state and queue slot are stale: start over */
            rast_prim(p, (Rows){ 0, 0 }, 1);
            state_dirty = 1; stat_serial++;
            return;
        }
        if (pixels >= 0) { int k = pixels < 2500 ? 0 : pixels < 60000 ? 1 : 2; g_gs_stat[k] += (u64)pixels; g_gs_stat[3 + k]++; }
        int barrier = batch_n && st->tkey != batch_key, texov = 0;
        for (u32 r = fr0; r <= fr1; r++) for (u32 cc = c0; cc <= c1; cc++) {
            u32 pg = fp0 + r * bw + cc;
            barrier |= ps_test(&batch_z, pg);
            texov |= ps_test(&st->texmask, pg);
            page_gen[pg]++;
        }
        if (st->touch_z) for (u32 r = zr0; r <= zr1; r++) for (u32 cc = c0; cc <= c1; cc++) {
            u32 pg = zp0 + r * bw + cc;
            barrier |= ps_test(&batch_f, pg);
            texov |= ps_test(&st->texmask, pg);
            page_gen[pg]++;
        }
        if (texov) state_dirty = 1;               /* drew into its own texture: the next primitive needs a new snapshot */
        if (!nthreads) { rast_prim(p, (Rows){ 0, 0 }, 0); return; }
        if (barrier) q_barrier();
        for (u32 r = fr0; r <= fr1; r++) for (u32 cc = c0; cc <= c1; cc++) {
            u32 pg = fp0 + r * bw + cc;
            ps_set(&batch_f, pg); ps_set(&pend_dirty, pg);
        }
        if (st->touch_z) for (u32 r = zr0; r <= zr1; r++) for (u32 cc = c0; cc <= c1; cc++) {
            u32 pg = zp0 + r * bw + cc;
            ps_set(&batch_z, pg); ps_set(&pend_dirty, pg);
        }
        batch_key = st->tkey; batch_n++;
        *q_new() = lp;
        q_commit();
        return;
    }
}

/* ---- vertex queue ------------------------------------------------------------- */
static void vertex_kick(u32 x, u32 y, u32 z, u32 fog, int draw) {
    u64 pr = r_prmodecont ? r_prim : ((r_prim & 7) | (r_prmode & ~7ull));
    const Ctxt *c = &ctxt[(pr >> 9) & 1];
    Vtx v;
    v.x = (s32)x - c->ofx; v.y = (s32)y - c->ofy; v.z = z;
    v.r = r_rgbaq & 255; v.g = (r_rgbaq >> 8) & 255; v.b = (r_rgbaq >> 16) & 255; v.a = r_rgbaq >> 24;
    v.q = r_q; v.s = r_s; v.t = r_t; v.u = r_u; v.v = r_v; v.f = (u8)fog;
    int type = r_prim & 7;
    static const u8 need[8] = { 1, 2, 2, 3, 3, 3, 2, 255 };
    if (vcount < 3) vq[vcount++] = v;
    else { if (type == 5) { vq[1] = vq[2]; vq[2] = v; } else { vq[0] = vq[1]; vq[1] = vq[2]; vq[2] = v; } }
    if (vcount < need[type]) return;
    if (draw) { g_gs_prims++; submit(type); }
    switch (type) {            /* what stays in the queue for the next primitive */
    case 0: case 1: case 3: case 6: vcount = 0; break;
    case 2: vq[0] = vq[1]; vcount = 1; break;
    case 4: vq[0] = vq[1]; vq[1] = vq[2]; vcount = 2; break;
    case 5: vq[1] = vq[2]; vcount = 2; break;
    }
}

/* ---- hardware renderer -----------------------------------------------------------------
 * Compiled only with -DGS_HW (the replay tool for now); enabled at run time by
 * ROR_GS_HW=<scale>. gs.c keeps doing everything about registers, local memory
 * and the texture cache; instead of rasterizing, primitives go to the Vulkan
 * layer (hw/gs_hw.c) with the state translated to an HwState.
 *
 * Who owns the pixels:
 *  - A frame buffer that is drawn to becomes a render target (HRt): a GPU
 *    image of FBW*64 x 512 native pixels times the scale. From then on the GPU
 *    image is the truth for it (gpu_dirty) and the copy in vram[] is stale.
 *  - Host uploads into a target's area with the target's own layout (the
 *    movies do this) are written to vram[] as always and remembered as a
 *    rectangle (cx0..cy1) that is replayed into the image, as N x N blocks,
 *    before the target is next used.
 *  - Anything else that needs the memory of a dirty target on the CPU side
 *    (a texture or CLUT with another layout, a foreign upload over it, a
 *    local copy, a snapshot) first downloads the target into vram[]
 *    (hwg_rt_download: waits for the GPU).
 *  - A texture with a target's address, width and a 32-bit format is the
 *    target itself, sampled at device resolution; no pixels move.
 *  - A 32-bit CSM1 CLUT load from a dirty target (the game "loads" one from
 *    the top of the frame buffer several times per frame) queues a 16x16 copy
 *    into a buffer and marks the CLUT groups pending, exactly like the
 *    software renderer's queued load; the values are collected after the
 *    frame's submit. Only a texture that really needs those entries earlier
 *    forces a wait (it does not happen in the captures).
 *  - Depth lives only on the GPU; depth memory in vram[] is never updated. */
#ifdef GS_HW
#include "hw/gs_hw.h"
static void rect_pages(PageSet *out, u32 psm, u32 bp, u32 bw, u32 x0, u32 y0, u32 w, u32 h);
static void xfer_bump(u32 psm, u32 bp, u32 bw, u32 x0, u32 x1, u32 y);
static PageSet tx_pages;
typedef struct HRt_ { int used, hw, rows, hrows, gpu_dirty, cx0, cy0, cx1, cy1, tail_for; u32 fbp, fbw, tail_word; } HRt;
typedef struct { int hw, hrows; u32 zbp, fbw, psm, clear_z; int uniform; } HDs;    /* uniform: every pixel holds clear_z (a full-target sprite wrote it, nothing since) */
#define HW_MAXRT 16
static HRt hrt[HW_MAXRT]; static HDs hds[HW_MAXRT]; static int hw_nds;
static int hw_tx_rt = -1, hw_tx_x, hw_tx_y, hw_tx_w, hw_tx_h;
static int hw_clut_ticket[64];
static u32 *hw_frame_px; static size_t hw_frame_cap;
static u64 hw_stat_downloads, hw_stat_lines;

static void tex_hw_forget(TexEnt *e) { if (e->hw_valid) { hw_tex_destroy(e->hwtex); e->hw_valid = 0; hwi_texgone++; } }

/* ---- frame interpolation prototype (ROR_HW_INTERP=n[,first[,count]]) ------------------------
 * The draws of every logic frame are retained (state + vertices per
 * primitive). At each display flip the new frame's list is matched against
 * the previous one, batch by batch in submission order (a batch is a run of
 * primitives with the same state; target identity is left out because the two
 * frame buffers alternate), and n in-between pictures are produced by putting
 * the render targets back to what they were when the frame started and
 * drawing the new list with vertices blended towards the previous frame's.
 * Primitives without a partner, and batches in which a vertex moves more than
 * hwi_snap_px pixels, are drawn at their new position (snap). No in-betweens at all
 * when less than 70% of the vertices have a partner (cut) or when the frame
 * uploaded pixels into a target or dropped a texture (movies; not replayable
 * from the draw list alone). */
typedef struct { u64 key; int n; HwState h; HwVertex v[6]; u8 path; } RPrim;      /* path: GIF path the primitive came by (PATH1 = VU1 = 3D) */
static RPrim *hwi_cur, *hwi_prev; static int hwi_ncur, hwi_nprev, hwi_ccur, hwi_cprev;
static int hw_interp_first, hw_interp_count = 1 << 30, hwi_frame, hwi_have_start; static u32 hwi_last_fbp = ~0u;
void save_png(const char *path, const u32 *px, int w, int h);
static void hwi_record(const struct HwState_ *h, const struct HwVertex_ *v, int n) {
    if (hwi_ncur == hwi_ccur) { hwi_ccur = hwi_ccur ? hwi_ccur * 2 : 4096; hwi_cur = realloc(hwi_cur, sizeof(RPrim) * (size_t)hwi_ccur); }
    RPrim *p = &hwi_cur[hwi_ncur++];
    /* identity for matching between frames (hwi_state_key, made with the
     * state): the state without what alternates or animates (targets, the
     * blend constant, fog colour, alpha reference) and, for a texture, its
     * TEX0 address and format rather than the GPU texture (a palette
     * animation makes a new one every frame) */
    p->key = hwi_state_key ^ (u64)n; p->n = n; p->h = *h; memcpy(p->v, v, sizeof(HwVertex) * (size_t)n);
    { extern int g_gif_path; p->path = (u8)g_gif_path; }
}
/* Identity of a primitive inside its mesh: its texture coordinates (they stay
 * with the triangle whatever order the game draws the mesh in). */
static u64 hwi_uvhash(const RPrim *p) {
    u64 h = 1469598103934665603ull;
    for (int q = 0; q < p->n; q++) {
        float w = p->v[q].q != 0.0f ? p->v[q].q : 1.0f;
        s32 a = (s32)lrintf(p->v[q].s / w * 4096.0f), b = (s32)lrintf(p->v[q].t / w * 4096.0f);
        h = (h ^ (u32)a) * 1099511628211ull; h = (h ^ (u32)b) * 1099511628211ull;
    }
    return h;
}
static int hwi_moved(const RPrim *c, const RPrim *p) {
    for (int q = 0; q < c->n; q++) {
        float dx = c->v[q].x - p->v[q].x, dy = c->v[q].y - p->v[q].y;
        if (dx * dx + dy * dy > hwi_snap_px * hwi_snap_px) return 1;
    }
    return 0;
}
/* A batch whose in-order partners jump about (the game drew the mesh in
 * another order, or moved triangles to another batch): pair its primitives
 * with the previous frame's primitives of the same state by texture
 * coordinates instead. Returns the number of vertices that found a partner
 * that has not moved further than the limit. */
static int hwi_check(const RPrim *c, const RPrim *p);
static RPrim *hwi_syn; static int hwi_nsyn, hwi_csyn;       /* previous positions made up for primitives without a partner: partner[i] = -2 - index */
static int hwi_strict = -1, hwi_stats;                       /* ROR_HW_INTERP_CHECK=0: pairs are not validated (as before 2026-11-24); ROR_HW_INTERP_STATS=1: log */
/* The previous frame's primitives grouped by state, in list order (made once per frame). */
static struct { u64 key; int start, n, fill; } *hwi_kt; static int hwi_ktn, *hwi_kidx, *hwi_kbno, hwi_kcap;   /* kbno: number of the batch a primitive is in */
static int hwi_kslot(u64 key) {
    int sl = (int)((key * 0x9E3779B97F4A7C15ull) >> 40) & (hwi_ktn - 1);
    while (hwi_kt[sl].n >= 0 && hwi_kt[sl].key != key) sl = (sl + 1) & (hwi_ktn - 1);
    return sl;
}
static void hwi_kbuild(void) {
    int tn = 256, at = 0; while (tn < hwi_nprev * 2) tn *= 2;
    if (tn != hwi_ktn) { hwi_ktn = tn; hwi_kt = realloc(hwi_kt, sizeof *hwi_kt * (size_t)tn); }
    if (hwi_kcap < hwi_nprev + 1) { hwi_kcap = hwi_nprev + 1024; hwi_kidx = realloc(hwi_kidx, sizeof(int) * (size_t)hwi_kcap); hwi_kbno = realloc(hwi_kbno, sizeof(int) * (size_t)hwi_kcap); }
    for (int k = 0; k < tn; k++) hwi_kt[k].n = -1;
    for (int q = 0; q < hwi_nprev; q++) {
        int sl = hwi_kslot(hwi_prev[q].key);
        if (hwi_kt[sl].n < 0) { hwi_kt[sl].key = hwi_prev[q].key; hwi_kt[sl].n = 0; hwi_kt[sl].start = -1; }
        hwi_kt[sl].n++;
    }
    for (int q = 0; q < hwi_nprev; q++) {
        int sl = hwi_kslot(hwi_prev[q].key);
        if (hwi_kt[sl].start < 0) { hwi_kt[sl].start = at; at += hwi_kt[sl].n; hwi_kt[sl].fill = 0; }
        hwi_kidx[hwi_kt[sl].start + hwi_kt[sl].fill++] = q;
        hwi_kbno[q] = q ? hwi_kbno[q - 1] + (hwi_prev[q].key != hwi_prev[q - 1].key) : 0;
    }
}
static long hwi_rematch(int i, int e, int *partner) {
    u64 key = hwi_cur[i].key; long nm = 0;
    int ks = hwi_kslot(key), np = hwi_kt[ks].n > 0 ? hwi_kt[ks].n : 0; const int *idx = hwi_kidx + (np ? hwi_kt[ks].start : 0);
    int tn = 16; while (tn < np * 2) tn *= 2;
    int *tab = malloc(sizeof(int) * (size_t)tn), *next = malloc(sizeof(int) * (size_t)(np + 1)); u64 *hv = malloc(sizeof(u64) * (size_t)(np + 1));
    for (int k = 0; k < tn; k++) tab[k] = -1;
    for (int k = np - 1; k >= 0; k--) {              /* chains in list order */
        hv[k] = hwi_uvhash(&hwi_prev[idx[k]]);
        int slot = (int)(hv[k] >> 40) & (tn - 1);
        next[k] = tab[slot]; tab[slot] = k;
    }
    if (!hwi_strict) {                               /* as before the pairs were validated: the first one in reach */
        for (int k = i; k < e; k++) {
            u64 h = hwi_uvhash(&hwi_cur[k]); int *link = &tab[(int)(h >> 40) & (tn - 1)];
            partner[k] = -1;
            for (; *link >= 0; link = &next[*link]) if (hv[*link] == h && hwi_prev[idx[*link]].n == hwi_cur[k].n && !hwi_moved(&hwi_cur[k], &hwi_prev[idx[*link]])) {
                partner[k] = idx[*link]; nm += hwi_cur[k].n;
                *link = next[*link];                 /* each previous primitive is used once */
                break;
            }
        }
        free(tab); free(next); free(hv);
        return nm;
    }
    /* In-order pairs that pass hwi_check stay. The others look for a previous
     * primitive with their texture coordinates that nobody has. Several
     * primitives of a mesh can have the same coordinates (rows of grass, leaf
     * cards): the nearest is taken, and only when it is clearly nearer than
     * the second nearest; a primitive left alone is placed by its shared
     * vertices later, a wrong partner would drag it across its neighbours. */
    u8 *used = calloc((size_t)np + 1, 1);
    for (int k = i; k < e; k++) {
        if (partner[k] < 0) continue;
        if (hwi_check(&hwi_cur[k], &hwi_prev[partner[k]])) { partner[k] = -1; continue; }
        int lo = 0, hi = np - 1;
        while (lo < hi) { int m = (lo + hi) / 2; if (idx[m] < partner[k]) lo = m + 1; else hi = m; }
        if (np && idx[lo] == partner[k] && !used[lo]) { used[lo] = 1; nm += hwi_cur[k].n; } else partner[k] = -1;
    }
    for (int k = i; k < e; k++) {
        if (partner[k] >= 0) continue;
        u64 h = hwi_uvhash(&hwi_cur[k]); int best = -1; float bestd = 1e30f, second = 1e30f;
        for (int c = tab[(int)(h >> 40) & (tn - 1)], tries = 0; c >= 0 && tries < 64; c = next[c], tries++) {
            const RPrim *pv = &hwi_prev[idx[c]];
            if (used[c] || hv[c] != h || pv->n != hwi_cur[k].n) continue;
            float dx = hwi_cur[k].v[0].x - pv->v[0].x, dy = hwi_cur[k].v[0].y - pv->v[0].y, d = dx * dx + dy * dy;
            if (d >= second || hwi_check(&hwi_cur[k], pv)) continue;
            if (d < bestd) { second = bestd; bestd = d; best = c; } else second = d;
        }
        if (best >= 0 && bestd * 4.0f <= second) { partner[k] = idx[best]; used[best] = 1; nm += hwi_cur[k].n; }
    }
    free(used);
    free(tab); free(next); free(hv);
    return nm;
}
/* ---- frame interpolation in the game: packages and the presenter thread ----------------------
 * At every display flip the game thread publishes a package: the frame's draw
 * list with, for each primitive, its vertices now and in the previous frame
 * (states already pointing at the presenter's own copies of the targets), the
 * slot holding the targets as the frame found them, and the slot holding the
 * finished picture. The presenter thread shows package k over the time span
 * [t_ns, t_ns + dur_ns): it copies the start-of-frame targets into its own
 * set, draws the list with vertices blended by t = elapsed / duration, and
 * presents; at t = 1, and for packages that must not be interpolated (cut,
 * movie upload), it shows the finished picture. t_ns is the game's ideal
 * 50 Hz vblank time plus one vblank, so in-betweens are spaced by the clock,
 * not by when the game thread happened to finish.
 * Hand-over is race-free because: packages and save slots rotate over four
 * buffers and the game thread waits (hwp_using) until the presenter has left
 * the one it is about to reuse; the game thread submits its copies (hw_flush)
 * before a package becomes visible; the presenter only reads shared images. */
typedef struct { HwState h; int n; u8 paired; HwVertex cur[6], prev[6]; u8 fix_prev; } PPrim;
typedef struct Pkg_ { PPrim *p; int n, cap, ok, nrt, src[HW_MAXRT], dst[HW_MAXRT], slot, fslot, pic_w, pic_game, sx, sy, sw, sh, pdx, pdy, pdw, pdh, pww, pwh; u64 t_ns, dur_ns, seq;
                 u8 *cls; int cls_cap, f_ok, f_cut, f_ovl, f_mv, f_mvds, f_ob, f_ow, f_ods; float cam[5]; } Pkg;      /* FSR 3.1: see hwf_classify */
static Pkg hwp_pkg[4]; static u64 hwp_seq, hwp_ideal_ns, hwp_last_ideal; static Pkg *hwp_newest;
static pthread_mutex_t hwp_mx = PTHREAD_MUTEX_INITIALIZER;
static atomic_ullong hwp_using = 1ull << 62;           /* lowest package number the presenter may still touch */
static int hwp_wmap[64], hwp_wds[64], hwp_maps_ready, hw_fps_target;
static struct { int dx, dy, dw, dh, ww, wh, linear, sat64; } hwp_par;
static u64 hwp_stat_frames, hwp_stat_ok, hwp_stat_tot, hwp_stat_matched, hwp_stat_dropped; static float hwp_lead_ms, hwp_pub_flush_ms; static int hw_fps_hz;
static int hwp_w_rt(int hw) {                      /* the presenter's twin of a game target */
    if (hwp_wmap[hw] < 0) for (int i = 0; i < HW_MAXRT; i++) if (hrt[i].used && hrt[i].hw == hw) hwp_wmap[hw] = hw_rt_create((int)hrt[i].fbw * 64, hrt[i].hrows);
    return hwp_wmap[hw];
}
static int hwp_w_ds(int hw) {
    if (hwp_wds[hw] < 0) for (int i = 0; i < hw_nds; i++) if (hds[i].hw == hw) hwp_wds[hw] = hw_ds_create((int)hds[i].fbw * 64, hds[i].hrows);
    return hwp_wds[hw];
}
static void hwf_classify(Pkg *P);
static int hwf_replay, hwf_cut;
static Pkg *hwp_publish(const int *partner, int ok, int pic, int sx, int sy, int sw, int sh) {
    if (!hwp_maps_ready) { for (int i = 0; i < 64; i++) hwp_wmap[i] = hwp_wds[i] = -1; hwp_maps_ready = 1; }
    u64 k = hwp_seq + 1; Pkg *P = &hwp_pkg[k & 3];
    /* Package k reuses the buffer and the "final" slot of package k-4 and
     * writes the "start" slot package k-3 reads. If the presenter is still
     * that far behind, this frame gets no package (the game thread never
     * waits); the next one then cannot be interpolated either, having no
     * start-of-frame copy. */
    if (atomic_load(&hwp_using) + 3 <= k) { hwi_have_start = -1; hwp_stat_dropped++; return 0; }
    hwp_seq = k;
    if (P->cap < hwi_ncur) { P->cap = hwi_ncur + 1024; P->p = realloc(P->p, sizeof(PPrim) * (size_t)P->cap); }
    P->n = hwi_ncur;
    for (int i = 0; i < hwi_ncur; i++) {
        PPrim *q = &P->p[i]; const RPrim *c = &hwi_cur[i];
        q->h = c->h; q->n = c->n; q->paired = 0; q->fix_prev = c->h.fix;
        q->h.rt = hwp_w_rt(c->h.rt); q->h.ds = hwp_w_ds(c->h.ds);
        if (q->h.tex_kind == 2) q->h.tex = hwp_w_rt(c->h.tex);
        memcpy(q->cur, c->v, sizeof(HwVertex) * (size_t)c->n);
        if ((ok || (hw_fsr3 && !hwf_cut)) && partner[i] != -1) {     /* (not ok: the pairs are only used for FSR 3.1 motion vectors) */
            const RPrim *pv = partner[i] >= 0 ? &hwi_prev[partner[i]] : &hwi_syn[-2 - partner[i]];
            q->paired = 1;
            if (c->h.flags & HWF_SPRITE) {         /* a sprite that changed size is another thing: let it jump, do not morph */
                float w0 = c->v[5].x - c->v[0].x, h0 = c->v[5].y - c->v[0].y, w1 = pv->v[5].x - pv->v[0].x, h1 = pv->v[5].y - pv->v[0].y;
                if (fabsf(w0 - w1) > 0.5f || fabsf(h0 - h1) > 0.5f) q->paired = 0;
                /* ... and one that shows another part of its texture (an animation
                 * cell, another glyph) must not slide across the sheet */
                if (fabsf(c->v[0].s - pv->v[0].s) > 0.5f || fabsf(c->v[0].t - pv->v[0].t) > 0.5f ||
                    fabsf(c->v[0].dudx - pv->v[0].dudx) > 0.01f || fabsf(c->v[0].dvdy - pv->v[0].dvdy) > 0.01f) q->paired = 0;
            }
            if (q->paired) { memcpy(q->prev, pv->v, sizeof(HwVertex) * (size_t)c->n); q->fix_prev = pv->h.fix; }
        }
    }
    P->nrt = 0;
    for (int i = 0; i < HW_MAXRT; i++) if (hrt[i].used) { P->src[P->nrt] = hrt[i].hw; P->dst[P->nrt++] = hwp_w_rt(hrt[i].hw); }
    P->slot = (int)((k - 1) & 3); P->fslot = (int)(k & 3);
    P->pic_game = pic; P->pic_w = hwp_w_rt(pic); P->sx = sx; P->sy = sy; P->sw = sw; P->sh = sh; P->ok = ok;
    /* where this picture belongs in the window: with --wide the rectangle is
     * the wide one for 3D frames and 4:3 for the rest, and the package is
     * shown some vblanks after it was made */
    P->pdx = hwp_par.dx; P->pdy = hwp_par.dy; P->pdw = hwp_par.dw; P->pdh = hwp_par.dh; P->pww = hwp_par.ww; P->pwh = hwp_par.wh;
    P->f_ok = 0;
    if (hw_fsr3) hwf_classify(P);
    hw_rt_save(pic, P->fslot, 1);
    hw_keep_generation();
    { u64 f0 = plat_time_ns(); hw_flush(); hwp_pub_flush_ms = (float)(plat_time_ns() - f0) / 1e6f; }      /* everything the package refers to is submitted before it can be seen */
    /* Start late enough that the package is there in time: one vblank after
     * the ideal time at least, more if the game thread has been finishing its
     * frames later than that (slowly forgotten). */
    static u64 lead = 20000000ull;
    {   u64 nowp = plat_time_ns(), late = nowp > hwp_ideal_ns ? nowp - hwp_ideal_ns : 0;
        lead = lead > 20200000ull ? lead - 200000ull : 20000000ull;
        if (late + 4000000ull > lead) lead = late + 4000000ull;
        if (lead > 60000000ull) lead = 60000000ull;
    }
    u64 d = hwp_ideal_ns - hwp_last_ideal;
    P->dur_ns = (hwp_last_ideal && d >= 10000000ull && d <= 100000000ull) ? d : 40000000ull;
    hwp_last_ideal = hwp_ideal_ns;
    P->t_ns = hwp_ideal_ns + lead; hwp_lead_ms = (float)lead / 1e6f;
    pthread_mutex_lock(&hwp_mx); P->seq = k; hwp_newest = P; pthread_mutex_unlock(&hwp_mx);
    for (int i = 0; i < HW_MAXRT; i++) if (hrt[i].used) hw_rt_save(hrt[i].hw, (int)(k & 3), 0);      /* as the next frame will find them */
    return P;
}
/* ---- FSR 3.1 temporal upscaling (--filter fsr3; Windows, AMD FidelityFX DLL) -----------------
 * The game's own targets are drawn as always (they are what the game reads
 * back and feeds into its next frame). The picture that is shown is drawn a
 * second time from the frame's retained draw list (the frame-interpolation
 * package), split in two:
 *  - the scene: everything up to the last VU1 (PATH1) primitive of the
 *    picture, plus later passes that sample a render target (the game's
 *    full-screen soften pass). Drawn at the internal resolution into the
 *    presenter's twin targets, VU1 geometry and textured 2D backdrops moved
 *    by the upscaler's sub-pixel jitter. VU1 triangles are drawn once more
 *    into a motion-vector target (gs.frag's MV variant): motion from the
 *    paired previous vertices, depth, and a reactive value for translucent
 *    fragments.
 *  - the 2D layer: PATH2/PATH3 primitives after the last VU1 primitive (film
 *    grain, fades, letterbox bars, captions, HUD), drawn at 1..4 times the
 *    PS2 resolution into two targets, once over black (B) and once over white
 *    (W). GS blending is affine in the destination, so the layer's effect on
 *    any picture P is P * (W - B) + B; that is applied to the upscaled picture
 *    at output size. Text is therefore never seen by the upscaler.
 * Frames without VU1 geometry in the picture (movies, menus, 2D screens) and
 * frames that cannot be replayed (pixels uploaded into a target) are shown
 * the old way, scaled with FSR 1.0, and the upscaler's history is dropped.
 * With --fps the presenter thread does this for every picture it shows
 * (vertices blended by time; motion is that of the time since the previous
 * picture); without, the game thread does it once per game frame. */
void wide_frustum(float *cam);                     /* rt/wide.c: near, far, vertical field of view, GS depth at near and far */
static int hwf_ovl_scale;
static struct { int w, h, n, rt, ds; } hwf_tg[12]; static int hwf_ntg;      /* targets by size: n = 0 motion vectors, else the 2D layer at scale n (two targets) */
static struct { int dx, dy, dw, dh, ww, wh; } hwf_rect;                    /* where the picture goes in the output (game thread, no presenter) */
static struct { int pic, w, h, valid; } hwf_out[2];                        /* upscaled pictures kept for the game's two display buffers */
static double hwf_last_tau; static int hwf_reset = 1; static u64 hwf_last_ns;
static int hwf_target(int w, int h, int n, int second) {
    for (int i = 0; i < hwf_ntg; i++) if (hwf_tg[i].w == w && hwf_tg[i].h == h && hwf_tg[i].n == n) return second == 2 ? hwf_tg[i].ds : hwf_tg[i].rt + (second == 1);
    if (hwf_ntg == 12 || hw_rt_count() > 26) return -1;
    hwf_tg[hwf_ntg].w = w; hwf_tg[hwf_ntg].h = h; hwf_tg[hwf_ntg].n = n;
    hwf_tg[hwf_ntg].rt = hw_rt_create_n(w, h, n, n == 0);
    if (n) hw_rt_create_n(w, h, n, 0);                              /* the "over white" twin: next index */
    hwf_tg[hwf_ntg].ds = n ? hw_ds_create_n(w, h, n) : hw_ds_create(w, h);
    hwf_ntg++;
    return hwf_target(w, h, n, second);
}
/* Game thread, when a package is made: what each primitive is for the upscaler.
 * cls bits: 1 = 2D layer, 2 = jittered, 4 = motion vectors + depth (opaque),
 * 8 = reactive only (translucent, no depth write), 16 = its alpha-failing fragments are reactive. */
static void hwf_classify(Pkg *P) {
    int pic = P->pic_w, last3d = -1;
    if (P->cls_cap < P->n) { P->cls_cap = P->n + 1024; P->cls = realloc(P->cls, (size_t)P->cls_cap); }
    for (int i = 0; i < P->n; i++) if (hwi_cur[i].path == GIF_PATH1 && P->p[i].h.rt == pic) last3d = i;
    P->f_cut = hwf_cut; P->f_ovl = 0; P->f_mv = -1;
    int dh = hw_fps_on ? P->pdh : hwf_rect.dh;
    if (!hwf_replay || last3d < 0 || dh <= 0 || P->sh <= 0) return;
    for (int i = 0; i < P->n; i++) {
        const HwState *h = &P->p[i].h; int p1 = hwi_cur[i].path == GIF_PATH1, in_pic = h->rt == pic; u8 c = 0;
        if (i > last3d && in_pic && h->tex_kind != 2 && h->ztest <= 1) { c = 1; P->f_ovl = 1; }
        else if (in_pic && i <= last3d && (p1 || h->tex_kind == 1)) c = 2;
        if (p1 && in_pic && P->p[i].n == 3 && !(h->flags & HWF_SPRITE)) {
            if (h->zwrite) c |= 4 | ((h->flags & HWF_ATE) && h->afail == 1 && (h->cmask & 7) ? 16 : 0);
            else if ((h->cmask & 7) || h->need_dst) c |= 8;
        }
        P->cls[i] = c;
    }
    int w = 0, hrows = 0, m = hwf_ovl_scale;
    for (int i = 0; i < HW_MAXRT; i++) if (hrt[i].used && hrt[i].hw == P->pic_game) { w = (int)hrt[i].fbw * 64; hrows = hrt[i].hrows; }
    if (!w || P->sx + P->sw > w || P->sy + P->sh > hrows) return;      /* the list painted something else than a display buffer */
    if (m < 1) { m = (dh + P->sh - 1) / P->sh; if (m > 4) m = 4; if (m < 1) m = 1; }     /* 2D layer scale: enough for the output height, at most 4 */
    P->f_mv = hwf_target(w, hrows, 0, 0); P->f_mvds = hwf_target(w, hrows, 0, 2);
    P->f_ob = hwf_target(w, hrows, m, 0); P->f_ow = hwf_target(w, hrows, m, 1); P->f_ods = hwf_target(w, hrows, m, 2);
    if (P->f_mv < 0 || P->f_ob < 0) { P->f_mv = -1; return; }
    wide_frustum(P->cam);
    P->f_ok = 1;
}
/* A primitive of a package at time t: vertices blended towards the previous frame's, as the presenter does. */
static void hwf_vtx(const PPrim *q, float t, HwState *h, HwVertex *v) {
    *h = q->h;
    if (!q->paired || t >= 1.0f) { memcpy(v, q->cur, sizeof(HwVertex) * (size_t)q->n); return; }
    for (int k = 0; k < q->n; k++) {
        const float *a = (const float *)&q->cur[k], *b = (const float *)&q->prev[k]; float *o = (float *)&v[k];
        for (size_t f = 0; f < sizeof(HwVertex) / sizeof(float); f++) o[f] = b[f] + (a[f] - b[f]) * t;
    }
    if (q->fix_prev != q->h.fix) {
        int fx = (int)lrintf((float)q->fix_prev + ((float)q->h.fix - (float)q->fix_prev) * t);
        h->fix = (u8)fx; if (h->blend >> 8) h->blend = (h->blend & 0xFF) | ((u32)fx << 8);
    }
}
/* One upscaled picture of package P at time t into output slot `slot` (dw x dh). Any thread; 0: not done. */
static int hwf_render(Pkg *P, float t, int slot, int dw, int dh) {
    int N = hw_scale; float jx, jy;
    if (!P->f_ok || !hw_fsr3_begin(P->sw * N, P->sh * N, dw, dh, &jx, &jy)) return 0;
    jx /= (float)N; jy /= (float)N;                     /* native pixels */
    double tau = (double)P->seq + (double)(t < 1.0f ? t : 1.0f);
    int reset = P->f_cut || hwf_reset;
    float mk = reset ? 0.0f : (float)(tau - hwf_last_tau);             /* game frames since the previous upscaled picture */
    if (mk < 0.0f) mk = 0.0f; if (mk > 1.5f) mk = 1.5f;
    static int noreact = -1, nomv; if (noreact < 0) { noreact = getenv("ROR_FSR3_NOREACTIVE") != 0; nomv = getenv("ROR_FSR3_NOMV") != 0; }
    if (nomv) mk = 0.0f;
    HwState h; HwVertex v[6];
    for (int i = 0; i < P->nrt; i++) hw_rt_load(P->dst[i], P->src[i], P->slot, 0);
    /* the scene, jittered */
    for (int i = 0; i < P->n; i++) {
        const PPrim *q = &P->p[i]; u8 c = P->cls[i];
        if (c & 1) continue;
        hwf_vtx(q, t, &h, v);
        if (c & 2) {
            for (int k = 0; k < q->n; k++) { v[k].x += jx; v[k].y += jy; }
            if (h.flags & HWF_SPRITE) {
                /* the texture moves with the sprite; an edge on the scissor's edge is kept covering it (else
                 * the jitter would let the clear colour through along the picture's border) */
                float x0 = v[0].x, y0 = v[0].y, x1 = v[5].x, y1 = v[5].y;
                for (int k = 0; k < 6; k++) { v[k].sx0 += jx; v[k].sy0 += jy; }
                if (x0 - jx <= (float)h.sx0) x0 -= 1.0f; if (y0 - jy <= (float)h.sy0) y0 -= 1.0f;
                if (x1 - jx >= (float)(h.sx1 + 1)) x1 += 1.0f; if (y1 - jy >= (float)(h.sy1 + 1)) y1 += 1.0f;
                v[0].x = v[2].x = v[3].x = x0; v[1].x = v[4].x = v[5].x = x1; v[0].y = v[1].y = v[4].y = y0; v[2].y = v[3].y = v[5].y = y1;
            }
        }
        hw_draw(&h, v, q->n);
    }
    /* motion vectors, depth, reactive values: VU1 triangles of the picture again */
    hw_rt_clear(P->f_mv, 0.0f, 0.0f, 0.0f, 0.0f); hw_ds_clear(P->f_mvds);
    hw_mv_params(P->cam[4], 1.0f / (P->cam[3] - P->cam[4]));
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < P->n; i++) {
            const PPrim *q = &P->p[i]; u8 c = P->cls[i];
            if (pass == 0 ? !(c & 4) : !(c & (8 | 16))) continue;
            hwf_vtx(q, t, &h, v);
            for (int k = 0; k < 3; k++) {
                v[k].x += jx; v[k].y += jy;
                v[k].sx0 = q->paired ? (q->prev[k].x - q->cur[k].x) * mk : 0.0f; v[k].sy0 = q->paired ? (q->prev[k].y - q->cur[k].y) * mk : 0.0f;
                v[k].dudx = v[k].dvdy = 0.0f;
            }
            h.rt = P->f_mv; h.ds = P->f_mvds;
            h.blend_mode = HWB_OFF; h.blend_const = 0; h.fix = 0; h.need_dst = 0; h.fbmsk = 0; h.blend = 0;
            h.flags &= ~(u32)(HWF_SHBLEND | HWF_DATE | HWF_DATM | HWF_FBA | HWF_COLCLAMP | HWF_FGE);
            if (pass == 0) {
                if (h.afail == 2) h.flags &= ~(u32)HWF_ATE;          /* ZB_ONLY: every fragment writes depth */
                h.cmask = 15; h.afail = 0; h.zwrite = 1;
            } else { h.cmask = 4; h.zwrite = 0; h.afail = (c & 16) ? 4 : 5; }
            hw_draw(&h, v, 3);
        }
    /* the 2D layer, over black and over white */
    if (P->f_ovl) {
        hw_rt_clear(P->f_ob, 0.0f, 0.0f, 0.0f, 0.0f); hw_rt_clear(P->f_ow, 1.0f, 1.0f, 1.0f, 0.0f);
        for (int pass = 0; pass < 2; pass++) {
            hw_ds_clear(P->f_ods);
            for (int i = 0; i < P->n; i++) {
                if (!(P->cls[i] & 1)) continue;
                hwf_vtx(&P->p[i], t, &h, v);
                h.rt = pass ? P->f_ow : P->f_ob; h.ds = P->f_ods;
                hw_draw(&h, v, P->p[i].n);
            }
        }
    }
    u64 now = plat_time_ns();
    HwFsr3 f; memset(&f, 0, sizeof f);
    f.color = P->pic_w; f.sx = P->sx; f.sy = P->sy; f.sw = P->sw; f.sh = P->sh; f.mv = P->f_mv;
    f.ovl_b = P->f_ovl ? P->f_ob : -1; f.ovl_w = P->f_ovl ? P->f_ow : -1; f.slot = slot; f.reset = reset; f.no_reactive = noreact;
    f.dt_ms = hw_fps_on && hwf_last_ns ? (float)(now - hwf_last_ns) / 1e6f : 40.0f; if (f.dt_ms > 100.0f) f.dt_ms = 100.0f;
    f.cam_near = P->cam[0]; f.cam_far = P->cam[1]; f.cam_fovy = P->cam[2];
    f.unit_m = 0.025f;               /* estimate: one game unit is about 2.5 cm (near plane 5, far 2000..5000) */
    hwf_last_ns = now;
    if (!hw_fsr3_upscale(&f)) { hw_fsr3 = 0; return 0; }
    {   static int log = -1; static u64 n, nreset; if (log < 0) log = getenv("ROR_FSR3_LOG") ? atoi(getenv("ROR_FSR3_LOG")) : 0;
        n++; nreset += (u64)reset;
        if (log && (n % (u64)log == 0 || reset)) rt_log("[fsr3] vblank %llu: picture %llu (package %llu, t %.2f) into slot %d: motion scale %.2f, jitter %.3f %.3f px, %s2D layer%s; %llu resets so far\n", (unsigned long long)g_gs_frames, (unsigned long long)n,
                    (unsigned long long)P->seq, t, slot, mk, jx * N, jy * N, P->f_ovl ? "" : "no ", reset ? ", history reset" : "", (unsigned long long)nreset); }
    hwf_reset = 0; hwf_last_tau = tau;
    return 1;
}
/* Game thread without a presenter: upscale the frame that was just completed.
 * It is shown from the moment the game displays the buffer it was drawn into. */
static void hwf_boundary(Pkg *P) {
    int pic = P->pic_game, slot = -1;
    for (int s = 0; s < 2; s++) if (hwf_out[s].pic == pic) { hwf_out[s].valid = 0; slot = s; }
    if (slot < 0) slot = !hwf_out[0].valid ? 0 : !hwf_out[1].valid ? 1 : 0;
    if (!P->f_ok || hwf_rect.dw <= 0 || !hwf_render(P, 1.0f, slot, hwf_rect.dw, hwf_rect.dh)) { hwf_reset = 1; return; }
    hwf_out[slot].pic = pic; hwf_out[slot].w = hwf_rect.dw; hwf_out[slot].h = hwf_rect.dh; hwf_out[slot].valid = 1;
}
/* Before a present of game target `hw` by the game thread: use its upscaled picture if there is one. */
static void hwf_show(int hw, int linear, int dw, int dh) {
    if (!hw_fsr3 || linear != 3 || hw_fps_on) return;
    { static int log = -1; if (log < 0) log = getenv("ROR_FSR3_LOG") != 0;
      if (log && (g_gs_frames % 200 == 0)) rt_log("[fsr3] vblank %llu: present of target %d at %dx%d; slots: %d %dx%d valid %d, %d %dx%d valid %d\n", (unsigned long long)g_gs_frames, hw, dw, dh, hwf_out[0].pic, hwf_out[0].w, hwf_out[0].h, hwf_out[0].valid, hwf_out[1].pic, hwf_out[1].w, hwf_out[1].h, hwf_out[1].valid); }
    for (int s = 0; s < 2; s++) if (hwf_out[s].valid && hwf_out[s].pic == hw && hwf_out[s].w == dw && hwf_out[s].h == dh) hw_fsr3_show(s);
}
static int cmp_u64(const void *a, const void *b) { u64 x = *(const u64 *)a, y = *(const u64 *)b; return x < y ? -1 : x > y; }
/* ROR_HW_GRAB=frame,count[,stride] (+ ROR_HW_GRAB_DIR): PNGs of `count`
 * swapchain images presented after that vblank (every stride-th present),
 * exactly as presented. */
static int hwg_grab_from = -2, hwg_grab_n, hwg_grab_i, hwg_grab_stride = 1, hwg_grab_k;
static void hwg_grab_before(void) {
    if (hwg_grab_from == -2) { hwg_grab_from = -1; if (getenv("ROR_HW_GRAB")) sscanf(getenv("ROR_HW_GRAB"), "%d,%d,%d", &hwg_grab_from, &hwg_grab_n, &hwg_grab_stride); }
    if (hwg_grab_from >= 0 && (int)g_gs_frames >= hwg_grab_from && hwg_grab_i < hwg_grab_n && hwg_grab_k++ % (hwg_grab_stride > 0 ? hwg_grab_stride : 1) == 0) hw_grab_request();
}
static void hwg_grab_after(int tag) {
    int w, h, bgra; const u32 *src = hw_grab_result(&w, &h, &bgra);
    if (!src) return;
    u32 *img = malloc((size_t)w * (size_t)h * 4); char path[300]; const char *dir = getenv("ROR_HW_GRAB_DIR");
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) { u32 c = src[i]; img[i] = bgra ? (c & 0xFFFFFF) : (((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF)); }
    snprintf(path, sizeof path, "%s/grab_%03d_v%06llu_t%03d.png", dir ? dir : ".", hwg_grab_i++, (unsigned long long)g_gs_frames, tag);
    save_png(path, img, w, h); free(img);
}
static void *hwp_thread(void *arg) {
    (void)arg;
    _mm_setcsr(worker_csr);
    hw_thread_init();
    Pkg *cur = 0, *next = 0; u64 taken = 0, last_present = 0, due = 0; int shown_final = 0, lw = 0, lh = 0;
    static u64 iv[1024]; int niv = 0; u64 render_ns = 0, presents = 0, t_log = 0;
    int strip_from = -1, strip_n = 0, strip_i = 0; u64 strip_seq = 0; const char *strip_dir = getenv("ROR_HW_STRIP_DIR");
    if (getenv("ROR_HW_STRIP")) sscanf(getenv("ROR_HW_STRIP"), "%d,%d", &strip_from, &strip_n);
    for (;;) {
        int dx, dy, dw, dh, ww, wh, linear, sat64;
        pthread_mutex_lock(&hwp_mx);
        if (hwp_newest && hwp_newest->seq > taken) { next = hwp_newest; taken = next->seq; }
        dx = hwp_par.dx; dy = hwp_par.dy; dw = hwp_par.dw; dh = hwp_par.dh; ww = hwp_par.ww; wh = hwp_par.wh; linear = hwp_par.linear; sat64 = hwp_par.sat64;
        pthread_mutex_unlock(&hwp_mx);
        u64 now = plat_time_ns();
        /* next package: at its time; at once after a cut; and as soon as the current one is finished (it starts at t = 0, the same picture) */
        if (next && (!cur || now >= next->t_ns || !next->ok || shown_final)) { cur = next; next = 0; shown_final = 0; }
        atomic_store(&hwp_using, cur ? cur->seq : 1ull << 62);
        if (!cur || ww <= 0 || wh <= 0) { plat_sleep_until(now + 2000000); continue; }
        if (cur->pww == ww && cur->pwh == wh) { dx = cur->pdx; dy = cur->pdy; dw = cur->pdw; dh = cur->pdh; }   /* the package's own rectangle unless the window changed size since */
        float t = 1.0f;
        if (cur->ok) { t = now <= cur->t_ns ? 0.0f : (float)((double)(now - cur->t_ns) / (double)cur->dur_ns); if (t > 1.0f) t = 1.0f; }
        if (t >= 1.0f && shown_final && ww == lw && wh == lh) { plat_sleep_until(now + 500000); continue; }
        /* Rate limit: --fps N; for the display rate only if presenting does not
         * wait for the display by itself (decided from the first intervals). */
        static int limit = -1; static u64 probe_sum; static int probe_n;
        if (limit < 0) limit = hw_fps_target;
        if (limit > 0) {
            if (due > now) { plat_sleep_until(due); now = plat_time_ns(); }
            due = (due && due + 100000000ull > now ? due : now) + 1000000000ull / (u64)limit;
        }
        int fs = 0;                       /* FSR 3.1: this picture comes from the temporal upscaler */
        if (hw_fsr3 && linear == 3) {
            if (cur->f_ok && dw > 0 && dh > 0) fs = hwf_render(cur, t, 0, dw, dh);
            if (!fs) hwf_reset = 1;
        }
        if (fs) { if (t >= 1.0f) shown_final = 1; }
        else if (t >= 1.0f) { hw_rt_load(cur->pic_w, cur->pic_game, cur->fslot, 1); shown_final = 1; }
        else {
            for (int i = 0; i < cur->nrt; i++) hw_rt_load(cur->dst[i], cur->src[i], cur->slot, 0);
            for (int i = 0; i < cur->n; i++) {
                PPrim *q = &cur->p[i];
                if (!q->paired) { hw_draw(&q->h, q->cur, q->n); continue; }
                HwVertex v[6]; HwState h = q->h;
                for (int k = 0; k < q->n; k++) {
                    const float *a = (const float *)&q->cur[k], *b = (const float *)&q->prev[k]; float *o = (float *)&v[k];
                    for (size_t f = 0; f < sizeof(HwVertex) / sizeof(float); f++) o[f] = b[f] + (a[f] - b[f]) * t;
                }
                if (q->fix_prev != q->h.fix) {     /* a fade by the blend constant */
                    int fx = (int)lrintf((float)q->fix_prev + ((float)q->h.fix - (float)q->fix_prev) * t);
                    h.fix = (u8)fx; if (h.blend >> 8) h.blend = (h.blend & 0xFF) | ((u32)fx << 8);
                }
                hw_draw(&h, v, q->n);
            }
        }
        lw = ww; lh = wh;
        /* ROR_HW_STRIP=frame,count: for the two packages current after that
         * vblank, `count` pictures at evenly spaced t from 0 to 1, rendered
         * off the clock and saved as PNGs (to look at the in-betweens). */
        if (strip_n > 1 && (int)g_gs_frames >= strip_from && strip_i < 2 && cur->ok && cur->seq != strip_seq) {
            strip_seq = cur->seq;
            size_t npx = (size_t)cur->sw * hw_scale * (size_t)cur->sh * hw_scale; u32 *img = malloc(npx * 4); char path[300];
            for (int m = 0; m < strip_n; m++) {
                float ts = (float)m / (float)(strip_n - 1);
                for (int i = 0; i < cur->nrt; i++) hw_rt_load(cur->dst[i], cur->src[i], cur->slot, 0);
                for (int i = 0; i < cur->n; i++) {
                    PPrim *q = &cur->p[i]; HwVertex v[6];
                    if (!q->paired) { if (m) hw_draw(&q->h, q->cur, q->n); continue; }
                    for (int k = 0; k < q->n; k++) {
                        const float *a = (const float *)&q->cur[k], *b = (const float *)&q->prev[k]; float *o = (float *)&v[k];
                        for (size_t f = 0; f < sizeof(HwVertex) / sizeof(float); f++) o[f] = b[f] + (a[f] - b[f]) * ts;
                    }
                    hw_draw(&q->h, v, q->n);
                }
                const u32 *src = hw_rt_read_scaled(cur->pic_w, cur->sx, cur->sy, cur->sw, cur->sh);
                for (size_t i = 0; i < npx; i++) { u32 cc = src[i]; img[i] = ((cc & 0xFF) << 16) | (cc & 0xFF00) | ((cc >> 16) & 0xFF); }
                snprintf(path, sizeof path, "%s/strip_pkg%llu_%02d_t%03d.png", strip_dir ? strip_dir : ".", (unsigned long long)cur->seq, m, (int)(ts * 100.0f));
                save_png(path, img, cur->sw * hw_scale, cur->sh * hw_scale);
            }
            free(img); strip_i++;
            continue;
        }
        u64 t1 = plat_time_ns();
        hwg_grab_before();
        if (fs) hw_fsr3_show(0);
        hw_present(cur->pic_w, cur->sx, cur->sy, cur->sw, cur->sh, dx, dy, dw, dh, ww, wh, linear, sat64);
        hwg_grab_after((int)(t * 100.0f));
        u64 t2 = plat_time_ns();
        render_ns += t2 - now; presents++;
        { static int trace = -1, ntr; if (trace < 0) trace = getenv("ROR_HW_PRESENT_TRACE") ? atoi(getenv("ROR_HW_PRESENT_TRACE")) : 0;
          if (trace && (int)g_gs_frames >= trace && ntr < 400) { ntr++; rt_log("[ptrace] at %.2f ms: pkg %llu t=%.2f ok=%d prims=%d; draw %.2f ms, present %.2f ms; next pkg in %.1f ms\n", now / 1e6, (unsigned long long)cur->seq, t, cur->ok, cur->n, (t1 - now) / 1e6, (t2 - t1) / 1e6, next ? ((double)next->t_ns - (double)now) / 1e6 : -1.0); } }
        if (last_present && niv < 1024) iv[niv++] = t2 - last_present;
        if (!hw_fps_target && limit == 0 && t < 1.0f && last_present && t2 - last_present < 30000000ull && probe_n < 200) {
            probe_sum += t2 - last_present;
            if (++probe_n == 200 && hw_fps_hz > 0 && probe_sum / 200 < 800000000ull / (u64)hw_fps_hz) {
                limit = hw_fps_hz;
                rt_log("[present] presenting does not wait for the display (mean interval %.2f ms): limiting to %d per second by the clock\n", probe_sum / 200 / 1e6, limit);
            }
        }
        last_present = t2;
        (void)t1;
        if (niv == 1024 || (t_log && t2 - t_log > 10000000000ull && niv > 50)) {
            static int log = -1; if (log < 0) log = getenv("ROR_FPSLOG") != 0;
            if (log) {
                u64 sum = 0; qsort(iv, (size_t)niv, sizeof(u64), cmp_u64);
                for (int i = 0; i < niv; i++) sum += iv[i];
                rt_log("[present] %d presents: %.1f per second while moving; interval ms mean %.2f, median %.2f, p95 %.2f, p99 %.2f, max %.2f; render+present %.2f ms; "
                       "logic frames %llu, interpolated %llu, no package %llu, vertices paired %.1f%%, lead %.1f ms\n", niv, niv * 1e9 / (double)sum, sum / 1e6 / niv, iv[niv / 2] / 1e6,
                       iv[niv * 95 / 100] / 1e6, iv[niv * 99 / 100] / 1e6, iv[niv - 1] / 1e6, render_ns / 1e6 / (double)presents,
                       (unsigned long long)hwp_stat_frames, (unsigned long long)hwp_stat_ok, (unsigned long long)hwp_stat_dropped, hwp_stat_tot ? 100.0 * (double)hwp_stat_matched / (double)hwp_stat_tot : 0.0, (double)hwp_lead_ms);
            }
            niv = 0; render_ns = 0; presents = 0; t_log = t2;
        }
        if (!t_log) t_log = t2;
    }
    return 0;
}
/* Is primitive p of the previous frame really c one frame earlier? Pairing is
 * by position in the draw list first, and a list changes between frames (the
 * game culls, clips and orders per frame), so a pair can join two different
 * triangles; blending their vertices draws a polygon that sweeps across the
 * picture. A pair is only kept when nothing speaks against it. Returns 0 or
 * the reasons: 1 other texture coordinates (another triangle of the mesh),
 * 2 area changed by more than four times, 4 facing flipped, 8 moved further
 * than the limit. */
static int hwi_check(const RPrim *c, const RPrim *p) {
    int bad = 0;
    if (c->n != p->n) return 15;
    if (hwi_moved(c, p)) bad |= 8;
    if (c->h.flags & HWF_SPRITE) return bad;                 /* (sprites: size and texture cell are compared when the package is made) */
    if (c->h.flags & HWF_TME) {
        float tol = (c->h.flags & HWF_TEXEL) ? 0.75f : 0.003f;
        for (int q = 0; q < c->n; q++) {
            float wc = c->v[q].q != 0.0f ? c->v[q].q : 1.0f, wp = p->v[q].q != 0.0f ? p->v[q].q : 1.0f;
            if (fabsf(c->v[q].s / wc - p->v[q].s / wp) > tol || fabsf(c->v[q].t / wc - p->v[q].t / wp) > tol) { bad |= 1; break; }
        }
    }
    if (c->n == 3) {
        float ac = (c->v[1].x - c->v[0].x) * (c->v[2].y - c->v[0].y) - (c->v[2].x - c->v[0].x) * (c->v[1].y - c->v[0].y);
        float ap = (p->v[1].x - p->v[0].x) * (p->v[2].y - p->v[0].y) - (p->v[2].x - p->v[0].x) * (p->v[1].y - p->v[0].y);
        float mc = fabsf(ac), mp = fabsf(ap);                /* twice the area, native pixels */
        /* (small triangles change their area and facing freely as they turn edge-on, and cannot make a
         * visible wrong shape: the tests are for triangles of some size, 2A > 128 = an 8 x 8 pixel square) */
        if ((mc > 128.0f || mp > 128.0f) && (mc > 4.0f * mp || mp > 4.0f * mc)) bad |= 2;
        if (mc > 32.0f && mp > 32.0f && (ac > 0) != (ap > 0)) bad |= 4;
    }
    return bad;
}
static void hwi_boundary(HRt *disp, u32 dbx, u32 dby, u32 ww, u32 hh) {
    int N = hw_scale, nrt = hw_rt_count();
    hwi_frame++;
    int want = hwi_frame >= hw_interp_first && hwi_frame < hw_interp_first + hw_interp_count;
    /* match batches */
    int *partner = malloc(sizeof(int) * (size_t)(hwi_ncur + 1)); u64 tm0 = plat_time_ns(), tm1 = 0, tm2 = 0;
    if (hwi_strict < 0) { hwi_strict = !(getenv("ROR_HW_INTERP_CHECK") && atoi(getenv("ROR_HW_INTERP_CHECK")) == 0); hwi_stats = getenv("ROR_HW_INTERP_STATS") != 0; }
    long tot = 0, matched = 0, snapped = 0, n_cross = 0; int j = 0, batches = 0, mb = 0, sb = 0, rb = 0;
    hwi_kbuild();
    /* Primitive by primitive, in order. When the states differ, the previous
     * list is searched forward for the start of a batch with this state (a
     * batch may have grown, shrunk or been split differently: its common
     * prefix still corresponds); the current primitive stays alone if there
     * is none. Then, per batch of the new frame, the motion test. */
    for (int i = 0; i < hwi_ncur; i++) {
        partner[i] = -1;
        if (j < hwi_nprev && hwi_prev[j].key == hwi_cur[i].key) { partner[i] = j++; continue; }
        if (j >= hwi_nprev) continue;
        /* the next batch with this state after j, at most 64 batches on (from the index by state: the first
         * primitive with the state behind j starts a batch, the one at j having another state) */
        int ks = hwi_kslot(hwi_cur[i].key);
        if (hwi_kt[ks].n <= 0) continue;
        const int *L = hwi_kidx + hwi_kt[ks].start; int lo = 0, hi = hwi_kt[ks].n;
        while (lo < hi) { int m = (lo + hi) / 2; if (L[m] <= j) lo = m + 1; else hi = m; }
        if (lo < hwi_kt[ks].n && hwi_kbno[L[lo]] - hwi_kbno[j] <= 64) { partner[i] = L[lo]; j = L[lo] + 1; }
    }
    for (int i = 0; i < hwi_ncur;) {
        int e = i; while (e < hwi_ncur && hwi_cur[e].key == hwi_cur[i].key) e++;
        long nv = 0, nm = 0; int snap = 0;
        for (int k = i; k < e; k++) {
            nv += hwi_cur[k].n;
            if (partner[k] < 0) continue;
            nm += hwi_cur[k].n;
            if (!snap && hwi_strict) snap = hwi_check(&hwi_cur[k], &hwi_prev[partner[k]]) != 0;      /* a wrong pair: the order changed */
            for (int q = 0; q < hwi_cur[k].n && !snap; q++) {
                float dx = hwi_cur[k].v[q].x - hwi_prev[partner[k]].v[q].x, dy = hwi_cur[k].v[q].y - hwi_prev[partner[k]].v[q].y;
                if (dx * dx + dy * dy > hwi_snap_px * hwi_snap_px) snap = 1;
            }
        }
        tot += nv; batches++;
        (void)sb; (void)snapped;
        if (snap && want && getenv("ROR_HW_INTERP_DEBUG")) {
            int big = 0, all = 0; float mx = 0;
            for (int k = i; k < e; k++) if (partner[k] >= 0) for (int q = 0; q < hwi_cur[k].n; q++) {
                float dx = hwi_cur[k].v[q].x - hwi_prev[partner[k]].v[q].x, dy = hwi_cur[k].v[q].y - hwi_prev[partner[k]].v[q].y, d2 = dx * dx + dy * dy;
                all++; if (d2 > hwi_snap_px * hwi_snap_px) big++; if (d2 > mx) mx = d2;
            }
            rt_log("[interp]   snapped batch at %d (%d prims): %d of %d vertices beyond %.0f px, largest %.0f px\n", i, e - i, big, all, hwi_snap_px, sqrtf(mx));
        }
        if (snap || nm < nv) {
            long r = hwi_rematch(i, e, partner);
            if (snap) { rb++; snapped += nv - r; }
            matched += r; if (r == nv) mb++;
        } else { matched += nm; mb++; }
        i = e;
    }
    /* Textured primitives still alone: the game may have moved them to a batch
     * with another state (the storybook swaps a page's triangles between its
     * front and back texture as it turns; left alone they open cracks along
     * the crease). Look for their texture coordinates among the previous
     * frame's primitives that nobody claimed, whatever their state. */
    {   int alone = 0;
        for (int i = 0; i < hwi_ncur; i++) alone += partner[i] < 0 && (hwi_cur[i].h.flags & HWF_TME) && !(hwi_cur[i].h.flags & HWF_SPRITE);
        if (alone && hwi_nprev) {
            u8 *used = calloc((size_t)hwi_nprev, 1);
            for (int i = 0; i < hwi_ncur; i++) if (partner[i] >= 0) used[partner[i]] = 1;
            int tn = 16; while (tn < hwi_nprev * 2) tn *= 2;
            int *tab = malloc(sizeof(int) * (size_t)tn), *next = malloc(sizeof(int) * (size_t)hwi_nprev); u64 *hv = malloc(sizeof(u64) * (size_t)hwi_nprev);
            for (int k = 0; k < tn; k++) tab[k] = -1;
            for (int k = hwi_nprev - 1; k >= 0; k--) if (!used[k] && (hwi_prev[k].h.flags & HWF_TME)) {
                hv[k] = hwi_uvhash(&hwi_prev[k]);
                int slot = (int)(hv[k] >> 40) & (tn - 1);
                next[k] = tab[slot]; tab[slot] = k;
            }
            for (int i = 0; i < hwi_ncur; i++) {
                if (partner[i] >= 0 || !(hwi_cur[i].h.flags & HWF_TME) || (hwi_cur[i].h.flags & HWF_SPRITE)) continue;
                u64 h = hwi_uvhash(&hwi_cur[i]); int *link = &tab[(int)(h >> 40) & (tn - 1)];
                /* at most 16 candidates each: scenery made of many primitives with the same
                 * texture coordinates would make this quadratic when its textures change */
                if (!hwi_strict) {
                    for (int tries = 0; *link >= 0 && tries < 16; link = &next[*link], tries++) if (hv[*link] == h && hwi_prev[*link].n == hwi_cur[i].n && !hwi_moved(&hwi_cur[i], &hwi_prev[*link])) {
                        partner[i] = *link; matched += hwi_cur[i].n; *link = next[*link];
                        break;
                    }
                    continue;
                }
                /* the nearest one that passes hwi_check, and not further than a quarter of the general limit:
                 * with the state unknown, other meshes with the same texture coordinates are candidates too */
                int *best = 0; float bestd = hwi_snap_px * hwi_snap_px / 16.0f;
                for (int tries = 0; *link >= 0 && tries < 64; link = &next[*link], tries++) {
                    const RPrim *pv = &hwi_prev[*link];
                    if (hv[*link] != h || pv->n != hwi_cur[i].n) continue;
                    float dx = hwi_cur[i].v[0].x - pv->v[0].x, dy = hwi_cur[i].v[0].y - pv->v[0].y, d = dx * dx + dy * dy;
                    if (d < bestd && !hwi_check(&hwi_cur[i], pv)) { bestd = d; best = link; }
                }
                if (best) { partner[i] = *best; matched += hwi_cur[i].n; *best = next[*best]; n_cross++; }
            }
            free(used); free(tab); free(next); free(hv);
        }
    }
    if (want && getenv("ROR_HW_INTERP_DEBUG")) {
        int cnt[32] = { 0 }, first[32], last[32];
        for (int i = 0; i < hwi_ncur; i++) { int r = hwi_cur[i].h.rt & 31; if (!cnt[r]++) first[r] = i; last[r] = i; }
        for (int r = 0; r < 32; r++) if (cnt[r]) rt_log("[interp]   target %d: %d prims, list positions %d..%d%s\n", r, cnt[r], first[r], last[r], r == disp->hw ? " (displayed)" : "");
    }
    {   static int ld = -2; if (ld == -2) ld = getenv("ROR_HW_LISTDUMP") ? atoi(getenv("ROR_HW_LISTDUMP")) : -1;
        if (ld >= 0 && (int)g_gs_frames >= ld && (int)g_gs_frames < ld + 4) {
            rt_log("[list] vblank %llu: %d prims, displayed target %d\n", (unsigned long long)g_gs_frames, hwi_ncur, disp->hw);
            for (int i = 0; i < hwi_ncur;) {
                const RPrim *a = &hwi_cur[i]; int e = i, full = 0; float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, z0 = 1e30f, z1 = -1e30f; int np = 0;
                while (e < hwi_ncur && hwi_cur[e].path == a->path && hwi_cur[e].h.rt == a->h.rt && hwi_cur[e].h.tex_kind == a->h.tex_kind && ((hwi_cur[e].h.flags ^ a->h.flags) & (HWF_SPRITE | HWF_TME | HWF_SHBLEND | HWF_ATE)) == 0 &&
                       hwi_cur[e].h.ztest == a->h.ztest && hwi_cur[e].h.zwrite == a->h.zwrite && hwi_cur[e].h.blend_mode == a->h.blend_mode && hwi_cur[e].h.blend == a->h.blend && (a->h.tex_kind != 2 || hwi_cur[e].h.tex == a->h.tex)) {
                    for (int q = 0; q < hwi_cur[e].n; q++) { const HwVertex *v = &hwi_cur[e].v[q]; if (v->x < x0) x0 = v->x; if (v->x > x1) x1 = v->x; if (v->y < y0) y0 = v->y; if (v->y > y1) y1 = v->y; if (v->z < z0) z0 = v->z; if (v->z > z1) z1 = v->z; }
                    np += partner[e] >= 0; e++;
                }
                rt_log("[list] %5d +%4d path%d rt%d %s tex%d(%d) zt%d zw%d bm%d shblend %x cmask %x ate %d afail %d  x %.1f..%.1f y %.1f..%.1f z %.0f..%.0f paired %d sc %d,%d-%d,%d\n", i, e - i, a->path + 1, a->h.rt, (a->h.flags & HWF_SPRITE) ? "sprite" : "tri   ", a->h.tex_kind, a->h.tex_kind == 2 ? a->h.tex : -1,
                       a->h.ztest, a->h.zwrite, a->h.blend_mode, (a->h.flags & HWF_SHBLEND) ? a->h.blend : 0xFFFFu, a->h.cmask, (a->h.flags & HWF_ATE) != 0, a->h.afail, x0, x1, y0, y1, z0, z1, np, a->h.sx0, a->h.sy0, a->h.sx1, a->h.sy1);
                if (a->h.tex_kind == 2) rt_log("[list]        rt-textured: s,t %.3f,%.3f .. %.3f,%.3f  sx0 %.3f sy0 %.3f dudx %.4f dvdy %.4f fix %d aref %d flags %x blend %x rgba %.0f %.0f %.0f %.0f texw %.0f texh %.0f\n", a->v[0].s, a->v[0].t, a->v[a->n-1].s, a->v[a->n-1].t, a->v[0].sx0, a->v[0].sy0, a->v[0].dudx, a->v[0].dvdy, a->h.fix, a->h.aref, a->h.flags, a->h.blend, a->v[0].r, a->v[0].g, a->v[0].b, a->v[0].a, a->h.texw, a->h.texh);
                (void)full; i = e;
            }
        }
    }
    /* The picture this list paints: the target most of it draws to. (At the
     * flip the game is already showing the previous buffer again; the list
     * collected since the last flip belongs to the other one.) */
    int pic = disp->hw;
    {   int cnt[32] = { 0 }, best = 0;
        for (int i = 0; i < hwi_ncur; i++) if (++cnt[hwi_cur[i].h.rt & 31] > best) { best = cnt[hwi_cur[i].h.rt & 31]; pic = hwi_cur[i].h.rt; }
    }
    /* Validation: a pair that fails hwi_check is dissolved; what becomes of the
     * primitive is decided below. */
    {   long n_pairs = 0, n_bad = 0, why[4] = { 0, 0, 0, 0 }, n_weld = 0, n_vote = 0; float worst = 0;
        for (int i = 0; i < hwi_ncur;) {
            int e = i; while (e < hwi_ncur && hwi_cur[e].key == hwi_cur[i].key) e++;
            long nv = 0, good = 0;
            for (int k = i; k < e; k++) {
                nv += hwi_cur[k].n;
                if (partner[k] < 0) continue;
                int bad = hwi_check(&hwi_cur[k], &hwi_prev[partner[k]]);
                n_pairs++;
                if (!bad) { good += hwi_cur[k].n; continue; }
                n_bad++; for (int b = 0; b < 4; b++) why[b] += (bad >> b) & 1;
                if (hwi_stats) for (int q = 0; q < hwi_cur[k].n; q++) {
                    float dx = hwi_cur[k].v[q].x - hwi_prev[partner[k]].v[q].x, dy = hwi_cur[k].v[q].y - hwi_prev[partner[k]].v[q].y, d = sqrtf(dx * dx + dy * dy);
                    if (d > worst) worst = d;
                }
                if (hwi_strict) { matched -= hwi_cur[k].n; partner[k] = -1; }
            }
            i = e;
        }
        /* Welding. A primitive without a partner used to be drawn where the new
         * frame has it for the whole interval; while the camera moves, its
         * neighbours are in between and a crack as wide as the motion opens
         * along the shared edges. Meshes share vertices: a vertex of such a
         * primitive that is also a vertex of a validly paired one (same
         * target, same position in the new frame) was at that one's previous
         * position. Vertices found nowhere move like the found ones of their
         * primitive; a primitive with none stays where it is. */
        tm1 = plat_time_ns();
        if (hwi_strict && hwi_nprev) {
            /* (a table of slots pointing at entries made in order of appearance: only the slots are cleared per frame) */
            typedef struct { u32 kx, ky, kz; int rt; float px, py, pz; float cx[3], cy[3], cz[3]; u16 cn[3]; } WV;
            static WV *went; static int *wslot, wtn, wcap; int tn = 1024, nent = 0; while (tn < hwi_ncur * 12) tn *= 2;
            if (tn != wtn) { wtn = tn; wslot = realloc(wslot, sizeof(int) * (size_t)tn); }
            if (wcap < hwi_ncur * 6 + 16) { wcap = hwi_ncur * 6 + 4096; went = realloc(went, sizeof(WV) * (size_t)wcap); }
            memset(wslot, 0xFF, sizeof(int) * (size_t)tn);
            WV *tab = went;
            /* WFIND: e = the entry of vertex q of c, or -1 with sl = the free slot where it would go */
            #define WFIND(e, sl, c, q) do { u32 kx_, ky_, kz_; memcpy(&kx_, &(c)->v[q].x, 4); memcpy(&ky_, &(c)->v[q].y, 4); memcpy(&kz_, &(c)->v[q].z, 4); \
                sl = (int)(((kx_ * 2654435761u) ^ (ky_ * 2246822519u) ^ (kz_ * 3266489917u) ^ ((u32)(c)->h.rt * 668265263u)) >> 7) & (tn - 1); \
                while ((e = wslot[sl]) >= 0 && !(tab[e].rt == (c)->h.rt && tab[e].kx == kx_ && tab[e].ky == ky_ && tab[e].kz == kz_)) sl = (sl + 1) & (tn - 1); } while (0)
            #define WADD(e, sl, c, q) do { e = nent++; wslot[sl] = e; memcpy(&tab[e].kx, &(c)->v[q].x, 4); memcpy(&tab[e].ky, &(c)->v[q].y, 4); memcpy(&tab[e].kz, &(c)->v[q].z, 4); tab[e].rt = (c)->h.rt; } while (0)
            /* One previous place per vertex. The pairs vote: primitives that share a
             * vertex (neighbours in a mesh, and the layers of a surface drawn in
             * several passes) must agree where it was; layers that disagree get
             * different depths in between and show through each other in a dot
             * pattern. The place most pairs name wins, the smaller move on a tie;
             * pairs that name another place are dissolved and placed below. */
            for (int i = 0; i < hwi_ncur; i++) {
                if (partner[i] < 0 || (hwi_cur[i].h.flags & HWF_SPRITE)) continue;
                const RPrim *c = &hwi_cur[i], *pv = &hwi_prev[partner[i]];
                for (int q = 0; q < c->n; q++) {
                    int en, sl; WFIND(en, sl, c, q);
                    if (en < 0) { WADD(en, sl, c, q); tab[en].cn[0] = tab[en].cn[1] = tab[en].cn[2] = 0; }
                    WV *w = &tab[en];
                    int k = 0;
                    for (; k < 3 && w->cn[k]; k++) if (fabsf(w->cx[k] - pv->v[q].x) <= 0.25f && fabsf(w->cy[k] - pv->v[q].y) <= 0.25f) break;
                    if (k == 3) continue;
                    if (!w->cn[k]) { w->cx[k] = pv->v[q].x; w->cy[k] = pv->v[q].y; w->cz[k] = pv->v[q].z; }
                    if (w->cn[k] < 65535) w->cn[k]++;
                }
            }
            for (int k = 0; k < nent; k++) {
                WV *w = &tab[k]; float cx, cy; memcpy(&cx, &w->kx, 4); memcpy(&cy, &w->ky, 4);
                int best = 0; float bd = 0;
                for (int m = 0; m < 3 && w->cn[m]; m++) {
                    float d = (w->cx[m] - cx) * (w->cx[m] - cx) + (w->cy[m] - cy) * (w->cy[m] - cy);
                    if (m == 0 || w->cn[m] > w->cn[best] || (w->cn[m] == w->cn[best] && d < bd)) { best = m; bd = d; }
                }
                w->px = w->cx[best]; w->py = w->cy[best]; w->pz = w->cz[best];
            }
            for (int i = 0; i < hwi_ncur; i++) {
                if (partner[i] < 0 || (hwi_cur[i].h.flags & HWF_SPRITE)) continue;
                const RPrim *c = &hwi_cur[i], *pv = &hwi_prev[partner[i]];
                for (int q = 0; q < c->n; q++) {
                    int en, sl; WFIND(en, sl, c, q); (void)sl;
                    if (fabsf(tab[en].px - pv->v[q].x) > 0.25f || fabsf(tab[en].py - pv->v[q].y) > 0.25f) { matched -= c->n; partner[i] = -1; n_vote++; break; }
                }
            }
            hwi_nsyn = 0;
            /* (three rounds: a vertex placed by estimate is entered in the table, so that every primitive
             * sharing it uses the same place, and primitives further inside an unpaired patch find it later) */
            for (int round = 0; round < 3; round++) {
                long placed = 0;
                for (int i = 0; i < hwi_ncur; i++) {
                    if (partner[i] != -1 || (hwi_cur[i].h.flags & HWF_SPRITE)) continue;
                    const RPrim *c = &hwi_cur[i]; float px[6], py[6], pz[6]; int have[6], nf = 0; float mx = 0, my = 0, mz = 0;
                    for (int q = 0; q < c->n; q++) {
                        int en, sl; WFIND(en, sl, c, q); (void)sl;
                        have[q] = en >= 0;
                        if (have[q]) { px[q] = tab[en].px; py[q] = tab[en].py; pz[q] = tab[en].pz; mx += px[q] - c->v[q].x; my += py[q] - c->v[q].y; mz += pz[q] - c->v[q].z; nf++; }
                    }
                    if (!nf) continue;
                    if (hwi_nsyn == hwi_csyn) { hwi_csyn = hwi_csyn ? hwi_csyn * 2 : 4096; hwi_syn = realloc(hwi_syn, sizeof(RPrim) * (size_t)hwi_csyn); }
                    RPrim *sp = &hwi_syn[hwi_nsyn]; *sp = *c;
                    for (int q = 0; q < c->n; q++) {
                        if (have[q]) { sp->v[q].x = px[q]; sp->v[q].y = py[q]; sp->v[q].z = pz[q]; }
                        else { sp->v[q].x += mx / (float)nf; sp->v[q].y += my / (float)nf; sp->v[q].z += mz / (float)nf; }
                    }
                    if (hwi_check(c, sp)) continue;            /* (the same tests as for a pair: no flips, no bursting) */
                    for (int q = 0; q < c->n; q++) if (!have[q]) {
                        int en, sl; WFIND(en, sl, c, q);       /* (an earlier vertex of this primitive may have entered the same place) */
                        if (en < 0) { WADD(en, sl, c, q); tab[en].px = sp->v[q].x; tab[en].py = sp->v[q].y; tab[en].pz = sp->v[q].z; }
                    }
                    partner[i] = -2 - hwi_nsyn++; matched += c->n; n_weld++; placed++;
                }
                if (!placed) break;
            }
            #undef WFIND
            #undef WADD
        }
        tm2 = plat_time_ns();
        if (hwi_stats) {
            static u64 frames, frames_bad, pairs, bads; static u64 ns_a, ns_b; ns_a += tm1 - tm0; ns_b += tm2 - tm1;
            frames++; frames_bad += n_bad > 0; pairs += (u64)n_pairs; bads += (u64)n_bad;
            {   /* why primitives are alone: is there anything with their state in the previous frame at all? */
                static u64 fr; long alone = 0, nokey = 0;
                if (++fr % 50 == 0) {
                    for (int i = 0; i < hwi_ncur; i++) if (partner[i] == -1) {
                        alone++; nokey += hwi_kt[hwi_kslot(hwi_cur[i].key)].n <= 0;
                    }
                    rt_log("[istat] vblank %llu: %d primitives (%d in the previous frame), %ld alone, of those %ld have no primitive with their state in the previous frame; %ld paired across states\n",
                           (unsigned long long)g_gs_frames, hwi_ncur, hwi_nprev, alone, nokey, n_cross);
                }
            }
            if (n_bad || n_vote) rt_log("[istat] vblank %llu: %ld of %ld pairs fail (texture coordinates %ld, area %ld, facing %ld, distance %ld; largest move %.0f px); %ld pairs outvoted at a shared vertex; %ld primitives placed by shared vertices; %ld of %ld vertices interpolated\n",
                              (unsigned long long)g_gs_frames, n_bad, n_pairs, why[0], why[1], why[2], why[3], worst, n_vote, n_weld, matched, tot);
            if (frames % 250 == 0) rt_log("[istat] total after vblank %llu: %llu frames, %llu with failing pairs; %llu of %llu pairs failed; per frame %.2f ms pairing, %.2f ms shared vertices\n", (unsigned long long)g_gs_frames,
                                          (unsigned long long)frames, (unsigned long long)frames_bad, (unsigned long long)bads, (unsigned long long)pairs, ns_a / 1e6 / (double)frames, ns_b / 1e6 / (double)frames);
        }
    }
    int ok = hwi_nprev > 0 && tot > 0 && matched * 10 >= tot * 7 && !hwi_uploads && !hwi_texgone && hwi_have_start;
    hwf_replay = !hwi_uploads && hwi_have_start > 0; hwf_cut = !(hwi_nprev > 0 && tot > 0 && matched * 10 >= tot * 7);
    if (hw_fps_on || hw_fsr3) {
        { static int trace = -1, ntr; if (trace < 0) trace = getenv("ROR_HW_PRESENT_TRACE") ? atoi(getenv("ROR_HW_PRESENT_TRACE")) : 0;
          if (trace && (int)g_gs_frames >= trace && ntr < 60) { ntr++; rt_log("[btrace] at %.2f ms: vblank %llu boundary, displayed fbp %x, list %d prims, picture target %d, ok %d, ideal %.2f\n", plat_time_ns() / 1e6, (unsigned long long)g_gs_frames, disp->fbp, hwi_ncur, pic, ok, hwp_ideal_ns / 1e6); } }
        Pkg *pk = hwp_publish(partner, ok, pic, (int)dbx, (int)dby, (int)ww, (int)hh);
        if (hw_fsr3 && !hw_fps_on) { if (pk) hwf_boundary(pk); else hwf_reset = 1; }
        hwp_stat_frames++; hwp_stat_ok += ok; hwp_stat_tot += tot; hwp_stat_matched += matched;
        free(partner);
        { RPrim *t = hwi_prev; hwi_prev = hwi_cur; hwi_cur = t; int c = hwi_cprev; hwi_cprev = hwi_ccur; hwi_ccur = c; hwi_nprev = hwi_ncur; hwi_ncur = 0; }
        hwi_have_start = hwi_have_start < 0 ? 0 : 1; hwi_uploads = 0; hwi_texgone = 0;
        return;
    }
    if (want)
        rt_log("[interp] frame %d: %d batches / %ld vertices; interpolated %.1f%% of vertices (%d batches completely), %d batches re-paired by texture coordinates, not interpolated %.1f%%%s\n",
               hwi_frame, batches, tot, tot ? 100.0 * matched / tot : 0, mb, rb,
               tot ? 100.0 * (tot - matched) / tot : 0, ok ? "" : hwi_uploads || hwi_texgone ? " -> not interpolated (uploads/textures changed)" : " -> not interpolated");
    if (want && ok) {
        size_t npx = (size_t)ww * N * (size_t)hh * N;
        u32 *img = malloc(npx * 4); char path[256];
        for (int r = 0; r < nrt; r++) hw_rt_save(r, 1, 0);
        for (int m = 0; m <= hw_interp_n + 1; m++) {
            float t = (float)m / (float)(hw_interp_n + 1);
            if (m <= hw_interp_n) {
                for (int r = 0; r < nrt; r++) hw_rt_restore(r, 0);
                for (int d = 0; d < hw_nds; d++) hw_ds_load(hds[d].hw, hds[d].hw, 0);
                for (int i = 0; i < hwi_ncur; i++) {
                    RPrim *c = &hwi_cur[i]; HwVertex v[6];
                    memcpy(v, c->v, sizeof(HwVertex) * (size_t)c->n);
                    if (partner[i] >= 0) {
                        const RPrim *p = &hwi_prev[partner[i]];
                        for (int q = 0; q < c->n; q++) {
                            float *a = (float *)&v[q]; const float *b = (const float *)&p->v[q];
                            for (size_t f = 0; f < sizeof(HwVertex) / sizeof(float); f++) a[f] = b[f] + (a[f] - b[f]) * t;
                        }
                    } else if (m == 0) continue;         /* t = 0 is the previous picture: nothing new in it yet */
                    hw_draw(&c->h, v, c->n);
                }
            } else for (int r = 0; r < nrt; r++) hw_rt_restore(r, 1);
            const u32 *src = hw_rt_read_scaled(pic, (int)dbx, (int)dby, (int)ww, (int)hh);
            for (size_t i = 0; i < npx; i++) { u32 cc = src[i]; img[i] = ((cc & 0xFF) << 16) | (cc & 0xFF00) | ((cc >> 16) & 0xFF); }
            snprintf(path, sizeof path, "%s/interp_%04d_%d.png", getenv("ROR_HW_INTERP_DIR") ? getenv("ROR_HW_INTERP_DIR") : ".", hwi_frame, m);
            save_png(path, img, (int)ww * N, (int)hh * N);
        }
        {   /* self-check: the list replayed at t = 1 from the start-of-frame targets must give the frame itself */
            u32 *chk = malloc(npx * 4); size_t bad = 0;
            for (int r = 0; r < nrt; r++) hw_rt_restore(r, 0);
            for (int d = 0; d < hw_nds; d++) hw_ds_load(hds[d].hw, hds[d].hw, 0);
            for (int i = 0; i < hwi_ncur; i++) hw_draw(&hwi_cur[i].h, hwi_cur[i].v, hwi_cur[i].n);
            const u32 *src = hw_rt_read_scaled(pic, (int)dbx, (int)dby, (int)ww, (int)hh);
            for (size_t i = 0; i < npx; i++) { u32 cc = src[i]; chk[i] = ((cc & 0xFF) << 16) | (cc & 0xFF00) | ((cc >> 16) & 0xFF); if (chk[i] != img[i]) bad++; }
            rt_log("[interp] frame %d: replay at t=1 differs from the real frame in %zu of %zu pixels\n", hwi_frame, bad, npx);
            free(chk);
            for (int r = 0; r < nrt; r++) hw_rt_restore(r, 1);
        }
        free(img);
    }
    free(partner);
    { RPrim *t = hwi_prev; hwi_prev = hwi_cur; hwi_cur = t; int c = hwi_cprev; hwi_cprev = hwi_ccur; hwi_ccur = c; hwi_nprev = hwi_ncur; hwi_ncur = 0; }
    for (int r = 0; r < nrt; r++) hw_rt_save(r, 0, 0);       /* the targets as the next frame will find them */
    for (int d = 0; d < hw_nds; d++) hw_ds_save(hds[d].hw, 0);
    hwi_have_start = 1; hwi_uploads = 0; hwi_texgone = 0;
}
static void hrt_pages(const HRt *r, PageSet *ps) { rect_pages(ps, PSMCT32, r->fbp, r->fbw, 0, 0, r->fbw * 64, (u32)(r->rows > 0 ? r->rows : 1)); }
static void hrt_cpu_rect(HRt *r, int x0, int y0, int x1, int y1) {
    if (r->cx1 <= r->cx0) { r->cx0 = x0; r->cy0 = y0; r->cx1 = x1; r->cy1 = y1; return; }
    if (x0 < r->cx0) r->cx0 = x0; if (y0 < r->cy0) r->cy0 = y0;
    if (x1 > r->cx1) r->cx1 = x1; if (y1 > r->cy1) r->cy1 = y1;
}
/* Replay pending host uploads into the image. */
static void hwg_rt_sync(HRt *r) {
    if (r->cx1 <= r->cx0) return;
    int w = r->cx1 - r->cx0, h = r->cy1 - r->cy0;
    u32 *tmp = malloc((size_t)w * (size_t)h * 4);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++)
        tmp[y * w + x] = vram[a32(swz32, r->fbp, r->fbw, (u32)(r->cx0 + x), (u32)(r->cy0 + y))];
    hw_rt_upload(r->hw, r->cx0, r->cy0, w, h, tmp);
    hwi_uploads++;
    free(tmp);
    r->cx1 = r->cx0 = 0;
}
/* Bring vram[] up to date with the image (waits for the GPU). */
static void hwg_rt_download(HRt *r) {
    if (!r->gpu_dirty) return;
    hwg_rt_sync(r);
    int w = (int)r->fbw * 64, h = r->rows;
    u32 *tmp = malloc((size_t)w * (size_t)h * 4);
    hw_rt_download(r->hw, 0, 0, w, h, tmp);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) vram[a32(swz32, r->fbp, r->fbw, (u32)x, (u32)y)] = tmp[y * w + x];
        xfer_bump(PSMCT32, r->fbp, r->fbw, 0, (u32)w - 1, (u32)y);
    }
    free(tmp);
    r->gpu_dirty = 0; state_dirty = 1; hw_stat_downloads++;
}
static void hwg_cpu_needs(const PageSet *pages) {
    for (int i = 0; i < HW_MAXRT; i++) if (hrt[i].used && hrt[i].gpu_dirty) {
        PageSet ps; hrt_pages(&hrt[i], &ps);
        if (ps_meet(&ps, pages)) hwg_rt_download(&hrt[i]);
    }
}
static void hwg_cpu_wrote_all(void) {
    for (int i = 0; i < HW_MAXRT; i++) if (hrt[i].used) hrt_cpu_rect(&hrt[i], 0, 0, (int)hrt[i].fbw * 64, hrt[i].hrows);
}
static HRt *hwg_rt_find(u32 fbp, u32 fbw) {
    for (int i = 0; i < HW_MAXRT; i++) if (hrt[i].used && hrt[i].fbp == fbp && hrt[i].fbw == fbw) return &hrt[i];
    return 0;
}
static HRt *hwg_rt_get(u32 fbp, u32 fbw, int rows) {
    HRt *r = hwg_rt_find(fbp, fbw);
    if (r) { if (rows > r->hrows) return 0; if (rows > r->rows) r->rows = rows; return r; }
    if (!fbw || rows > 1024) return 0;
    for (int i = 0; i < HW_MAXRT && !r; i++) if (!hrt[i].used) r = &hrt[i];
    if (!r) return 0;
    memset(r, 0, sizeof *r);
    r->used = 1; r->fbp = fbp; r->fbw = fbw; r->rows = rows; r->hrows = rows > 512 ? 1024 : 512;
    {   /* other targets' pixels this one starts from must be in vram[] */
        PageSet ps; hrt_pages(r, &ps); r->used = 0; hwg_cpu_needs(&ps); r->used = 1;
    }
    r->hw = hw_rt_create((int)fbw * 64, r->hrows);
    hrt_cpu_rect(r, 0, 0, (int)fbw * 64, r->hrows);      /* initial contents: what local memory holds */
    return r;
}
static int hwg_ds_get(u32 zbp, u32 fbw, int hrows) {
    for (int i = 0; i < hw_nds; i++) if (hds[i].zbp == zbp && hds[i].fbw == fbw && hds[i].hrows == hrows) return hds[i].hw;
    if (hw_nds == HW_MAXRT) return hds[0].hw;
    hds[hw_nds].zbp = zbp; hds[hw_nds].fbw = fbw; hds[hw_nds].hrows = hrows;
    hds[hw_nds].hw = hw_ds_create((int)fbw * 64, hrows);
    return hds[hw_nds++].hw;
}
static void hwg_reset(void) {            /* local memory was replaced (snapshot loaded) */
    hw_flush();
    for (int i = 0; i < HW_MAXRT; i++) if (hrt[i].used) { hrt[i].gpu_dirty = 0; hrt_cpu_rect(&hrt[i], 0, 0, (int)hrt[i].fbw * 64, hrt[i].hrows); }
    for (int i = 0; i < hw_nds; i++) hw_ds_clear(hds[i].hw);
}

/* Texture of a state being built. */
static void hwg_tex(DState *s) {
    const Ctxt *c = &s->c;
    if (c->tpsm == PSMCT32 || c->tpsm == PSMCT24) {
        HRt *r = hwg_rt_find(c->tbp, c->tbw);
        if (r) { s->hw_kind = 2; s->hw_tex = (int)(r - hrt); s->tex = 0; return; }
    }
    hwg_cpu_needs(&s->texmask);
    s->tex = texcache_get(s);
    TexEnt *e = tex_last;
    if (!e->hw_valid) { e->hwtex = hw_tex_create(1 << c->tw, 1 << c->th, e->px); e->hw_valid = 1; }
    s->hw_kind = 1; s->hw_tex = e->hwtex;
}
/* Translate the draw state. Returns 0 when nothing can be drawn. */
static int hwg_state(const DState *st, HwState *h, int texel_coords, int sprite) {
    const Ctxt *c = &st->c;
    memset(h, 0, sizeof *h);
    h->sx0 = c->sx0; h->sy0 = c->sy0; h->sx1 = c->sx1; h->sy1 = c->sy1;
    u32 fl = 0;
    if (st->tme) {
        fl |= HWF_TME | (texel_coords ? HWF_TEXEL : 0) | (c->mmag ? HWF_BILIN : 0) | (c->tcc ? HWF_TCC : 0) | (c->tfx << HWF_TFX_SHIFT) |
              (c->wms << HWF_WMS_SHIFT) | (c->wmt << HWF_WMT_SHIFT);
        h->tex_kind = st->hw_kind; h->tex = st->hw_kind == 2 ? hrt[st->hw_tex].hw : st->hw_tex;
        if (st->hw_kind == 2 && c->tpsm == PSMCT24) fl |= HWF_TEX24 | (st->aem ? HWF_AEM : 0);
        h->region[0] = (int)c->minu; h->region[1] = (int)c->maxu; h->region[2] = (int)c->minv; h->region[3] = (int)c->maxv;
        h->texw = (float)(1 << c->tw); h->texh = (float)(1 << c->th); h->ta0 = (float)st->ta0;
    }
    if (st->fge) { fl |= HWF_FGE; h->fogcol = (u32)st->fogr | ((u32)st->fogg << 8) | ((u32)st->fogb << 16); }
    if (st->fba) fl |= HWF_FBA;
    /* depth */
    h->ztest = st->zte ? (u8)st->ztst : 0; h->zwrite = (u8)(st->zte && !c->zmsk);
    h->zmax = (float)st->zmax; h->zscale = c->zpsm == PSMZ32 ? 1.0f / 4294967296.0f : c->zpsm == PSMZ24 ? 1.0f / 16777216.0f : 1.0f / 65536.0f;
    /* colour mask */
    h->cmask = 15;
    int shader = 0;
    for (int k = 0; k < 4; k++) {
        u32 m = (c->fbmsk >> (k * 8)) & 255;
        if (m == 255) h->cmask &= (u8)~(1 << k); else if (m) shader = 1;
    }
    if (st->date && c->fpsm != PSMCT24) { shader = 1; fl |= HWF_DATE | (st->datm ? HWF_DATM : 0); }
    /* blending: (A - B) * C + D */
    if (st->abe) {
        u32 fix = st->fix;
        int cok = st->ac == 0 || (st->ac == 2 && fix <= 128);
        if (st->pabe || !st->colclamp) shader = 2;
        else if (st->aa == st->ab) {
            if (st->ad == 1) h->cmask &= 8;                          /* result is the destination: colour untouched */
            else if (st->ad == 2) shader = 2;
        } else if (cok && st->aa == 0 && st->ab == 1 && st->ad == 1) h->blend_mode = HWB_MIX;
        else if (cok && st->aa == 0 && st->ab == 2 && st->ad == 1) h->blend_mode = HWB_ADD;
        else if (cok && st->aa == 2 && st->ab == 0 && st->ad == 1) h->blend_mode = HWB_REVSUB;
        else shader = 2;
        if (h->blend_mode) { h->blend_const = st->ac == 2; h->fix = (u8)fix; }
        /* Sprites (the 2D layer, full-screen passes) always blend in the shader:
         * exact GS arithmetic, and affordable because a sprite is one primitive. */
        if (sprite && h->blend_mode) shader = 2;
        if (hw_shblend) shader = 2;                                  /* ROR_HW_SHBLEND: every blend in the shader (exact, slow) */
    }
    if (shader) {
        h->need_dst = 1; h->cmask = 15; h->fbmsk = c->fbmsk;
        if (shader == 2 || (st->abe && h->blend_mode)) {
            if (st->abe && !(st->aa == st->ab && st->ad == 0)) {
                fl |= HWF_SHBLEND | (st->colclamp ? HWF_COLCLAMP : 0);
                h->blend = st->aa | (st->ab << 2) | (st->ac << 4) | (st->ad << 6) | (st->fix << 8);
            }
            h->blend_mode = HWB_OFF; h->blend_const = 0; h->fix = 0;
        }
    }
    /* alpha test */
    if (st->ate && st->atst == 0) {                                   /* NEVER: every fragment takes the fail action */
        if (st->afail == 0) return 0;
        if (st->afail == 1) h->zwrite = 0;
        else if (st->afail == 2) h->cmask = 0;
        else { h->cmask &= 7; h->zwrite = 0; }
    } else if (st->ate && st->atst != 1) {
        int af = st->afail;
        if (af == 1 && !h->zwrite) af = -1;                           /* FB_ONLY without depth writes: the test changes nothing */
        if (af == 2 && !h->zwrite) af = 0;                            /* ZB_ONLY without depth writes: failing fragments do nothing */
        if (af >= 0) {
            fl |= HWF_ATE | ((u32)st->atst << HWF_ATST_SHIFT); h->aref = st->aref;
            h->afail = (u8)af;
        }
    }
    h->flags = fl;
    static int statelog = -1;
    if (statelog < 0) statelog = getenv("ROR_HW_STATELOG") != 0;
    if (statelog) {                               /* debug: each distinct alpha-test / blend combination once */
        static u32 seen[64]; static int ns; u32 k = (u32)st->atst | ((u32)st->aref << 3) | ((u32)st->afail << 11) | ((u32)st->ate << 13) | (st->aa << 14) | (st->ab << 16) | (st->ac << 18) | (st->ad << 20) | ((u32)st->abe << 22) | ((u32)h->zwrite << 23) | ((u32)sprite << 24) | (st->fix << 25);
        int f = 0; for (int i = 0; i < ns; i++) f |= seen[i] == k;
        if (!f && ns < 64) { seen[ns++] = k; rt_log("[hwstate] %s ate=%d atst=%d aref=%d afail=%d zwrite=%d abe=%d blend=%u%u%u%u fix=%u -> hw afail %d blend_mode %d shader %d\n", sprite ? "sprite" : "tri", st->ate, st->atst, st->aref, st->afail, h->zwrite, st->abe, st->aa, st->ab, st->ac, st->ad, st->fix, h->afail, h->blend_mode, h->need_dst); }
    }
    return 1;
}
static void hwg_vtx(HwVertex *o, const Vtx *v, const Vtx *col, float s, float t, float q) {
    o->x = (float)v->x / 16.0f; o->y = (float)v->y / 16.0f; o->z = (float)v->z; o->fog = (float)v->f;
    o->s = s; o->t = t; o->q = q;
    o->r = col->r; o->g = col->g; o->b = col->b; o->a = col->a;
    o->sx0 = o->sy0 = o->dudx = o->dvdy = 0;
}
static void hwg_prim(const DState *st, const Prim *p) {
    const Ctxt *c = &st->c;
    static int warned;
    if (c->fpsm != PSMCT32 && c->fpsm != PSMCT24) { if (!warned++) rt_log("[gs] hardware renderer: frame format %x not supported, primitives dropped\n", c->fpsm); return; }
    {   /* ROR_HW_SKIPTEX=hex[,hex...] (debugging): primitives textured from these TBP0 addresses are not drawn; "fffff" = untextured ones */
        static int nskip = -1; static u32 skip[512];
        if (nskip < 0) { nskip = 0; const char *e = getenv("ROR_HW_SKIPTEX"); while (e && *e && nskip < 512) { char *q; skip[nskip++] = (u32)strtoul(e, &q, 16); e = *q == ',' ? q + 1 : q; if (!*q) break; } }
        for (int i = 0; i < nskip; i++) if (st->tme ? c->tbp == skip[i] : skip[i] == 0xfffff) {
            static int nlog; static int slog = -1; if (slog < 0) slog = getenv("ROR_HW_SKIPLOG") ? atoi(getenv("ROR_HW_SKIPLOG")) : 0;
            if (nlog < slog) { nlog++; extern int g_gif_path;
                rt_log("[skip] vblank %llu path%d type %d tex %x %dx%d psm %x tcc %d tfx %d mmag %d mmin %d mxl %d | abe %d blend %d%d%d%d fix %x | ate %d atst %d aref %x afail %d | zte %d ztst %d zmsk %d | fge %d iip %d | v", (unsigned long long)g_gs_frames, g_gif_path + 1, p->type, c->tbp, 1 << c->tw, 1 << c->th, c->tpsm, c->tcc, c->tfx, c->mmag, (int)((c->tex1 >> 6) & 7), (int)((c->tex1 >> 2) & 7),
                       st->abe, st->aa, st->ab, st->ac, st->ad, st->fix, st->ate, st->atst, st->aref, st->afail, st->zte, st->ztst, c->zmsk, st->fge, st->iip);
                for (int k = 0; k < (p->type == PT_TRI ? 3 : 2); k++) rt_log(" (%.2f,%.2f z %x rgba %02x%02x%02x%02x)", p->v[k].x / 16.0 - 1792, p->v[k].y / 16.0 - 1824, p->v[k].z, p->v[k].r, p->v[k].g, p->v[k].b, p->v[k].a);
                rt_log("\n"); }
            return;
        }
    }
    int nvx = p->type == PT_TRI ? 3 : p->type == PT_POINT ? 1 : 2, rows = 0;
    for (int i = 0; i < nvx; i++) { int y = (p->v[i].y >> 4) + 2; if (y > rows) rows = y; }
    if (rows > c->sy1 + 1) rows = c->sy1 + 1;
    if (rows < 1) rows = 1;
    HRt *r = hwg_rt_get(c->fbp, c->fbw, rows);
    if (!r) { if (!warned++) rt_log("[gs] hardware renderer: no render target for fbp %x fbw %u\n", c->fbp, c->fbw); return; }
    hwg_rt_sync(r);
    if (st->tme && st->hw_kind == 2) {
        HRt *t = &hrt[st->hw_tex];
        hwg_rt_sync(t);
        /* A texture taller than the target it names continues into whatever
         * follows in local memory. When that is another target of the same
         * width (the two frame buffers are adjacent, and the game declares
         * them as 512 rows), bring those rows over so that filtering at the
         * bottom edge sees what the software renderer sees. */
        int need = (1 << c->th) < t->hrows ? (1 << c->th) : t->hrows;
        if (need > t->rows && t->rows > 0 && !(t->rows & 31)) {
            HRt *o = hwg_rt_find(t->fbp + (u32)(t->rows / 32) * t->fbw * 32, t->fbw);
            if (o && o != t && o->rows > 0) {
                hwg_rt_sync(o);
                hw_rt_copy_rows(o->hw, 0, t->hw, t->rows, need - t->rows < o->rows ? need - t->rows : o->rows);
            } else if (!o) {
                /* What follows is not a colour target. For the second frame
                 * buffer it is the depth buffer, and the game's full-screen
                 * feedback pass wraps into it at the top screen row. Depth
                 * lives only on the GPU, but when the pass is drawn right
                 * after the depth clear the memory is one known value: show
                 * the rows as that value read as colour (two 16-bit depths,
                 * or one 24/32-bit depth, per pixel), else as zero. */
                u32 word = 0, tp = (t->fbp >> 5) + (u32)(t->rows / 32) * t->fbw; int done = 0;
                for (int i = 0; i < hw_nds && !done; i++) {
                    HDs *d = &hds[i];
                    /* a 16-bit depth buffer of the same width covering those pages: convert the real thing on the GPU */
                    if (d->fbw == t->fbw && (d->psm == PSMZ16 || d->psm == PSMZ16S) && tp >= (d->zbp >> 5) &&
                        tp + (u32)((need - t->rows + 31) / 32) * t->fbw <= (d->zbp >> 5) + (u32)(d->hrows / 64) * t->fbw) {
                        static u32 tab[2][64 * 32]; static int tab_ok;
                        if (!tab_ok) {                       /* which two 16-bit pixels each 32-bit pixel of a page is made of */
                            tab_ok = 1;
                            for (int k = 0; k < 2; k++) {
                                static u8 inv[4096][2];
                                const u16 (*zt)[64] = k ? swz16sz : swz16z;
                                for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) { inv[zt[y][x]][0] = (u8)x; inv[zt[y][x]][1] = (u8)y; }
                                for (int y = 0; y < 32; y++) for (int x = 0; x < 64; x++) {
                                    u32 w = swz32[y][x];
                                    tab[k][y * 64 + x] = (u32)inv[2 * w][0] | ((u32)inv[2 * w][1] << 8) | ((u32)inv[2 * w + 1][0] << 16) | ((u32)inv[2 * w + 1][1] << 24);
                                }
                            }
                        }
                        hw_rt_rows_from_depth(t->hw, t->rows, need - t->rows, d->hw, tab[d->psm == PSMZ16S], (int)(tp - (d->zbp >> 5)), (int)t->fbw);
                        t->tail_for = -1; done = 1;
                    } else if ((d->zbp >> 5) == tp && d->fbw == t->fbw && d->uniform) word = d->clear_z;
                }
                if (!done && (t->tail_for != t->rows || t->tail_word != word)) {
                    size_t n = (size_t)t->fbw * 64 * (size_t)(need - t->rows);
                    u32 *z = malloc(n * 4);
                    for (size_t i = 0; i < n; i++) z[i] = word;
                    hw_rt_upload(t->hw, 0, t->rows, (int)t->fbw * 64, need - t->rows, z);
                    free(z); t->tail_for = t->rows; t->tail_word = word;
                }
            }
        }
    }
    HwState h; HwVertex v[6]; int n = 0;
    int texel = st->fst || p->type != PT_TRI;
    {   /* the translated state is the same for every primitive of a draw state and kind: keep the last one */
        static const DState *c_st; static u64 c_gen; static int c_texel, c_sprite, c_ok; static HwState c_h;
        if (c_st != st || c_gen != hw_state_gen || c_texel != texel || c_sprite != (p->type == PT_SPRITE)) {
            c_st = st; c_gen = hw_state_gen; c_texel = texel; c_sprite = p->type == PT_SPRITE;
            c_ok = hwg_state(st, &c_h, texel, c_sprite);
            HwState k = c_h; k.rt = k.ds = 0; k.tex = 0; k.fix = 0; k.blend &= 0xFF; k.fogcol = 0; k.aref = 0;
            if (c_sprite) k.flags |= HWF_SPRITE;
            u64 hh = 1469598103934665603ull;
            for (size_t i = 0; i < sizeof k; i++) hh = (hh ^ ((const u8 *)&k)[i]) * 1099511628211ull;
            hwi_state_key = hh ^ (k.tex_kind == 2 || !st->tme ? 0 : (c->tex0 & 0xFFFFFFFFFFull) * 0x9E3779B97F4A7C15ull);
        }
        if (!c_ok) return;
        h = c_h;
    }
    h.rt = r->hw; h.ds = hwg_ds_get(c->zbp, c->fbw, r->hrows);
    for (int i = 0; i < hw_nds; i++) if (hds[i].hw == h.ds) hds[i].psm = c->zpsm;
    if (h.zwrite) {                                    /* keep track of "the depth buffer is one value" (see the tail rows above) */
        HDs *d = 0;
        for (int i = 0; i < hw_nds; i++) if (hds[i].hw == h.ds) d = &hds[i];
        if (d) {
            const Vtx *a = &p->v[0], *b = &p->v[1];
            int full = p->type == PT_SPRITE && h.ztest <= 1 && !(h.flags & HWF_ATE) &&
                       (a->x < b->x ? a->x : b->x) <= c->sx0 * 16 && (a->y < b->y ? a->y : b->y) <= c->sy0 * 16 &&
                       (a->x > b->x ? a->x : b->x) > c->sx1 * 16 && (a->y > b->y ? a->y : b->y) > c->sy1 * 16 && c->sx0 == 0 && c->sy0 == 0 &&
                       c->sx1 + 1 >= (s32)c->fbw * 64 - 0 && c->sy1 + 1 >= 448;
            if (full) { d->uniform = 1; d->clear_z = b->z > st->zmax ? st->zmax : b->z; }
            else d->uniform = 0;
        }
    }
    if (p->type == PT_TRI) {
        for (int i = 0; i < 3; i++) {
            const Vtx *s = &p->v[i];
            if (st->fst) hwg_vtx(&v[i], s, st->iip ? s : &p->v[2], (float)s->u / 16.0f, (float)s->v / 16.0f, 1.0f);
            else hwg_vtx(&v[i], s, st->iip ? s : &p->v[2], s->s, s->t, s->q);
        }
        n = 3;
    } else if (p->type == PT_SPRITE) {
        const Vtx *a = &p->v[0], *b = &p->v[1];
        float u0, v0, u1, v1;
        if (st->fst) { u0 = (float)a->u / 16.0f; v0 = (float)a->v / 16.0f; u1 = (float)b->u / 16.0f; v1 = (float)b->v / 16.0f; }
        else {
            float q = b->q != 0.0f ? b->q : 1.0f, tw = (float)(1 << c->tw), th = (float)(1 << c->th);
            u0 = a->s / q * tw; v0 = a->t / q * th; u1 = b->s / q * tw; v1 = b->t / q * th;
        }
        /* texture coordinates by the software renderer's formula u0 + (x - x0) * dudx, in the shader */
        s32 x0 = a->x, y0 = a->y, x1 = b->x, y1 = b->y;
        if (x0 > x1) { s32 t = x0; x0 = x1; x1 = t; float f = u0; u0 = u1; u1 = f; }
        if (y0 > y1) { s32 t = y0; y0 = y1; y1 = t; float f = v0; v0 = v1; v1 = f; }
        float dudx = (u1 - u0) * 16.0f / (float)(x1 - x0), dvdy = (v1 - v0) * 16.0f / (float)(y1 - y0);
        Vtx k[4] = { *b, *b, *b, *b };                /* depth, colour and fog of the second vertex */
        k[0].x = x0; k[0].y = y0; k[1].x = x1; k[1].y = y0; k[2].x = x0; k[2].y = y1; k[3].x = x1; k[3].y = y1;
        HwVertex q4[4];
        for (int i = 0; i < 4; i++) {
            hwg_vtx(&q4[i], &k[i], b, u0, v0, 1.0f);
            q4[i].sx0 = (float)x0 / 16.0f; q4[i].sy0 = (float)y0 / 16.0f; q4[i].dudx = dudx; q4[i].dvdy = dvdy;
        }
        h.flags |= HWF_SPRITE;
        v[0] = q4[0]; v[1] = q4[1]; v[2] = q4[2]; v[3] = q4[2]; v[4] = q4[1]; v[5] = q4[3];
        n = 6;
    } else if (p->type == PT_POINT) {                  /* the pixel's whole block */
        const Vtx *a = &p->v[0];
        float tu, tv; tex_of(st, a, &tu, &tv);
        Vtx k[4] = { *a, *a, *a, *a };
        s32 x = ((a->x + 8) >> 4) * 16, y = ((a->y + 8) >> 4) * 16;
        k[0].x = x; k[0].y = y; k[1].x = x + 16; k[1].y = y; k[2].x = x; k[2].y = y + 16; k[3].x = x + 16; k[3].y = y + 16;
        HwVertex q4[4];
        for (int i = 0; i < 4; i++) hwg_vtx(&q4[i], &k[i], a, tu, tv, 1.0f);
        v[0] = q4[0]; v[1] = q4[1]; v[2] = q4[2]; v[3] = q4[2]; v[4] = q4[1]; v[5] = q4[3];
        n = 6;
    } else {
        /* Line: a quad one native pixel wide around the segment. The GS steps
         * along the major axis and lights the nearest pixel, last point
         * excluded; this covers the same pixels except at the ends and where
         * the line is steeper than its width (approximation). */
        const Vtx *a = &p->v[0], *b = &p->v[1];
        float ax = (float)a->x / 16.0f, ay = (float)a->y / 16.0f, bx = (float)b->x / 16.0f, by = (float)b->y / 16.0f;
        float dx = bx - ax, dy = by - ay, len = sqrtf(dx * dx + dy * dy);
        hw_stat_lines++;
        if (len <= 0.0f) return;
        float nx = -dy / len * 0.5f, ny = dx / len * 0.5f, au, av, bu, bv;
        tex_of(st, a, &au, &av); tex_of(st, b, &bu, &bv);
        HwVertex q4[4];
        hwg_vtx(&q4[0], a, st->iip ? a : b, au, av, 1.0f); hwg_vtx(&q4[1], a, st->iip ? a : b, au, av, 1.0f);
        hwg_vtx(&q4[2], b, b, bu, bv, 1.0f); hwg_vtx(&q4[3], b, b, bu, bv, 1.0f);
        q4[0].x = ax + nx; q4[0].y = ay + ny; q4[1].x = ax - nx; q4[1].y = ay - ny;
        q4[2].x = bx + nx; q4[2].y = by + ny; q4[3].x = bx - nx; q4[3].y = by - ny;
        v[0] = q4[0]; v[1] = q4[1]; v[2] = q4[2]; v[3] = q4[2]; v[4] = q4[1]; v[5] = q4[3];
        n = 6;
    }
    hw_draw(&h, v, n);
    r->gpu_dirty = 1;
    if (hw_interp_n || hw_fps_on || hw_fsr3) hwi_record(&h, v, n);
}

/* CLUT load whose source may be on the GPU. Returns 1 if the load was deferred. */
static void hwg_apply_cluts(void) {
    for (int k = 0; k < clut_njobs; k++) {
        ClutJob *j = clut_jobs[k]; u32 px[256];
        hw_ticket_read(hw_clut_ticket[k], px);
        int count = j->d.n4 ? 16 : 256; u32 base = j->d.n4 ? (j->d.csa & 15) * 16 : 0;
        for (int i = 0; i < count; i++) {
            int jj = j->d.n4 ? i : ((i & 0xE7) | ((i & 8) << 1) | ((i & 16) >> 1));
            u32 x = j->d.n4 ? (u32)(i & 7) : (u32)(jj & 15), y = j->d.n4 ? (u32)(i >> 3) : (u32)(jj >> 4);
            j->val[(base + (u32)i) & 255] = px[y * 16 + x];
        }
    }
    clut_apply_jobs();
}
static void hwg_sync(void) {
    if (!clut_njobs) return;
    hw_flush();
    hwg_apply_cluts();
}
static int hwg_clut(const ClutDesc *d, u32 seq) {
    u32 pg = d->cbp >> 5;
    for (int i = 0; i < HW_MAXRT; i++) {
        HRt *r = &hrt[i];
        if (!r->used || !r->gpu_dirty) continue;
        PageSet ps; hrt_pages(r, &ps);
        if (!ps_test(&ps, pg & 511) && !((d->cbp & 31) && ps_test(&ps, (pg + 1) & 511)) && d->csm == 0) continue;
        s32 po = (s32)pg - (s32)(r->fbp >> 5);
        if (d->csm == 0 && d->cpsm == PSMCT32 && !(d->cbp & 31) && po >= 0 && clut_njobs < 64) {
            int x = (int)((u32)po % r->fbw) * 64, y = (int)((u32)po / r->fbw) * 32;
            hwg_rt_sync(r);
            int t = hw_rt_copy_async(r->hw, x, y, 16, 16);
            if (t >= 0) {
                ClutJob *j = malloc(sizeof *j);
                j->d = *d; j->seq = seq;
                hw_clut_ticket[clut_njobs] = t; clut_jobs[clut_njobs++] = j;
                return 1;
            }
        }
        if (clut_njobs) hwg_sync();
        hwg_rt_download(r);
    }
    return 0;
}

/* Host -> local transfer starting (TRXDIR written). */
static void hwg_xfer_begin(void) {
    u32 dbp = (r_bitblt >> 32) & 0x3FFF, dbw = (r_bitblt >> 48) & 63, dpsm = (r_bitblt >> 56) & 63;
    int dsax = (int)((r_trxpos >> 32) & 0x7FF), dsay = (int)((r_trxpos >> 48) & 0x7FF), w = (int)(r_trxreg & 0xFFF), h = (int)((r_trxreg >> 32) & 0xFFF);
    hw_tx_rt = -1;
    for (int i = 0; i < HW_MAXRT; i++) {
        HRt *r = &hrt[i];
        if (!r->used) continue;
        s32 po = (s32)(dbp >> 5) - (s32)(r->fbp >> 5);
        if ((dpsm == PSMCT32 || dpsm == PSMCT24) && dbw == r->fbw && !(dbp & 31) && po >= 0) {
            int ox = (int)((u32)po % r->fbw) * 64 + dsax, oy = (int)((u32)po / r->fbw) * 32 + dsay;
            if (ox + w <= (int)r->fbw * 64 && oy + h <= r->hrows) {      /* the target's own layout: replay into the image later */
                hw_tx_rt = i; hw_tx_x = ox; hw_tx_y = oy; hw_tx_w = w; hw_tx_h = h;
                continue;
            }
        }
        PageSet ps; hrt_pages(r, &ps);
        if (ps_meet(&ps, &tx_pages)) {                                  /* foreign data over the target: back to local memory */
            hwg_rt_download(r);
            hrt_cpu_rect(r, 0, 0, (int)r->fbw * 64, r->hrows);
        }
    }
}
static void hwg_xfer_data(void) {
    if (hw_tx_rt < 0) return;
    HRt *r = &hrt[hw_tx_rt];
    hrt_cpu_rect(r, hw_tx_x, hw_tx_y, hw_tx_x + hw_tx_w, hw_tx_y + hw_tx_h);
    if (hw_tx_y + hw_tx_h > r->rows) r->rows = hw_tx_y + hw_tx_h;
}
/* The displayed image at device resolution, if the display buffer is a target. */
/* The displayed buffer as a target (made from local memory if nothing has drawn to it yet). */
static HRt *hwg_display_rt(u32 fbp, u32 fbw, u32 dbx, u32 dby, u32 ww, u32 hh) {
    HRt *r = hwg_rt_get(fbp, fbw, (int)(dby + hh));
    if (!r || (int)(dby + hh) > r->hrows || dbx + ww > fbw * 64) return 0;
    hwg_rt_sync(r);
    if ((hw_interp_n || hw_fps_on || hw_fsr3) && fbp != hwi_last_fbp) { hwi_last_fbp = fbp; hwi_boundary(r, dbx, dby, ww, hh); }
    return r;
}
static int hwg_frame(u32 fbp, u32 fbw, u32 dbx, u32 dby, u32 ww, u32 hh, u32 **pixels, int *w, int *h) {
    HRt *r = hwg_display_rt(fbp, fbw, dbx, dby, ww, hh);
    if (!r) return 0;
    int N = hw_scale;
    size_t n = (size_t)ww * N * (size_t)hh * N;
    if (n > hw_frame_cap) { free(hw_frame_px); hw_frame_px = malloc(n * 4); hw_frame_cap = n; }
    static int noread = -1;
    if (noread < 0) noread = getenv("ROR_HW_NOREAD") != 0;     /* timing aid: submit and wait, but do not read the image back */
    if (noread) hw_flush();
    else {
        const u32 *src = hw_rt_read_scaled(r->hw, (int)dbx, (int)dby, (int)ww, (int)hh);
        for (size_t i = 0; i < n; i++) { u32 c = src[i]; hw_frame_px[i] = ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF); }
    }
    {   static u64 frames;
        if (++frames % 100 == 0 && getenv("ROR_FPSLOG")) {
            static u64 ld, lc, lf;
            rt_log("[gs_hw] per frame: %.0f draws, %.1f image copies, %.2f submits, %.2f target downloads; lines drawn (as quads) so far: %llu\n", (hw_stat_draws - ld) / 100.0,
                   (hw_stat_copies - lc) / 100.0, (hw_stat_flushes - lf) / 100.0, hw_stat_downloads / (double)frames, (unsigned long long)hw_stat_lines);
            ld = hw_stat_draws; lc = hw_stat_copies; lf = hw_stat_flushes;
        }
    }
    if (clut_njobs) hwg_apply_cluts();                 /* the submit above completed the queued CLUT reads */
    *pixels = hw_frame_px; *w = (int)ww * N; *h = (int)hh * N;
    return 1;
}
#else
static void tex_hw_forget(TexEnt *e) { (void)e; }
#endif

/* ---- capture / snapshot (ROR_GSCAP) ------------------------------------------------
 * ROR_GSCAP=prefix ROR_GSCAP_RANGES=a-b,c-d records, for each frame range, a
 * snapshot of the complete GS state followed by every call gs.c receives, into
 * prefix_<a>.gscap. recomp/tools/gsreplay.sh replays such a file through the
 * real gs.c without the rest of the game. */
static FILE *cap_f; static const char *cap_prefix; static u64 cap_end;
static struct { u64 a, b; } cap_ranges[16]; static int cap_nranges;
static int snap_io(void *p, size_t n, FILE *f, int save) {
    return (save ? fwrite(p, 1, n, f) : fread(p, 1, n, f)) == n;
}
#define SNAP(x) ok &= snap_io(&(x), sizeof(x), f, save)
int gs_snapshot(FILE *f, int save) {
    int ok = 1;
    tx_flush();
    gs_sync(SY_SNAP);
#ifdef GS_HW
    if (hw_on && save) { PageSet all; memset(&all, 0xFF, sizeof all); hwg_cpu_needs(&all); }
#endif
    SNAP(vram); SNAP(ctxt);
    SNAP(r_prim); SNAP(r_prmode); SNAP(r_prmodecont); SNAP(r_texclut); SNAP(r_texa); SNAP(r_fogcol); SNAP(r_pabe);
    SNAP(r_colclamp); SNAP(r_dthe); SNAP(r_bitblt); SNAP(r_trxpos); SNAP(r_trxreg); SNAP(r_trxdir);
    SNAP(r_rgbaq); SNAP(r_q); SNAP(r_s); SNAP(r_t); SNAP(r_u); SNAP(r_v); SNAP(r_fog);
    if (save) clut_full_sig();
    SNAP(vq); SNAP(vcount); SNAP(clut); SNAP(clut16); SNAP(clut_sig); SNAP(tx);
    SNAP(p_pmode); SNAP(p_smode2); SNAP(p_dispfb); SNAP(p_display); SNAP(p_bgcolor); SNAP(p_siglblid); SNAP(p_csr);
    SNAP(g_gs_imr); SNAP(g_gs_frames);
    if (!save) {                           /* everything derived from the old contents is stale */
        for (int i = 0; i < 512; i++) page_gen[i]++;
#ifdef GS_HW
        if (hw_on) hwg_reset();
#endif
        state_dirty = 1; clut_sig_valid = 0;
        tx.active = 0;                     /* (the transfer rectangle's page set is not part of the snapshot) */
    }
    return ok;
}
#undef SNAP
static void cap_ev(u8 type, const void *a, size_t an, const void *b, size_t bn) {
    fputc(type, cap_f);
    if (an) fwrite(a, 1, an, cap_f);
    if (bn) fwrite(b, 1, bn, cap_f);
}
static void cap_frame(void) {              /* called after g_gs_frames advanced */
    if (cap_f && g_gs_frames >= cap_end) { fclose(cap_f); cap_f = 0; rt_log("[gscap] capture ended at frame %llu\n", (unsigned long long)g_gs_frames); }
    if (!cap_f) for (int i = 0; i < cap_nranges; i++) if (cap_ranges[i].a == g_gs_frames) {
        char path[256]; snprintf(path, sizeof path, "%s_%llu.gscap", cap_prefix, (unsigned long long)g_gs_frames);
        cap_f = fopen(path, "wb");
        if (!cap_f) { rt_log("[gscap] cannot write %s\n", path); return; }
        fwrite("GSCAP001", 1, 8, cap_f);
        gs_snapshot(cap_f, 1);
        cap_end = cap_ranges[i].b;
        rt_log("[gscap] capturing frames %llu-%llu to %s\n", (unsigned long long)cap_ranges[i].a, (unsigned long long)cap_end, path);
    }
}
static void cap_init(void) {
    cap_prefix = getenv("ROR_GSCAP");
    const char *r = getenv("ROR_GSCAP_RANGES");
    if (!cap_prefix || !r) { cap_prefix = 0; return; }
    while (*r && cap_nranges < 16) {
        char *e; cap_ranges[cap_nranges].a = strtoull(r, &e, 10);
        if (*e != '-') break;
        cap_ranges[cap_nranges++].b = strtoull(e + 1, &e, 10);
        r = (*e == ',') ? e + 1 : e;
    }
}

/* ---- image transfers ------------------------------------------------------------ */
/* Pages a w x h rectangle at (x0, y0) of a buffer occupies. */
static void rect_pages(PageSet *out, u32 psm, u32 bp, u32 bw, u32 x0, u32 y0, u32 w, u32 h) {
    ps_clear(out);
    if (!w || !h) return;
    if (x0 + w > 2048 || y0 + h > 2048) { memset(out, 0xFF, sizeof *out); return; }      /* wraps around: anything */
    u32 xs = (psm == PSMT8 || psm == PSMT4) ? 7 : 6;
    u32 ys = (psm == PSMT4) ? 7 : (psm == PSMT8 || psm == PSMCT16 || psm == PSMCT16S || psm == PSMZ16 || psm == PSMZ16S) ? 6 : 5;
    u32 rp = (psm == PSMT8 || psm == PSMT4) ? (bw >> 1) : bw;
    for (u32 r = y0 >> ys; r <= (y0 + h - 1) >> ys; r++) for (u32 cc = x0 >> xs; cc <= (x0 + w - 1) >> xs; cc++) {
        u32 pg = (bp >> 5) + r * rp + cc;
        ps_set(out, pg & 511);
        if (bp & 31) ps_set(out, (pg + 1) & 511);
    }
}
static PageSet tx_pages;                   /* destination of the host->local transfer in progress */

static void local_to_local(void) {
    u32 sbp = r_bitblt & 0x3FFF, sbw = (r_bitblt >> 16) & 63, spsm = (r_bitblt >> 24) & 63;
    u32 dbp = (r_bitblt >> 32) & 0x3FFF, dbw = (r_bitblt >> 48) & 63, dpsm = (r_bitblt >> 56) & 63;
    u32 ssax = r_trxpos & 0x7FF, ssay = (r_trxpos >> 16) & 0x7FF, dsax = (r_trxpos >> 32) & 0x7FF, dsay = (r_trxpos >> 48) & 0x7FF;
    u32 dir = (r_trxpos >> 59) & 3, w = r_trxreg & 0xFFF, h = (r_trxreg >> 32) & 0xFFF;
    gs_sync(SY_L2L);
    state_dirty = 1;
#ifdef GS_HW
    if (hw_on) { PageSet all; memset(&all, 0xFF, sizeof all); hwg_cpu_needs(&all); hwg_cpu_wrote_all(); }
#endif
    for (u32 j = 0; j < h; j++) {
        u32 yy = (dir & 1) ? h - 1 - j : j;
        for (u32 i = 0; i < w; i++) {
            u32 xx = (dir & 2) ? w - 1 - i : i;
            wr_px(dpsm, dbp, dbw, (dsax + xx) & 2047, (dsay + yy) & 2047,
                  rd_px(spsm, sbp, sbw, (ssax + xx) & 2047, (ssay + yy) & 2047));
        }
    }
}

/* Inverse of the PSMT4 column layout: for each of the 64 bytes of a column
 * (4 rows x 32 pixels), which source nibbles its low and high halves take.
 * Index 0: even columns of a block, 1: odd ones (halves swapped). */
static u8 t4inv[2][64][2];
/* The same layout seen as sixteen 32-bit words: byte k of word w holds, in its
 * low half, nibble t4lo[w][1] of the k-th 32-bit word of source row
 * t4lo[w][0], and in its high half the nibble named by t4hi (that the layout
 * has this regular form is checked when the tables are built). This lets a
 * column be assembled with a handful of vector shifts instead of 128 nibble
 * moves; the game uploads about a million PSMT4 texels per frame. */
static u8 t4lo[16][2], t4hi[16][2]; static int t4_vec_ok;
static void t4_column_vec(u8 *dst, const u8 *r0, const u8 *r1, const u8 *r2, const u8 *r3, int odd) {
    const __m128i V[4] = { _mm_loadu_si128((const __m128i *)r0), _mm_loadu_si128((const __m128i *)r1),
                           _mm_loadu_si128((const __m128i *)r2), _mm_loadu_si128((const __m128i *)r3) };
    const __m128i m = _mm_set1_epi32(15);
    __m128i x[2];
    for (int w = 0; w < 16; w += 2) {
        for (int i = 0; i < 2; i++) {
            __m128i lo = _mm_and_si128(_mm_srl_epi32(V[t4lo[w + i][0]], _mm_cvtsi32_si128(4 * t4lo[w + i][1])), m);
            __m128i hi = _mm_and_si128(_mm_srl_epi32(V[t4hi[w + i][0]], _mm_cvtsi32_si128(4 * t4hi[w + i][1])), m);
            x[i] = _mm_or_si128(lo, _mm_slli_epi32(hi, 4));           /* one byte value per 32-bit lane */
        }
        __m128i p = _mm_packus_epi16(_mm_packs_epi32(x[0], x[1]), _mm_setzero_si128());
        _mm_storel_epi64((__m128i *)(dst + (w ^ (odd ? 8 : 0)) * 4), p);
    }
}
static void t4_column_ref(u8 *dst, const u8 *r0, const u8 *r1, const u8 *r2, const u8 *r3, int odd) {
    const u8 (*inv)[2] = t4inv[odd];
    u8 sb[64];
    memcpy(sb, r0, 16); memcpy(sb + 16, r1, 16); memcpy(sb + 32, r2, 16); memcpy(sb + 48, r3, 16);
    for (int b = 0; b < 64; b++) {
        u32 lo = inv[b][0], hi = inv[b][1];
        dst[b] = (u8)(((sb[lo >> 1] >> ((lo & 1) * 4)) & 15) | (((sb[hi >> 1] >> ((hi & 1) * 4)) & 15) << 4));
    }
}
static void build_xfer_tables(void) {
    for (int par = 0; par < 2; par++) for (int r = 0; r < 4; r++) for (int x = 0; x < 32; x++) {
        int n = colp4[r][x] ^ (par ? 64 : 0);
        t4inv[par][n >> 1][n & 1] = (u8)(r * 32 + x);
    }
    for (int r = 0; r < 4; r++) for (int x = 0; x < 32; x++) {
        int n = colp4[r][x], b = n >> 1;
        u8 (*t)[2] = (n & 1) ? t4hi : t4lo;
        t[b >> 2][0] = (u8)r; t[b >> 2][1] = (u8)(x & 7);
    }
    /* use the vector form only if it reproduces the table-driven one */
    u8 src[4][16], a[64], b[64]; u32 seed = 12345;
    t4_vec_ok = 1;
    for (int t = 0; t < 8 && t4_vec_ok; t++) {
        for (int i = 0; i < 64; i++) { seed = seed * 1664525u + 1013904223u; src[i >> 4][i & 15] = (u8)(seed >> 24); }
        t4_column_vec(a, src[0], src[1], src[2], src[3], t & 1); t4_column_ref(b, src[0], src[1], src[2], src[3], t & 1);
        if (memcmp(a, b, 64)) t4_vec_ok = 0;
    }
    if (!t4_vec_ok) rt_log("[gs] PSMT4 vector upload path disabled (self-test failed)\n");
}
static void xfer_bump(u32 psm, u32 bp, u32 bw, u32 x0, u32 x1, u32 y) {      /* pages of pixels [x0, x1] of row y */
    u32 xs = (psm == PSMT8 || psm == PSMT4) ? 7 : 6;
    u32 base = (bp >> 5) + page_off(psm, bw, 0, y);
    for (u32 cc = x0 >> xs; cc <= x1 >> xs; cc++) {
        page_gen[(base + cc) & 511]++;
        if (bp & 31) page_gen[(base + cc + 1) & 511]++;
    }
}
/* Host->local data may arrive in pieces of any size (VIF1 DIRECT delivers one
 * quadword per call). Bytes that do not complete a unit of the fast paths
 * below wait in tx_buf; tx_flush() writes them pixel by pixel, and is called
 * before anything else can look at local memory or change the transfer
 * registers, so the staging is not observable. */
static u8 tx_buf[16384]; static u32 tx_staged, tx_unit, tx_amask;
static void xfer_generic(const u8 *data, u32 bytes) {
    u32 dbp = (r_bitblt >> 32) & 0x3FFF, dbw = (r_bitblt >> 48) & 63, dpsm = (r_bitblt >> 56) & 63;
    u32 dsax = (r_trxpos >> 32) & 0x7FF, dsay = (r_trxpos >> 48) & 0x7FF;
    u32 w = r_trxreg & 0xFFF, h = (r_trxreg >> 32) & 0xFFF;
    u32 pos = 0, nib = 0;
    while (tx.y < h) {
        u32 v;
        switch (dpsm) {
        case PSMCT32: case PSMZ32:
            if (pos + 4 > bytes) return;
            memcpy(&v, data + pos, 4); pos += 4; break;
        case PSMCT24: case PSMZ24:
            if (pos + 3 > bytes) return;
            v = data[pos] | (data[pos + 1] << 8) | (data[pos + 2] << 16); pos += 3; break;
        case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S:
            if (pos + 2 > bytes) return;
            v = data[pos] | (data[pos + 1] << 8); pos += 2; break;
        case PSMT8: case PSMT8H:
            if (pos + 1 > bytes) return;
            v = data[pos++]; break;
        default:   /* 4-bit: low nibble first */
            if (pos + 1 > bytes) return;
            v = (data[pos] >> (nib * 4)) & 15;
            if (nib) pos++;
            nib ^= 1;
            break;
        }
        wr_px(dpsm, dbp, dbw, (dsax + tx.x) & 2047, (dsay + tx.y) & 2047, v);
        stat_xfer_px++;
        if (++tx.x >= w) { tx.x = 0; tx.y++; }
    }
    tx.active = 0;
}
static void tx_flush(void) {
    if (!tx_staged) return;
    u32 n = tx_staged; tx_staged = 0;
    if (tx.active) xfer_generic(tx_buf, n);
}
static void write_hwreg(const u8 *data, u32 bytes) {
    if (!tx.active) return;
#ifdef GS_HW
    if (hw_on) hwg_xfer_data();
#endif
    if (tx_staged && tx_staged + bytes < tx_unit && !(bytes & tx_amask)) {      /* still collecting a unit: nothing else to do */
        if (bytes == 16) memcpy(tx_buf + tx_staged, data, 16);                  /* one quadword (VIF1 DIRECT): inline copy */
        else memcpy(tx_buf + tx_staged, data, bytes);
        tx_staged += bytes;
        return;
    }
    u32 dbp = (r_bitblt >> 32) & 0x3FFF, dbw = (r_bitblt >> 48) & 63, dpsm = (r_bitblt >> 56) & 63;
    u32 dsax = (r_trxpos >> 32) & 0x7FF, dsay = (r_trxpos >> 48) & 0x7FF;
    u32 w = r_trxreg & 0xFFF, h = (r_trxreg >> 32) & 0xFFF;
    /* Queued primitives sample decoded snapshots, never local memory, so an
     * upload only has to wait when it lands in pages the queue still touches. */
    if (q_pending() && ps_meet(&tx_pages, &pend_dirty)) gs_sync(SY_XFER);
    state_dirty = 1;
    /* Fast paths: whole rows of the common formats, taken from the byte stream
     * in units (PSMT4: four rows, the others: one row). The stream is the same
     * however it is cut into calls, except that a call with a partial 32-bit
     * pixel at its end drops it (as the pixel loop does): no staging then. */
    int fast = w && dsax + w <= 2048 && dsay + h <= 2048 &&
               ((dpsm == PSMT4 && !(w & 31) && !(dsax & 31)) || dpsm == PSMT8 || (dpsm == PSMCT32 && !(bytes & 3)));
    while (fast && tx.x == 0) {
        u32 y = dsay + tx.y, unit;
        if (dpsm == PSMT4) { if ((y & 3) || h - tx.y < 4) break; unit = 2 * w; }
        else unit = dpsm == PSMT8 ? w : 4 * w;
        const u8 *src;
        if (tx_staged) {
            u32 n = unit - tx_staged;
            if (n > bytes) n = bytes;
            memcpy(tx_buf + tx_staged, data, n); tx_staged += n; data += n; bytes -= n;
            if (tx_staged < unit) return;
            src = tx_buf; tx_staged = 0;
        } else if (bytes >= unit) { src = data; data += unit; bytes -= unit; }
        else { memcpy(tx_buf, data, bytes); tx_staged = bytes; tx_unit = unit; tx_amask = dpsm == PSMCT32 ? 3 : 0; return; }
        if (dpsm == PSMT4) {                 /* each 32x4 column is 64 contiguous bytes */
            u32 c = (y >> 2) & 3;
            for (u32 x = dsax; x < dsax + w; x += 32, src += 16) {
                u8 *dst = VRAM8 + ((dbp * 256 + ((y >> 7) * (dbw >> 1) + (x >> 7)) * 8192 + blk4[(y >> 4) & 7][(x >> 5) & 3] * 256 + c * 64) & (VRAM_WORDS * 4 - 1));
                (t4_vec_ok ? t4_column_vec : t4_column_ref)(dst, src, src + w / 2, src + w, src + w / 2 * 3, (int)(c & 1));
            }
            xfer_bump(dpsm, dbp, dbw, dsax, dsax + w - 1, y);
            tx.y += 4; stat_xfer_px += 4 * w;
        } else if (dpsm == PSMT8) {
            u32 rowbase = dbp * 256 + (y >> 6) * (dbw >> 1) * 8192;
            const u16 *t = swz8[y & 63];
            for (u32 i = 0, x = dsax; i < w; i++, x++) VRAM8[(rowbase + (x >> 7) * 8192 + t[x & 127]) & (VRAM_WORDS * 4 - 1)] = src[i];
            xfer_bump(dpsm, dbp, dbw, dsax, dsax + w - 1, y);
            tx.y++; stat_xfer_px += w;
        } else {
            u32 rowbase = dbp * 64 + (y >> 5) * dbw * 2048;
            const u16 *t = swz32[y & 31];
            for (u32 i = 0, x = dsax; i < w; i++, x++) memcpy(&vram[(rowbase + (x >> 6) * 2048 + t[x & 63]) & (VRAM_WORDS - 1)], src + 4 * i, 4);
            xfer_bump(dpsm, dbp, dbw, dsax, dsax + w - 1, y);
            tx.y++; stat_xfer_px += w;
        }
        if (tx.y >= h) { tx.active = 0; return; }
    }
    tx_flush();
    xfer_generic(data, bytes);
}

/* ---- general registers ------------------------------------------------------------ */
void gs_write_hwreg(const u8 *data, u32 bytes) {
    if (cap_f) cap_ev(2, &bytes, 4, data, bytes);
    write_hwreg(data, bytes);
}
void gs_set_q(float q) { if (cap_f) cap_ev(5, &q, 4, 0, 0); r_q = q; }

/* Draw-state registers: remember that the state snapshot is out of date, but
 * only on a real change (the game rewrites most of them for every object). */
#define SETREG(field, val) do { if ((field) != (val)) { (field) = (val); state_dirty = 1; } } while (0)

void gs_write_reg(u32 reg, u64 d) {
    if (cap_f) { u8 r8 = (u8)reg; cap_ev(1, &r8, 1, &d, 8); }
    if (tx_staged && reg != 0x54) tx_flush();
    Ctxt *c = &ctxt[reg & 1];
    if (gs_dump_frame && g_gs_frames == gs_dump_frame && (reg == 0x47 || reg == 0x48 || reg == 0x4e || reg == 0x4f || reg == 0x1a || reg == 0x1b))
        rt_log("[gsdump] reg %02x <- %016llx\n", reg, (unsigned long long)d);
    switch (reg) {
    case 0x00: r_prim = d & 0x7FF; vcount = 0; break;
    case 0x01: r_rgbaq = (u32)d; { u32 q = (u32)(d >> 32); memcpy(&r_q, &q, 4); } break;
    case 0x02: { u32 s = (u32)d, t = (u32)(d >> 32); memcpy(&r_s, &s, 4); memcpy(&r_t, &t, 4); break; }
    case 0x03: r_u = d & 0x3FFF; r_v = (d >> 16) & 0x3FFF; break;
    case 0x04: vertex_kick(d & 0xFFFF, (d >> 16) & 0xFFFF, (d >> 32) & 0xFFFFFF, (u32)(d >> 56), 1); break;
    case 0x05: vertex_kick(d & 0xFFFF, (d >> 16) & 0xFFFF, (u32)(d >> 32), r_fog, 1); break;
    case 0x06: case 0x07: case 0x16: case 0x17: {
        u64 old = c->tex0;
        if (set_tex0(c, d, reg >= 0x16) || c->tex0 != old) state_dirty = 1;
        break;
    }
    case 0x08: case 0x09:
        if (c->clamp == d) break;
        state_dirty = 1;
        c->clamp = d; c->wms = d & 3; c->wmt = (d >> 2) & 3;
        c->minu = (d >> 4) & 1023; c->maxu = (d >> 14) & 1023; c->minv = (d >> 24) & 1023; c->maxv = (d >> 34) & 1023;
        break;
    case 0x0A: r_fog = (u32)(d >> 56); break;
    case 0x0C: vertex_kick(d & 0xFFFF, (d >> 16) & 0xFFFF, (d >> 32) & 0xFFFFFF, (u32)(d >> 56), 0); break;
    case 0x0D: vertex_kick(d & 0xFFFF, (d >> 16) & 0xFFFF, (u32)(d >> 32), r_fog, 0); break;
    case 0x14: case 0x15: if (c->tex1 != d) { c->tex1 = d; c->mmag = (d >> 5) & 1; state_dirty = 1; } break;
    case 0x18: case 0x19: c->xyoffset = d; c->ofx = d & 0xFFFF; c->ofy = (d >> 32) & 0xFFFF; break;
    case 0x1A: r_prmodecont = d & 1; break;
    case 0x1B: r_prmode = d & 0x7F8; break;
    case 0x1C: r_texclut = d; break;
    case 0x22: case 0x34: case 0x35: case 0x36: case 0x37: break;   /* SCANMSK, MIPTBP: not modelled */
    case 0x3B: SETREG(r_texa, d); break;
    case 0x3D: SETREG(r_fogcol, d); break;
    case 0x3F: break;                                              /* TEXFLUSH */
    case 0x40: case 0x41:
        if (c->scissor == d) break;
        state_dirty = 1;
        c->scissor = d; c->sx0 = d & 0x7FF; c->sx1 = (d >> 16) & 0x7FF; c->sy0 = (d >> 32) & 0x7FF; c->sy1 = (d >> 48) & 0x7FF;
        break;
    case 0x42: case 0x43: SETREG(c->alpha, d); break;
    case 0x44: break;                                              /* DIMX */
    case 0x45: r_dthe = d; break;
    case 0x46: SETREG(r_colclamp, d); break;
    case 0x47: case 0x48: SETREG(ctxt[reg - 0x47].test, d); break;   /* TEST_1 is the odd one of this pair */
    case 0x49: SETREG(r_pabe, d); break;
    case 0x4A: case 0x4B: SETREG(c->fba, d); break;
    case 0x4C: case 0x4D:
        if (c->frame == d) break;
        state_dirty = 1;
        c->frame = d; c->fbp = (d & 0x1FF) << 5; c->fbw = (d >> 16) & 63; c->fpsm = (d >> 24) & 63; c->fbmsk = (u32)(d >> 32);
        break;
    case 0x4E: case 0x4F:
        if (c->zbuf == d) break;
        state_dirty = 1;
        c->zbuf = d; c->zbp = (d & 0x1FF) << 5; c->zpsm = 0x30 | ((d >> 24) & 15); c->zmsk = (d >> 32) & 1;
        break;
    case 0x50: r_bitblt = d; break;
    case 0x51: r_trxpos = d; break;
    case 0x52: r_trxreg = d; break;
    case 0x53:
        r_trxdir = d & 3;
        tx.x = tx.y = 0; tx.active = 0;
        if (r_trxdir == 0) {
            tx.active = 1;
            rect_pages(&tx_pages, (r_bitblt >> 56) & 63, (r_bitblt >> 32) & 0x3FFF, (r_bitblt >> 48) & 63,
                       (r_trxpos >> 32) & 0x7FF, (r_trxpos >> 48) & 0x7FF, r_trxreg & 0xFFF, (r_trxreg >> 32) & 0xFFF);
#ifdef GS_HW
            if (hw_on) hwg_xfer_begin();
#endif
        }
        else if (r_trxdir == 2) local_to_local();
        else if (r_trxdir == 1) rt_log("[gs] local->host transfer requested (not implemented)\n");
        break;
    case 0x54: { u8 b[8]; memcpy(b, &d, 8); write_hwreg(b, 8); break; }
    case 0x60: {                                                   /* SIGNAL */
        u32 id = (u32)d, mask = (u32)(d >> 32);
        p_siglblid = (p_siglblid & ~(u64)mask) | (id & mask);
        p_csr |= 1;
        if (!(g_gs_imr & 0x100)) intc_raise(INTC_GS);
        break;
    }
    case 0x61:                                                     /* FINISH */
        /* Nothing the game can observe depends on the drawing being complete
         * (there is no local->host transfer), so the event is raised at once;
         * the queue is only handed to the workers. */
        q_publish();
        p_csr |= 2;
        if (!(g_gs_imr & 0x200)) intc_raise(INTC_GS);
        break;
    case 0x62: {                                                   /* LABEL */
        u64 id = (u32)d, mask = (u32)(d >> 32);
        p_siglblid = (p_siglblid & ~(mask << 32)) | ((id & mask) << 32);
        break;
    }
    default: {
        static u8 seen[256];
        if (reg < 256 && !seen[reg]) { seen[reg] = 1; rt_log("[gs] write to unknown register 0x%02x\n", reg); }
    }
    }
}

/* ---- privileged registers ----------------------------------------------------------- */
u64 gs_read_priv(u32 a) {
    switch (a & 0xFFFF) {
    case 0x1000: return p_csr;
    case 0x1010: return g_gs_imr;
    case 0x1080: return p_siglblid;
    }
    return 0;
}
void gs_write_priv(u32 a, u64 v) {
    if (cap_f) cap_ev(3, &a, 4, &v, 8);
    switch (a & 0xFFFF) {
    case 0x0000: p_pmode = v; break;
    case 0x0020: p_smode2 = v; break;
    case 0x0070: case 0x0090: {            /* --wide: a change of the displayed buffer is the game's frame flip */
        void wide_flip(u32 fbp); int i = (a & 0xFFFF) == 0x90;
        if (((p_pmode & 2) ? 1 : 0) == i && ((v ^ p_dispfb[i]) & 0x1FF)) wide_flip((u32)(v & 0x1FF) << 5);
        p_dispfb[i] = v; break;
    }
    case 0x0080: p_display[0] = v; break;
    case 0x00A0: p_display[1] = v; break;
    case 0x00E0: p_bgcolor = v; break;
    case 0x1000:
        if (v & 0x200) { p_csr = (p_csr & 0xFFFF0000u) | 0x4000u; g_gs_imr = 0xFF00; }   /* reset */
        p_csr &= ~((u32)v & 0x1F);                                  /* write 1 clears events */
        break;
    case 0x1010: g_gs_imr = (u32)v; break;
    case 0x1080: p_siglblid = v; break;
    default: break;        /* SMODE1, SRFSH, SYNCH*, SYNCV, EXT*: CRT timing, nothing to model */
    }
}
void gs_vblank(int start) {
    if (cap_f) { u8 s8 = (u8)start; cap_ev(4, &s8, 1, 0, 0); }
    if (start) {
        p_csr |= 8;
        if (!(g_gs_imr & 0x800)) intc_raise(INTC_GS);
        g_gs_frames++;
        q_publish();
        state_dirty = 1;        /* re-resolve once per frame: keeps the current texture's age fresh for tex_age */
        tex_age();
        if (cap_prefix) cap_frame();
        static u64 stat_frames;
        if (++stat_frames % 500 == 0) {
            static int log = -1;
            if (log < 0) log = getenv("ROR_FPSLOG") != 0 || gs_timing;
            if (log) {
                u64 syncs = 0; for (int i = 0; i < SY_N; i++) syncs += stat_sync[i];
                rt_log("[gs] %d threads; per frame: %.2f syncs (queue %.2f, states %.2f, serial %.2f, texture %.2f, clut %.2f, transfer %.2f, "
                       "copy %.2f, display %.2f), %.1f barriers, %.2f queued decodes, %.2f serial prims, %.0f states, %.0f transfer px; textures %llu MiB\n",
                       nthreads, syncs / 500.0, stat_sync[SY_QFULL] / 500.0, stat_sync[SY_STATES] / 500.0, stat_sync[SY_SERIAL] / 500.0,
                       stat_sync[SY_TEX] / 500.0, stat_sync[SY_CLUT] / 500.0, stat_sync[SY_XFER] / 500.0, stat_sync[SY_L2L] / 500.0,
                       stat_sync[SY_FRAME] / 500.0, stat_barriers / 500.0, stat_decodes / 500.0, stat_serial / 500.0, stat_states / 500.0,
                       stat_xfer_px / 500.0, (unsigned long long)(tex_bytes >> 20));
            }
            if (gs_timing) {
                char buf[512]; int o = 0;
                for (int i = 0; i < nthreads; i++) o += snprintf(buf + o, sizeof buf - (size_t)o, " %.2f/%.2f", workers[i].t_work / 500.0 / 1e6, workers[i].t_wait / 500.0 / 1e6);
                rt_log("[gs] Mcycles per frame: sync wait %.2f; workers work/barrier-wait:%s\n", t_sync_wait / 500.0 / 1e6, buf);
                for (int i = 0; i < nthreads; i++) workers[i].t_work = workers[i].t_wait = workers[i].t_idle = 0;      /* (racy reset: debug only) */
                t_sync_wait = 0;
            }
            memset(stat_sync, 0, sizeof stat_sync); stat_barriers = stat_decodes = stat_serial = stat_states = stat_xfer_px = 0;
        }
    } else p_csr ^= 0x2000;
}

/* What the display registers show: buffer and rectangle. Returns 0 if the display is off. */
static int disp_rect(u64 *pfb, u64 *pdp, u32 *pfbp, u32 *pfbw, u32 *ppsm, u32 *pdbx, u32 *pdby, u32 *pww, u32 *phh) {
    int en = (p_pmode & 2) ? 1 : (p_pmode & 1) ? 0 : -1;
    if (en < 0) return 0;
    u64 fb = p_dispfb[en], dp = p_display[en];
    u32 fbp = (fb & 0x1FF) << 5, fbw = (fb >> 9) & 63, psm = (fb >> 15) & 31;
    u32 dbx = (fb >> 32) & 0x7FF, dby = (fb >> 43) & 0x7FF;
    u32 magh = ((dp >> 23) & 15) + 1, dw = ((dp >> 32) & 0xFFF) + 1, dh = ((dp >> 44) & 0x7FF) + 1;
    u32 ww = dw / magh, hh = dh;
    /* Interlaced FRAME mode (FFMD = 1) shows the same half-height buffer in both
     * fields; FIELD mode (FFMD = 0) takes alternate lines of a full-height buffer. */
    if ((p_smode2 & 3) == 3) hh = dh / 2;
    if (ww == 0 || hh == 0 || fbw == 0) return 0;
    if (ww > 1024) ww = 1024;
    if (hh > 1024) hh = 1024;
    *pfb = fb; *pdp = dp; *pfbp = fbp; *pfbw = fbw; *ppsm = psm; *pdbx = dbx; *pdby = dby; *pww = ww; *phh = hh;
    return 1;
}
static int want_gpu, want_internal = 4, want_fsr3;
void gs_fsr3(int want) { want_fsr3 = want; }
void gs_renderer(int gpu, int internal) { want_gpu = gpu; want_internal = internal < 1 ? 1 : internal > 8 ? 8 : internal; }
#ifdef GS_HW
int gs_hw_active(void) { return hw_on; }
int gs_hw_attach(void *a, void *b, int vsync) { return hw_on && hw_swap_attach(a, b, vsync); }
int gs_hw_show(int dx, int dy, int dw, int dh, int ww_, int wh_, int linear, const u8 *lut, int sat64) {
    u64 fb, dp; u32 fbp, fbw, psm, dbx, dby, ww, hh;
    static const u8 *last_lut = (const u8 *)1; static u8 lut_copy[256];
    if (!hw_on) return 0;
    if (ww_ > 0 && ((lut != 0) != (last_lut != 0) || (lut && memcmp(lut, lut_copy, 256)))) { hw_set_lut(lut); if (lut) memcpy(lut_copy, lut, 256); last_lut = lut; }
    tx_flush();
    HRt *r = 0;
    if (ww_ > 0) { hwf_rect.dx = dx; hwf_rect.dy = dy; hwf_rect.dw = dw; hwf_rect.dh = dh; hwf_rect.ww = ww_; hwf_rect.wh = wh_; }
    if (disp_rect(&fb, &dp, &fbp, &fbw, &psm, &dbx, &dby, &ww, &hh) && (psm == PSMCT32 || psm == PSMCT24)) r = hwg_display_rt(fbp, fbw, dbx, dby, ww, hh);
    if (ww_ > 0) {
        hwg_grab_before();
        if (r) hwf_show(r->hw, linear, dw, dh);
        hw_present(r ? r->hw : -1, (int)dbx, (int)dby, (int)ww, (int)hh, dx, dy, dw, dh, ww_, wh_, linear, sat64);
        hwg_grab_after(100);
    } else hw_flush();
    if (clut_njobs) hwg_apply_cluts();
    return r != 0;
}
void gs_hw_present_opts(float rcas, int bloom) { if (hw_on) hw_present_opts(rcas, bloom); }
/* FSR 3.1 without a window to present to: where the picture will go in the output (as for gs_hw_show). */
void gs_hw_fsr3_rect(int dx, int dy, int dw, int dh, int ww_, int wh_) {
    if (hw_fsr3 && !hw_fps_on) { hwf_rect.dx = dx; hwf_rect.dy = dy; hwf_rect.dw = dw; hwf_rect.dh = dh; hwf_rect.ww = ww_; hwf_rect.wh = wh_; }
}
/* Frame interpolation: from now on a presenter thread owns the window. fps > 0: present at most that often. */
void gs_hw_interp(int fps, int display_hz) {
    if (!hw_on || hw_fps_on) return;
    hw_fps_target = fps; hw_fps_hz = display_hz; hw_fps_on = 1; hwi_snap_px = hwi_snap_px > 0 ? hwi_snap_px : 96.0f;
    pthread_t t;
    if (pthread_create(&t, 0, hwp_thread, 0) != 0) { hw_fps_on = 0; rt_log("[gs] cannot start the presentation thread: frame interpolation off\n"); }
}
/* Once per emulated vblank instead of gs_hw_show when interpolating: where and how to present, and the vblank's ideal time. */
void gs_hw_vblank(int dx, int dy, int dw, int dh, int ww_, int wh_, int linear, const u8 *lut, int sat64, u64 ideal_ns) {
    u64 fb, dp; u32 fbp, fbw, psm, dbx, dby, ww, hh;
    static const u8 *last_lut = (const u8 *)1; static u8 lut_copy[256];
    if (!hw_on) return;
    if ((lut != 0) != (last_lut != 0) || (lut && memcmp(lut, lut_copy, 256))) { hw_set_lut(lut); if (lut) memcpy(lut_copy, lut, 256); last_lut = lut; }
    pthread_mutex_lock(&hwp_mx);
    hwp_par.dx = dx; hwp_par.dy = dy; hwp_par.dw = dw; hwp_par.dh = dh; hwp_par.ww = ww_; hwp_par.wh = wh_; hwp_par.linear = linear; hwp_par.sat64 = sat64;
    pthread_mutex_unlock(&hwp_mx);
    hwp_ideal_ns = ideal_ns;
    tx_flush();
    u64 ta = plat_time_ns();
    if (disp_rect(&fb, &dp, &fbp, &fbw, &psm, &dbx, &dby, &ww, &hh) && (psm == PSMCT32 || psm == PSMCT24)) hwg_display_rt(fbp, fbw, dbx, dby, ww, hh);
    u64 tb = plat_time_ns();
    hw_flush();
    { static int trace = -1; static u64 last; u64 tc = plat_time_ns();
      if (trace < 0) trace = getenv("ROR_HW_PRESENT_TRACE") ? atoi(getenv("ROR_HW_PRESENT_TRACE")) : 0;
      if (trace && (int)g_gs_frames >= trace && (tc - last > 30000000ull || tc - ta > 8000000ull)) rt_log("[vtrace] vblank %llu: since last vblank call %.1f ms, late %.1f ms; boundary work %.1f ms (publish wait for GPU %.1f), flush %.1f ms\n", (unsigned long long)g_gs_frames, (tc - last) / 1e6, ((double)ta - (double)ideal_ns) / 1e6, (tb - ta) / 1e6, hwp_pub_flush_ms, (tc - tb) / 1e6);
      last = tc; }
    if (clut_njobs) hwg_apply_cluts();
}
int gs_hw_show_pixels(int dx, int dy, int dw, int dh, int ww_, int wh_, int linear, const u8 *lut, int sat64, u32 **pixels) {
    u64 fb, dp; u32 fbp, fbw, psm, dbx, dby, ww, hh;
    static u32 *buf; static size_t cap;
    if (!hw_on || ww_ <= 0 || wh_ <= 0) return 0;
    hw_set_lut(lut);
    tx_flush();
    HRt *r = 0;
    if (!hw_fps_on) { hwf_rect.dx = dx; hwf_rect.dy = dy; hwf_rect.dw = dw; hwf_rect.dh = dh; hwf_rect.ww = ww_; hwf_rect.wh = wh_; }
    if (disp_rect(&fb, &dp, &fbp, &fbw, &psm, &dbx, &dby, &ww, &hh) && (psm == PSMCT32 || psm == PSMCT24)) r = hwg_display_rt(fbp, fbw, dbx, dby, ww, hh);
    if (r) hwf_show(r->hw, linear, dw, dh);
    size_t n = (size_t)ww_ * (size_t)wh_;
    if (n > cap) { free(buf); buf = malloc(n * 4); cap = n; }
    const u32 *src = hw_present_pixels(r ? r->hw : -1, (int)dbx, (int)dby, (int)ww, (int)hh, dx, dy, dw, dh, ww_, wh_, linear, sat64);
    for (size_t i = 0; i < n; i++) { u32 c = src[i]; buf[i] = ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF); }
    if (clut_njobs) hwg_apply_cluts();
    *pixels = buf;
    return r != 0;
}
#else
void gs_hw_interp(int fps, int display_hz) { (void)fps; (void)display_hz; }
void gs_hw_fsr3_rect(int dx, int dy, int dw, int dh, int ww, int wh) { (void)dx; (void)dy; (void)dw; (void)dh; (void)ww; (void)wh; }
void gs_hw_present_opts(float rcas, int bloom) { (void)rcas; (void)bloom; }
void gs_hw_vblank(int dx, int dy, int dw, int dh, int ww, int wh, int linear, const u8 *lut, int sat64, u64 ideal_ns) { (void)dx; (void)dy; (void)dw; (void)dh; (void)ww; (void)wh; (void)linear; (void)lut; (void)sat64; (void)ideal_ns; }
int gs_hw_show_pixels(int dx, int dy, int dw, int dh, int ww, int wh, int linear, const u8 *lut, int sat64, u32 **pixels) { (void)dx; (void)dy; (void)dw; (void)dh; (void)ww; (void)wh; (void)linear; (void)lut; (void)sat64; (void)pixels; return 0; }
int gs_hw_active(void) { return 0; }
int gs_hw_attach(void *a, void *b, int vsync) { (void)a; (void)b; (void)vsync; return 0; }
int gs_hw_show(int dx, int dy, int dw, int dh, int ww, int wh, int linear, const u8 *lut, int sat64) { (void)dx; (void)dy; (void)dw; (void)dh; (void)ww; (void)wh; (void)linear; (void)lut; (void)sat64; return 0; }
#endif
int gs_get_frame(u32 **pixels, int *w, int *h) {
    u64 fb, dp; u32 fbp, fbw, psm, dbx, dby, ww, hh;
    if (!disp_rect(&fb, &dp, &fbp, &fbw, &psm, &dbx, &dby, &ww, &hh)) return 0;
    tx_flush();
#ifdef GS_HW
    if (hw_on && (psm == PSMCT32 || psm == PSMCT24) && hwg_frame(fbp, fbw, dbx, dby, ww, hh, pixels, w, h)) return 1;
#endif
    PageSet disp; rect_pages(&disp, psm, fbp, fbw, dbx, dby, ww, hh);
    {   /* Same registers and no possible write to the displayed pages since the
         * last call (every write bumps its pages' generation when it is issued):
         * the image is still the one in frame_out. */
        static u64 last_sig, last_fb, last_dp, last_mode; static int have;
        u64 sig = 0;
        for (u32 pg = 0; pg < 512; pg++) if (ps_test(&disp, pg)) sig = sig * 1099511628211ull + page_gen[pg] + 1;
        u64 mode = (p_smode2 & 3) | ((u64)ww << 8) | ((u64)hh << 32);
        if (have && sig == last_sig && fb == last_fb && dp == last_dp && mode == last_mode) {
            *pixels = frame_out; *w = (int)ww; *h = (int)hh;
            return 1;
        }
        have = 1; last_sig = sig; last_fb = fb; last_dp = dp; last_mode = mode;
    }
    if (q_pending() && ps_meet(&disp, &pend_dirty)) gs_sync(SY_FRAME);     /* wait only if the displayed buffer is still being drawn */
    if (psm == PSMCT32 || psm == PSMCT24) {
        for (u32 y = 0; y < hh; y++) {
            u32 *o = frame_out + y * ww;
            const u32 yy = dby + y, rowbase = fbp * 64 + (yy >> 5) * fbw * 2048;
            const u16 *t = swz32[yy & 31];
            for (u32 x = 0, xx = dbx; x < ww; x++, xx++) {
                u32 c = vram[(rowbase + (xx >> 6) * 2048 + t[xx & 63]) & (VRAM_WORDS - 1)];
                o[x] = ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);   /* XRGB */
            }
        }
    } else {
        for (u32 y = 0; y < hh; y++)
            for (u32 x = 0; x < ww; x++) {
                u32 c = rd_px(psm, fbp, fbw, dbx + x, dby + y);
                if (psm == PSMCT16 || psm == PSMCT16S) c = c16to32(c);
                frame_out[y * ww + x] = ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
            }
    }
    *pixels = frame_out; *w = (int)ww; *h = (int)hh;
    return 1;
}

void gs_init(void) {
#ifdef GS_HW
    {   const char *e = getenv("ROR_GS_HW");        /* replay tool: internal resolution scale; unset or 0: software */
        if (e && atoi(e) > 0) gs_renderer(1, atoi(e));
        e = getenv("ROR_HW_TEXFILTER");               /* replay tool: original | smooth (as --texture-filter) */
        if (e) want_texfilter = !strcmp(e, "smooth") ? 1 : !strcmp(e, "original") ? 0 : -1;
        if (want_gpu) {
            hw_scale = want_internal;
            hw_shblend = getenv("ROR_HW_SHBLEND") != 0;
            hwi_snap_px = 96.0f;
            if (getenv("ROR_HW_INTERP_SNAP")) hwi_snap_px = (float)atof(getenv("ROR_HW_INTERP_SNAP"));
            if (getenv("ROR_HW_INTERP")) sscanf(getenv("ROR_HW_INTERP"), "%d,%d,%d", &hw_interp_n, &hw_interp_first, &hw_interp_count);
            if (want_fsr3) hw_fsr3_want();
            if (getenv("ROR_FSR3_OVL")) hwf_ovl_scale = atoi(getenv("ROR_FSR3_OVL"));
            if (hw_start(hw_scale)) {
                /* auto: where the picture gains (from 2x) and the extra texture reads cost nothing measurable (a discrete card;
                 * +18% frame time on a software driver, so not on integrated graphics unless asked for) */
                int smooth = want_texfilter < 0 ? hw_scale >= 2 && hw_device_discrete() : want_texfilter;
                hw_on = 1; hw_tex_filter(smooth);
                rt_log("[gs] hardware renderer on %s, %dx, texture filter %s\n", hw_device_name(), hw_scale, smooth ? "smooth" : "original");
                if (want_fsr3) hw_fsr3 = hw_fsr3_init();
            }
            else rt_log("[gs] GPU renderer not available (no Vulkan 1.3 device with the needed features): using the software renderer\n");
        }
    }
#endif
    build_tables();
    build_xfer_tables();
    for (int i = 0; i < 512; i++) page_hgen[i] = ~0ull;
    pool_init();
    cap_init();
    gs_timing = getenv("ROR_GS_TIMING") != 0;
    { const char *e = getenv("ROR_GS_SPIN"); if (e) idle_spins = atoi(e); }
    { const char *e = getenv("ROR_GS_FORCE"); if (e) gs_force = atoi(e); }
    { const char *e = getenv("ROR_GSDUMP"); if (e) gs_dump_frame = strtoull(e, 0, 0); }
    for (int i = 0; i < 2; i++) { ctxt[i].sx1 = 2047; ctxt[i].sy1 = 2047; ctxt[i].zpsm = PSMZ32; }
}
