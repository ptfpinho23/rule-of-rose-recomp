/* audio_out.c — host audio output and WAV dump.
 *
 * audio_frame() is called once per video frame: it pulls 960 stereo frames
 * (20 ms at 48 kHz) from the sound driver's mixer (rxsnd_mix) and
 *   - appends them to the WAV file named by $ROR_WAV, if set;
 *   - when the run is paced in real time, sends them to the sound device
 *     (plat.h adev_*: audio_pulse.c on Linux, audio_waveout.c on Windows).
 *     The device never stalls the emulation, at worst a frame of sound is
 *     dropped; without a device there is one log line and silence.
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

/* ---- entry points ----------------------------------------------------- */
/* Also runs when the process exits normally (destructor; atexit is not
 * linkable in this build, which has no crtbegin). */
__attribute__((destructor)) void audio_shutdown(void) {
    if (wav) { wav_header(); fclose(wav); wav = 0; }
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
    s16 pcm[FRAMES_PER_CALL * 2];
    rxsnd_mix(pcm, FRAMES_PER_CALL);
    if (wav) {
        fwrite(pcm, 1, FRAME_BYTES, wav); wav_bytes += FRAME_BYTES;
        if (++wav_calls % 250 == 0) { wav_header(); fflush(wav); }      /* keep the file valid if the run is killed */
    }
    if (realtime) adev_play(pcm);
}
