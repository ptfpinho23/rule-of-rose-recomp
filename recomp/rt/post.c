/* post.c — picture post-processing (see post.h).
 *
 * Pipeline per output row, run on a small pool of worker threads in chunks
 * of rows:
 *   scale     nearest | bilinear | FSR 1.0 (EASU upscale, then RCAS sharpen)
 *   colour    brightness -> contrast -> gamma (one 256-entry table),
 *             saturation, bloom (screen-blended blurred highlights)
 *
 * The FSR passes are a CPU port of AMD FidelityFX Super Resolution 1.0
 * (ffx_fsr1.h, FsrEasuF / FsrRcasF, and the approximations they use from
 * ffx_a.h): same 12 + 5 taps, constants and clamps, computed in 32-bit
 * float on 0..1 gamma-encoded colour as the original expects. That code is
 * Copyright (c) 2021 Advanced Micro Devices, Inc. and used under the MIT
 * licence reproduced in ffx_fsr1_license.txt beside this file. What differs from
 * the shader is only the arrangement: the parts of EASU that do not depend
 * on the output position (per-texel gradient terms of FsrEasuSetF, the tap
 * colours of a source cell) are computed once per source texel / cell, and
 * four output pixels are computed at a time with SSE2. RCAS reads the
 * unquantised float output of EASU.
 *
 * No libm calls: curves use the small exp/log below, so the output is the
 * same on every platform and for every thread count. */
#include <emmintrin.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "post.h"

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef int32_t s32;

/* ---- settings ----------------------------------------------------------- */
void post_defaults(PostSettings *s) {
    s->filter = POST_FSR; s->stretch = 0; s->sharpness = 60;
    s->brightness = 0; s->contrast = 0; s->gamma = 100; s->saturation = 100; s->bloom = 0;
}
static int parse_int(const char *v, int lo, int hi, int *out) {
    if (!v || !*v) return 0;
    char *e; long n = strtol(v, &e, 10);
    if (*e || n < lo || n > hi) return 0;
    *out = (int)n; return 1;
}
int post_set(PostSettings *s, const char *name, const char *value) {
    if (!value) return 0;
    if (!strcmp(name, "filter")) {
        if (!strcmp(value, "nearest")) s->filter = POST_NEAREST;
        else if (!strcmp(value, "bilinear")) s->filter = POST_BILINEAR;
        else if (!strcmp(value, "fsr")) s->filter = POST_FSR;
        else return 0;
        return 1;
    }
    if (!strcmp(name, "aspect")) {
        if (!strcmp(value, "4:3")) s->stretch = 0;
        else if (!strcmp(value, "stretch")) s->stretch = 1;
        else return 0;
        return 1;
    }
    if (!strcmp(name, "sharpness"))  return parse_int(value, 0, 100, &s->sharpness);
    if (!strcmp(name, "brightness")) return parse_int(value, -100, 100, &s->brightness);
    if (!strcmp(name, "contrast"))   return parse_int(value, -100, 100, &s->contrast);
    if (!strcmp(name, "gamma"))      return parse_int(value, 50, 200, &s->gamma);
    if (!strcmp(name, "saturation")) return parse_int(value, 0, 200, &s->saturation);
    if (!strcmp(name, "bloom"))      return parse_int(value, 0, 100, &s->bloom);
    return 0;
}
void post_rect(int dw, int dh, const PostSettings *s, int *x, int *y, int *w, int *h) {
    int rw = dw, rh = dh;
    if (!s->stretch) {
        /* the picture's aspect: 4:3, or the --wide aspect while widened 3D is on display (rt/wide.c) */
        void wide_present(int *num, int *den) __attribute__((weak)); int an = 4, ad = 3;
        if (wide_present) wide_present(&an, &ad);            /* absent in recomp/tools/posttool */
        if ((long long)dw * ad >= (long long)dh * an) rw = (int)(((long long)dh * an + ad / 2) / ad);
        else rh = (int)(((long long)dw * ad + an / 2) / an);
        if (rw > dw) rw = dw;
        if (rh > dh) rh = dh;
    }
    *w = rw; *h = rh; *x = (dw - rw) / 2; *y = (dh - rh) / 2;
}

/* ---- deterministic exp / log (double, basic operations only) ------------- */
#define LN2 0.69314718055994530942
static double det_exp(double t) {
    int k = (int)(t / LN2 + (t < 0 ? -0.5 : 0.5));
    double r = t - k * LN2, term = 1.0, sum = 1.0;
    for (int i = 1; i < 24; i++) { term *= r / i; sum += term; }
    for (; k > 0; k--) sum *= 2.0;
    for (; k < 0; k++) sum *= 0.5;
    return sum;
}
static double det_log(double x) {          /* x > 0 */
    int e = 0;
    while (x < 0.70710678) { x *= 2.0; e--; }
    while (x > 1.41421357) { x *= 0.5; e++; }
    double z = (x - 1.0) / (x + 1.0), z2 = z * z, term = z, sum = 0.0;
    for (int i = 1; i < 60; i += 2) { sum += term / i; term *= z2; }
    return e * LN2 + 2.0 * sum;
}
static double det_pow(double x, double y) { return x <= 0.0 ? 0.0 : det_exp(y * det_log(x)); }

/* ---- state shared by the rows of one post_run call ------------------------ */
#define PAD 2                               /* replicated border of the source planes */
typedef struct {
    /* call arguments */
    const u32 *src; int sw, sh; u32 *dst; int dw, dh; PostSettings s;
    int rx, ry, rw, rh;                     /* picture rectangle in the output */
    /* colour stage */
    int use_lut, use_sat, use_bloom, sat64;
    u8 lut[256];
    u32 *glow; int gw, gh;                  /* blurred highlights, 0x00RRGGBB */
    int *gx0; u8 *gxw;                      /* per output column: glow column and 8-bit weight */
    /* scaling tables per output column */
    int *nx;                                /* nearest: source column */
    int *bx0; u8 *bxw;                      /* bilinear: left source column and weight */
    /* EASU: per output column / row the fraction inside the source cell, and the
     * runs of columns / rows that fall in one source cell */
    float *fx; int *run_start, *run_cell; int nruns;
    float *fy; int *vrun_start, *vrun_cell; int nvruns;
    float rcas_con; int use_rcas;
    /* FSR source: planar float colour, and the per-texel terms of FsrEasuSetF */
    int stride; float *pr, *pg, *pb, *pl, *dx, *dy, *lx, *ly;
} Job;

