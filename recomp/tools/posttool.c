/* posttool.c — stand-alone driver for recomp/rt/post.c (build: posttool.sh).
 *
 *   posttool run IN.png OUT.png W H [name=value ...]     scale/colour one screenshot
 *   posttool bench IN.png W H N [name=value ...]         time N calls of post_run
 *   posttool check IN.png W H [name=value ...]           compare the FSR path with the scalar
 *                                                        transliteration of ffx_fsr1.h below
 *   posttool montage OUT.png ZOOM X Y W H label=FILE ... same crop of several images, side by side, labelled
 *
 * IN.png must be a screenshot written by the game (--shots / --post-shots:
 * 8-bit RGB, stored deflate); that is the only PNG flavour read here.
 * name=value are the game's switches without dashes: filter=fsr sharpness=60
 * gamma=130 aspect=stretch ... */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <emmintrin.h>
#include "post.h"

typedef uint8_t u8; typedef uint32_t u32;

/* ---- PNG (stored deflate only) ---- */
static u32 crc_tab[256];
static u32 crc32_(u32 c, const u8 *p, size_t n) {
    if (!crc_tab[1]) for (u32 i = 0; i < 256; i++) { u32 x = i; for (int k = 0; k < 8; k++) x = (x & 1) ? 0xEDB88320u ^ (x >> 1) : x >> 1; crc_tab[i] = x; }
    c = ~c; while (n--) c = crc_tab[(c ^ *p++) & 255] ^ (c >> 8); return ~c;
}
static void be32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static u32 rbe32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static void chunk(FILE *f, const char *type, const u8 *data, u32 n) {
    u8 h[8]; be32(h, n); memcpy(h + 4, type, 4); fwrite(h, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    u32 c = crc32_(0, h + 4, 4); if (n) c = crc32_(c, data, n);
    u8 t[4]; be32(t, c); fwrite(t, 1, 4, f);
}
static void save_png(const char *path, const u32 *px, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    size_t row = (size_t)w * 3 + 1, raw_n = row * (size_t)h;
    u8 *raw = malloc(raw_n), *z = malloc(raw_n + raw_n / 65535 * 5 + 16);
    for (int y = 0; y < h; y++) {
        u8 *r = raw + row * (size_t)y; *r++ = 0;
        for (int x = 0; x < w; x++) { u32 c = px[(size_t)y * w + x]; *r++ = (u8)(c >> 16); *r++ = (u8)(c >> 8); *r++ = (u8)c; }
    }
    size_t o = 0; z[o++] = 0x78; z[o++] = 0x01;
    u32 a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t pos = 0; pos < raw_n;) {
        size_t n = raw_n - pos > 65535 ? 65535 : raw_n - pos;
        z[o++] = (pos + n == raw_n); z[o++] = (u8)n; z[o++] = (u8)(n >> 8); z[o++] = (u8)~n; z[o++] = (u8)(~n >> 8);
        memcpy(z + o, raw + pos, n); o += n; pos += n;
    }
    be32(z + o, (b << 16) | a); o += 4;
    static const u8 sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);
    u8 ih[13]; be32(ih, (u32)w); be32(ih + 4, (u32)h); ih[8] = 8; ih[9] = 2; ih[10] = ih[11] = ih[12] = 0;
    chunk(f, "IHDR", ih, 13); chunk(f, "IDAT", z, (u32)o); chunk(f, "IEND", 0, 0);
    fclose(f); free(raw); free(z);
}
static u32 *load_png(const char *path, int *w, int *h) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    u8 *d = malloc((size_t)n), *z = malloc((size_t)n); size_t zn = 0;
    if (fread(d, 1, (size_t)n, f) != (size_t)n || n < 33 || memcmp(d + 1, "PNG", 3)) { fprintf(stderr, "%s: not a PNG\n", path); exit(1); }
    fclose(f);
    *w = (int)rbe32(d + 16); *h = (int)rbe32(d + 20);
    if (d[24] != 8 || d[25] != 2) { fprintf(stderr, "%s: only 8-bit RGB PNGs written by the game are supported\n", path); exit(1); }
    for (size_t p = 8; p + 12 <= (size_t)n;) {
        u32 len = rbe32(d + p);
        if (!memcmp(d + p + 4, "IDAT", 4)) { memcpy(z + zn, d + p + 8, len); zn += len; }
        p += 12 + len;
    }
    size_t row = (size_t)*w * 3 + 1, raw_n = row * (size_t)*h, o = 0;
    u8 *raw = malloc(raw_n);
    for (size_t p = 2; p + 5 <= zn && o < raw_n;) {
        if (z[p] > 1) { fprintf(stderr, "%s: compressed PNG data is not supported (stored blocks only)\n", path); exit(1); }
        size_t len = z[p + 1] | (size_t)z[p + 2] << 8;
        if (o + len > raw_n) len = raw_n - o;
        memcpy(raw + o, z + p + 5, len); o += len; p += 5 + len;
    }
    u32 *px = malloc((size_t)*w * (size_t)*h * 4);
    for (int y = 0; y < *h; y++) {
        const u8 *r = raw + row * (size_t)y + 1;
        for (int x = 0; x < *w; x++, r += 3) px[(size_t)y * *w + x] = (u32)r[0] << 16 | (u32)r[1] << 8 | r[2];
    }
    free(d); free(z); free(raw);
    return px;
}

