/* gs.c — Graphics Synthesizer, software implementation.
 *
 * Models the GS as the game sees it: 4 MiB of local memory in the real
 * block/column layout (so textures, frame buffers and CLUTs written in one
 * format read back correctly in another), the general registers written
 * through GIF, host->local and local->local image transfers, and a
 * rasterizer for points, lines, triangles and sprites with texturing,
 * fog, alpha/destination-alpha/depth tests and alpha blending. */
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include "gs.h"

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
}

static inline u32 a32(const u8 (*bt)[8], u32 bp, u32 bw, u32 x, u32 y) {
    u32 page = (bp >> 5) + (y >> 5) * bw + (x >> 6);
    u32 block = (bp & 31) + bt[(y >> 3) & 3][(x >> 3) & 7];
    return (((page << 5) + block) * 64 + col32[y & 7][x & 7]) & (VRAM_WORDS - 1);
}
static inline u32 a16(const u8 (*bt)[4], u32 bp, u32 bw, u32 x, u32 y) {
    u32 page = (bp >> 5) + (y >> 6) * bw + (x >> 6);
    u32 block = (bp & 31) + bt[(y >> 3) & 7][(x >> 4) & 3];
    return (((page << 5) + block) * 128 + col16[y & 7][x & 15]) & (VRAM_WORDS * 2 - 1);
}
static inline u32 a8(u32 bp, u32 bw, u32 x, u32 y) {
    u32 page = (bp >> 5) + (y >> 6) * (bw >> 1) + (x >> 7);
    u32 block = (bp & 31) + blk8[(y >> 4) & 3][(x >> 4) & 7];
    return (((page << 5) + block) * 256 + col8[y & 15][x & 15]) & (VRAM_WORDS * 4 - 1);
}
static inline u32 a4(u32 bp, u32 bw, u32 x, u32 y) {     /* nibble index */
    u32 page = (bp >> 5) + (y >> 7) * (bw >> 1) + (x >> 7);
    u32 block = (bp & 31) + blk4[(y >> 4) & 7][(x >> 5) & 3];
    return (((page << 5) + block) * 512 + col4[y & 15][x & 31]) & (VRAM_WORDS * 8 - 1);
}

/* Raw pixel access in any format. */
static u32 rd_px(u32 psm, u32 bp, u32 bw, u32 x, u32 y) {
    switch (psm) {
    case PSMCT32: return vram[a32(blk32, bp, bw, x, y)];
    case PSMCT24: return vram[a32(blk32, bp, bw, x, y)] & 0xFFFFFF;
    case PSMZ32:  return vram[a32(blk32z, bp, bw, x, y)];
    case PSMZ24:  return vram[a32(blk32z, bp, bw, x, y)] & 0xFFFFFF;
    case PSMCT16: return VRAM16[a16(blk16, bp, bw, x, y)];
    case PSMCT16S: return VRAM16[a16(blk16s, bp, bw, x, y)];
    case PSMZ16:  return VRAM16[a16(blk16z, bp, bw, x, y)];
    case PSMZ16S: return VRAM16[a16(blk16sz, bp, bw, x, y)];
    case PSMT8:   return VRAM8[a8(bp, bw, x, y)];
    case PSMT4: { u32 n = a4(bp, bw, x, y); return (VRAM8[n >> 1] >> ((n & 1) * 4)) & 15; }
    case PSMT8H:  return vram[a32(blk32, bp, bw, x, y)] >> 24;
    case PSMT4HL: return (vram[a32(blk32, bp, bw, x, y)] >> 24) & 15;
    case PSMT4HH: return vram[a32(blk32, bp, bw, x, y)] >> 28;
    }
    return 0;
}
/* Write counters per 8 KiB page of local memory; the texture cache uses them
 * to notice that the memory behind a decoded texture changed. */
static u32 page_ver[512];
static void wr_px(u32 psm, u32 bp, u32 bw, u32 x, u32 y, u32 v) {
    u32 *p;
    {   /* same page arithmetic as the address functions */
        u32 pg = bp >> 5;
        switch (psm) {
        case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: pg += (y >> 6) * bw + (x >> 6); break;
        case PSMT8: pg += (y >> 6) * (bw >> 1) + (x >> 7); break;
        case PSMT4: pg += (y >> 7) * (bw >> 1) + (x >> 7); break;
        default: pg += (y >> 5) * bw + (x >> 6); break;
        }
        page_ver[pg & 511]++;
        if (bp & 31) page_ver[(pg + 1) & 511]++;               /* not page-aligned: may spill into the next page */
    }
    switch (psm) {
    case PSMCT32: vram[a32(blk32, bp, bw, x, y)] = v; break;
    case PSMCT24: p = &vram[a32(blk32, bp, bw, x, y)]; *p = (*p & 0xFF000000u) | (v & 0xFFFFFF); break;
    case PSMZ32:  vram[a32(blk32z, bp, bw, x, y)] = v; break;
    case PSMZ24:  p = &vram[a32(blk32z, bp, bw, x, y)]; *p = (*p & 0xFF000000u) | (v & 0xFFFFFF); break;
    case PSMCT16: VRAM16[a16(blk16, bp, bw, x, y)] = (u16)v; break;
    case PSMCT16S: VRAM16[a16(blk16s, bp, bw, x, y)] = (u16)v; break;
    case PSMZ16:  VRAM16[a16(blk16z, bp, bw, x, y)] = (u16)v; break;
    case PSMZ16S: VRAM16[a16(blk16sz, bp, bw, x, y)] = (u16)v; break;
    case PSMT8:   VRAM8[a8(bp, bw, x, y)] = (u8)v; break;
    case PSMT4: { u32 n = a4(bp, bw, x, y); u8 *b = &VRAM8[n >> 1]; int s = (n & 1) * 4;
                  *b = (u8)((*b & ~(15 << s)) | ((v & 15) << s)); break; }
    case PSMT8H:  p = &vram[a32(blk32, bp, bw, x, y)]; *p = (*p & 0x00FFFFFFu) | (v << 24); break;
    case PSMT4HL: p = &vram[a32(blk32, bp, bw, x, y)]; *p = (*p & 0xF0FFFFFFu) | ((v & 15) << 24); break;
    case PSMT4HH: p = &vram[a32(blk32, bp, bw, x, y)]; *p = (*p & 0x0FFFFFFFu) | ((v & 15) << 28); break;
    }
}
static inline u32 c16to32(u32 c) {        /* RGBA5551 -> RGBA8888, A = bit 15 in bit 31 */
    return ((c & 0x1F) << 3) | ((c & 0x3E0) << 6) | ((c & 0x7C00) << 9) | ((c & 0x8000) << 16);
}
static inline u32 c32to16(u32 c) {
    return ((c >> 3) & 0x1F) | ((c >> 6) & 0x3E0) | ((c >> 9) & 0x7C00) | ((c >> 16) & 0x8000);
}

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