typedef struct {
    float *band[3];                         /* EASU output (r,g,b) for the rows of a chunk plus one above and below */
    u32 *tmp;                               /* bilinear: vertically blended source row */
    u32 *grow[2]; int grow_y[2];            /* glow rows expanded to the picture width */
    int cap_w, cap_sw;
} Scratch;

static Job J;
static int cap_dw, cap_dh, cap_n;           /* table capacities */

/* ---- FSR approximations (ffx_a.h) ---------------------------------------- */
static inline __m128 prx_lo_rcp(__m128 a) { return _mm_castsi128_ps(_mm_sub_epi32(_mm_set1_epi32(0x7ef07ebb), _mm_castps_si128(a))); }
static inline __m128 prx_lo_rsq(__m128 a) { return _mm_castsi128_ps(_mm_sub_epi32(_mm_set1_epi32(0x5f347d74), _mm_srli_epi32(_mm_castps_si128(a), 1))); }
static inline __m128 prx_med_rcp(__m128 a) {
    __m128 b = _mm_castsi128_ps(_mm_sub_epi32(_mm_set1_epi32(0x7ef19fff), _mm_castps_si128(a)));
    return _mm_mul_ps(b, _mm_add_ps(_mm_mul_ps(_mm_sub_ps(_mm_setzero_ps(), b), a), _mm_set1_ps(2.0f)));
}
static inline float f_prx_lo_rcp(float a) { u32 u; memcpy(&u, &a, 4); u = 0x7ef07ebbu - u; memcpy(&a, &u, 4); return a; }
static inline __m128 vabs(__m128 a) { return _mm_and_ps(a, _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF))); }
static inline __m128 vsel(__m128 m, __m128 a, __m128 b) { return _mm_or_ps(_mm_and_ps(m, a), _mm_andnot_ps(m, b)); }

/* Two SSE2 vectors computed side by side. EASU for a vector of pixels is one
 * long dependency chain (the 12 sequential tap accumulations above all), so
 * running two independent chains in the same loop body nearly doubles the
 * throughput; the arithmetic per lane is unchanged. */
typedef struct { __m128 a, b; } V;
static inline V v_set1(float x) { V r = { _mm_set1_ps(x), _mm_set1_ps(x) }; return r; }
static inline V v_splat(const float *p) { V r; r.a = r.b = _mm_load1_ps(p); return r; }
static inline V v_load(const float *p) { V r = { _mm_loadu_ps(p), _mm_loadu_ps(p + 4) }; return r; }
static inline void v_store(float *p, V x) { _mm_storeu_ps(p, x.a); _mm_storeu_ps(p + 4, x.b); }
static inline V v_add(V x, V y) { V r = { _mm_add_ps(x.a, y.a), _mm_add_ps(x.b, y.b) }; return r; }
static inline V v_sub(V x, V y) { V r = { _mm_sub_ps(x.a, y.a), _mm_sub_ps(x.b, y.b) }; return r; }
static inline V v_mul(V x, V y) { V r = { _mm_mul_ps(x.a, y.a), _mm_mul_ps(x.b, y.b) }; return r; }
static inline V v_div(V x, V y) { V r = { _mm_div_ps(x.a, y.a), _mm_div_ps(x.b, y.b) }; return r; }
static inline V v_min(V x, V y) { V r = { _mm_min_ps(x.a, y.a), _mm_min_ps(x.b, y.b) }; return r; }
static inline V v_max(V x, V y) { V r = { _mm_max_ps(x.a, y.a), _mm_max_ps(x.b, y.b) }; return r; }
static inline V v_lt(V x, V y) { V r = { _mm_cmplt_ps(x.a, y.a), _mm_cmplt_ps(x.b, y.b) }; return r; }
static inline V v_sel(V m, V x, V y) { V r = { vsel(m.a, x.a, y.a), vsel(m.b, x.b, y.b) }; return r; }
static inline V v_abs(V x) { V r = { vabs(x.a), vabs(x.b) }; return r; }
static inline V v_lo_rcp(V x) { V r = { prx_lo_rcp(x.a), prx_lo_rcp(x.b) }; return r; }
static inline V v_lo_rsq(V x) { V r = { prx_lo_rsq(x.a), prx_lo_rsq(x.b) }; return r; }

/* Source planes and the position-independent half of FsrEasuSetF, per texel:
 *   dirX = lD - lB, lenX = sat(|dirX| * APrxLoRcp(max(|lD - lC|, |lC - lB|)))^2, same for Y.
 * Two passes over the rows of the padded planes (the second needs the luma
 * of the rows above and below). */
static void fsr_planes(int y0, int y1) {
    int sw = J.sw, sh = J.sh, st = J.stride;
    for (int y = y0; y < y1; y++) {
        int sy = y - PAD; sy = sy < 0 ? 0 : sy >= sh ? sh - 1 : sy;
        const u32 *s = J.src + (size_t)sy * (size_t)sw;
        float *r = J.pr + (size_t)y * st, *g = J.pg + (size_t)y * st, *b = J.pb + (size_t)y * st, *l = J.pl + (size_t)y * st;
        for (int x = 0; x < st; x++) {
            int sx = x - PAD; sx = sx < 0 ? 0 : sx >= sw ? sw - 1 : sx;
            u32 c = s[sx];
            r[x] = (float)((c >> 16) & 255) * (1.0f / 255.0f);
            g[x] = (float)((c >> 8) & 255) * (1.0f / 255.0f);
            b[x] = (float)(c & 255) * (1.0f / 255.0f);
            l[x] = b[x] * 0.5f + (r[x] * 0.5f + g[x]);
        }
    }
}
static void fsr_sets(int y0, int y1) {
    int st = J.stride, ph = J.sh + 2 * PAD;
    const float *lum = J.pl;
    if (y0 < 1) y0 = 1;
    if (y1 > ph - 1) y1 = ph - 1;
    for (int y = y0; y < y1; y++)
        for (int x = 1; x < st - 1; x++) {
            size_t i = (size_t)y * st + (size_t)x;
            float lA = lum[i - st], lB = lum[i - 1], lC = lum[i], lD = lum[i + 1], lE = lum[i + st];
            float dc = lD - lC, cb = lC - lB; if (dc < 0) dc = -dc; if (cb < 0) cb = -cb;
            float lenX = f_prx_lo_rcp(dc > cb ? dc : cb), dirX = lD - lB;
            lenX = (dirX < 0 ? -dirX : dirX) * lenX; lenX = lenX < 0.0f ? 0.0f : lenX > 1.0f ? 1.0f : lenX;
            float ec = lE - lC, ca = lC - lA; if (ec < 0) ec = -ec; if (ca < 0) ca = -ca;
            float lenY = f_prx_lo_rcp(ec > ca ? ec : ca), dirY = lE - lA;
            lenY = (dirY < 0 ? -dirY : dirY) * lenY; lenY = lenY < 0.0f ? 0.0f : lenY > 1.0f ? 1.0f : lenY;
            J.dx[i] = dirX; J.dy[i] = dirY; J.lx[i] = lenX * lenX; J.ly[i] = lenY * lenY;
        }
}

