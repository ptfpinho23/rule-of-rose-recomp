/* audio_slow_test.c — checks rt/audio_out.c's handling of a game that runs slow,
 * without a sound device: the file is compiled in with a simulated clock, a
 * sine from the "mixer" and a device that collects what it is given.
 *   cc -O2 -Irecomp/rt recomp/tools/audio_slow_test.c -lm -o audio_slow_test && ./audio_slow_test
 *   (add -DSLOWTEST_NTSC for the USA game's 59.94 Hz cases)
 * Cases: full speed (the device must get exactly the mixer's samples), a
 * steady 25 ms and 33 ms per frame (as much sound as time has passed, no jump
 * between consecutive samples larger than the sine allows), single stalls at
 * full speed (nothing stretched), and slow then fast again (back to exact). */
#include <math.h>
#include <stdarg.h>
#include "../rt/audio_out.c"

#ifdef SLOWTEST_NTSC      /* -DSLOWTEST_NTSC: the USA game's timing instead (800.8 sample frames per field; see ntsc_cases) */
const RorGame ror_game = { "ntsc", "SLUS-21448", "SLUS_214.48", "USA", 1, 0, 0 };
#else
const RorGame ror_game = { "pal", "SLES-54218", "SLES_542.18", "Europe", 0, 0, 0 };   /* 50 Hz timing: 960 sample frames per field */
#endif

static u64 sim_ns = 1; static double mix_t; static double mix_hz = 440.0;
static s16 *got; static size_t got_n, got_cap;
u64 plat_time_ns(void) { return sim_ns; }
const char *plat_getenv(const char *n) { (void)n; return 0; }
FILE *plat_fopen(const char *p, const char *m) { (void)p; (void)m; return 0; }
void rt_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); }
void rxsnd_mix(s16 *out, int frames) {
    for (int i = 0; i < frames; i++, mix_t += 1.0 / RATE) { s16 v = (s16)(12000.0 * sin(2 * 3.14159265358979 * mix_hz * mix_t)); out[i * 2] = v; out[i * 2 + 1] = (s16)-v; }
}
void adev_open(void) {}
void adev_close(void) {}
void adev_play(const s16 *pcm) {
    if (got_n + FRAMES_PER_CALL > got_cap) { got_cap = got_cap ? got_cap * 2 : 1 << 20; got = realloc(got, got_cap * 4); }
    memcpy(got + got_n * 2, pcm, FRAME_BYTES); got_n += FRAMES_PER_CALL;
}
static int max_jump(size_t from) { int m = 0; for (size_t i = from + 1; i < got_n; i++) { int d = abs(got[i * 2] - got[(i - 1) * 2]); if (d > m) m = d; } return m; }
static int fails;
static void check(int ok, const char *what) { printf("%s  %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }
#ifdef SLOWTEST_NTSC
/* 59.94 Hz: fields of 800 or 801 sample frames are collected into the device's blocks of 960. */
static int ntsc_cases(void) {
    char line[200];
    const int sine_step = (int)(12000.0 * 2 * 3.14159265358979 * mix_hz / RATE) + 2;
    audio_init(1);
    s16 *ref = malloc(600 * 801 * 4); double t0 = mix_t;
    for (int i = 0; i < 600; i++) { sim_ns += 16683333; audio_frame(); }             /* 600 fields = 480,480 sample frames = 500.5 blocks */
    mix_t = t0; rxsnd_mix(ref, 600 * 4004 / 5);                                       /* (exactly what the 600 fields pulled: the sine goes on in phase) */
    check(got_n == 500 * FRAMES_PER_CALL && !memcmp(got, ref, got_n * 4), "59.94 Hz, full speed: the device gets the mixer's samples unchanged, in whole blocks, less than one block behind");
    size_t n0 = got_n; u64 c0 = sim_ns;
    for (int i = 0; i < 1200; i++) { sim_ns += 25000000; audio_frame(); }             /* steadily slow: 25 ms per field */
    double want = (double)(sim_ns - c0) / 1e9 * RATE, have = (double)(got_n - n0);
    snprintf(line, sizeof line, "59.94 Hz, 25 ms per field: %.0f sample frames for %.0f of real time (%.1f %%), largest step %d (sine alone %d)", have, want, 100.0 * have / want, max_jump(n0 + 30 * FRAMES_PER_CALL), sine_step);
    check(have > want * 0.96 && have < want * 1.005 && max_jump(n0) <= sine_step, line);
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails != 0;
}
#endif
int main(void) {
    char line[200];
#ifdef SLOWTEST_NTSC
    return ntsc_cases();
#endif
    const int sine_step = (int)(12000.0 * 2 * 3.14159265358979 * mix_hz / RATE) + 2;      /* largest step of the unstretched sine */
    audio_init(1);
    /* 1: full speed, 500 frames */
    s16 *ref = malloc(500 * FRAME_BYTES); double t0 = mix_t;
    for (int i = 0; i < 500; i++) { sim_ns += 20000000; audio_frame(); }
    mix_t = t0; rxsnd_mix(ref, 500 * FRAMES_PER_CALL);
    check(got_n == 500 * FRAMES_PER_CALL && !memcmp(got, ref, 500 * FRAME_BYTES), "full speed: the device gets the mixer's samples unchanged");
    /* 2: single stalls of 80 ms every 50 frames, caught up by short frames, as the pacing does */
    size_t n0 = got_n; t0 = mix_t;
    for (int i = 0; i < 500; i++) { sim_ns += i % 50 == 0 ? 80000000 : i % 50 <= 6 ? 10000000 : 20000000; audio_frame(); }
    mix_t = t0; rxsnd_mix(ref, 500 * FRAMES_PER_CALL);
    check(got_n - n0 == 500 * FRAMES_PER_CALL && !memcmp(got + n0 * 2, ref, 500 * FRAME_BYTES), "stalls that are caught up: nothing is stretched");
    /* 3, 4: steadily slow */
    for (int ms = 25; ms <= 33; ms += 8) {
        n0 = got_n; u64 c0 = sim_ns;
        for (int i = 0; i < 1000; i++) { sim_ns += (u64)ms * 1000000; audio_frame(); }
        double want = (double)(sim_ns - c0) / 1e9 * RATE, have = (double)(got_n - n0);
        snprintf(line, sizeof line, "%d ms per frame: %.0f sample frames for %.0f of real time (%.1f %%), largest step %d (sine alone %d)", ms, have, want, 100.0 * have / want, max_jump(n0 + 30 * FRAMES_PER_CALL), sine_step);
        check(have > want * 0.96 && have < want * 1.005 && max_jump(n0) <= sine_step, line);
        size_t n1 = got_n;
        for (int i = 0; i < 100; i++) { sim_ns += 20000000; audio_frame(); }      /* back to full speed */
        n0 = got_n; t0 = mix_t;
        for (int i = 0; i < 100; i++) { sim_ns += 20000000; audio_frame(); }
        mix_t = t0; rxsnd_mix(ref, 100 * FRAMES_PER_CALL);
        /* after a slow stretch the queue may hold part of a block: the samples are then the mixer's, delayed by less than one block */
        int exact = 0;
        for (size_t d = 0; d < FRAMES_PER_CALL && !exact; d++) exact = !memcmp(got + (n0 + d) * 2, ref, (100 * FRAMES_PER_CALL - d - FRAMES_PER_CALL) * 4);
        check(got_n - n0 == 100 * FRAMES_PER_CALL && exact && max_jump(n1 - FRAMES_PER_CALL) <= sine_step, "full speed again: the mixer's samples again, no click on the way back");
    }
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails != 0;
}
