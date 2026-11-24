/* video.c — presentation: shows the GS display buffer in a window at 50 Hz
 * (video_x11.c / video_win32.c, which also map the keyboard to the
 * controller), and can save PNG screenshots (used for testing without a
 * display). */
#include "gs.h"
#include "plat.h"
#include "post.h"

void pad_set_state(u16 buttons, u8 rx, u8 ry, u8 lx, u8 ly) __attribute__((weak));
void pad_update(u64 frame, u16 kbd, int live);   /* rt/pad.c: keyboard word + gamepad -> the emulated pad */
int  pad_fake(void);                             /* rt/pad.c: ROR_PAD_FAKE is set */
void audio_init(int realtime); void audio_frame(void);
double wide_aspect(void);                /* rt/wide.c: --wide aspect, 0 = off */

static int want_window = 1, shot_every, max_frames, no_sync;
static u64 frame_no, last_prims;
static int win_open;
static u16 pad_buttons = 0xFFFF;
/* Scripted input for tests: "frame:buttons,frame:buttons,..." where buttons
 * is the active-low pad word in hex (ffff = nothing pressed). */
static struct { u64 frame; u16 buttons; } script[256];
static int script_n;
void video_script(const char *s) {
    while (*s && script_n < 256) {
        char *e;
        script[script_n].frame = strtoull(s, &e, 10);
        if (*e != ':') break;
        script[script_n++].buttons = (u16)strtoul(e + 1, &e, 16);
        s = (*e == ',') ? e + 1 : e;
    }
}

static int win_scale = 2, res_w, res_h, want_fullscreen, post_shots;
static int want_gpu, gpu_internal = 4, gpu_vsync = 1, gpu_swap;      /* --renderer gpu, --internal, --vsync; gpu_swap: presenting through the swapchain */
static int gpu_fps, gpu_interp;          /* --fps: 0 off, -1 display rate, N; gpu_interp: the presenter thread is running */
static int want_fsr3;                    /* --filter fsr3: FSR 3.1 temporal upscaling on the GPU renderer; everywhere else it is --filter fsr */
static u64 pace_next;                    /* ideal time of the next emulated vblank (50 Hz pacing clock) */
/* --frameskip auto|off. auto: while the game runs behind its 50 Hz clock in a window, the renderer leaves out
 * whole frames (gs_frameskip) so that game speed and sound stay right and only the picture rate drops.
 * Never in unpaced or headless runs, with frame interpolation or FSR 3.1 (they pair consecutive frames). */
static int frameskip = 1;
#define FRAMESKIP_LATE_NS 6000000ll      /* ask for a skip when a vblank ends this far behind its time ... */
#define FRAMESKIP_OK_NS 1000000ll        /* ... and stop asking once within this of it */
static PostSettings post_cfg = { POST_FSR, 0, 60, 0, 0, 100, 100, 0 };
static u32 *out_px; static int out_w, out_h; static size_t out_cap;
static double post_ms, show_ms; static u32 post_n;
void video_set_scale(int n) { if (n >= 1 && n <= 4) win_scale = n; }
/* Picture options from the command line (name without the dashes; value 0
 * for flags). Returns 0 if the name or the value is not valid. */