/* FsrEasuF for the picture rows [b0, b1): writes row y at band[c] + (y - b0) * bstride + 1.
 * All output pixels that fall into one source cell share their 12 taps, so
 * the work goes cell by cell: the pixels of a cell (a few columns of a few
 * rows) are packed into lanes and computed four at a time. */
#define LANES 128                           /* multiple of 8 */
static void easu_band(Scratch *sc, int b0, int b1) {
    const int st = J.stride, bstride = J.rw + 8;
    const V one = v_set1(1.0f), two = v_set1(2.0f), zero = v_set1(0.0f);
    const V c_lob = v_set1((float)((1.0 / 4.0 - 0.04) - 0.5)), c_25 = v_set1((float)(2.0 / 5.0)),
                 c_2516 = v_set1((float)(25.0 / 16.0)), c_m = v_set1((float)(-(25.0 / 16.0 - 1.0))),
                 m_one = v_set1(-1.0f), half = v_set1(0.5f), c_zro = v_set1(1.0f / 32768.0f), m_half = v_set1(-0.5f);
    float lfx[LANES + 8], lfy[LANES + 8], oR[LANES + 8], oG[LANES + 8], oB[LANES + 8];
    for (int vr = 0; vr < J.nvruns; vr++) {
        int ya = J.vrun_start[vr], yb = J.vrun_start[vr + 1];
        if (ya < b0) ya = b0;
        if (yb > b1) yb = b1;
        if (ya >= yb) continue;
        const size_t rowbase = (size_t)(J.vrun_cell[vr] + PAD) * (size_t)st + PAD;
        for (int run = 0; run < J.nruns; run++) {
            const int rx0 = J.run_start[run], rx1 = J.run_start[run + 1];
            size_t i0 = rowbase + (size_t)(ptrdiff_t)J.run_cell[run];     /* texel f */
            const float *pr = J.pr + i0, *pg = J.pg + i0, *pb = J.pb + i0;
            /* the 12 taps of this cell:   b c / e f g h / i j k l / n o */
#define SPLAT v_splat
#define TAPS(p, q) const V q##b = SPLAT(p - st), q##c = SPLAT(p - st + 1), \
                q##e = SPLAT(p - 1), q##f = SPLAT(p), q##g = SPLAT(p + 1), q##h = SPLAT(p + 2), \
                q##i = SPLAT(p + st - 1), q##j = SPLAT(p + st), q##k = SPLAT(p + st + 1), q##l = SPLAT(p + st + 2), \
                q##n = SPLAT(p + 2 * st), q##o = SPLAT(p + 2 * st + 1)
            TAPS(pr, r); TAPS(pg, g); TAPS(pb, b);
#undef TAPS
            const V mnr = v_min(v_min(v_min(rf, rg), rj), rk), mxr = v_max(v_max(v_max(rf, rg), rj), rk);
            const V mng = v_min(v_min(v_min(gf, gg), gj), gk), mxg = v_max(v_max(v_max(gf, gg), gj), gk);
            const V mnb = v_min(v_min(v_min(bf, bg), bj), bk), mxb = v_max(v_max(v_max(bf, bg), bj), bk);
            const float *dx = J.dx + i0, *dy = J.dy + i0, *lx = J.lx + i0, *ly = J.ly + i0;
            const V dxf = SPLAT(dx), dxg = SPLAT(dx + 1), dxj = SPLAT(dx + st), dxk = SPLAT(dx + st + 1);
            const V dyf = SPLAT(dy), dyg = SPLAT(dy + 1), dyj = SPLAT(dy + st), dyk = SPLAT(dy + st + 1);
            const V lxf = SPLAT(lx), lxg = SPLAT(lx + 1), lxj = SPLAT(lx + st), lxk = SPLAT(lx + st + 1);
            const V lyf = SPLAT(ly), lyg = SPLAT(ly + 1), lyj = SPLAT(ly + st), lyk = SPLAT(ly + st + 1);
            for (int x0 = rx0; x0 < rx1; x0 += LANES) {
                const int xn = rx1 - x0 < LANES ? rx1 - x0 : LANES, rows_per = LANES / xn;
                for (int y0 = ya; y0 < yb; y0 += rows_per) {
                    const int yn = yb - y0 < rows_per ? yb - y0 : rows_per, n = xn * yn;
                    for (int r = 0, k = 0; r < yn; r++)
                        for (int x = 0; x < xn; x++, k++) { lfx[k] = J.fx[x0 + x]; lfy[k] = J.fy[y0 + r]; }
                    for (int k = n; k < n + 7; k++) lfx[k] = lfy[k] = 0.0f;
                    for (int k = 0; k < n; k += 8) {
                        const V fx = v_load(lfx + k), fy = v_load(lfy + k), ifx = v_sub(one, fx), ify = v_sub(one, fy);
                        /* FsrEasuSetF x4: bilinear weights of the four '+' patterns */
                        const V ws = v_mul(ifx, ify), wt = v_mul(fx, ify), wu = v_mul(ifx, fy), wv = v_mul(fx, fy);
                        V dirx = v_mul(dxf, ws), diry = v_mul(dyf, ws);
                        V len = v_mul(lxf, ws); len = v_add(len, v_mul(lyf, ws));
                        dirx = v_add(dirx, v_mul(dxg, wt)); len = v_add(len, v_mul(lxg, wt));
                        diry = v_add(diry, v_mul(dyg, wt)); len = v_add(len, v_mul(lyg, wt));
                        dirx = v_add(dirx, v_mul(dxj, wu)); len = v_add(len, v_mul(lxj, wu));
                        diry = v_add(diry, v_mul(dyj, wu)); len = v_add(len, v_mul(lyj, wu));
                        dirx = v_add(dirx, v_mul(dxk, wv)); len = v_add(len, v_mul(lxk, wv));
                        diry = v_add(diry, v_mul(dyk, wv)); len = v_add(len, v_mul(lyk, wv));
                        /* normalise with approximation, clean up close to zero */
                        V dirr = v_add(v_mul(dirx, dirx), v_mul(diry, diry));
                        const V zro = v_lt(dirr, c_zro);
                        dirr = v_sel(zro, one, v_lo_rsq(dirr));
                        dirx = v_sel(zro, one, dirx);
                        dirx = v_mul(dirx, dirr); diry = v_mul(diry, dirr);
                        /* {0 to 2} -> {0 to 1}, shaped with a square */
                        len = v_mul(len, half); len = v_mul(len, len);
                        /* stretch kernel {1.0 vert|horz, to sqrt(2.0) on the diagonal} */
                        const V stretch = v_mul(v_add(v_mul(dirx, dirx), v_mul(diry, diry)),
                                                          v_lo_rcp(v_max(v_abs(dirx), v_abs(diry))));
                        const V len2x = v_add(one, v_mul(v_sub(stretch, one), len));
                        const V len2y = v_add(one, v_mul(m_half, len));
                        const V lob = v_add(half, v_mul(c_lob, len));
                        const V clp = v_lo_rcp(lob);
                        /* rotated tap offsets: v.x = off.x*dir.x + off.y*dir.y, v.y = off.x*(-dir.y) + off.y*dir.x */
                        const V ox_m1 = v_sub(m_one, fx), ox_0 = v_sub(zero, fx), ox_1 = ifx, ox_2 = v_sub(two, fx);
                        const V oy_m1 = v_sub(m_one, fy), oy_0 = v_sub(zero, fy), oy_1 = ify, oy_2 = v_sub(two, fy);
                        const V ndiry = v_sub(zero, diry);
                        const V ax_m1 = v_mul(ox_m1, dirx), ax_0 = v_mul(ox_0, dirx), ax_1 = v_mul(ox_1, dirx), ax_2 = v_mul(ox_2, dirx);
                        const V cx_m1 = v_mul(ox_m1, ndiry), cx_0 = v_mul(ox_0, ndiry), cx_1 = v_mul(ox_1, ndiry), cx_2 = v_mul(ox_2, ndiry);
                        const V by_m1 = v_mul(oy_m1, diry), by_0 = v_mul(oy_0, diry), by_1 = v_mul(oy_1, diry), by_2 = v_mul(oy_2, diry);
                        const V ey_m1 = v_mul(oy_m1, dirx), ey_0 = v_mul(oy_0, dirx), ey_1 = v_mul(oy_1, dirx), ey_2 = v_mul(oy_2, dirx);
                        V ar = zero, ag = zero, ab = zero, aw = zero;
                        /* FsrEasuTapF */
#define TAP(X, Y, t) do { \
                            V vx = v_mul(v_add(ax_##X, by_##Y), len2x), vy = v_mul(v_add(cx_##X, ey_##Y), len2y); \
                            V d2 = v_min(v_add(v_mul(vx, vx), v_mul(vy, vy)), clp); \
                            V wB = v_add(v_mul(c_25, d2), m_one), wA = v_add(v_mul(lob, d2), m_one); \
                            wB = v_mul(wB, wB); wA = v_mul(wA, wA); \
                            wB = v_add(v_mul(c_2516, wB), c_m); \
                            V w = v_mul(wB, wA); \
                            ar = v_add(ar, v_mul(r##t, w)); ag = v_add(ag, v_mul(g##t, w)); \
                            ab = v_add(ab, v_mul(b##t, w)); aw = v_add(aw, w); } while (0)
                        TAP(0, m1, b); TAP(1, m1, c); TAP(m1, 1, i); TAP(0, 1, j); TAP(0, 0, f); TAP(m1, 0, e);
                        TAP(1, 1, k); TAP(2, 1, l); TAP(2, 0, h); TAP(1, 0, g); TAP(1, 2, o); TAP(0, 2, n);
#undef TAP
                        const V rcp = v_div(one, aw);
                        v_store(oR + k, v_min(mxr, v_max(mnr, v_mul(ar, rcp))));
                        v_store(oG + k, v_min(mxg, v_max(mng, v_mul(ag, rcp))));
                        v_store(oB + k, v_min(mxb, v_max(mnb, v_mul(ab, rcp))));
                    }
                    for (int r = 0, k = 0; r < yn; r++) {
                        size_t at = (size_t)(y0 + r - b0) * (size_t)bstride + 1 + (size_t)x0;
                        for (int x = 0; x < xn; x++, k++) { sc->band[0][at + x] = oR[k]; sc->band[1][at + x] = oG[k]; sc->band[2][at + x] = oB[k]; }
                    }
                }
            }
        }
    }
    for (int c = 0; c < 3; c++)                           /* replicate the left/right edges for RCAS */
        for (int y = b0; y < b1; y++) {
            float *p = sc->band[c] + (size_t)(y - b0) * (size_t)bstride + 1;
            p[-1] = p[0];
            for (int k = 0; k < 4; k++) p[J.rw + k] = p[J.rw - 1];
        }
}

/* FsrRcasF (no denoise) for one row: a/e/h are the rows above, at and below,
 * each [3] = r,g,b with valid data at index -1 .. n. Output 0x00RRGGBB. */
static inline __m128i pack_rgb(__m128 r, __m128 g, __m128 b) {
    const __m128 zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f), k = _mm_set1_ps(255.0f);
    __m128i ri = _mm_cvtps_epi32(_mm_mul_ps(_mm_min_ps(_mm_max_ps(r, zero), one), k));
    __m128i gi = _mm_cvtps_epi32(_mm_mul_ps(_mm_min_ps(_mm_max_ps(g, zero), one), k));
    __m128i bi = _mm_cvtps_epi32(_mm_mul_ps(_mm_min_ps(_mm_max_ps(b, zero), one), k));
    return _mm_or_si128(_mm_or_si128(_mm_slli_epi32(ri, 16), _mm_slli_epi32(gi, 8)), bi);
}
static void rcas_row(float *const a[3], float *const e[3], float *const h[3], u32 *out, int n) {
    const __m128 half = _mm_set1_ps(0.5f), four = _mm_set1_ps(4.0f), one = _mm_set1_ps(1.0f), zero = _mm_setzero_ps();
    const __m128 limit = _mm_set1_ps((float)(-(0.25 - (1.0 / 16.0)))), con = _mm_set1_ps(J.rcas_con), peak_y = _mm_set1_ps(-4.0f);
    (void)half;
    for (int x = 0; x < n; x += 4) {
        __m128 lobe_max = zero, px[3], bq[3], dq[3], eq[3], fq[3], hq[3];
        for (int c = 0; c < 3; c++) {
            const __m128 b = _mm_loadu_ps(a[c] + x), d = _mm_loadu_ps(e[c] + x - 1), ee = _mm_loadu_ps(e[c] + x),
                         f = _mm_loadu_ps(e[c] + x + 1), hh = _mm_loadu_ps(h[c] + x);
            bq[c] = b; dq[c] = d; eq[c] = ee; fq[c] = f; hq[c] = hh;
            const __m128 mn4 = _mm_min_ps(_mm_min_ps(_mm_min_ps(b, d), f), hh), mx4 = _mm_max_ps(_mm_max_ps(_mm_max_ps(b, d), f), hh);
            /* hitMin = min(mn4,e) / (4*mx4);  hitMax = (1 - max(mx4,e)) / (4*mn4 - 4) */
            const __m128 hit_min = _mm_mul_ps(_mm_min_ps(mn4, ee), _mm_div_ps(one, _mm_mul_ps(four, mx4)));
            const __m128 hit_max = _mm_mul_ps(_mm_sub_ps(one, _mm_max_ps(mx4, ee)), _mm_div_ps(one, _mm_add_ps(_mm_mul_ps(four, mn4), peak_y)));
            /* max() that ignores a NaN operand (0 * inf where the neighbourhood is
             * pure black or pure white), as the shader's max does */
            const __m128 nhit_min = _mm_sub_ps(zero, hit_min);
            __m128 lobe = _mm_max_ps(nhit_min, hit_max);           /* = hit_max if nhit_min is NaN */
            lobe = vsel(_mm_cmpunord_ps(hit_max, hit_max), nhit_min, lobe);
            lobe_max = c == 0 ? lobe : _mm_max_ps(lobe_max, lobe);
        }
        const __m128 lobe = _mm_mul_ps(_mm_max_ps(limit, _mm_min_ps(lobe_max, zero)), con);
        const __m128 rcpl = prx_med_rcp(_mm_add_ps(_mm_mul_ps(four, lobe), one));
        for (int c = 0; c < 3; c++)
            px[c] = _mm_mul_ps(_mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(lobe, bq[c]), _mm_mul_ps(lobe, dq[c])),
                                                              _mm_mul_ps(lobe, hq[c])), _mm_mul_ps(lobe, fq[c])), eq[c]), rcpl);
        _mm_storeu_si128((__m128i *)(out + x), pack_rgb(px[0], px[1], px[2]));
    }
}

/* ---- colour stage --------------------------------------------------------- */
static inline u32 lerp_px(u32 a, u32 b, u32 w) {      /* w 0..256 */
    u32 rb = ((a & 0xFF00FFu) * (256 - w) + (b & 0xFF00FFu) * w + 0x800080u) >> 8;
    u32 g = ((a & 0x00FF00u) * (256 - w) + (b & 0x00FF00u) * w + 0x008000u) >> 8;
    return (rb & 0xFF00FFu) | (g & 0x00FF00u);
}
static inline u32 colour_px(u32 c) {                  /* table, then saturation */
    u32 r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    if (J.use_lut) { r = J.lut[r]; g = J.lut[g]; b = J.lut[b]; }
    if (J.use_sat) {
        s32 l = (s32)((77 * r + 150 * g + 29 * b + 128) >> 8), k = J.sat64;
        s32 rr = l + ((((s32)r - l) * k + 32) >> 6), gg = l + ((((s32)g - l) * k + 32) >> 6), bb = l + ((((s32)b - l) * k + 32) >> 6);
        r = (u32)(rr < 0 ? 0 : rr > 255 ? 255 : rr); g = (u32)(gg < 0 ? 0 : gg > 255 ? 255 : gg); b = (u32)(bb < 0 ? 0 : bb > 255 ? 255 : bb);
    }
    return (r << 16) | (g << 8) | b;
}
static void box_blur(u32 *buf, u32 *tmp, int w, int h, int rx, int ry) {
    for (int y = 0; y < h; y++) {                     /* horizontal: buf -> tmp */
        const u32 *s = buf + (size_t)y * w; u32 *d = tmp + (size_t)y * w;
        u32 n = (u32)(2 * rx + 1), ar = 0, ag = 0, ab = 0;
        for (int k = -rx; k <= rx; k++) { u32 c = s[k < 0 ? 0 : k >= w ? w - 1 : k]; ar += (c >> 16) & 255; ag += (c >> 8) & 255; ab += c & 255; }
        for (int x = 0; x < w; x++) {
            d[x] = (((ar + n / 2) / n) << 16) | (((ag + n / 2) / n) << 8) | ((ab + n / 2) / n);
            u32 o = s[x - rx < 0 ? 0 : x - rx], i = s[x + rx + 1 >= w ? w - 1 : x + rx + 1];
            ar += ((i >> 16) & 255) - ((o >> 16) & 255); ag += ((i >> 8) & 255) - ((o >> 8) & 255); ab += (i & 255) - (o & 255);
        }
    }
    for (int x = 0; x < w; x++) {                     /* vertical: tmp -> buf */
        u32 n = (u32)(2 * ry + 1), ar = 0, ag = 0, ab = 0;
        for (int k = -ry; k <= ry; k++) { u32 c = tmp[(size_t)(k < 0 ? 0 : k >= h ? h - 1 : k) * w + x]; ar += (c >> 16) & 255; ag += (c >> 8) & 255; ab += c & 255; }
        for (int y = 0; y < h; y++) {
            buf[(size_t)y * w + x] = (((ar + n / 2) / n) << 16) | (((ag + n / 2) / n) << 8) | ((ab + n / 2) / n);
            u32 o = tmp[(size_t)(y - ry < 0 ? 0 : y - ry) * w + x], i = tmp[(size_t)(y + ry + 1 >= h ? h - 1 : y + ry + 1) * w + x];
            ar += ((i >> 16) & 255) - ((o >> 16) & 255); ag += ((i >> 8) & 255) - ((o >> 8) & 255); ab += (i & 255) - (o & 255);
        }
    }
}
/* Highlights of the (colour-corrected) frame at quarter resolution, blurred.
 * Averaging 4x4 blocks before the threshold keeps thin bright detail such as
 * text from glowing; only areas that are bright as a whole do. */
#define BLOOM_THRESHOLD 40                  /* luma where the glow starts (this game is dark) ... */
#define BLOOM_FULL 150                      /* ... and where it reaches full weight */
static void bloom_prepare(void) {
    int gw = J.gw, gh = J.gh, sw = J.sw, sh = J.sh;
    u32 *tmp = malloc((size_t)gw * (size_t)gh * sizeof *tmp);
    for (int gy = 0; gy < gh; gy++) {
        int y0 = gy * sh / gh, y1 = (gy + 1) * sh / gh; if (y1 <= y0) y1 = y0 + 1;
        for (int gx = 0; gx < gw; gx++) {
            int x0 = gx * sw / gw, x1 = (gx + 1) * sw / gw; if (x1 <= x0) x1 = x0 + 1;
            u32 ar = 0, ag = 0, ab = 0, n = (u32)((x1 - x0) * (y1 - y0));
            for (int y = y0; y < y1; y++) {
                const u32 *s = J.src + (size_t)y * sw;
                for (int x = x0; x < x1; x++) { u32 c = s[x]; ar += (c >> 16) & 255; ag += (c >> 8) & 255; ab += c & 255; }
            }
            u32 c = colour_px((((ar + n / 2) / n) << 16) | (((ag + n / 2) / n) << 8) | ((ab + n / 2) / n));
            u32 r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
            s32 l = (s32)((77 * r + 150 * g + 29 * b + 128) >> 8) - BLOOM_THRESHOLD;
            /* weight 0 at the threshold rising to bloom/100 at BLOOM_FULL and above */
            if (l > BLOOM_FULL - BLOOM_THRESHOLD) l = BLOOM_FULL - BLOOM_THRESHOLD;
            u32 k = l <= 0 ? 0 : (u32)l * (u32)J.s.bloom * 256u / ((BLOOM_FULL - BLOOM_THRESHOLD) * 100u);
            J.glow[(size_t)gy * gw + gx] = (((r * k + 128) >> 8) << 16) | (((g * k + 128) >> 8) << 8) | ((b * k + 128) >> 8);
        }
    }
    int rx = (gw * 2 + 50) / 100, ry = (gh * 27 + 500) / 1000;
    if (rx < 1) rx = 1;
    if (ry < 1) ry = 1;
    for (int pass = 0; pass < 3; pass++) box_blur(J.glow, tmp, gw, gh, rx, ry);
    free(tmp);
}
static void colour_row(Scratch *sc, u32 *row, int y) {
    int n = J.rw;
    if (J.use_lut || J.use_sat) for (int x = 0; x < n; x++) row[x] = colour_px(row[x]);
    if (!J.use_bloom) return;
    /* glow sampled bilinearly: rows expanded to the picture width are cached */
    s32 pos = (s32)(((long long)(2 * y + 1) * J.gh * 128) / J.rh) - 128;
    if (pos < 0) pos = 0;
    int g0 = pos >> 8; u32 wy = (u32)pos & 255;
    if (g0 >= J.gh - 1) { g0 = J.gh - 1; wy = 0; }
    int g1 = g0 + 1 < J.gh ? g0 + 1 : g0;
    const u32 *line[2];
    for (int k = 0; k < 2; k++) {
        int want = k ? g1 : g0, slot = -1;
        for (int i = 0; i < 2; i++) if (sc->grow_y[i] == want) slot = i;
        if (slot < 0) {
            slot = (sc->grow_y[0] == g0 || sc->grow_y[0] == g1) ? 1 : 0;
            const u32 *g = J.glow + (size_t)want * J.gw; u32 *d = sc->grow[slot];
            for (int x = 0; x < n; x++) { int i0 = J.gx0[x]; d[x] = lerp_px(g[i0], g[i0 + 1 < J.gw ? i0 + 1 : i0], J.gxw[x]); }
            sc->grow_y[slot] = want;
        }
        line[k] = sc->grow[slot];
    }
    const __m128i zero = _mm_setzero_si128(), w1 = _mm_set1_epi16((short)wy), w0 = _mm_set1_epi16((short)(256 - wy)),
                  c255 = _mm_set1_epi16(255), c128 = _mm_set1_epi16(128);
    for (int x = 0; x < n; x += 4) {                  /* screen: c + g*(255 - c)/255 */
        __m128i c = _mm_loadu_si128((const __m128i *)(row + x));
        __m128i ga = _mm_loadu_si128((const __m128i *)(line[0] + x)), gb = _mm_loadu_si128((const __m128i *)(line[1] + x));
        __m128i out[2];
        for (int hlf = 0; hlf < 2; hlf++) {
            __m128i c16 = hlf ? _mm_unpackhi_epi8(c, zero) : _mm_unpacklo_epi8(c, zero);
            __m128i a16 = hlf ? _mm_unpackhi_epi8(ga, zero) : _mm_unpacklo_epi8(ga, zero);
            __m128i b16 = hlf ? _mm_unpackhi_epi8(gb, zero) : _mm_unpacklo_epi8(gb, zero);
            __m128i g16 = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(a16, w0), _mm_mullo_epi16(b16, w1)), c128), 8);
            __m128i t = _mm_add_epi16(_mm_mullo_epi16(g16, _mm_sub_epi16(c255, c16)), c128);
            t = _mm_srli_epi16(_mm_add_epi16(t, _mm_srli_epi16(t, 8)), 8);
            out[hlf] = _mm_add_epi16(c16, t);
        }
        _mm_storeu_si128((__m128i *)(row + x), _mm_packus_epi16(out[0], out[1]));
    }
}

