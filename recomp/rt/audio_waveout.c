/* audio_waveout.c — sound device for Windows (plat.h adev_*): waveOut.
 *
 * A ring of 20 ms blocks is handed to the default output device. Nothing
 * waits for the device: finished blocks are found by polling WHDR_DONE when
 * the next frame arrives, and a frame is dropped when more than MAX_QUEUED
 * blocks are still waiting (the game is running ahead of the sound card).
 * When the queue has run dry, a short cushion of silence goes in first so
 * frame-time jitter does not turn into a string of gaps. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include "rt.h"
#include "plat.h"

#define RATE 48000
#define FRAMES_PER_CALL 960
#define FRAME_BYTES (FRAMES_PER_CALL * 4)
#define NBLK 16
#define MAX_QUEUED 10                       /* 200 ms */
#define CUSHION 3                           /* 60 ms */

static HWAVEOUT dev;
static WAVEHDR hdr[NBLK];
static s16 blk[NBLK][FRAMES_PER_CALL * 2];
static int busy[NBLK];
static u32 sent, dropped, underruns, max_queued;

static int queued(void) {
    int n = 0;
    for (int i = 0; i < NBLK; i++) {
        if (busy[i] && (hdr[i].dwFlags & WHDR_DONE)) busy[i] = 0;
        n += busy[i];
    }
    return n;
}
static int submit(const s16 *pcm) {
    for (int i = 0; i < NBLK; i++) {
        if (busy[i]) continue;
        if (pcm) memcpy(blk[i], pcm, FRAME_BYTES); else memset(blk[i], 0, FRAME_BYTES);
        hdr[i].dwFlags &= ~(DWORD)WHDR_DONE;
        if (waveOutWrite(dev, &hdr[i], sizeof hdr[i]) != MMSYSERR_NOERROR) return 0;
        busy[i] = 1;
        return 1;
    }
    return 0;
}
void adev_open(void) {
    WAVEFORMATEX wf; memset(&wf, 0, sizeof wf);
    wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 2; wf.nSamplesPerSec = RATE; wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4; wf.nAvgBytesPerSec = RATE * 4;
    MMRESULT r = waveOutOpen(&dev, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL);
    if (r != MMSYSERR_NOERROR) { dev = 0; rt_log("[audio] no sound output device (waveOutOpen error %u); continuing without sound\n", (unsigned)r); return; }
    for (int i = 0; i < NBLK; i++) {
        memset(&hdr[i], 0, sizeof hdr[i]);
        hdr[i].lpData = (LPSTR)blk[i]; hdr[i].dwBufferLength = FRAME_BYTES;
        if (waveOutPrepareHeader(dev, &hdr[i], sizeof hdr[i]) != MMSYSERR_NOERROR) {
            waveOutClose(dev); dev = 0; rt_log("[audio] cannot prepare sound buffers; continuing without sound\n"); return;
        }
    }
    rt_log("[audio] waveOut output on the default device, 48 kHz stereo\n");
}
void adev_play(const s16 *pcm) {
    if (!dev) return;
    int q = queued();
    if (q == 0) {                            /* start, or the device ran dry */
        if (sent) underruns++;
        for (int i = 0; i < CUSHION; i++) submit(0);
        q = CUSHION;
    }
    if ((u32)q > max_queued) max_queued = (u32)q;
    if (q >= MAX_QUEUED || !submit(pcm)) { dropped++; return; }
    sent++;
    if (getenv("ROR_AUDIODBG") && sent % 250 == 0)
        rt_log("[audio] sent %u frames (20 ms each), dropped %u, underruns %u, queued now %d (max %u)\n", sent, dropped, underruns, q + 1, max_queued);
}
void adev_close(void) {
    if (!dev) return;
    if (getenv("ROR_AUDIODBG")) rt_log("[audio] sent %u frames (20 ms each), dropped %u, underruns %u, max queued %u\n", sent, dropped, underruns, max_queued);
    waveOutReset(dev);
    for (int i = 0; i < NBLK; i++) waveOutUnprepareHeader(dev, &hdr[i], sizeof hdr[i]);
    waveOutClose(dev); dev = 0;
}
#endif