int video_option(const char *name, const char *value) {
    if (!strcmp(name, "fullscreen")) { want_fullscreen = 1; return 1; }
    if (!strcmp(name, "post-shots")) { post_shots = 1; return 1; }
    if (!strcmp(name, "res")) {                   /* WxH, real pixels; overrides --scale */
        int w = 0, h = 0, n = 0;
        if (!value || sscanf(value, "%dx%d%n", &w, &h, &n) != 2 || value[n] || value[0] < '0' || value[0] > '9') return 0;
        if (w < 320 || w > 7680 || h < 240 || h > 4320) return 0;
        res_w = w; res_h = h; return 1;
    }
    if (!strcmp(name, "renderer")) {
        if (value && !strcmp(value, "software")) want_gpu = 0; else if (value && !strcmp(value, "gpu")) want_gpu = 1; else return 0;
        return 1;
    }
    if (!strcmp(name, "internal")) {
        if (!value || strlen(value) != 1 || value[0] < '1' || value[0] > '8') return 0;
        gpu_internal = value[0] - '0'; return 1;
    }
    if (!strcmp(name, "frameskip")) {
        if (value && !strcmp(value, "auto")) frameskip = 1; else if (value && !strcmp(value, "off")) frameskip = 0; else return 0;
        return 1;
    }
    if (!strcmp(name, "fps")) {                   /* frame interpolation target: 0 = off, display, or 25..1000 */
        if (value && !strcmp(value, "display")) { gpu_fps = -1; return 1; }
        char *e; long n = value ? strtol(value, &e, 10) : -1;
        if (!value || !*value || *e || (n != 0 && (n < 25 || n > 1000))) return 0;
        gpu_fps = (int)n; return 1;
    }
    if (!strcmp(name, "vsync")) {
        if (!value || strlen(value) != 1 || (value[0] != '0' && value[0] != '1')) return 0;
        gpu_vsync = value[0] - '0'; return 1;
    }
    if (!strcmp(name, "scale")) {
        if (!value || strlen(value) != 1 || value[0] < '1' || value[0] > '4') return 0;
        win_scale = value[0] - '0'; return 1;
    }
    if (!strcmp(name, "filter")) { want_fsr3 = value && !strcmp(value, "fsr3"); if (want_fsr3) value = "fsr"; }
    return post_set(&post_cfg, name, value);
}
/* GPU path: scaling filter number for gs_hw_show (also hands over sharpness and bloom). */
static int gpu_filter(void) {
    gs_hw_present_opts(post_cfg.filter == POST_FSR && post_cfg.sharpness > 0 ? post_rcas_con(post_cfg.sharpness) : 0.0f, post_cfg.bloom);
    return post_cfg.filter == POST_NEAREST ? 0 : post_cfg.filter == POST_BILINEAR ? 1 : want_fsr3 ? 3 : 2;
}
/* Called by the window code: shrink a requested client size that does not
 * fit in max_w x max_h, keeping its aspect. */
void video_fit(int *w, int *h, int max_w, int max_h) {
    if (max_w < 64 || max_h < 64 || (*w <= max_w && *h <= max_h)) return;
    int nw = max_w, nh = (int)((long long)*h * max_w / *w);
    if (nh > max_h) { nh = max_h; nw = (int)((long long)*w * max_h / *h); }
    rt_log("[video] a %dx%d window does not fit on this screen; using %dx%d\n", *w, *h, nw, nh);
    *w = nw; *h = nh;
}
/* Run the post-processing into the output buffer at ow x oh. */
static void run_post(const u32 *px, int w, int h, int ow, int oh) {
    size_t need = (size_t)ow * (size_t)oh;
    if (need > out_cap) { free(out_px); out_px = malloc(need * 4); out_cap = need; }
    if (ow != out_w || oh != out_h) rt_log("[video] output %dx%d\n", ow, oh);
    out_w = ow; out_h = oh;
    u64 t0 = plat_time_ns();
    post_run(px, w, h, out_px, ow, oh, &post_cfg);
    post_ms += (double)(plat_time_ns() - t0) / 1e6; post_n++;
}
/* ROR_SHOT_FROM=frame (testing): --shots saves nothing before that frame */
static u64 shot_from(void) { static s64 f = -1; if (f < 0) { const char *e = getenv("ROR_SHOT_FROM"); f = e ? (s64)strtoll(e, 0, 10) : 0; } return (u64)f; }
/* The same, unless it was done already for these very pixels: the game draws a new picture every second vblank
 * at most, and while the GS hands out the same pixels (g_gs_frame_id) for the same output size, rectangle and
 * settings, the picture made last time is still right. Returns 1 if a new one was made. */
static int post_frame(const u32 *px, int w, int h, int ow, int oh) {
    static u64 done_id = ~0ull; static int done_w, done_h, done_ow, done_oh, done_rect[4];
    static PostSettings done_cfg;
    int rect[4]; post_rect(ow, oh, &post_cfg, &rect[0], &rect[1], &rect[2], &rect[3]);
    if (g_gs_frame_id == done_id && w == done_w && h == done_h && ow == done_ow && oh == done_oh && out_px && out_w == ow && out_h == oh &&
        !memcmp(rect, done_rect, sizeof rect) && !memcmp(&post_cfg, &done_cfg, sizeof post_cfg)) return 0;
    run_post(px, w, h, ow, oh);
    done_id = g_gs_frame_id; done_w = w; done_h = h; done_ow = ow; done_oh = oh; memcpy(done_rect, rect, sizeof rect); done_cfg = post_cfg;
    return 1;
}
void video_config(int window, int shots, int frames, int nosync) {
    want_window = window; shot_every = shots; max_frames = frames; no_sync = nosync;
    gs_renderer(want_gpu, gpu_internal);        /* takes effect in gs_init (hw_init, called next) */
    gs_fsr3(want_gpu && want_fsr3);
    if (want_fsr3 && !want_gpu) rt_log("[video] --filter fsr3 (FSR 3.1) needs --renderer gpu: using FSR 1.0\n");
    if (wide_aspect() > 0) rt_log("[video] widescreen: 3D scenes are rendered and shown at %.3f:1; movies and menus stay 4:3\n", wide_aspect());
    audio_init(!nosync);
}