/* ---- CLUT ------------------------------------------------------------------ */
static u64 clut_sig;             /* hash of the CLUT buffer contents */
static void load_clut(const Ctxt *c) {
    int n4 = (c->tpsm == PSMT4 || c->tpsm == PSMT4HL || c->tpsm == PSMT4HH);
    int n8 = (c->tpsm == PSMT8 || c->tpsm == PSMT8H);
    if (!n4 && !n8) return;
    clut16 = (c->cpsm != PSMCT32);
    if (c->csm == 0) {                      /* CSM1: 16x16 (or 8x2) grid, bw = 1 */
        int count = n4 ? 16 : 256;
        u32 base = n4 ? (c->csa & 15) * 16 : 0;
        for (int i = 0; i < count; i++) {
            int j = n4 ? i : ((i & 0xE7) | ((i & 8) << 1) | ((i & 16) >> 1));
            u32 x = n4 ? (u32)(i & 7) : (u32)(j & 15), y = n4 ? (u32)(i >> 3) : (u32)(j >> 4);
            clut[(base + (u32)i) & 255] = rd_px(c->cpsm, c->cbp, 1, x, y);
        }
    } else {                                /* CSM2: linear strip described by TEXCLUT */
        u32 cbw = r_texclut & 63, cou = ((r_texclut >> 6) & 63) * 16, cov = (r_texclut >> 12) & 1023;
        int count = n4 ? 16 : 256;
        u32 base = n4 ? (c->csa & 15) * 16 : 0;
        for (int i = 0; i < count; i++)
            clut[(base + (u32)i) & 255] = rd_px(PSMCT16, c->cbp, cbw, cou + (u32)i, cov);
        clut16 = 1;
    }
    { u64 h = 1469598103934665603ull; for (int i = 0; i < 256; i++) h = (h ^ clut[i]) * 1099511628211ull; clut_sig = h ^ (u64)clut16; }
}

static void set_tex0(Ctxt *c, u64 v, int tex2) {
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
    if ((v >> 61) & 7) load_clut(c);
}

/* ---- pixel pipeline ---------------------------------------------------------- */
typedef struct {
    const Ctxt *c;
    int tme, fge, abe, iip, fst, dbg;
    u32 aa, ab, ac, ad, fix;
    int ate, atst, aref, afail, date, datm, zte, ztst;
    int pabe, colclamp, fba, need_fb;
    const u32 *tex;          /* decoded texture, or 0 to read local memory directly */
    u32 ta0, ta1, aem;
    u32 zmax;
    u8 fogr, fogg, fogb;
} Draw;
static Draw D;

typedef void (*RowFn)(const void *arg, s32 y0, s32 y1);
static void run_rows(RowFn fn, const void *arg, s32 y0, s32 y1, s64 pixels);

/* Fetch and expand one texel straight from local memory (u, v already wrapped). */
static u32 texel_fetch(const Ctxt *c, s32 u, s32 v) {
    u32 raw = rd_px(c->tpsm, c->tbp, c->tbw, (u32)u, (u32)v), col;
    int is16 = 0;
    switch (c->tpsm) {
    case PSMCT32: return raw;
    case PSMCT24:
        return raw | ((D.aem && !raw) ? 0 : D.ta0 << 24);
    case PSMCT16: case PSMCT16S: col = raw; is16 = 1; break;
    case PSMT4: case PSMT4HL: case PSMT4HH:
        col = clut[((c->csa & 15) * 16 + raw) & 255]; is16 = clut16; break;
    default:
        col = clut[raw & 255]; is16 = clut16; break;
    }
    if (!is16) return col;
    u32 rgb = c16to32(col) & 0xFFFFFF;
    u32 a = (col & 0x8000) ? D.ta1 : ((D.aem && !(col & 0x7FFF)) ? 0 : D.ta0);
    return rgb | (a << 24);
}


/* ---- decoded-texture cache --------------------------------------------------
 * A texture is expanded once into a linear RGBA array and reused while the
 * pages it lives in, its CLUT and TEXA are unchanged. Textures that keep
 * changing within a frame (the frame buffer used as a texture) bypass it. */
