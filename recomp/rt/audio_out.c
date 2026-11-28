/* audio_out.c — host audio output and WAV dump.
 *
 * audio_frame() is called once per video field: it pulls that field's stereo
 * sample frames at 48 kHz (960 = 20 ms on the European game; 800 or 801 =
 * 16.68 ms on the USA game, rt.h video_field_samples) from the sound driver's
 * mixer (rxsnd_mix) and
 *   - appends them to the WAV file named by $ROR_WAV, if set;
 *   - when the run is paced in real time, sends them to the sound device
 *     (plat.h adev_*: audio_pulse.c on Linux, audio_waveout.c on Windows),
 *     which takes blocks of 960; fields that are shorter are collected into
 *     such blocks first.
 *     The device never stalls the emulation, at worst a frame of sound is
 *     dropped; without a device there is one log line and silence.
 *     When the game runs slower than real time for a while (frames arrive
 *     further apart than 20 ms for a quarter of a second) each frame's sound is stretched
 *     to the time it has to cover, so the device is fed without gaps: the
 *     sound gets lower and slower with the game instead of crackling. See
 *     "slow game" below.
 *
 *   void audio_init(int realtime);   optional; realtime = 0 (--no-sync runs)
 *                                    keeps the device closed, WAV still written
 *   void audio_frame(void);          once per video frame
 *   void audio_shutdown(void);       optional; also runs at normal process exit
 *
 * Env: ROR_WAV=path  dump everything mixed;  ROR_NOAUDIO=1  no device. */
#include "rt.h"
#include "plat.h"

void rxsnd_mix(s16 *out, int frames);
void audio_init(int realtime);
void audio_frame(void);
void audio_shutdown(void);

#define RATE 48000
#define FRAMES_PER_CALL 960
#define FRAME_BYTES (FRAMES_PER_CALL * 4)

static int inited, realtime = 1;

/* ---- WAV ------------------------------------------------------------- */
static FILE *wav;
static u32 wav_bytes, wav_calls;
static void wav_header(void) {
    u8 h[44] = "RIFF....WAVEfmt ";
    u32 v;
    v = 36 + wav_bytes; memcpy(h + 4, &v, 4);
    v = 16; memcpy(h + 16, &v, 4);
    h[20] = 1; h[22] = 2;                                   /* PCM, stereo */
    v = RATE; memcpy(h + 24, &v, 4);
    v = RATE * 4; memcpy(h + 28, &v, 4);
    h[32] = 4; h[34] = 16;
    memcpy(h + 36, "data", 4); memcpy(h + 40, &wav_bytes, 4);
    long pos = ftell(wav);
    fseek(wav, 0, SEEK_SET); fwrite(h, 1, 44, wav);
    if (pos > 44) fseek(wav, pos, SEEK_SET);
}

/* ---- slow game ----------------------------------------------------------
 * The game makes one field of sound (20 ms at 50 Hz) per video field. If a field takes 25 ms of
 * real time, the device runs dry for 5 ms out of every 25: a click 40 times a
 * second. So the real time the last 25 frames took is compared with the half
 * second of sound they made. A stall that the pacing catches up again leaves
 * that at 1 within a few frames and changes nothing; when it has been more
 * than 3 % above 1 for 12 frames in a row the game is running slow, and from
 * then on each frame's 960 sample frames are resampled (linear interpolation,
 * phase carried from frame to frame) to 960 * ratio and handed to the device
 * in whole 960-frame blocks from a small queue, until the ratio is back under
 * 1.01. The sound then gets lower and slower with the game instead of
 * breaking up. At full speed nothing is touched: the samples go to the device
 * as they are (after a slow stretch possibly a fraction of a block late). The
 * WAV file always gets the unstretched sound. */