/* ---- PNG (stored deflate) ---- */
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
void save_png(const char *path, const u32 *px, int w, int h) {
    FILE *f = plat_fopen(path, "wb");
    if (!f) return;
    size_t row = (size_t)w * 3 + 1, raw_n = row * (size_t)h;
    u8 *raw = malloc(raw_n), *z = malloc(raw_n + raw_n / 65535 * 5 + 16);
    for (int y = 0; y < h; y++) {
        u8 *r = raw + row * (size_t)y; *r++ = 0;
        for (int x = 0; x < w; x++) { u32 c = px[y * w + x]; *r++ = (u8)(c >> 16); *r++ = (u8)(c >> 8); *r++ = (u8)c; }
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

/* ---- ROR_PERFLOG=1: one line per second and a summary at exit --------------------------------
 * [perf] F vblanks N behind B slip S max M ms | ee E vu V gs G tex T(n) pair P gpu W show S sleep Z ms
 *        | pres R/s p50 a p95 b p99 c max d ms over8 x over16 y | frames F interp I up U cut C nostart S nopkg D texrepl X
 *        | audio min Q ms starved K
 * F: number of the last tick. vblanks: emulated 50 Hz ticks in the interval (paced ones only);
 * behind: ticks that started more than a whole tick (20 ms) after their time on the 50 Hz schedule
 * (the game ran in slow motion there; a game frame is two ticks, and the first of the two may run
 * long without harm); slip: the largest delay against the schedule; max: the longest tick. Per tick,
 * mean ms, each without the time of the ones after it in this list: ee = everything not named
 * (game code, VIF, GIF and GS command handling outside VU1 kicks), vu = VU1 programs, gs = handling
 * the primitives they send, tex = decoding textures and handing them to the GPU (n = textures
 * decoded in the interval), pair = frame interpolation bookkeeping at the frame boundary, gpu =
 * waiting for the GPU (wherever it happened), show = window, input and presenting, sleep = waiting
 * for the next tick. pres: pictures presented
 * per second by the interpolation thread and their intervals; over8 / over16: intervals longer than
 * 8.4 / 16.7 ms. frames: game frames, how many were interpolated, and why the others were not (upload
 * into a render target, a cut, no start-of-frame copy, presenter behind); texrepl: frames in which a
 * GPU texture was replaced (for information). audio:
 * the lowest level in the interval of a model queue (starts at 60 ms, holds at most 100, fed 20 ms
 * per tick, drained by the clock, refilled to 60 after running empty) and how often it ran empty; no
 * device is involved. */
extern int g_perf_on; extern u64 g_perf[12]; extern u32 g_perf_hist[258]; extern u64 g_perf_pres_max;
extern int g_vu_stat; extern u64 g_vu_stat_ticks[5];
extern u64 hw_stat_wait_ns __attribute__((weak));
typedef struct { u64 n, late, slip, maxi, ns, vu, gs1, tex, pair, gpu, gpu_show, gpu_pair, show, sleep, texn, pres, o8, o16, pmax, fr[7], starved; double amin; u32 hist[258]; } PerfAcc;
static int perf_sites[16];
static PerfAcc perf_sec, perf_all; static double perf_tick_ns; static u64 perf_show_ns, perf_sleep_ns, perf_show_gpu, perf_slip; static int perf_paced; static double perf_audio = 60.0;
static double perf_pct(const u32 *h, u64 n, double q) { u64 want = (u64)((double)n * q), c = 0; for (int i = 0; i < 258; i++) { c += h[i]; if (c > want) return (i + 0.5) * 0.25; } return 0; }
static void perf_line(const char *tag, const PerfAcc *a, double secs) {
    double k = a->n ? 1e-6 / (double)a->n : 0;
    /* show holds the boundary work (pair) and its GPU waits; the kicks (gs1) hold texture work and the other GPU waits */
    double rest = (double)a->ns - (double)a->vu - (double)a->gs1 - (double)a->show - (double)a->sleep;
    double gs = (double)a->gs1 - (double)a->tex - ((double)a->gpu - (double)a->gpu_show); if (gs < 0) gs = 0;
    double pair = (double)a->pair - (double)a->gpu_pair, show = (double)a->show - (double)a->pair - ((double)a->gpu_show - (double)a->gpu_pair);
    if (rest < 0) rest = 0; if (pair < 0) pair = 0; if (show < 0) show = 0;
    rt_log("[perf] %s vblanks %llu behind %llu slip %.1f max %.1f ms | ee %.2f vu %.2f gs %.2f tex %.2f(%llu) pair %.2f gpu %.2f show %.2f sleep %.2f ms"
           " | pres %.0f/s p50 %.1f p95 %.1f p99 %.1f max %.1f ms over8 %llu over16 %llu | frames %llu interp %llu up %llu cut %llu nostart %llu nopkg %llu texrepl %llu"
           " | audio min %.0f ms starved %llu\n", tag, (unsigned long long)a->n, (unsigned long long)a->late, a->slip / 1e6, a->maxi / 1e6,
           rest * k, a->vu * k, gs * k, a->tex * k, (unsigned long long)a->texn, pair * k, a->gpu * k, show * k, a->sleep * k,
           secs > 0 ? (double)a->pres / secs : 0.0, perf_pct(a->hist, a->pres, 0.5), perf_pct(a->hist, a->pres, 0.95), perf_pct(a->hist, a->pres, 0.99), a->pmax / 1e6,
           (unsigned long long)a->o8, (unsigned long long)a->o16, (unsigned long long)a->fr[0], (unsigned long long)a->fr[1], (unsigned long long)a->fr[2],
           (unsigned long long)a->fr[4], (unsigned long long)a->fr[5], (unsigned long long)a->fr[6], (unsigned long long)a->fr[3], a->amin, (unsigned long long)a->starved);
}
static void perf_exit(void) { if (perf_all.n) perf_line("total", &perf_all, (double)perf_all.ns / 1e9); }
static void perf_tick(void) {
    static int on = -1; static u64 last, lw, tsc0, t0;
    if (on < 0) { on = getenv("ROR_PERFLOG") != 0; if (on) { g_perf_on = atoi(getenv("ROR_PERFLOG")) > 1 ? 2 : 1; g_vu_stat = 2; atexit(perf_exit); perf_sec.amin = perf_all.amin = 100.0; } }
    if (!on) return;
    u64 now = plat_time_ns(), tsc = __builtin_ia32_rdtsc();
    if (!last) { last = now; tsc0 = tsc; t0 = now; lw = &hw_stat_wait_ns ? hw_stat_wait_ns : 0; memset(g_vu_stat_ticks, 0, sizeof g_vu_stat_ticks); memset(g_perf, 0, sizeof g_perf); memset(g_perf_hist, 0, sizeof g_perf_hist); return; }
    if (now - t0 > 200000000ull) perf_tick_ns = (double)(now - t0) / (double)(tsc - tsc0);
    PerfAcc *a = &perf_sec; u64 d = now - last; last = now;
    if (!perf_paced) {                 /* fast-forwarded or unpaced ticks are not counted */
        memset(g_vu_stat_ticks, 0, sizeof g_vu_stat_ticks); memset(g_perf, 0, sizeof g_perf); memset(g_perf_hist, 0, sizeof g_perf_hist); g_perf_pres_max = 0;
        lw = &hw_stat_wait_ns ? hw_stat_wait_ns : 0; perf_show_ns = perf_sleep_ns = perf_show_gpu = perf_slip = 0;
        return;
    }
    a->n++; a->ns += d; if (perf_slip > 20000000ull) a->late++; if (perf_slip > a->slip) a->slip = perf_slip; if (d > a->maxi) a->maxi = d;
    perf_slip = 0;
    a->vu += (u64)((double)(g_vu_stat_ticks[0] - g_vu_stat_ticks[1]) * perf_tick_ns); a->gs1 += (u64)((double)g_vu_stat_ticks[1] * perf_tick_ns);
    memset(g_vu_stat_ticks, 0, sizeof g_vu_stat_ticks);
    a->tex += g_perf[0] + g_perf[2]; a->texn += g_perf[1]; a->pair += g_perf[3]; a->gpu_pair += g_perf[11]; a->gpu_show += perf_show_gpu; perf_show_gpu = 0;
    for (int i = 0; i < 7; i++) a->fr[i] += g_perf[4 + i];
    memset(g_perf, 0, sizeof g_perf);
    { u64 w = &hw_stat_wait_ns ? hw_stat_wait_ns : 0; a->gpu += w - lw; lw = w; }
    a->show += perf_show_ns; a->sleep += perf_sleep_ns; perf_show_ns = perf_sleep_ns = 0;
    if (g_perf_on > 1) { extern int hw_flush_counts[16] __attribute__((weak)); if (&hw_flush_counts[0]) for (int i = 0; i < 16; i++) perf_sites[i] += hw_flush_counts[i]; }
    perf_audio += 20.0 - (double)d / 1e6;
    if (perf_audio > 100.0) perf_audio = 100.0;
    if (perf_audio < 0.0) { perf_audio = 60.0; a->starved++; if (a->amin > 0.0) a->amin = 0.0; }      /* ran empty: a device would fill up again */
    if (perf_audio < a->amin) a->amin = perf_audio;
    if (a->n < 50) return;
    for (int i = 0; i < 258; i++) { u32 c = g_perf_hist[i]; g_perf_hist[i] = 0; a->hist[i] = c; a->pres += c; if (i >= 34) a->o8 += c; if (i >= 67) a->o16 += c; }
    a->pmax = g_perf_pres_max; g_perf_pres_max = 0;
    { char tag[32]; snprintf(tag, sizeof tag, "%llu", (unsigned long long)frame_no); perf_line(tag, a, (double)a->ns / 1e9); }
    if (g_perf_on > 1) { extern int hw_flush_sites[16], hw_flush_counts[16] __attribute__((weak)); char b[256]; int o = 0; b[0] = 0;
        if (&hw_flush_counts[0]) for (int i = 0; i < 16; i++) if (perf_sites[i]) { o += snprintf(b + o, sizeof b - (size_t)o, " %d:%d", hw_flush_sites[i], perf_sites[i]); perf_sites[i] = 0; }
        rt_log("[perf]   blocking GPU submits by source line:%s\n", b[0] ? b : " none"); }
    if (g_perf_on > 1) { extern u64 g_perf_tx[8]; rt_log("[perf]   textures decoded: %llu new, %llu other palette, %llu other content, %llu drawn into (%llu texels); %llu found under another address\n", (unsigned long long)g_perf_tx[0], (unsigned long long)g_perf_tx[1], (unsigned long long)g_perf_tx[2], (unsigned long long)g_perf_tx[3], (unsigned long long)g_perf_tx[5], (unsigned long long)g_perf_tx[4]); memset(g_perf_tx, 0, sizeof g_perf_tx); }
    PerfAcc *t = &perf_all;
    t->n += a->n; t->late += a->late; if (a->slip > t->slip) t->slip = a->slip; t->gpu_show += a->gpu_show; t->gpu_pair += a->gpu_pair; if (a->maxi > t->maxi) t->maxi = a->maxi; t->ns += a->ns; t->vu += a->vu; t->gs1 += a->gs1; t->tex += a->tex; t->pair += a->pair; t->gpu += a->gpu;
    t->show += a->show; t->sleep += a->sleep; t->texn += a->texn; t->pres += a->pres; t->o8 += a->o8; t->o16 += a->o16; if (a->pmax > t->pmax) t->pmax = a->pmax;
    for (int i = 0; i < 7; i++) t->fr[i] += a->fr[i];
    for (int i = 0; i < 258; i++) t->hist[i] += a->hist[i];
    t->starved += a->starved; if (a->amin < t->amin) t->amin = a->amin;
    memset(a, 0, sizeof *a); a->amin = 100.0;
}
void video_present(void) {
    u32 *px = 0; int w = 0, h = 0;
    frame_no++;
    perf_tick();
    u64 perf_in = plat_time_ns(), perf_gw = &hw_stat_wait_ns ? hw_stat_wait_ns : 0;
    { static int trace = -1; static u64 last, lastc; u64 t = plat_time_ns(), c = plat_cpu_ns();      /* ROR_STALLTRACE=frame: log late vblanks */
      if (trace < 0) trace = getenv("ROR_STALLTRACE") ? atoi(getenv("ROR_STALLTRACE")) : 0;
      extern u64 hw_stat_wait_ns, hw_stat_flushes __attribute__((weak)); static u64 lw, lf;
      if (trace && (int)frame_no >= trace && last && t - last > 35000000ull) rt_log("[stall] vblank %llu came %.1f ms after the previous one (process cpu in between %.1f ms; waiting for the GPU %.1f ms in %llu submits)\n", (unsigned long long)frame_no, (t - last) / 1e6, (c - lastc) / 1e6, (hw_stat_wait_ns - lw) / 1e6, (unsigned long long)(hw_stat_flushes - lf));
      { extern int hw_flush_sites[16], hw_flush_counts[16];
        if (trace && (int)frame_no >= trace && t - last > 35000000ull) { for (int i = 0; i < 16; i++) if (hw_flush_counts[i]) rt_log("        submits from line %d: %d\n", hw_flush_sites[i], hw_flush_counts[i]); }
        memset(hw_flush_counts, 0, sizeof hw_flush_counts); }
      last = t; lastc = c; lw = hw_stat_wait_ns; lf = hw_stat_flushes; }
    /* GPU renderer presenting through its swapchain: the picture stays on the
     * GPU; it is read back only for a screenshot. */
    int shot_due = shot_every && frame_no % (u64)shot_every == 0 && frame_no >= shot_from();
    int gpu = gs_hw_active(), have = 0;
    if (gpu && want_fsr3 && !gpu_swap) {             /* FSR 3.1 works across frames: it needs the output rectangle on every frame, shot or not */
        int ow = res_w ? res_w : (wide_aspect() > 0 ? (int)(480 * win_scale * wide_aspect() + 0.5) : 640 * win_scale), oh = res_w ? res_h : 480 * win_scale, rx, ry, rw, rh;
        if (win_open) vwin_size(&ow, &oh);
        post_rect(ow, oh, &post_cfg, &rx, &ry, &rw, &rh);
        if (ow > 0 && oh > 0) gs_hw_fsr3_rect(rx, ry, rw, rh, ow, oh);
    }
    if (!gpu || shot_due || (want_window && win_open && !gpu_swap)) have = gs_get_frame(&px, &w, &h);
    else if (!gpu_swap) gs_hw_show(0, 0, 0, 0, 0, 0, 0, 0, 64);          /* no window: just let the GPU finish the frame */
    audio_frame();
    if (frame_no % 500 == 0 && getenv("ROR_FPSLOG")) {            /* unthrottled speed per 500 frames */
        static u64 last, lastc; u64 now = plat_time_ns(), nowc = plat_cpu_ns();
        if (last) {
            double wall = (double)(now - last) / 1e9;
            double cpu = (double)(nowc - lastc) / 1e9;
            rt_log("[video] frames %llu-%llu: %.0f fps (%.1f ms wall, %.1f ms cpu per frame, %llu prims/frame)\n",
                   (unsigned long long)frame_no - 500, (unsigned long long)frame_no, 500.0 / wall, wall * 2, cpu * 2,
                   (unsigned long long)(g_gs_prims - last_prims) / 500);
        }
        { extern u64 g_gs_stat[6];
          rt_log("        per frame: small %llu prims / %llu px, medium %llu / %llu, large %llu / %llu\n",
                 (unsigned long long)g_gs_stat[3] / 500, (unsigned long long)g_gs_stat[0] / 500, (unsigned long long)g_gs_stat[4] / 500,
                 (unsigned long long)g_gs_stat[1] / 500, (unsigned long long)g_gs_stat[5] / 500, (unsigned long long)g_gs_stat[2] / 500);
          { extern u64 g_gs_tex_rebuilt; rt_log("        texture cache: %llu texels decoded per frame\n", (unsigned long long)g_gs_tex_rebuilt / 500); g_gs_tex_rebuilt = 0; }
          memset(g_gs_stat, 0, sizeof g_gs_stat); }
        if (post_n) rt_log("        post-processing: %.2f ms per frame at %dx%d, window update %.2f ms\n", post_ms / post_n, out_w, out_h, show_ms / post_n);
        post_ms = show_ms = 0; post_n = 0;
        last = now; lastc = nowc; last_prims = g_gs_prims;
    }
    { static u64 from = ~0ull; void prof_reset(void);
      if (from == ~0ull) { const char *e = getenv("ROR_PROF_FROM"); from = e ? strtoull(e, 0, 0) : 0; }
      if (from && frame_no == from) prof_reset(); }
    for (int i = 0; i < script_n; i++)
        if (script[i].frame == frame_no) {
            pad_buttons = script[i].buttons;
            if (pad_set_state) pad_set_state(pad_buttons, 0x80, 0x80, 0x80, 0x80);
        }
    if (pad_fake()) pad_update(frame_no, pad_buttons, 0);       /* test hook: synthetic gamepad, with or without a window */
    if (have && shot_every && frame_no % (u64)shot_every == 0 && frame_no >= shot_from()) {
        char path[128];
        snprintf(path, sizeof path, "build/recomp/shots/frame_%06llu.png", (unsigned long long)frame_no);
        save_png(path, px, w, h);
    }
    /* window / output size: --res, else 480N high and as wide as the picture (4:3, or the --wide aspect) */
    int sw = res_w ? res_w : 640 * win_scale, sh = res_w ? res_h : 480 * win_scale;
    if (!res_w && wide_aspect() > 0) sw = (int)(480 * win_scale * wide_aspect() + 0.5);
    /* ROR_FAST_UNTIL=frame (testing): no window and no pacing before that frame */
    static s64 fast_until = -1;
    if (fast_until < 0) { const char *e = getenv("ROR_FAST_UNTIL"); fast_until = e ? (s64)strtoll(e, 0, 10) : 0; }
    int fast = fast_until > 0 && (s64)frame_no < fast_until;
    int shot_now = have && post_shots && shot_every && frame_no % (u64)shot_every == 0 && frame_no >= shot_from(), posted = 0;
    if (want_window && !fast) {
        if (!win_open) {
            if (vwin_open(sw, sh, want_fullscreen)) {
                win_open = 1;
                if (gs_hw_active()) {
                    void *na, *nb; vwin_native(&na, &nb);
                    gpu_swap = gs_hw_attach(na, nb, gpu_vsync ? (gpu_fps ? 2 : 1) : 0);
                    if (gpu_swap && gpu_fps) {
                        int hz = vwin_refresh_hz();
                        /* vsync on: the display paces the presenter (FIFO); a lower --fps N limits it.
                         * vsync off: the limit is N, or the display's rate for --fps display. */
                        /* The presenter is paced by the clock: N per second, or the display's
                         * rate for --fps display (a FIFO swapchain alone was seen not to hold
                         * a windowed presenter to the refresh rate). */
                        int limit = gpu_fps > 0 ? gpu_fps : hz;
                        rt_log("[video] frame interpolation: display %d Hz, presenting %d pictures per second\n", hz, limit);
                        gs_hw_interp(limit, hz); gpu_interp = 1;
                    }
                    if (!gpu_swap) rt_log("[video] no Vulkan swapchain on this window: the GPU picture is read back and shown by the CPU path\n");
                }
            }
            else { rt_log("[video] no display; running without a window\n"); want_window = 0; }
        }
        if (win_open) {
            vwin_pump(&pad_buttons);
            /* A real gamepad is read only here: in a window, and not in a scripted run. */
            pad_update(frame_no, pad_buttons, script_n == 0);
            int ow = sw, oh = sh;
            vwin_size(&ow, &oh);
            if (gpu_swap && ow > 0 && oh > 0) {
                /* The 4:3 rectangle, colour table and saturation are post.c's;
                 * scaling, sharpening and bloom are done by the present shaders (hw/present.frag). */
                int rx, ry, rw, rh; static u8 lut[256];
                post_rect(ow, oh, &post_cfg, &rx, &ry, &rw, &rh);
                u64 t0 = plat_time_ns();
                if (gpu_interp) gs_hw_vblank(rx, ry, rw, rh, ow, oh, gpu_filter(), post_lut(&post_cfg, lut) ? lut : 0,
                                             (post_cfg.saturation * 64 + 50) / 100, no_sync || !pace_next ? t0 : pace_next);
                else
                gs_hw_show(rx, ry, rw, rh, ow, oh, gpu_filter(), post_lut(&post_cfg, lut) ? lut : 0, (post_cfg.saturation * 64 + 50) / 100);
                show_ms += (double)(plat_time_ns() - t0) / 1e6; post_n++;
            } else if (have && ow > 0 && oh > 0) {
                static int since;
                int fresh = post_frame(px, w, h, ow, oh); posted = 1;
                if (fresh || ++since >= 12) {            /* an unchanged picture: only refreshed now and then (the window may have been covered) */
                    u64 t0 = plat_time_ns();
                    vwin_show(out_px, ow, oh);
                    show_ms += (double)(plat_time_ns() - t0) / 1e6;
                    since = 0;
                }
            }
            vwin_flush();
        }
    }
    { static s64 bench_from = -2;                    /* ROR_POSTBENCH=frame: headless, post-process every frame from there on (timing) */
      if (bench_from == -2) { const char *e = getenv("ROR_POSTBENCH"); bench_from = e ? (s64)strtoll(e, 0, 10) : -1; }
      if (bench_from >= 0 && (s64)frame_no >= bench_from) shot_now |= 2; }
    if (!want_window && have && shot_now && !gpu) {
        /* headless: the output size is the --res / --scale window, or ROR_POST_SIZE=WxH */
        int ow = sw, oh = sh; const char *e = getenv("ROR_POST_SIZE");
        if (e && sscanf(e, "%dx%d", &ow, &oh) != 2) { ow = sw; oh = sh; }
        if (ow < 16 || oh < 16 || ow > 8192 || oh > 8192) { ow = sw; oh = sh; }
        post_frame(px, w, h, ow, oh); posted = 1;
    }
    if (gpu && (shot_now & 1) && !posted) {                /* GPU renderer: the presentation pass, off screen */
        int ow = sw, oh = sh, rx, ry, rw, rh; static u8 lut[256]; u32 *gp;
        if (win_open) vwin_size(&ow, &oh);
        post_rect(ow, oh, &post_cfg, &rx, &ry, &rw, &rh);
        if (ow > 0 && oh > 0 && gs_hw_show_pixels(rx, ry, rw, rh, ow, oh, gpu_filter(), post_lut(&post_cfg, lut) ? lut : 0, (post_cfg.saturation * 64 + 50) / 100, &gp)) {
            char path[128];
            snprintf(path, sizeof path, "build/recomp/shots/post_%06llu.png", (unsigned long long)frame_no);
            save_png(path, gp, ow, oh);
        }
    }
    if ((shot_now & 1) && posted) {                        /* --post-shots: the processed picture as shown */
        char path[128];
        snprintf(path, sizeof path, "build/recomp/shots/post_%06llu.png", (unsigned long long)frame_no);
        save_png(path, out_px, out_w, out_h);
    }
    if (max_frames && frame_no >= (u64)max_frames) {
        rt_log("[video] %llu frames, %llu primitives drawn\n", (unsigned long long)frame_no, (unsigned long long)g_gs_prims);
        exit(0);
    }
    perf_show_ns += plat_time_ns() - perf_in; perf_show_gpu += (&hw_stat_wait_ns ? hw_stat_wait_ns : 0) - perf_gw; perf_paced = !no_sync && !fast;
    if (!no_sync && !fast) {                         /* pace to 50 Hz */
        u64 now = plat_time_ns();
        if (!pace_next) pace_next = now;
        pace_next += 20000000;
        s64 ahead = (s64)(pace_next - now);
        {   static int asking, logged; static u64 last_skipped;
            if (frameskip && win_open && !gpu_interp && !want_fsr3) {
                if (ahead < -FRAMESKIP_LATE_NS) asking = 1; else if (ahead > -FRAMESKIP_OK_NS) asking = 0;
            } else asking = 0;
            gs_frameskip(asking);
            if (frame_no % 500 == 0) {
                u64 sk, fr; gs_frameskip_stats(&sk, &fr);
                if (sk != last_skipped && (!logged || getenv("ROR_FPSLOG"))) {       /* once; with ROR_FPSLOG every time */
                    rt_log("[video] this machine is not keeping up at these settings: %llu pictures were left out in the last 500 vblanks (10 s) to keep the game and its sound at full speed (--frameskip off turns this off)\n",
                           (unsigned long long)(sk - last_skipped));
                    logged = 1;
                }
                last_skipped = sk;
            }
        }
        if (ahead > 0) { plat_sleep_until(pace_next); perf_sleep_ns += plat_time_ns() - now; }
        else if (ahead < -200000000ll) pace_next = now;
        if (ahead < 0) perf_slip = (u64)-ahead;
    }
}
