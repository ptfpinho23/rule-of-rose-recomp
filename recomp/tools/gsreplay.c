/* gsreplay.c — replay a ROR_GSCAP capture through the real GS (recomp/rt/gs.c
 * is included, or the reference copy tools/gs_ref.c with -DGS_REF), write every
 * frame's display image and report timing.
 *   gsreplay CAP [-o frames.raw] [-n loops] [-f N (stop after N frames)] [-c]
 * -c: merge consecutive image-data calls into one, as gif.c delivers them now
 *     (captures made before gif.c batched IMAGE data hold 16-byte calls).
 * -fpsm N / -zpsm N: rewrite the pixel format in every FRAME / ZBUF write. The
 *     pictures are then garbage, but they are the same garbage in any correct
 *     renderer, which lets the captures exercise formats and buffer overlaps
 *     the game itself never uses.
 * frames.raw: per frame "u32 w, u32 h, w*h u32 pixels" (w = h = 0: display off).
 * Build/run with recomp/tools/gsreplay.sh. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <time.h>
#include <stdarg.h>
#ifndef _WIN32
#include <signal.h>
#include <sys/time.h>
#include <ucontext.h>
#else
#include <windows.h>
#define setenv(n, v, o) _putenv_s(n, v)
long ror_win_sysconf(int name) { SYSTEM_INFO si; (void)name; GetSystemInfo(&si); return (long)si.dwNumberOfProcessors; }
#endif
#include <xmmintrin.h>
#if defined(GS_SRC)
#include GS_SRC
#elif defined(GS_REF)
#include "gs_ref.c"
#else
#include "../rt/gs.c"
#endif

void rt_fatal(Ctx *c, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); exit(1);
}
void rt_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); }
void intc_raise(int cause) { (void)cause; }
/* --wide (rt/wide.c, rt/gif.c) is not part of a replay: captures hold the primitives as the game sent them */
int g_gif_path, g_wide_want_p1;
void wide_path1(u32 fbp) { (void)fbp; }
int wide_squeeze(u32 fbp, float *inv) { (void)fbp; (void)inv; return 0; }
void wide_flip(u32 fbp) { (void)fbp; }
void wide_frustum(float *cam) { cam[0] = 5.0f; cam[1] = 2000.0f; cam[2] = 0.83f; cam[3] = 65530.0f; cam[4] = 1.0f; }
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
void save_png(const char *path, const u32 *px, int w, int h) {      /* XRGB, stored (uncompressed) */
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


/* GSPROF=file: sampling profile (1 kHz of process CPU time, all threads) in the
 * format of recomp/tools/prof.py; samples from the calling thread go to
 * file.main, those from render threads to file.work. */
recomp_fn rt_lookup(Ctx *c, u32 addr) { (void)c; (void)addr; return 0; }
#ifndef _WIN32
#define PROF_MAX (1u << 22)
static u64 *prof_buf[2]; static atomic_uint prof_n[2]; static pthread_t prof_main;
static void on_prof(int sig, siginfo_t *si, void *ucv) {
    (void)sig; (void)si;
    int k = !pthread_equal(pthread_self(), prof_main);
    unsigned i = atomic_fetch_add(&prof_n[k], 1);
    if (i < PROF_MAX) prof_buf[k][i] = (u64)((ucontext_t *)ucv)->uc_mcontext.gregs[16 /* REG_RIP */];
}
static void prof_start(void) {
    if (!getenv("GSPROF")) return;
    prof_main = pthread_self();
    for (int k = 0; k < 2; k++) { prof_buf[k] = malloc(PROF_MAX * 8); prof_buf[k][0] = (u64)&rt_lookup; prof_n[k] = 1; }
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_prof; sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, 0);
    struct itimerval it = { { 0, 1000 }, { 0, 1000 } };
    setitimer(ITIMER_PROF, &it, 0);
}
static void prof_stop(void) {
    const char *path = getenv("GSPROF");
    if (!path) return;
    struct itimerval it = { { 0, 0 }, { 0, 0 } };
    setitimer(ITIMER_PROF, &it, 0);
    for (int k = 0; k < 2; k++) {
        char name[512]; snprintf(name, sizeof name, "%s.%s", path, k ? "work" : "main");
        FILE *f = fopen(name, "wb");
        unsigned n = prof_n[k] < PROF_MAX ? prof_n[k] : PROF_MAX;
        if (f) { fwrite(prof_buf[k], 8, n, f); fclose(f); }
    }
}