/* ---- rows ----------------------------------------------------------------- */
/* One row of the picture (y inside the picture rectangle) into out[0..rw). */
static void picture_row(Scratch *sc, int y, int band0, u32 *out) {
    int rw = J.rw, rh = J.rh, sw = J.sw, sh = J.sh;
    if (J.s.filter == POST_NEAREST) {
        const u32 *s = J.src + (size_t)((long long)y * sh / rh) * (size_t)sw;
        for (int x = 0; x < rw; x++) out[x] = s[J.nx[x]] & 0xFFFFFFu;
    } else if (J.s.filter == POST_BILINEAR) {
        s32 pos = (s32)(((long long)(2 * y + 1) * sh * 128) / rh) - 128;
        if (pos < 0) pos = 0;
        int y0 = pos >> 8; u32 wy = (u32)pos & 255;
        if (y0 >= sh - 1) { y0 = sh - 1; wy = 0; }
        const u32 *a = J.src + (size_t)y0 * (size_t)sw, *b = y0 + 1 < sh ? a + sw : a;
        u32 *t = sc->tmp;
        for (int x = 0; x < sw; x++) t[x] = lerp_px(a[x], b[x], wy);
        t[sw] = t[sw - 1];
        for (int x = 0; x < rw; x++) { int i0 = J.bx0[x]; out[x] = lerp_px(t[i0], t[i0 + 1], J.bxw[x]); }
    } else {                                 /* the chunk's rows are in the band (run_chunks) */
        const size_t bs = (size_t)rw + 8;
        int ya = y > 0 ? y - 1 : 0, yh = y + 1 < rh ? y + 1 : rh - 1;
        float *e[3], *a[3], *h[3];
        for (int c = 0; c < 3; c++) {
            a[c] = sc->band[c] + (size_t)(ya - band0) * bs + 1; e[c] = sc->band[c] + (size_t)(y - band0) * bs + 1;
            h[c] = sc->band[c] + (size_t)(yh - band0) * bs + 1;
        }
        if (!J.use_rcas) {
            for (int x = 0; x < rw; x += 4)
                _mm_storeu_si128((__m128i *)(out + x), pack_rgb(_mm_loadu_ps(e[0] + x), _mm_loadu_ps(e[1] + x), _mm_loadu_ps(e[2] + x)));
        } else rcas_row(a, e, h, out, rw);
    }
    colour_row(sc, out, y);
}