typedef struct { u64 key, sig, clut_sig; u32 texa; u32 *px; u32 w, h; u64 inval_frame; u32 inval_count; u64 volatile_until; } TexEnt;
#define TEXCACHE 1024
static TexEnt texcache[TEXCACHE];
static u64 tex_pages_sig(const Ctxt *c) {
    u32 w = 1u << c->tw, h = 1u << c->th, first = c->tbp >> 5, last;
    switch (c->tpsm) {
    case PSMCT16: case PSMCT16S: last = first + ((h - 1) >> 6) * c->tbw + ((w - 1) >> 6); break;
    case PSMT8: last = first + ((h - 1) >> 6) * (c->tbw >> 1) + ((w - 1) >> 7); break;
    case PSMT4: last = first + ((h - 1) >> 7) * (c->tbw >> 1) + ((w - 1) >> 7); break;
    default: last = first + ((h - 1) >> 5) * c->tbw + ((w - 1) >> 6); break;
    }
    u64 sig = 0;
    if (c->tbp & 31) last++;
    for (u32 pg = first; pg <= last; pg++) sig = sig * 1099511628211ull + page_ver[pg & 511] + 1;
    return sig;
}
static const u32 *texcache_get(const Ctxt *c) {
    int paletted = !(c->tpsm == PSMCT32 || c->tpsm == PSMCT24 || c->tpsm == PSMCT16 || c->tpsm == PSMCT16S);
    u64 key = (u64)c->tbp | ((u64)c->tbw << 14) | ((u64)c->tpsm << 20) | ((u64)c->tw << 26) | ((u64)c->th << 30) |
              ((u64)(paletted ? c->csa : 0) << 34);
    u32 texa = D.ta0 | (D.ta1 << 8) | (D.aem << 16);
    u64 sig = tex_pages_sig(c), cs = paletted ? clut_sig : 0;
    /* the palette is part of the identity: the same texels are routinely drawn
     * with several CLUTs in turn */
    key ^= cs * 0xD6E8FEB86659FD93ull;
    TexEnt *e = &texcache[(key * 0x9E3779B97F4A7C15ull) >> 54];
    if (e->px && e->key == key && e->sig == sig && e->clut_sig == cs && e->texa == texa) return e->px;
    if (e->key == key) {
        if (e->volatile_until > g_gs_frames) return 0;
        if (e->inval_frame == g_gs_frames) { if (++e->inval_count >= 2) { e->volatile_until = g_gs_frames + 30; return 0; } }
        else { e->inval_frame = g_gs_frames; e->inval_count = 0; }
    } else { e->inval_frame = g_gs_frames; e->inval_count = 0; e->volatile_until = 0; }
    u32 w = 1u << c->tw, h = 1u << c->th;
    if (!e->px || e->w * e->h < w * h) { free(e->px); e->px = malloc((size_t)w * h * 4); }
    e->key = key; e->sig = sig; e->clut_sig = cs; e->texa = texa; e->w = w; e->h = h;
    for (u32 y = 0; y < h; y++) for (u32 x = 0; x < w; x++) e->px[(y << c->tw) + x] = texel_fetch(c, (s32)x, (s32)y);
    g_gs_tex_rebuilt += (u64)w * h;
    return e->px;
}

static inline u32 texel(const Ctxt *c, s32 u, s32 v) {
    s32 w = 1 << c->tw, h = 1 << c->th;
    switch (c->wms) {
    case 0: u &= w - 1; break;
    case 1: u = u < 0 ? 0 : u >= w ? w - 1 : u; break;
    case 2: u = u < (s32)c->minu ? (s32)c->minu : u > (s32)c->maxu ? (s32)c->maxu : u; break;
    default: u = (u & (s32)c->minu) | (s32)c->maxu; break;
    }
    switch (c->wmt) {
    case 0: v &= h - 1; break;
    case 1: v = v < 0 ? 0 : v >= h ? h - 1 : v; break;
    case 2: v = v < (s32)c->minv ? (s32)c->minv : v > (s32)c->maxv ? (s32)c->maxv : v; break;
    default: v = (v & (s32)c->minv) | (s32)c->maxv; break;
    }
    if (D.tex && (u32)u < (1u << c->tw) && (u32)v < (1u << c->th)) return D.tex[((u32)v << c->tw) + (u32)u];
    return texel_fetch(c, u, v);
}

static inline u32 sample(const Ctxt *c, float u, float v) {     /* u,v in texels */
    if (!c->mmag) return texel(c, (s32)floorf(u), (s32)floorf(v));
    u -= 0.5f; v -= 0.5f;
    s32 iu = (s32)floorf(u), iv = (s32)floorf(v);
    u32 fu = (u32)((u - (float)iu) * 256.0f), fv = (u32)((v - (float)iv) * 256.0f);
    u32 t00 = texel(c, iu, iv), t10 = texel(c, iu + 1, iv), t01 = texel(c, iu, iv + 1), t11 = texel(c, iu + 1, iv + 1);
    u32 out = 0;
    for (int s = 0; s < 32; s += 8) {
        u32 a = (t00 >> s) & 255, b = (t10 >> s) & 255, cc = (t01 >> s) & 255, d = (t11 >> s) & 255;
        u32 top = a * (256 - fu) + b * fu, bot = cc * (256 - fu) + d * fu;
        out |= (((top * (256 - fv) + bot * fv) >> 16) & 255) << s;
    }
    return out;
}