#else
static void prof_start(void) {}
static void prof_stop(void) {}
#endif

u64 plat_time_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (u64)t.tv_sec * 1000000000ull + (u64)t.tv_nsec; }
void plat_sleep_until(u64 t_ns) { while (plat_time_ns() < t_ns) { struct timespec d = { 0, 200000 }; nanosleep(&d, 0); } }
static double now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}
static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

int main(int argc, char **argv) {
    const char *cap = 0, *out = 0; int loops = 1, max_frames = 0, coalesce = 0, fpsm = -1, zpsm = -1;
    static u8 cbuf[1u << 22];
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) loops = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) max_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-c")) coalesce = 1;
        else if (!strcmp(argv[i], "-hw") && i + 1 < argc) setenv("ROR_GS_HW", argv[++i], 1);   /* GPU renderer at this scale */
        else if (!strcmp(argv[i], "-fpsm") && i + 1 < argc) fpsm = (int)strtol(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "-zpsm") && i + 1 < argc) zpsm = (int)strtol(argv[++i], 0, 0);
        else cap = argv[i];
    }
    if (!cap) { fprintf(stderr, "usage: gsreplay CAP [-o frames.raw] [-n loops] [-f frames]\n"); return 2; }
    FILE *f = fopen(cap, "rb");
    if (!f) { perror(cap); return 1; }
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "GSCAP001", 8)) { fprintf(stderr, "%s: not a capture\n", cap); return 1; }
    _mm_setcsr(_mm_getcsr() | 0x8040);       /* FTZ | DAZ, as the game runs (main.c) */
    gs_init();
    long snap_pos = ftell(f);
    if (!gs_snapshot(f, 0)) { fprintf(stderr, "%s: truncated snapshot\n", cap); return 1; }
    long ev_pos = ftell(f);
    fseek(f, 0, SEEK_END);
    size_t n = (size_t)(ftell(f) - ev_pos);
    u8 *ev = malloc(n);
    fseek(f, ev_pos, SEEK_SET);
    if (fread(ev, 1, n, f) != n) return 1;
    if (getenv("GSBATCH")) {
        /* Draw-list structure, for the frame interpolation study: one line per
         * batch (run of drawing vertex kicks with unchanged PRIM/TEX0/
         * ALPHA/TEST; FRAME is left out because the two buffers alternate), logic frames separated at display flips (DISPFB writes
         * with a new value). No rendering. */
        FILE *fb = fopen(getenv("GSBATCH"), "w");
        u64 reg[256] = { 0 }, key = 0, lastkey = ~0ull, disp = 0; unsigned nv = 0; size_t p = 0;
        while (p < n) {
            u8 type = ev[p++];
            if (type == 1) {
                u8 r = ev[p]; u64 d; memcpy(&d, ev + p + 1, 8); p += 9;
                if (r == 4 || r == 5) {
                    int cx = (int)((reg[0] >> 9) & 1);
                    key = reg[0] * 0x9E3779B97F4A7C15ull ^ (reg[6 + cx] & 0xFFFFFFFFFFull) * 31 ^ reg[0x42 + cx] * 17 ^ reg[0x47 + cx] * 7;
                    if (key != lastkey) { if (nv) fprintf(fb, "B %016llx %u\n", (unsigned long long)lastkey, nv); lastkey = key; nv = 0; }
                    nv++;
                    if (getenv("GSBATCH_XY")) fprintf(fb, "V %u %u\n", (unsigned)(d & 0xFFFF), (unsigned)((d >> 16) & 0xFFFF));
                } else if (r != 1 && r != 2 && r != 3 && r != 0x0A && r != 0x0C && r != 0x0D) reg[r] = d;
            } else if (type == 2) { u32 b; memcpy(&b, ev + p, 4); p += 4 + b; }
            else if (type == 3) {
                u32 a; u64 v; memcpy(&a, ev + p, 4); memcpy(&v, ev + p + 4, 8); p += 12;
                if (((a & 0xFFFF) == 0x70 || (a & 0xFFFF) == 0x90) && v != disp) {
                    if (nv) fprintf(fb, "B %016llx %u\n", (unsigned long long)lastkey, nv);
                    nv = 0; lastkey = ~0ull; disp = v; fprintf(fb, "F\n");
                }
            } else if (type == 5) p += 4;
            else if (type == 4) p++;
            else break;
        }
        fclose(fb);
        return 0;
    }
    FILE *fo = out ? fopen(out, "wb") : 0;
    double *times = malloc(sizeof(double) * 100000); int nt = 0;
    u64 hash_all = 1469598103934665603ull;
    prof_start();
    for (int loop = 0; loop < loops; loop++) {
        if (loop) { fseek(f, snap_pos, SEEK_SET); gs_snapshot(f, 0); }
        size_t p = 0; int frames = 0;
        double t0 = now_ms();
        while (p < n) {
            u8 type = ev[p++];
            if (type == 1) {
                u64 d; memcpy(&d, ev + p + 1, 8);
                if (fpsm >= 0 && (ev[p] == 0x4C || ev[p] == 0x4D)) d = (d & ~(0x3Full << 24)) | ((u64)fpsm << 24);
                if (zpsm >= 0 && (ev[p] == 0x4E || ev[p] == 0x4F)) d = (d & ~(0xFull << 24)) | ((u64)zpsm << 24);
                gs_write_reg(ev[p], d); p += 9;
            }
            else if (type == 2) {
                u32 b; memcpy(&b, ev + p, 4);
                if (coalesce) {                 /* deliver a run of consecutive data events as one call */
                    u32 total = 0; size_t q = p - 1;
                    while (q < n && ev[q] == 2) { memcpy(&b, ev + q + 1, 4); if (total + b > (1u << 22)) break; memcpy(cbuf + total, ev + q + 5, b); total += b; q += 5 + b; }
                    gs_write_hwreg(cbuf, total); p = q;
                } else { gs_write_hwreg(ev + p + 4, b); p += 4 + b; }
            }
            else if (type == 3) { u32 a; u64 v; memcpy(&a, ev + p, 4); memcpy(&v, ev + p + 4, 8); gs_write_priv(a, v); p += 12; }
            else if (type == 5) { float q; memcpy(&q, ev + p, 4); gs_set_q(q); p += 4; }
            else if (type == 4) {
                int start = ev[p++];
                gs_vblank(start);
                if (!start) continue;
                u32 *px; int w = 0, h = 0;
                if (!gs_get_frame(&px, &w, &h)) w = h = 0;
                double t1 = now_ms();
                if (nt < 100000) times[nt++] = t1 - t0;
                if (loop == 0) {
                    u32 hdr[2] = { (u32)w, (u32)h };
                    if (fo) { fwrite(hdr, 4, 2, fo); fwrite(px, 4, (size_t)w * (size_t)h, fo); }
                    hash_all = (hash_all ^ hdr[0]) * 1099511628211ull; hash_all = (hash_all ^ hdr[1]) * 1099511628211ull;
                    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) hash_all = (hash_all ^ px[i]) * 1099511628211ull;
                }
                frames++;
                if (max_frames && frames >= max_frames) break;
                t0 = now_ms();
            } else { fprintf(stderr, "bad event %u at %zu\n", type, p - 1); return 1; }
        }
    }
    prof_stop();
    if (fo) fclose(fo);
    double sum = 0, mx = 0;
    for (int i = 0; i < nt; i++) { sum += times[i]; if (times[i] > mx) mx = times[i]; }
    qsort(times, (size_t)nt, sizeof(double), cmp_d);
    printf("%s: %d frames, %.2f ms/frame mean (%.0f fps), median %.2f, p95 %.2f, max %.2f; image hash %016llx\n", cap, nt,
           nt ? sum / nt : 0, sum > 0 ? nt * 1000.0 / sum : 0, nt ? times[nt / 2] : 0, nt ? times[nt * 95 / 100] : 0, mx,
           (unsigned long long)hash_all);
    return 0;
}