/* ---- worker pool ---------------------------------------------------------- */
#define CHUNK 16
#define MAX_THREADS 32
static Scratch scratch[MAX_THREADS];
static pthread_mutex_t pool_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pool_go = PTHREAD_COND_INITIALIZER, pool_done = PTHREAD_COND_INITIALIZER;
static int pool_n = -1, pool_gen, pool_active;
static atomic_int next_chunk;

static int phase, phase_rows;                 /* 0, 1: fsr_planes, fsr_sets; 2: output rows */
static void run_chunks(Scratch *sc) {
    unsigned csr = _mm_getcsr();
    _mm_setcsr(0x1F80 | 0x8040);                       /* round to nearest, no denormals: same on every thread */
    sc->grow_y[0] = sc->grow_y[1] = -2;
    for (;;) {
        int y0 = atomic_fetch_add(&next_chunk, CHUNK);
        if (y0 >= phase_rows) break;
        if (phase < 2) {
            int ye = y0 + CHUNK < phase_rows ? y0 + CHUNK : phase_rows;
            if (phase == 0) fsr_planes(y0, ye); else fsr_sets(y0, ye);
            continue;
        }
        int y1 = y0 + CHUNK < J.dh ? y0 + CHUNK : J.dh, band0 = 0;
        if (J.s.filter == POST_FSR) {                  /* EASU for the picture rows of this chunk, plus RCAS's neighbours */
            int p0 = y0 - J.ry, p1 = y1 - J.ry;
            if (p0 < 0) p0 = 0;
            if (p1 > J.rh) p1 = J.rh;
            if (p0 < p1) {
                int b1 = p1;
                band0 = p0;
                if (J.use_rcas) { if (band0 > 0) band0--; if (b1 < J.rh) b1++; }
                easu_band(sc, band0, b1);
            }
        }
        for (int y = y0; y < y1; y++) {
            u32 *d = J.dst + (size_t)y * (size_t)J.dw;
            if (y < J.ry || y >= J.ry + J.rh) { memset(d, 0, (size_t)J.dw * 4); continue; }
            if (J.rx > 0) memset(d, 0, (size_t)J.rx * 4);
            if (J.rx + J.rw < J.dw) memset(d + J.rx + J.rw, 0, (size_t)(J.dw - J.rx - J.rw) * 4);
            /* rows are produced in a scratch line (the SIMD loops write up to 3 pixels past the end) */
            u32 *line = sc->grow[0] + J.rw + 8;
            picture_row(sc, y - J.ry, band0, line);
            memcpy(d + J.rx, line, (size_t)J.rw * 4);
        }
    }
    _mm_setcsr(csr);
}
static void *pool_main(void *arg) {
    Scratch *sc = arg; int seen = 0;
    for (;;) {
        pthread_mutex_lock(&pool_m);
        while (pool_gen == seen) pthread_cond_wait(&pool_go, &pool_m);
        seen = pool_gen;
        pthread_mutex_unlock(&pool_m);
        run_chunks(sc);
        pthread_mutex_lock(&pool_m);
        if (--pool_active == 0) pthread_cond_signal(&pool_done);
        pthread_mutex_unlock(&pool_m);
    }
    return 0;
}
static void pool_start(void) {
    const char *e = getenv("ROR_POST_THREADS");
    long n = e ? atol(e) : sysconf(_SC_NPROCESSORS_ONLN) / 2;
    if (n < 1) n = 1;
    if (n > MAX_THREADS) n = MAX_THREADS;
    pool_n = 1;
    for (long i = 1; i < n; i++) {
        pthread_t t;
        if (pthread_create(&t, 0, pool_main, &scratch[pool_n]) != 0) break;
        pthread_detach(t);
        pool_n++;
    }
}