static inline int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* Shade and write one pixel. (r,g,b,a) is the interpolated vertex colour. */
static inline void pixel(s32 x, s32 y, u32 z, int r, int g, int b, int a, float tu, float tv, int fog) {
    const Ctxt *c = D.c;
    if (D.tme) {
        u32 t = sample(c, tu, tv);
        int tr = t & 255, tg = (t >> 8) & 255, tb = (t >> 16) & 255, ta = t >> 24;
        switch (c->tfx) {
        case 0:   /* modulate */
            r = clamp8((tr * r) >> 7); g = clamp8((tg * g) >> 7); b = clamp8((tb * b) >> 7);
            if (c->tcc) a = clamp8((ta * a) >> 7);
            break;
        case 1:   /* decal */
            r = tr; g = tg; b = tb;
            if (c->tcc) a = ta;
            break;
        case 2:   /* highlight */
            r = clamp8(((tr * r) >> 7) + a); g = clamp8(((tg * g) >> 7) + a); b = clamp8(((tb * b) >> 7) + a);
            if (c->tcc) a = clamp8(ta + a);
            break;
        default:  /* highlight 2 */
            r = clamp8(((tr * r) >> 7) + a); g = clamp8(((tg * g) >> 7) + a); b = clamp8(((tb * b) >> 7) + a);
            if (c->tcc) a = ta;
            break;
        }
    }
    if (D.fge) {
        r = (r * fog + D.fogr * (255 - fog)) >> 8;
        g = (g * fog + D.fogg * (255 - fog)) >> 8;
        b = (b * fog + D.fogb * (255 - fog)) >> 8;
    }
    int wr_rgb = 1, wr_a = 1, wr_z = !c->zmsk;
    if (D.ate) {
        int pass;
        switch (D.atst) {
        case 0: pass = 0; break;        case 1: pass = 1; break;
        case 2: pass = a < D.aref; break;  case 3: pass = a <= D.aref; break;
        case 4: pass = a == D.aref; break; case 5: pass = a >= D.aref; break;
        case 6: pass = a > D.aref; break;  default: pass = a != D.aref; break;
        }
        if (!pass) switch (D.afail) {
            case 0: return;
            case 1: wr_z = 0; break;
            case 2: wr_rgb = wr_a = 0; break;
            default: wr_a = 0; wr_z = 0; break;
        }
    }
    u32 fa = 0;                           /* frame pixel as RGBA8888 (read only when something needs it) */
    int f16 = (c->fpsm == PSMCT16 || c->fpsm == PSMCT16S), f32 = (c->fpsm == PSMCT32);
    u32 *fp32 = f32 ? &vram[a32(blk32, c->fbp, c->fbw, (u32)x, (u32)y)] : 0;
    if (D.need_fb || !wr_rgb || !wr_a) {
        u32 fraw = f32 ? *fp32 : rd_px(c->fpsm, c->fbp, c->fbw, (u32)x, (u32)y);
        if (f16) fa = c16to32(fraw);
        else if (c->fpsm == PSMCT24) fa = fraw | 0x80000000u;
        else fa = fraw;
    }
    if (D.date && c->fpsm != PSMCT24 && ((fa >> 31) != (u32)D.datm)) return;
    if (D.zte) {
        if (z > D.zmax) z = D.zmax;
        if (D.ztst == 0) return;
        if (D.ztst >= 2) {
            u32 zb = rd_px(c->zpsm, c->zbp, c->fbw, (u32)x, (u32)y);
            if (D.ztst == 2 ? z < zb : z <= zb) return;
        }
    } else wr_z = 0;
    if (D.abe && (!D.pabe || (a & 0x80))) {
        int dr = fa & 255, dg = (fa >> 8) & 255, db = (fa >> 16) & 255, da = fa >> 24;
        int cf = D.ac == 0 ? a : D.ac == 1 ? da : (int)D.fix;
        int A[3] = { D.aa == 0 ? r : D.aa == 1 ? dr : 0, D.aa == 0 ? g : D.aa == 1 ? dg : 0, D.aa == 0 ? b : D.aa == 1 ? db : 0 };
        int B[3] = { D.ab == 0 ? r : D.ab == 1 ? dr : 0, D.ab == 0 ? g : D.ab == 1 ? dg : 0, D.ab == 0 ? b : D.ab == 1 ? db : 0 };
        int Dd[3] = { D.ad == 0 ? r : D.ad == 1 ? dr : 0, D.ad == 0 ? g : D.ad == 1 ? dg : 0, D.ad == 0 ? b : D.ad == 1 ? db : 0 };
        r = (((A[0] - B[0]) * cf) >> 7) + Dd[0];
        g = (((A[1] - B[1]) * cf) >> 7) + Dd[1];
        b = (((A[2] - B[2]) * cf) >> 7) + Dd[2];
        if (D.colclamp) { r = clamp8(r); g = clamp8(g); b = clamp8(b); }
        else { r &= 255; g &= 255; b &= 255; }
    }
    if (wr_rgb || wr_a) {
        if (D.fba) a |= 0x80;
        u32 col = (u32)r | ((u32)g << 8) | ((u32)b << 16) | ((u32)a << 24);
        u32 mask = c->fbmsk;
        if (!wr_rgb) mask |= 0x00FFFFFFu;
        if (!wr_a) mask |= 0xFF000000u;
        col = (col & ~mask) | (fa & mask);
        if (f32) { *fp32 = col; page_ver[((c->fbp >> 5) + ((u32)y >> 5) * c->fbw + ((u32)x >> 6)) & 511]++; }
        else wr_px(c->fpsm, c->fbp, c->fbw, (u32)x, (u32)y, f16 ? c32to16(col) : col);
    }
    if (wr_z) wr_px(c->zpsm, c->zbp, c->fbw, (u32)x, (u32)y, z);
}

static void setup_draw(void) {
    u64 pr = r_prmodecont ? r_prim : ((r_prim & 7) | (r_prmode & ~7ull));
    const Ctxt *c = &ctxt[(pr >> 9) & 1];
    D.c = c;
    D.iip = (pr >> 3) & 1; D.tme = (pr >> 4) & 1; D.fge = (pr >> 5) & 1;
    D.abe = (pr >> 6) & 1; D.fst = (pr >> 8) & 1;
    D.aa = c->alpha & 3; D.ab = (c->alpha >> 2) & 3; D.ac = (c->alpha >> 4) & 3; D.ad = (c->alpha >> 6) & 3;
    D.fix = (c->alpha >> 32) & 255;
    D.ate = c->test & 1; D.atst = (c->test >> 1) & 7; D.aref = (c->test >> 4) & 255; D.afail = (c->test >> 12) & 3;
    D.date = (c->test >> 14) & 1; D.datm = (c->test >> 15) & 1; D.zte = (c->test >> 16) & 1; D.ztst = (c->test >> 17) & 3;
    D.pabe = r_pabe & 1; D.colclamp = r_colclamp & 1; D.fba = c->fba & 1;
    D.ta0 = r_texa & 255; D.aem = (r_texa >> 15) & 1; D.ta1 = (r_texa >> 32) & 255;
    D.zmax = c->zpsm == PSMZ32 ? 0xFFFFFFFFu : c->zpsm == PSMZ24 ? 0xFFFFFFu : 0xFFFFu;
    D.need_fb = D.abe || D.date || c->fbmsk != 0;
    D.tex = 0;
    if (D.tme) D.tex = texcache_get(c);
    D.fogr = r_fogcol & 255; D.fogg = (r_fogcol >> 8) & 255; D.fogb = (r_fogcol >> 16) & 255;
}