/* ---- scalar reference: FsrEasuF / FsrRcasF transliterated from ffx_fsr1.h ---- */
static float prx_lo_rcp(float a) { u32 u; memcpy(&u, &a, 4); u = 0x7ef07ebbu - u; memcpy(&a, &u, 4); return a; }
static float prx_lo_rsq(float a) { u32 u; memcpy(&u, &a, 4); u = 0x5f347d74u - (u >> 1); memcpy(&a, &u, 4); return a; }
static float prx_med_rcp(float a) { u32 u; float b; memcpy(&u, &a, 4); u = 0x7ef19fffu - u; memcpy(&b, &u, 4); return b * (-b * a + 2.0f); }
static float fmin_(float a, float b) { return a < b ? a : b; }     /* like GLSL min/max; a NaN operand loses */
static float fmax_(float a, float b) { return a > b ? a : b; }
static float fabs_(float a) { return a < 0 ? -a : a; }
static float fsat(float a) { return fmin_(1.0f, fmax_(0.0f, a)); }
typedef struct { float r, g, b; } C3;
static const u32 *ref_src; static int ref_sw, ref_sh;
static C3 ref_tex(int x, int y) {
    x = x < 0 ? 0 : x >= ref_sw ? ref_sw - 1 : x; y = y < 0 ? 0 : y >= ref_sh ? ref_sh - 1 : y;
    u32 c = ref_src[(size_t)y * ref_sw + x];
    C3 o = { (float)((c >> 16) & 255) * (1.0f / 255.0f), (float)((c >> 8) & 255) * (1.0f / 255.0f), (float)(c & 255) * (1.0f / 255.0f) };
    return o;
}
static float luma(C3 c) { return c.b * 0.5f + (c.r * 0.5f + c.g); }
static void easu_set(float *dirx, float *diry, float *len, float w, float lA, float lB, float lC, float lD, float lE) {
    float dc = lD - lC, cb = lC - lB;
    float lenX = fmax_(fabs_(dc), fabs_(cb)); lenX = prx_lo_rcp(lenX);
    float dirX = lD - lB;
    *dirx += dirX * w;
    lenX = fsat(fabs_(dirX) * lenX); lenX *= lenX; *len += lenX * w;
    float ec = lE - lC, ca = lC - lA;
    float lenY = fmax_(fabs_(ec), fabs_(ca)); lenY = prx_lo_rcp(lenY);
    float dirY = lE - lA;
    *diry += dirY * w;
    lenY = fsat(fabs_(dirY) * lenY); lenY *= lenY; *len += lenY * w;
}
static void easu_tap(C3 *aC, float *aW, float offx, float offy, float dirx, float diry, float lenx, float leny, float lob, float clp, C3 c) {
    float vx = (offx * dirx) + (offy * diry), vy = (offx * (-diry)) + (offy * dirx);
    vx *= lenx; vy *= leny;
    float d2 = vx * vx + vy * vy;
    d2 = fmin_(d2, clp);
    float wB = (float)(2.0 / 5.0) * d2 + -1.0f, wA = lob * d2 + -1.0f;
    wB *= wB; wA *= wA;
    wB = (float)(25.0 / 16.0) * wB + (float)(-(25.0 / 16.0 - 1.0));
    float w = wB * wA;
    aC->r += c.r * w; aC->g += c.g * w; aC->b += c.b * w; *aW += w;
}
static C3 ref_easu(int ipx, int ipy, float c0x, float c0y, float c0z, float c0w) {
    float ppx = (float)ipx * c0x + c0z, ppy = (float)ipy * c0y + c0w;
    int fx = (int)ppx; if ((float)fx > ppx) fx--;
    int fy = (int)ppy; if ((float)fy > ppy) fy--;
    ppx -= (float)fx; ppy -= (float)fy;
    C3 b = ref_tex(fx, fy - 1), c = ref_tex(fx + 1, fy - 1), e = ref_tex(fx - 1, fy), f = ref_tex(fx, fy), g = ref_tex(fx + 1, fy),
       h = ref_tex(fx + 2, fy), i = ref_tex(fx - 1, fy + 1), j = ref_tex(fx, fy + 1), k = ref_tex(fx + 1, fy + 1), l = ref_tex(fx + 2, fy + 1),
       n = ref_tex(fx, fy + 2), o = ref_tex(fx + 1, fy + 2);
    float bL = luma(b), cL = luma(c), eL = luma(e), fL = luma(f), gL = luma(g), hL = luma(h), iL = luma(i), jL = luma(j),
          kL = luma(k), lL = luma(l), nL = luma(n), oL = luma(o);
    float dirx = 0, diry = 0, len = 0;
    easu_set(&dirx, &diry, &len, (1.0f - ppx) * (1.0f - ppy), bL, eL, fL, gL, jL);
    easu_set(&dirx, &diry, &len, ppx * (1.0f - ppy), cL, fL, gL, hL, kL);
    easu_set(&dirx, &diry, &len, (1.0f - ppx) * ppy, fL, iL, jL, kL, nL);
    easu_set(&dirx, &diry, &len, ppx * ppy, gL, jL, kL, lL, oL);
    float dirR = dirx * dirx + diry * diry;
    int zro = dirR < (1.0f / 32768.0f);
    dirR = prx_lo_rsq(dirR);
    dirR = zro ? 1.0f : dirR;
    dirx = zro ? 1.0f : dirx;
    dirx *= dirR; diry *= dirR;
    len = len * 0.5f; len *= len;
    float stretch = (dirx * dirx + diry * diry) * prx_lo_rcp(fmax_(fabs_(dirx), fabs_(diry)));
    float len2x = 1.0f + (stretch - 1.0f) * len, len2y = 1.0f + -0.5f * len;
    float lob = 0.5f + (float)((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = prx_lo_rcp(lob);
    C3 mn = { fmin_(fmin_(fmin_(f.r, g.r), j.r), k.r), fmin_(fmin_(fmin_(f.g, g.g), j.g), k.g), fmin_(fmin_(fmin_(f.b, g.b), j.b), k.b) };
    C3 mx = { fmax_(fmax_(fmax_(f.r, g.r), j.r), k.r), fmax_(fmax_(fmax_(f.g, g.g), j.g), k.g), fmax_(fmax_(fmax_(f.b, g.b), j.b), k.b) };
    C3 aC = { 0, 0, 0 }; float aW = 0;
    easu_tap(&aC, &aW, 0.0f - ppx, -1.0f - ppy, dirx, diry, len2x, len2y, lob, clp, b);
    easu_tap(&aC, &aW, 1.0f - ppx, -1.0f - ppy, dirx, diry, len2x, len2y, lob, clp, c);
    easu_tap(&aC, &aW, -1.0f - ppx, 1.0f - ppy, dirx, diry, len2x, len2y, lob, clp, i);
    easu_tap(&aC, &aW, 0.0f - ppx, 1.0f - ppy, dirx, diry, len2x, len2y, lob, clp, j);
    easu_tap(&aC, &aW, 0.0f - ppx, 0.0f - ppy, dirx, diry, len2x, len2y, lob, clp, f);
    easu_tap(&aC, &aW, -1.0f - ppx, 0.0f - ppy, dirx, diry, len2x, len2y, lob, clp, e);
    easu_tap(&aC, &aW, 1.0f - ppx, 1.0f - ppy, dirx, diry, len2x, len2y, lob, clp, k);
    easu_tap(&aC, &aW, 2.0f - ppx, 1.0f - ppy, dirx, diry, len2x, len2y, lob, clp, l);
    easu_tap(&aC, &aW, 2.0f - ppx, 0.0f - ppy, dirx, diry, len2x, len2y, lob, clp, h);
    easu_tap(&aC, &aW, 1.0f - ppx, 0.0f - ppy, dirx, diry, len2x, len2y, lob, clp, g);
    easu_tap(&aC, &aW, 1.0f - ppx, 2.0f - ppy, dirx, diry, len2x, len2y, lob, clp, o);
    easu_tap(&aC, &aW, 0.0f - ppx, 2.0f - ppy, dirx, diry, len2x, len2y, lob, clp, n);
    float rcp = 1.0f / aW;
    C3 pix = { fmin_(mx.r, fmax_(mn.r, aC.r * rcp)), fmin_(mx.g, fmax_(mn.g, aC.g * rcp)), fmin_(mx.b, fmax_(mn.b, aC.b * rcp)) };
    return pix;
}
static float rcas_lobe(float b, float d, float e, float f, float h) {
    float mn4 = fmin_(fmin_(fmin_(b, d), f), h), mx4 = fmax_(fmax_(fmax_(b, d), f), h);
    float hitMin = fmin_(mn4, e) * (1.0f / (4.0f * mx4));
    float hitMax = (1.0f - fmax_(mx4, e)) * (1.0f / (4.0f * mn4 + -4.0f));
    float nh = -hitMin;
    if (nh != nh) return hitMax;            /* shader max() drops a NaN operand */
    if (hitMax != hitMax) return nh;
    return fmax_(nh, hitMax);
}
static C3 ref_rcas(const C3 *img, int w, int h, int x, int y, float con) {
#define AT(xx, yy) img[(size_t)((yy) < 0 ? 0 : (yy) >= h ? h - 1 : (yy)) * w + ((xx) < 0 ? 0 : (xx) >= w ? w - 1 : (xx))]
    C3 b = AT(x, y - 1), d = AT(x - 1, y), e = AT(x, y), f = AT(x + 1, y), hh = AT(x, y + 1);
#undef AT
    float lobeR = rcas_lobe(b.r, d.r, e.r, f.r, hh.r), lobeG = rcas_lobe(b.g, d.g, e.g, f.g, hh.g), lobeB = rcas_lobe(b.b, d.b, e.b, f.b, hh.b);
    float lobe = fmax_((float)(-(0.25 - (1.0 / 16.0))), fmin_(fmax_(fmax_(lobeR, lobeG), lobeB), 0.0f)) * con;
    float rcpL = prx_med_rcp(4.0f * lobe + 1.0f);
    C3 o = { (lobe * b.r + lobe * d.r + lobe * hh.r + lobe * f.r + e.r) * rcpL, (lobe * b.g + lobe * d.g + lobe * hh.g + lobe * f.g + e.g) * rcpL,
             (lobe * b.b + lobe * d.b + lobe * hh.b + lobe * f.b + e.b) * rcpL };
    return o;
}
/* UNORM8 store: round to nearest (ties to even), as the SSE2 conversion in post.c does */
static u32 to8(float v) { v = v < 0 ? 0 : v > 1 ? 1 : v; return (u32)_mm_cvtss_si32(_mm_set_ss(v * 255.0f)); }

/* ---- 5x7 labels ---- */
static const char font_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-:=+/% ";
static const u8 font[][7] = {
    {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},{30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
    {14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},{7,2,2,2,2,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},{30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},{17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4},{31,1,2,4,8,16,31},
    {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},{31,2,4,2,1,17,14},{2,6,10,18,31,2,2},{31,16,30,1,1,17,14},
    {6,8,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},{14,17,17,15,1,2,12},
    {0,0,0,0,0,12,12},{0,0,0,31,0,0,0},{0,12,12,0,12,12,0},{0,0,31,0,31,0,0},{0,4,4,31,4,4,0},{1,1,2,4,8,16,16},{25,25,2,4,8,19,19},{0,0,0,0,0,0,0},
};
static void draw_text(u32 *img, int w, int h, int x, int y, const char *s, int scale, u32 col) {
    for (; *s; s++, x += 6 * scale) {
        char ch = *s; if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        const char *p = strchr(font_chars, ch); if (!p) continue;
        const u8 *g = font[p - font_chars];
        for (int r = 0; r < 7; r++) for (int c = 0; c < 5; c++) if (g[r] & (16 >> c))
            for (int yy = 0; yy < scale; yy++) for (int xx = 0; xx < scale; xx++) {
                int px = x + c * scale + xx, py = y + r * scale + yy;
                if (px >= 0 && px < w && py >= 0 && py < h) img[(size_t)py * w + px] = col;
            }
    }
}

static void settings(PostSettings *s, int argc, char **argv, int from) {
    post_defaults(s);
    for (int i = from; i < argc; i++) {
        char name[64]; const char *eq = strchr(argv[i], '=');
        if (!eq || eq - argv[i] >= (int)sizeof name) { fprintf(stderr, "bad setting %s\n", argv[i]); exit(2); }
        memcpy(name, argv[i], (size_t)(eq - argv[i])); name[eq - argv[i]] = 0;
        if (!post_set(s, name, eq + 1)) { fprintf(stderr, "bad setting %s\n", argv[i]); exit(2); }
    }
}
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6; }

int main(int argc, char **argv) {
    PostSettings s; int sw, sh;
    if (argc >= 6 && !strcmp(argv[1], "run")) {
        u32 *src = load_png(argv[2], &sw, &sh);
        int dw = atoi(argv[4]), dh = atoi(argv[5]);
        settings(&s, argc, argv, 6);
        u32 *dst = malloc((size_t)dw * (size_t)dh * 4);
        post_run(src, sw, sh, dst, dw, dh, &s);
        save_png(argv[3], dst, dw, dh);
        return 0;
    }
    if (argc >= 6 && !strcmp(argv[1], "bench")) {
        u32 *src = load_png(argv[2], &sw, &sh);
        int dw = atoi(argv[3]), dh = atoi(argv[4]), n = atoi(argv[5]);
        settings(&s, argc, argv, 6);
        u32 *dst = malloc((size_t)dw * (size_t)dh * 4);
        for (int i = 0; i < 3; i++) post_run(src, sw, sh, dst, dw, dh, &s);
        double best = 1e9, t0 = now_ms();
        for (int i = 0; i < n; i++) { double a = now_ms(); post_run(src, sw, sh, dst, dw, dh, &s); double b = now_ms() - a; if (b < best) best = b; }
        printf("%dx%d -> %dx%d: %.2f ms average, %.2f ms best\n", sw, sh, dw, dh, (now_ms() - t0) / n, best);
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "check")) {
        u32 *src = load_png(argv[2], &sw, &sh);
        int dw = atoi(argv[3]), dh = atoi(argv[4]);
        settings(&s, argc, argv, 5);
        s.filter = POST_FSR; s.brightness = s.contrast = s.bloom = 0; s.gamma = s.saturation = 100;
        u32 *dst = malloc((size_t)dw * (size_t)dh * 4);
        post_run(src, sw, sh, dst, dw, dh, &s);
        int rx, ry, rw, rh; post_rect(dw, dh, &s, &rx, &ry, &rw, &rh);
        ref_src = src; ref_sw = sw; ref_sh = sh;
        float c0x = (float)sw * (1.0f / (float)rw), c0y = (float)sh * (1.0f / (float)rh);
        float c0z = 0.5f * (float)sw * (1.0f / (float)rw) - 0.5f, c0w = 0.5f * (float)sh * (1.0f / (float)rh) - 0.5f;
        C3 *mid = malloc((size_t)rw * (size_t)rh * sizeof *mid);
        for (int y = 0; y < rh; y++) for (int x = 0; x < rw; x++) mid[(size_t)y * rw + x] = ref_easu(x, y, c0x, c0y, c0z, c0w);
        float con = post_rcas_con(s.sharpness);
        long diff = 0, maxd = 0;
        for (int y = 0; y < rh; y++) for (int x = 0; x < rw; x++) {
            C3 p = s.sharpness > 0 ? ref_rcas(mid, rw, rh, x, y, con) : mid[(size_t)y * rw + x];
            u32 want = to8(p.r) << 16 | to8(p.g) << 8 | to8(p.b), got = dst[(size_t)(y + ry) * dw + x + rx];
            if (want != got) {
                diff++;
                for (int sft = 0; sft < 24; sft += 8) { long dd = labs((long)((want >> sft) & 255) - (long)((got >> sft) & 255)); if (dd > maxd) maxd = dd; }
            }
        }
        printf("%dx%d -> %dx%d sharpness %d: %ld of %ld pixels differ from the scalar reference (largest channel difference %ld)\n",
               sw, sh, rw, rh, s.sharpness, diff, (long)rw * rh, maxd);
        return diff ? 1 : 0;
    }
    if (argc >= 9 && !strcmp(argv[1], "montage")) {
        int zoom = atoi(argv[3]), cx = atoi(argv[4]), cy = atoi(argv[5]), cw = atoi(argv[6]), ch = atoi(argv[7]);
        int n = argc - 8, tw = cw * zoom, th = ch * zoom, gap = 6, bar = 22;
        int W = n * tw + (n + 1) * gap, H = th + bar + gap;
        u32 *img = malloc((size_t)W * (size_t)H * 4);
        for (size_t i = 0; i < (size_t)W * (size_t)H; i++) img[i] = 0x202020;
        for (int i = 0; i < n; i++) {
            char label[128]; const char *eq = strchr(argv[8 + i], '=');
            if (!eq) { fprintf(stderr, "montage: label=FILE expected\n"); return 2; }
            snprintf(label, sizeof label, "%.*s", (int)(eq - argv[8 + i]), argv[8 + i]);
            int w, h; u32 *p = load_png(eq + 1, &w, &h);
            int x0 = gap + i * (tw + gap);
            for (int y = 0; y < th; y++) for (int x = 0; x < tw; x++) {
                int sx = cx + x / zoom, sy = cy + y / zoom;
                img[(size_t)(y + bar) * W + x0 + x] = (sx >= 0 && sx < w && sy >= 0 && sy < h) ? p[(size_t)sy * w + sx] : 0;
            }
            draw_text(img, W, H, x0 + 2, 4, label, 2, 0xFFFFFF);
            free(p);
        }
        save_png(argv[2], img, W, H);
        return 0;
    }
    fprintf(stderr, "usage: posttool run IN.png OUT.png W H [name=value ...]\n"
                    "       posttool bench IN.png W H N [name=value ...]\n"
                    "       posttool check IN.png W H [name=value ...]\n"
                    "       posttool montage OUT.png ZOOM X Y W H label=FILE ...\n");
    return 2;
}