#define SLOW_WINDOW 25
#define SLOW_ON 1.03
#define SLOW_OFF 1.01
#define SLOW_AFTER 12
#define SLOW_MAX 5.0
static u64 slow_t[SLOW_WINDOW]; static u32 slow_k; static int slow_over;
static int slow_stretching; static double slow_phase; static s16 slow_prev[2];
static s16 slow_q[FRAMES_PER_CALL * 2 * 8]; static int slow_qn;          /* sound not yet sent, in sample frames */
static u32 slow_blocks;
#define SLOW_QCAP ((int)(sizeof slow_q / sizeof slow_q[0] / 2))
/* in: one frame of sound; ratio >= 1: how much longer it has to last. Appends to the queue. */
static void slow_stretch(const s16 *in, int n, double ratio) {
    const double step = 1.0 / ratio;
    double pos = slow_phase;                      /* position in the input; -1 is the last sample frame of the previous call */
    while (pos < n - 1) {
        int i = (int)(pos + 1.0) - 1;             /* floor, for pos >= -1 */
        double fr = pos - (double)i;
        const s16 *a = i < 0 ? slow_prev : in + i * 2, *b = in + (i + 1) * 2;
        if (slow_qn < SLOW_QCAP) {
            slow_q[slow_qn * 2] = (s16)((double)a[0] + ((double)b[0] - (double)a[0]) * fr);
            slow_q[slow_qn * 2 + 1] = (s16)((double)a[1] + ((double)b[1] - (double)a[1]) * fr);
            slow_qn++;
        }
        pos += step;
    }
    slow_phase = pos - n;
}
/* pcm: the n sample frames of one video field (n = 960 at 50 Hz) */
static void slow_play(const s16 *pcm, int n) {
    u64 now = plat_time_ns();
    double ratio = 1.0;
    if (slow_k >= SLOW_WINDOW) {
        ratio = (double)(now - slow_t[slow_k % SLOW_WINDOW]) / (SLOW_WINDOW * (double)VIDEO_FIELD_NS);
        if (ratio > SLOW_MAX) ratio = SLOW_MAX;
    }
    slow_t[slow_k++ % SLOW_WINDOW] = now;
    slow_over = ratio >= SLOW_ON ? slow_over + 1 : 0;
    if (!slow_stretching && slow_over >= SLOW_AFTER) { slow_stretching = 1; slow_phase = 0.0; }
    else if (slow_stretching && ratio < SLOW_OFF) slow_stretching = 0;
    if (slow_stretching) slow_stretch(pcm, n, ratio < 1.0 ? 1.0 : ratio);
    else if (slow_qn == 0 && n == FRAMES_PER_CALL) { adev_play(pcm); goto done; }
    else if (slow_qn + n <= SLOW_QCAP) { memcpy(slow_q + slow_qn * 2, pcm, (size_t)n * 4); slow_qn += n; }
    while (slow_qn >= FRAMES_PER_CALL) {
        adev_play(slow_q); slow_blocks += (u32)slow_stretching;
        slow_qn -= FRAMES_PER_CALL;
        memmove(slow_q, slow_q + FRAMES_PER_CALL * 2, (size_t)slow_qn * 4);
    }
done:
    slow_prev[0] = pcm[(n - 1) * 2]; slow_prev[1] = pcm[(n - 1) * 2 + 1];
}

/* ---- entry points ----------------------------------------------------- */
/* Also runs when the process exits normally (destructor; atexit is not
 * linkable in this build, which has no crtbegin). */
__attribute__((destructor)) void audio_shutdown(void) {
    if (wav) { wav_header(); fclose(wav); wav = 0; }
    if (slow_blocks && getenv("ROR_AUDIODBG")) rt_log("[audio] slow game: %u blocks (20 ms each) of stretched sound were played\n", slow_blocks);
    adev_close();
}
void audio_init(int rt) {
    if (inited) { realtime = rt; return; }
    inited = 1; realtime = rt;
    const char *w = plat_getenv("ROR_WAV");
    if (w && *w) {
        wav = plat_fopen(w, "wb");
        if (!wav) rt_log("[audio] cannot write %s\n", w);
        else wav_header();
    }
    if (realtime && !getenv("ROR_NOAUDIO")) adev_open();
}
void audio_frame(void) {
    if (!inited) audio_init(1);
    static u64 field;
    s16 pcm[MAX_FIELD_SAMPLES * 2];
    const int n = (int)video_field_samples(field++);
    rxsnd_mix(pcm, n);
    if (wav) {
        fwrite(pcm, 1, (size_t)n * 4, wav); wav_bytes += (u32)n * 4;
        if (++wav_calls % 250 == 0) { wav_header(); fflush(wav); }      /* keep the file valid if the run is killed */
    }
    if (realtime) slow_play(pcm, n);
}