typedef struct { const Vtx *b; s32 x0, y0, px0, px1; float u0, v0, dudx, dvdy; } SpriteJob;
static void sprite_rows(const void *arg, s32 ya, s32 yb) {
    const SpriteJob *j = arg; const Vtx *b = j->b;
    for (s32 y = ya; y < yb; y++) {
        float tv = j->v0 + ((float)(y * 16 - j->y0) / 16.0f) * j->dvdy;
        for (s32 x = j->px0; x < j->px1; x++) {
            float tu = j->u0 + ((float)(x * 16 - j->x0) / 16.0f) * j->dudx;
            pixel(x, y, b->z, b->r, b->g, b->b, b->a, tu, tv, b->f);
        }
    }
}
static void draw_sprite(const Vtx *a, const Vtx *b) {
    const Ctxt *c = D.c;
    s32 x0 = a->x, y0 = a->y, x1 = b->x, y1 = b->y;
    float u0, v0, u1, v1;
    if (D.fst) { u0 = (float)a->u / 16.0f; v0 = (float)a->v / 16.0f; u1 = (float)b->u / 16.0f; v1 = (float)b->v / 16.0f; }
    else {
        float q = b->q != 0.0f ? b->q : 1.0f, tw = (float)(1 << c->tw), th = (float)(1 << c->th);
        u0 = a->s / q * tw; v0 = a->t / q * th; u1 = b->s / q * tw; v1 = b->t / q * th;
    }
    if (x0 > x1) { s32 t = x0; x0 = x1; x1 = t; float f = u0; u0 = u1; u1 = f; }
    if (y0 > y1) { s32 t = y0; y0 = y1; y1 = t; float f = v0; v0 = v1; v1 = f; }
    if (x1 == x0 || y1 == y0) return;
    float dudx = (u1 - u0) * 16.0f / (float)(x1 - x0), dvdy = (v1 - v0) * 16.0f / (float)(y1 - y0);
    s32 px0 = (x0 + 15) >> 4, px1 = (x1 + 15) >> 4, py0 = (y0 + 15) >> 4, py1 = (y1 + 15) >> 4;   /* [..) */
    if (px0 < c->sx0) px0 = c->sx0;
    if (py0 < c->sy0) py0 = c->sy0;
    if (px1 > c->sx1 + 1) px1 = c->sx1 + 1;
    if (py1 > c->sy1 + 1) py1 = c->sy1 + 1;
    SpriteJob j = { b, x0, y0, px0, px1, u0, v0, dudx, dvdy };
    run_rows(sprite_rows, &j, py0, py1, (s64)(px1 - px0) * (py1 - py0));
}

typedef struct { const Vtx *v0, *v1, *v2, *flat; s32 px0, px1, py0; s64 dx[3], dy[3], w0[3]; int tl[3]; double inv; } TriJob;
static void tri_rows(const void *arg, s32 ya, s32 yb) {
    const TriJob *j = arg; const Ctxt *c = D.c;
    const Vtx *v0 = j->v0, *v1 = j->v1, *v2 = j->v2, *flat = j->flat;
    double inv = j->inv;
    float tw = (float)(1 << c->tw), th = (float)(1 << c->th);
    for (s32 y = ya; y < yb; y++) {
        s64 w0 = j->w0[0] + j->dx[0] * 16 * (y - j->py0), w1 = j->w0[1] + j->dx[1] * 16 * (y - j->py0),
            w2 = j->w0[2] + j->dx[2] * 16 * (y - j->py0);
        for (s32 x = j->px0; x <= j->px1; x++) {
            if ((w0 > 0 || (w0 == 0 && j->tl[0])) && (w1 > 0 || (w1 == 0 && j->tl[1])) && (w2 > 0 || (w2 == 0 && j->tl[2]))) {
                double b0 = (double)w0 * inv, b1 = (double)w1 * inv, b2 = (double)w2 * inv;
                u32 z = (u32)((double)v0->z * b0 + (double)v1->z * b1 + (double)v2->z * b2);
                int r, g, b, a;
                if (D.iip) {
                    r = (int)(v0->r * b0 + v1->r * b1 + v2->r * b2 + 0.5);
                    g = (int)(v0->g * b0 + v1->g * b1 + v2->g * b2 + 0.5);
                    b = (int)(v0->b * b0 + v1->b * b1 + v2->b * b2 + 0.5);
                    a = (int)(v0->a * b0 + v1->a * b1 + v2->a * b2 + 0.5);
                } else { r = flat->r; g = flat->g; b = flat->b; a = flat->a; }
                float tu = 0, tv = 0;
                if (D.tme) {
                    if (D.fst) {
                        tu = (float)(v0->u * b0 + v1->u * b1 + v2->u * b2) / 16.0f;
                        tv = (float)(v0->v * b0 + v1->v * b1 + v2->v * b2) / 16.0f;
                    } else {
                        double q = v0->q * b0 + v1->q * b1 + v2->q * b2;
                        if (q == 0.0) q = 1.0;
                        tu = (float)((v0->s * b0 + v1->s * b1 + v2->s * b2) / q) * tw;
                        tv = (float)((v0->t * b0 + v1->t * b1 + v2->t * b2) / q) * th;
                    }
                }
                int fog = D.fge ? (int)(v0->f * b0 + v1->f * b1 + v2->f * b2) : 0;
                pixel(x, y, z, r, g, b, a, tu, tv, fog);
            }
            w0 -= j->dy[0] * 16; w1 -= j->dy[1] * 16; w2 -= j->dy[2] * 16;
        }
    }
}
static void draw_triangle(const Vtx *v0, const Vtx *v1, const Vtx *v2) {
    const Ctxt *c = D.c;
    const Vtx *flat = v2;
    s64 area = (s64)(v1->x - v0->x) * (v2->y - v0->y) - (s64)(v1->y - v0->y) * (v2->x - v0->x);
    if (area == 0) return;
    if (area < 0) { const Vtx *t = v1; v1 = v2; v2 = t; area = -area; }
    s32 minx = v0->x, maxx = v0->x, miny = v0->y, maxy = v0->y;
    if (v1->x < minx) minx = v1->x; if (v2->x < minx) minx = v2->x;
    if (v1->x > maxx) maxx = v1->x; if (v2->x > maxx) maxx = v2->x;
    if (v1->y < miny) miny = v1->y; if (v2->y < miny) miny = v2->y;
    if (v1->y > maxy) maxy = v1->y; if (v2->y > maxy) maxy = v2->y;
    s32 px0 = (minx + 15) >> 4, px1 = maxx >> 4, py0 = (miny + 15) >> 4, py1 = maxy >> 4;
    if (px0 < c->sx0) px0 = c->sx0;
    if (py0 < c->sy0) py0 = c->sy0;
    if (px1 > c->sx1) px1 = c->sx1;
    if (py1 > c->sy1) py1 = c->sy1;
    if (px0 > px1 || py0 > py1) return;
    /* edge i is opposite vertex i; inside when w > 0, or w == 0 on a top/left edge */
    const Vtx *e[3][2] = { { v1, v2 }, { v2, v0 }, { v0, v1 } };
    s64 dx[3], dy[3], w_row[3]; int tl[3];
    for (int i = 0; i < 3; i++) {
        dx[i] = e[i][1]->x - e[i][0]->x; dy[i] = e[i][1]->y - e[i][0]->y;
        tl[i] = (dy[i] < 0) || (dy[i] == 0 && dx[i] > 0);
        w_row[i] = dx[i] * ((s64)py0 * 16 - e[i][0]->y) - dy[i] * ((s64)px0 * 16 - e[i][0]->x);
    }
    TriJob j = { v0, v1, v2, flat, px0, px1, py0, { dx[0], dx[1], dx[2] }, { dy[0], dy[1], dy[2] },
                 { w_row[0], w_row[1], w_row[2] }, { tl[0], tl[1], tl[2] }, 1.0 / (double)area };
    run_rows(tri_rows, &j, py0, py1 + 1, (s64)(px1 - px0 + 1) * (py1 - py0 + 1) / 2);
}