/* FsrRcasCon: sharpness 100 -> 0 stops (maximum), 1 -> 1.98 stops; con = exp2(-stops) */
float post_rcas_con(int sharpness) { return (float)det_exp(-((100 - sharpness) / 50.0) * LN2); }

static void run_phase(int ph, int rows);

/* ---- entry ---------------------------------------------------------------- */
static void *grow_buf(void *p, size_t bytes) { free(p); p = malloc(bytes); if (!p) { fprintf(stderr, "post: out of memory\n"); exit(1); } return p; }

/* The brightness -> contrast -> gamma table (also used by the GPU presentation). */
int post_lut(const PostSettings *s, uint8_t *lut) {
    if (!(s->brightness != 0 || s->contrast != 0 || s->gamma != 100)) return 0;
    double k = s->contrast >= 0 ? 1.0 + s->contrast / 100.0 : 1.0 + s->contrast / 200.0;
    for (int i = 0; i < 256; i++) {
        double v = i / 255.0 + s->brightness * 0.004;
        v = (v - 0.5) * k + 0.5;
        v = v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v;
        if (s->gamma != 100) v = det_pow(v, 100.0 / s->gamma);
        lut[i] = (u8)(int)(v * 255.0 + 0.5);
    }
    return 1;
}
void post_run(const u32 *src, int sw, int sh, u32 *dst, int dw, int dh, const PostSettings *s) {
    if (sw < 1 || sh < 1 || dw < 1 || dh < 1) return;
    if (pool_n < 0) pool_start();
    J.src = src; J.sw = sw; J.sh = sh; J.dst = dst; J.dw = dw; J.dh = dh; J.s = *s;
    post_rect(dw, dh, s, &J.rx, &J.ry, &J.rw, &J.rh);
    int rw = J.rw, rh = J.rh;

    /* tables (sizes only ever grow) */
    if (dw > cap_dw) {
        cap_dw = dw;
        J.nx = grow_buf(J.nx, (size_t)dw * sizeof(int)); J.bx0 = grow_buf(J.bx0, (size_t)dw * sizeof(int));
        J.bxw = grow_buf(J.bxw, (size_t)dw); J.gx0 = grow_buf(J.gx0, (size_t)dw * sizeof(int)); J.gxw = grow_buf(J.gxw, (size_t)dw);
        J.fx = grow_buf(J.fx, ((size_t)dw + 8) * sizeof(float));
        J.run_start = grow_buf(J.run_start, ((size_t)dw + 1) * sizeof(int)); J.run_cell = grow_buf(J.run_cell, ((size_t)dw + 1) * sizeof(int));
    }
    if (dh > cap_dh) {
        cap_dh = dh;
        J.fy = grow_buf(J.fy, ((size_t)dh + 8) * sizeof(float));
        J.vrun_start = grow_buf(J.vrun_start, ((size_t)dh + 1) * sizeof(int)); J.vrun_cell = grow_buf(J.vrun_cell, ((size_t)dh + 1) * sizeof(int));
    }
    size_t plane = (size_t)(sw + 2 * PAD) * (size_t)(sh + 2 * PAD);
    if ((int)plane > cap_n) {
        cap_n = (int)plane;
        J.pl = grow_buf(J.pl, plane * 4); J.pr = grow_buf(J.pr, plane * 4); J.pg = grow_buf(J.pg, plane * 4); J.pb = grow_buf(J.pb, plane * 4);
        J.dx = grow_buf(J.dx, plane * 4); J.dy = grow_buf(J.dy, plane * 4); J.lx = grow_buf(J.lx, plane * 4); J.ly = grow_buf(J.ly, plane * 4);
        J.glow = grow_buf(J.glow, plane * 4);
    }
    for (int i = 0; i < pool_n; i++) {
        Scratch *sc = &scratch[i];
        if (dw > sc->cap_w) {
            sc->cap_w = dw;
            for (int k = 0; k < 3; k++) sc->band[k] = grow_buf(sc->band[k], ((size_t)dw + 8) * (CHUNK + 2) * sizeof(float) + 64);
            /* grow[0] also holds the output scratch line after the first rw + 8 entries */
            sc->grow[0] = grow_buf(sc->grow[0], ((size_t)dw * 2 + 24) * 4); sc->grow[1] = grow_buf(sc->grow[1], ((size_t)dw + 8) * 4);
        }
        if (sw > sc->cap_sw) { sc->cap_sw = sw; sc->tmp = grow_buf(sc->tmp, ((size_t)sw + 2) * 4); }
    }

    /* per-column maps */
    J.gw = sw / 4 > 0 ? sw / 4 : 1; J.gh = sh / 4 > 0 ? sh / 4 : 1;
    for (int x = 0; x < rw; x++) {
        J.nx[x] = (int)((long long)x * sw / rw);
        s32 pos = (s32)(((long long)(2 * x + 1) * sw * 128) / rw) - 128;
        if (pos < 0) pos = 0;
        int i0 = pos >> 8; u32 w = (u32)pos & 255;
        if (i0 >= sw - 1) { i0 = sw - 1; w = 0; }
        J.bx0[x] = i0; J.bxw[x] = (u8)w;
        pos = (s32)(((long long)(2 * x + 1) * J.gw * 128) / rw) - 128;
        if (pos < 0) pos = 0;
        i0 = pos >> 8; w = (u32)pos & 255;
        if (i0 >= J.gw - 1) { i0 = J.gw - 1; w = 0; }
        J.gx0[x] = i0; J.gxw[x] = (u8)w;
    }

    /* colour stage */
    J.use_lut = s->brightness != 0 || s->contrast != 0 || s->gamma != 100;
    J.use_sat = s->saturation != 100; J.sat64 = (s->saturation * 64 + 50) / 100;
    J.use_bloom = s->bloom > 0;
    if (J.use_lut) post_lut(s, J.lut);
    if (J.use_bloom) bloom_prepare();

    /* FSR set-up: FsrEasuCon / FsrRcasCon */
    if (s->filter == POST_FSR) {
        J.stride = sw + 2 * PAD;
        float cx = (float)sw * (1.0f / (float)rw), zx = 0.5f * (float)sw * (1.0f / (float)rw) - 0.5f;
        float cy = (float)sh * (1.0f / (float)rh), zy = 0.5f * (float)sh * (1.0f / (float)rh) - 0.5f;
        J.nruns = 0;
        for (int x = 0; x < rw; x++) {
            float pp = (float)x * cx + zx;
            int fp = (int)pp; if ((float)fp > pp) fp--;
            J.fx[x] = pp - (float)fp;
            if (J.nruns == 0 || J.run_cell[J.nruns - 1] != fp) { J.run_start[J.nruns] = x; J.run_cell[J.nruns] = fp; J.nruns++; }
        }
        J.run_start[J.nruns] = rw;
        J.nvruns = 0;
        for (int y = 0; y < rh; y++) {
            float pp = (float)y * cy + zy;
            int fp = (int)pp; if ((float)fp > pp) fp--;
            J.fy[y] = pp - (float)fp;
            if (J.nvruns == 0 || J.vrun_cell[J.nvruns - 1] != fp) { J.vrun_start[J.nvruns] = y; J.vrun_cell[J.nvruns] = fp; J.nvruns++; }
        }
        J.vrun_start[J.nvruns] = rh;
        J.use_rcas = s->sharpness > 0;
        J.rcas_con = post_rcas_con(s->sharpness);
    }

    if (s->filter == POST_FSR) { run_phase(0, sh + 2 * PAD); run_phase(1, sh + 2 * PAD); }
    run_phase(2, dh);
}
static void run_phase(int ph, int rows) {
    phase = ph; phase_rows = rows;
    atomic_store(&next_chunk, 0);
    if (pool_n > 1) {
        pthread_mutex_lock(&pool_m);
        pool_gen++; pool_active = pool_n - 1;
        pthread_cond_broadcast(&pool_go);
        pthread_mutex_unlock(&pool_m);
    }
    run_chunks(&scratch[0]);
    if (pool_n > 1) {
        pthread_mutex_lock(&pool_m);
        while (pool_active > 0) pthread_cond_wait(&pool_done, &pool_m);
        pthread_mutex_unlock(&pool_m);
    }
}
