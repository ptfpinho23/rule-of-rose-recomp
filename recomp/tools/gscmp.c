/* gscmp.c — compare two gsreplay -o files frame by frame.
 *   gscmp REF.raw OUT.raw [-scale N] [-png DIR FRAME...]
 * OUT may be N times the size of REF (-scale N): it is then point-sampled at
 * the first sub-pixel of each block for the numbers. -png writes ref_NNN.png,
 * out_NNN.png (full size) and diff_NNN.png (differences x16) for the frames
 * listed. Build/run with recomp/tools/gscmp.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef uint32_t u32; typedef uint8_t u8;
static u32 crc_tab[256];
static u32 crc32_(u32 c, const u8 *p, size_t n) {
    if (!crc_tab[1]) for (u32 i = 0; i < 256; i++) { u32 x = i; for (int k = 0; k < 8; k++) x = (x & 1) ? 0xEDB88320u ^ (x >> 1) : x >> 1; crc_tab[i] = x; }
    c = ~c; while (n--) c = crc_tab[(c ^ *p++) & 255] ^ (c >> 8); return ~c;
}
static void be32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static void chunk(FILE *f, const char *type, const u8 *data, u32 n) {
    u8 h[8]; be32(h, n); memcpy(h + 4, type, 4); fwrite(h, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    u32 c = crc32_(0, h + 4, 4); if (n) c = crc32_(c, data, n);
    u8 t[4]; be32(t, c); fwrite(t, 1, 4, f);
}
static void save_png(const char *path, const u32 *px, int w, int h) {      /* XRGB, stored (uncompressed) */
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
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
static u32 *rd(FILE *f, u32 *w, u32 *h) {
    u32 hd[2];
    if (fread(hd, 4, 2, f) != 2) return 0;
    *w = hd[0]; *h = hd[1];
    u32 *p = malloc((size_t)hd[0] * hd[1] * 4 + 4);
    if (fread(p, 4, (size_t)hd[0] * hd[1], f) != (size_t)hd[0] * hd[1]) { free(p); return 0; }
    return p;
}
int main(int argc, char **argv) {
    const char *png = 0; int want[64], nwant = 0, scale = 1, npos = 0; const char *pos[2] = { 0, 0 };
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-png") && i + 1 < argc) { png = argv[++i]; while (i + 1 < argc && nwant < 64) want[nwant++] = atoi(argv[++i]); }
        else if (npos < 2) pos[npos++] = argv[i];
    }
    if (npos < 2) { fprintf(stderr, "usage: gscmp REF.raw OUT.raw [-scale N] [-png DIR FRAME...]\n"); return 2; }
    FILE *fa = fopen(pos[0], "rb"), *fb = fopen(pos[1], "rb");
    if (!fa || !fb) { perror("open"); return 1; }
    unsigned long long tot = 0, totpx = 0, gt1 = 0, gt8 = 0, worst = 0; int mx = 0, worstk = -1, bad = 0, k = 0;
    for (;; k++) {
        u32 wa, ha, wb, hb; u32 *a = rd(fa, &wa, &ha), *b = rd(fb, &wb, &hb);
        if (!a || !b) break;
        int dump = 0; for (int i = 0; i < nwant; i++) if (want[i] == k) dump = 1;
        if (wa * (u32)scale != wb || ha * (u32)scale != hb) { bad++; }
        else if (wa) {
            unsigned long long n = 0; u32 *d = dump ? malloc((size_t)wa * ha * 4) : 0;
            for (u32 y = 0; y < ha; y++) for (u32 x = 0; x < wa; x++) {
                u32 p = a[y * wa + x], q = b[(size_t)(y * scale) * wb + x * scale]; int m = 0;
                for (int s = 0; s < 24; s += 8) { int v = (int)((p >> s) & 255) - (int)((q >> s) & 255); if (v < 0) v = -v; if (v > m) m = v; }
                if (m) n++; if (m > 1) gt1++; if (m > 8) gt8++; if (m > mx) mx = m;
                if (d) { u32 g = (u32)(m * 16 > 255 ? 255 : m * 16); d[y * wa + x] = g * 0x010101u; }
            }
            tot += n; totpx += (unsigned long long)wa * ha;
            if (n > worst) { worst = n; worstk = k; }
            if (dump) {
                char path[512];
                snprintf(path, sizeof path, "%s/ref_%03d.png", png, k); save_png(path, a, (int)wa, (int)ha);
                snprintf(path, sizeof path, "%s/out_%03d.png", png, k); save_png(path, b, (int)wb, (int)hb);
                snprintf(path, sizeof path, "%s/diff_%03d.png", png, k); save_png(path, d, (int)wa, (int)ha);
                free(d);
            }
        }
        free(a); free(b);
    }
    printf("%d frames: %llu of %llu pixels differ (%.3f%%), by more than 1: %llu (%.3f%%), by more than 8: %llu (%.4f%%), max channel difference %d, worst frame %d (%llu px)%s\n",
           k, tot, totpx, totpx ? 100.0 * tot / totpx : 0, gt1, totpx ? 100.0 * gt1 / totpx : 0, gt8, totpx ? 100.0 * gt8 / totpx : 0, mx, worstk, worst, bad ? " SIZE MISMATCHES" : "");
    return 0;
}