static void tex_of(const Vtx *v, float *tu, float *tv) {
    const Ctxt *c = D.c;
    if (D.fst) { *tu = (float)v->u / 16.0f; *tv = (float)v->v / 16.0f; }
    else { float q = v->q != 0.0f ? v->q : 1.0f; *tu = v->s / q * (float)(1 << c->tw); *tv = v->t / q * (float)(1 << c->th); }
}
static void draw_point(const Vtx *v) {
    const Ctxt *c = D.c;
    s32 x = (v->x + 8) >> 4, y = (v->y + 8) >> 4;
    if (x < c->sx0 || x > c->sx1 || y < c->sy0 || y > c->sy1) return;
    float tu, tv; tex_of(v, &tu, &tv);
    pixel(x, y, v->z, v->r, v->g, v->b, v->a, tu, tv, v->f);
}
static void draw_line(const Vtx *a, const Vtx *b) {
    const Ctxt *c = D.c;
    s32 dx = b->x - a->x, dy = b->y - a->y;
    s32 adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    s32 steps = ((adx > ady ? adx : ady) + 15) >> 4;
    if (steps <= 0) { draw_point(b); return; }
    float au, av, bu, bv; tex_of(a, &au, &av); tex_of(b, &bu, &bv);
    for (s32 i = 0; i < steps; i++) {             /* last point excluded */
        double t = (double)i / (double)steps;
        s32 x = (s32)((a->x + dx * t) + 8) >> 4, y = (s32)((a->y + dy * t) + 8) >> 4;
        if (x < c->sx0 || x > c->sx1 || y < c->sy0 || y > c->sy1) continue;
        int r = b->r, g = b->g, bl = b->b, al = b->a;
        if (D.iip) {
            r = (int)(a->r + (b->r - a->r) * t); g = (int)(a->g + (b->g - a->g) * t);
            bl = (int)(a->b + (b->b - a->b) * t); al = (int)(a->a + (b->a - a->a) * t);
        }
        pixel(x, y, (u32)(a->z + ((double)b->z - (double)a->z) * t), r, g, bl, al,
              (float)(au + (bu - au) * t), (float)(av + (bv - av) * t), (int)(a->f + (b->f - a->f) * t));
    }
}

/* Debug: ROR_GSDUMP=<frame> logs that frame's draw calls, grouped by state. */
static void dump_prim(int type) {
    static u64 last_key[6]; static u32 run; static s32 bx0, by0, bx1, by1; static u32 zmin, zmax;
    const Ctxt *c = D.c;
    u64 key[6] = { r_prim, c->tex0, c->test, c->alpha, c->frame, c->zbuf };
    int n = type == 0 ? 1 : (type == 1 || type == 2 || type == 6) ? 2 : 3;
    if (memcmp(key, last_key, sizeof key) != 0) {
        if (D.tme) {                                   /* save the texture once (RGB, then alpha) */
            static u32 saved[64]; static int ns; int seen = 0;
            for (int i = 0; i < ns; i++) if (saved[i] == c->tbp) seen = 1;
            if (!seen && ns < 64) {
                saved[ns++] = c->tbp;
                int w = 1 << c->tw, h = 1 << c->th;
                u32 *px = malloc((size_t)w * 2 * (size_t)h * 4);
                for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                    u32 t = texel(c, x, y), a8 = t >> 24; a8 = a8 > 127 ? 255 : a8 * 2;
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
               type, D.iip, D.tme, D.fge, D.abe, D.fst, (int)((r_prim >> 9) & 1), c->tbp, c->tbw, c->tpsm, 1u << c->tw, 1u << c->th,
               c->tcc, c->tfx, c->cbp, D.ate, D.atst, D.aref, D.afail, D.date, D.zte, D.ztst, D.aa, D.ab, D.ac, D.ad, D.fix,
               c->fbp, c->fbw, c->fpsm, c->fbmsk, c->zbp, c->zpsm, c->zmsk);
    }
    if (run < 2) for (int i = 0; i < n; i++)
        rt_log("       v%d xy=(%.1f,%.1f) z=%x rgba=%02x%02x%02x%02x stq=(%g,%g,%g) uv=(%u,%u)\n", i, vq[i].x / 16.0, vq[i].y / 16.0, vq[i].z,
               vq[i].r, vq[i].g, vq[i].b, vq[i].a, vq[i].s, vq[i].t, vq[i].q, vq[i].u, vq[i].v);
    run++;
    for (int i = 0; i < n; i++) {
        if (vq[i].x < bx0) bx0 = vq[i].x; if (vq[i].x > bx1) bx1 = vq[i].x;
        if (vq[i].y < by0) by0 = vq[i].y; if (vq[i].y > by1) by1 = vq[i].y;
        if (vq[i].z < zmin) zmin = vq[i].z; if (vq[i].z > zmax) zmax = vq[i].z;
    }
}

/* ---- row-parallel rasterization ---------------------------------------------
 * A large primitive is split into bands of rows shaded by worker threads.
 * Within one primitive every pixel is independent (it reads and writes only
 * its own frame/Z location), so the result is identical to the serial order. */
#define ROW_CHUNK 8
static struct {
    pthread_mutex_t m; pthread_cond_t cv;
    RowFn fn; const void *arg; s32 y1;
    atomic_int next, pending;
    int nthreads;
} pool = { .m = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

static void pool_work(void) {
    for (;;) {
        s32 y = atomic_fetch_add(&pool.next, ROW_CHUNK);
        if (y >= pool.y1) break;
        pool.fn(pool.arg, y, y + ROW_CHUNK < pool.y1 ? y + ROW_CHUNK : pool.y1);
    }
}
/* Workers poll for the next job for a short while before sleeping: during
 * rendering jobs arrive microseconds apart, and a sleeping thread takes far
 * longer to wake than a medium triangle takes to shade. */
static atomic_ullong pool_gen;
static atomic_int pool_sleepers;
static void *pool_thread(void *unused) {
    (void)unused;
    unsigned long long seen = 0;
    for (;;) {
        int spins = 0;
        while (atomic_load(&pool_gen) == seen) {
            if (++spins < 20000) { __builtin_ia32_pause(); continue; }
            pthread_mutex_lock(&pool.m);
            atomic_fetch_add(&pool_sleepers, 1);
            while (atomic_load(&pool_gen) == seen) pthread_cond_wait(&pool.cv, &pool.m);
            atomic_fetch_sub(&pool_sleepers, 1);
            pthread_mutex_unlock(&pool.m);
        }
        seen = atomic_load(&pool_gen);
        pool_work();
        atomic_fetch_sub(&pool.pending, 1);
    }
    return 0;
}
static void pool_init(void) {
    const char *e = getenv("ROR_GS_THREADS");
    long n = e ? atol(e) : sysconf(_SC_NPROCESSORS_ONLN) / 2 - 1;
    if (n > 5) n = 5;
    for (long i = 0; i < n; i++) {
        pthread_t t;
        if (pthread_create(&t, 0, pool_thread, 0) != 0) break;
        pool.nthreads++;
    }
}
static void run_rows(RowFn fn, const void *arg, s32 y0, s32 y1, s64 pixels) {
    if (y1 <= y0) return;
    g_gs_stat[pixels < 2500 ? 0 : pixels < 60000 ? 1 : 2] += (u64)pixels; g_gs_stat[3 + (pixels < 2500 ? 0 : pixels < 60000 ? 1 : 2)]++;
    if (pool.nthreads == 0 || pixels < 2500) { fn(arg, y0, y1); return; }
    pool.fn = fn; pool.arg = arg; pool.y1 = y1;
    atomic_store(&pool.next, y0);
    atomic_store(&pool.pending, pool.nthreads);
    atomic_fetch_add(&pool_gen, 1);                 /* publishes the job */
    if (atomic_load(&pool_sleepers)) {
        pthread_mutex_lock(&pool.m);
        pthread_cond_broadcast(&pool.cv);
        pthread_mutex_unlock(&pool.m);
    }
    pool_work();
    while (atomic_load(&pool.pending)) __builtin_ia32_pause();
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
    if (draw) {
        setup_draw();
        g_gs_prims++;
        if (gs_dump_frame && g_gs_frames == gs_dump_frame) dump_prim(type);
        switch (type) {
        case 0: draw_point(&vq[0]); break;
        case 1: case 2: draw_line(&vq[0], &vq[1]); break;
        case 3: case 4: case 5: draw_triangle(&vq[0], &vq[1], &vq[2]); break;
        case 6: draw_sprite(&vq[0], &vq[1]); break;
        }
    }
    switch (type) {            /* what stays in the queue for the next primitive */
    case 0: case 1: case 3: case 6: vcount = 0; break;
    case 2: vq[0] = vq[1]; vcount = 1; break;
    case 4: vq[0] = vq[1]; vq[1] = vq[2]; vcount = 2; break;
    case 5: vq[1] = vq[2]; vcount = 2; break;
    }
}

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
    SNAP(vram); SNAP(ctxt);
    SNAP(r_prim); SNAP(r_prmode); SNAP(r_prmodecont); SNAP(r_texclut); SNAP(r_texa); SNAP(r_fogcol); SNAP(r_pabe);
    SNAP(r_colclamp); SNAP(r_dthe); SNAP(r_bitblt); SNAP(r_trxpos); SNAP(r_trxreg); SNAP(r_trxdir);
    SNAP(r_rgbaq); SNAP(r_q); SNAP(r_s); SNAP(r_t); SNAP(r_u); SNAP(r_v); SNAP(r_fog);
    SNAP(vq); SNAP(vcount); SNAP(clut); SNAP(clut16); SNAP(clut_sig); SNAP(tx);
    SNAP(p_pmode); SNAP(p_smode2); SNAP(p_dispfb); SNAP(p_display); SNAP(p_bgcolor); SNAP(p_siglblid); SNAP(p_csr);
    SNAP(g_gs_imr); SNAP(g_gs_frames);
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
static void local_to_local(void) {
    u32 sbp = r_bitblt & 0x3FFF, sbw = (r_bitblt >> 16) & 63, spsm = (r_bitblt >> 24) & 63;
    u32 dbp = (r_bitblt >> 32) & 0x3FFF, dbw = (r_bitblt >> 48) & 63, dpsm = (r_bitblt >> 56) & 63;
    u32 ssax = r_trxpos & 0x7FF, ssay = (r_trxpos >> 16) & 0x7FF, dsax = (r_trxpos >> 32) & 0x7FF, dsay = (r_trxpos >> 48) & 0x7FF;
    u32 dir = (r_trxpos >> 59) & 3, w = r_trxreg & 0xFFF, h = (r_trxreg >> 32) & 0xFFF;
    for (u32 j = 0; j < h; j++) {
        u32 yy = (dir & 1) ? h - 1 - j : j;
        for (u32 i = 0; i < w; i++) {
            u32 xx = (dir & 2) ? w - 1 - i : i;
            wr_px(dpsm, dbp, dbw, (dsax + xx) & 2047, (dsay + yy) & 2047,
                  rd_px(spsm, sbp, sbw, (ssax + xx) & 2047, (ssay + yy) & 2047));
        }
    }
}

static void write_hwreg(const u8 *data, u32 bytes) {
    if (!tx.active) return;
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
        if (++tx.x >= w) { tx.x = 0; tx.y++; }
    }
    tx.active = 0;
}

/* ---- general registers ------------------------------------------------------------ */
void gs_write_hwreg(const u8 *data, u32 bytes) {
    if (cap_f) cap_ev(2, &bytes, 4, data, bytes);
    write_hwreg(data, bytes);
}
void gs_set_q(float q) { if (cap_f) cap_ev(5, &q, 4, 0, 0); r_q = q; }

void gs_write_reg(u32 reg, u64 d) {
    if (cap_f) { u8 r8 = (u8)reg; cap_ev(1, &r8, 1, &d, 8); }
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
    case 0x06: case 0x07: set_tex0(c, d, 0); break;
    case 0x08: case 0x09:
        c->clamp = d; c->wms = d & 3; c->wmt = (d >> 2) & 3;
        c->minu = (d >> 4) & 1023; c->maxu = (d >> 14) & 1023; c->minv = (d >> 24) & 1023; c->maxv = (d >> 34) & 1023;
        break;
    case 0x0A: r_fog = (u32)(d >> 56); break;
    case 0x0C: vertex_kick(d & 0xFFFF, (d >> 16) & 0xFFFF, (d >> 32) & 0xFFFFFF, (u32)(d >> 56), 0); break;
    case 0x0D: vertex_kick(d & 0xFFFF, (d >> 16) & 0xFFFF, (u32)(d >> 32), r_fog, 0); break;
    case 0x14: case 0x15: c->tex1 = d; c->mmag = (d >> 5) & 1; break;
    case 0x16: case 0x17: set_tex0(c, d, 1); break;
    case 0x18: case 0x19: c->xyoffset = d; c->ofx = d & 0xFFFF; c->ofy = (d >> 32) & 0xFFFF; break;
    case 0x1A: r_prmodecont = d & 1; break;
    case 0x1B: r_prmode = d & 0x7F8; break;
    case 0x1C: r_texclut = d; break;
    case 0x22: case 0x34: case 0x35: case 0x36: case 0x37: break;   /* SCANMSK, MIPTBP: not modelled */
    case 0x3B: r_texa = d; break;
    case 0x3D: r_fogcol = d; break;
    case 0x3F: break;                                              /* TEXFLUSH */
    case 0x40: case 0x41:
        c->scissor = d; c->sx0 = d & 0x7FF; c->sx1 = (d >> 16) & 0x7FF; c->sy0 = (d >> 32) & 0x7FF; c->sy1 = (d >> 48) & 0x7FF;
        break;
    case 0x42: case 0x43: c->alpha = d; break;
    case 0x44: break;                                              /* DIMX */
    case 0x45: r_dthe = d; break;
    case 0x46: r_colclamp = d; break;
    case 0x47: case 0x48: ctxt[reg - 0x47].test = d; break;   /* TEST_1 is the odd one of this pair */
    case 0x49: r_pabe = d; break;
    case 0x4A: case 0x4B: c->fba = d; break;
    case 0x4C: case 0x4D:
        c->frame = d; c->fbp = (d & 0x1FF) << 5; c->fbw = (d >> 16) & 63; c->fpsm = (d >> 24) & 63; c->fbmsk = (u32)(d >> 32);
        break;
    case 0x4E: case 0x4F:
        c->zbuf = d; c->zbp = (d & 0x1FF) << 5; c->zpsm = 0x30 | ((d >> 24) & 15); c->zmsk = (d >> 32) & 1;
        break;
    case 0x50: r_bitblt = d; break;
    case 0x51: r_trxpos = d; break;
    case 0x52: r_trxreg = d; break;
    case 0x53:
        r_trxdir = d & 3;
        tx.x = tx.y = 0; tx.active = 0;
        if (r_trxdir == 0) tx.active = 1;
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
    case 0x0070: p_dispfb[0] = v; break;
    case 0x0080: p_display[0] = v; break;
    case 0x0090: p_dispfb[1] = v; break;
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
        if (cap_prefix) cap_frame();
    } else p_csr ^= 0x2000;
}

int gs_get_frame(u32 **pixels, int *w, int *h) {
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
    for (u32 y = 0; y < hh; y++)
        for (u32 x = 0; x < ww; x++) {
            u32 c = rd_px(psm, fbp, fbw, dbx + x, dby + y);
            if (psm == PSMCT16 || psm == PSMCT16S) c = c16to32(c);
            frame_out[y * ww + x] = ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);   /* XRGB */
        }
    *pixels = frame_out; *w = (int)ww; *h = (int)hh;
    return 1;
}

void gs_init(void) {
    build_tables();
    pool_init();
    cap_init();
    { const char *e = getenv("ROR_GSDUMP"); if (e) gs_dump_frame = strtoull(e, 0, 0); }
    for (int i = 0; i < 2; i++) { ctxt[i].sx1 = 2047; ctxt[i].sy1 = 2047; ctxt[i].zpsm = PSMZ32; }
}
