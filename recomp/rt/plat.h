/* plat.h — the operating-system interface of the runtime.
 *
 * Everything the runtime needs from the host OS goes through this header.
 * Implementations: plat_posix.c (Linux) and plat_win32.c (Windows); the
 * window in video_x11.c / video_win32.c; the sound device in audio_pulse.c /
 * audio_waveout.c; the gamepad in pad_evdev.c /
 * pad_win32.c (pad_xinput.c, pad_hid.c). Each of those files compiles to nothing on the other
 * platform, so both build scripts can simply compile every .c file in recomp/rt.
 *
 * All paths and strings are UTF-8 on both platforms (the Windows side
 * converts to UTF-16 and uses the wide Win32/CRT calls). */
#ifndef ROR_PLAT_H
#define ROR_PLAT_H
#include <stdio.h>
#include <time.h>
#include "cpu.h"

/* ---- process ------------------------------------------------------------ */
/* First call in main(): on Windows replaces argv with UTF-8 strings taken
 * from the wide command line. */
void plat_init(int *argc, char ***argv);
/* Directory of the running executable, '/' separated, no trailing slash. */
void plat_exe_dir(char *out, size_t n);
const char *plat_getenv(const char *name);          /* UTF-8; valid until the process ends */
void plat_setenv(const char *name, const char *value);
void plat_exit_now(int code) __attribute__((noreturn));   /* no atexit handlers, no flushing */

/* Fatal host faults (bad memory access, illegal instruction, divide error)
 * call on_fault(what, addr, has_addr) and Ctrl-C / termination requests call
 * on_interrupt(what); both are expected to report and plat_exit_now().
 * Also starts the sampling profiler when $ROR_PROF is set (POSIX only). */
void plat_install_handlers(void (*on_fault)(const char *what, void *addr, int has_addr),
                           void (*on_interrupt)(const char *what));
void prof_reset(void);                              /* drop profiler samples so far */

/* ---- guest address space ------------------------------------------------ */
/* Reserve `total` bytes at the fixed address `base`, inaccessible, and place
 * the listed mappings inside it. Mappings with the same object number
 * (>= 0) are views of one shared zero-filled memory object (the RAM
 * mirrors); object -1 is private zero-filled memory. Offsets must be
 * multiples of 64 KiB. Anything not listed faults when touched. Exits with
 * a message on failure. */
typedef struct { u32 offset, size; int object; } PlatMap;
void plat_guest_map(void *base, u64 total, const PlatMap *maps, int n);

/* ---- coroutines ---------------------------------------------------------
 * Guest threads and the IPU decoder are coroutines on one host thread. */
typedef struct PlatCo PlatCo;
PlatCo *plat_co_new(size_t stack_size);             /* 0: a slot that only saves the caller in plat_co_switch */
void plat_co_start(PlatCo *co, void (*entry)(void));/* next switch to co enters entry() on a fresh stack; entry must not return */
void plat_co_switch(PlatCo *from, PlatCo *to);      /* save the running context in `from`, continue `to` */
void plat_co_jump(PlatCo *to) __attribute__((noreturn));  /* continue `to`, abandoning the running context */
void plat_co_free(PlatCo *co);                      /* co must not be running */

/* ---- time --------------------------------------------------------------- */
u64  plat_time_ns(void);                            /* monotonic */
u64  plat_cpu_ns(void);                             /* CPU time used by the process, all threads */
void plat_sleep_until(u64 t_ns);                    /* on the plat_time_ns clock */
void plat_gmtime(s64 t, struct tm *out);

/* ---- files -------------------------------------------------------------- */
FILE *plat_fopen(const char *path, const char *mode);
int   plat_fseek64(FILE *f, u64 offset);            /* from the start of the file; 0 on success */
typedef struct { int is_dir; u64 size; s64 ctime, mtime; } PlatStat;
int   plat_stat(const char *path, PlatStat *st);    /* 0 on success */
int   plat_mkdir(const char *path);                 /* 0 on success */
int   plat_rmdir(const char *path);
int   plat_unlink(const char *path);
typedef struct PlatDir PlatDir;
PlatDir    *plat_opendir(const char *path);
const char *plat_readdir(PlatDir *d);               /* next entry name (includes "." and ".." where the OS lists them), 0 at the end */
void        plat_closedir(PlatDir *d);
#ifdef _WIN32
#define PLAT_IS_SEP(ch) ((ch) == '/' || (ch) == '\\')
#else
#define PLAT_IS_SEP(ch) ((ch) == '/')
#endif

/* ---- window (video_x11.c / video_win32.c) -------------------------------- */
/* w x h is the client area in real pixels; a size that does not fit on the
 * screen is reduced with video_fit(). Returns 0 if no display is available. */
int  vwin_open(int w, int h, int fullscreen);
void video_fit(int *w, int *h, int max_w, int max_h);   /* video.c */
/* Handle window events; key presses/releases update the active-low pad word.
 * Alt+Enter and F11 switch between the window and borderless fullscreen.
 * Exits the process when the window is closed or Esc is pressed. */
void vwin_pump(u16 *pad_buttons);
void vwin_size(int *w, int *h);                     /* current size of the drawable area (left alone if unknown) */
/* Show a w*h image of 0x00RRGGBB pixels 1:1 (it is made at the vwin_size size). */
void vwin_show(const u32 *px, int w, int h);
void vwin_flush(void);
/* Native handles for a Vulkan surface: X11 Display* and Window, or Windows HINSTANCE and HWND. */
void vwin_native(void **a, void **b);
int  vwin_refresh_hz(void);                         /* refresh rate of the display the window is on (60 if unknown) */

/* ---- gamepad (pad_evdev.c / pad_win32.c; mapping to the PS2 pad in pad.c) --
 * One host gamepad in Xbox terms. Buttons use the XInput bit layout. */
enum { HPAD_UP = 0x0001, HPAD_DOWN = 0x0002, HPAD_LEFT = 0x0004, HPAD_RIGHT = 0x0008,
       HPAD_START = 0x0010, HPAD_BACK = 0x0020, HPAD_LS = 0x0040, HPAD_RS = 0x0080,
       HPAD_LB = 0x0100, HPAD_RB = 0x0200,
       HPAD_A = 0x1000, HPAD_B = 0x2000, HPAD_X = 0x4000, HPAD_Y = 0x8000 };
typedef struct {
    u16 buttons;                                    /* HPAD_*, bit set = pressed */
    u8  lt, rt;                                     /* triggers, 0..255 */
    s16 lx, ly, rx, ry;                             /* sticks, -32768..32767, +x right, +y up */
} HostPad;
/* Called once per frame. Returns 1 and fills *st and name (device name) if a
 * gamepad is connected. Looks for a device about once per second while there
 * is none; never blocks. */
int  hpad_poll(HostPad *st, char *name, size_t n);
const char *hpad_status(void);                      /* why there is no pad / which interface is used; for the log */
const char *hpad_kind(void);                        /* button legends of the connected pad: "xbox" or "playstation" */
/* Which devices to consider: 0 any, 1 XInput only (Linux: pads that are not Sony's), 2 Sony pads only. */
void hpad_select(int which);
void hpad_rumble(int small, int big);               /* motors 0..255 each (high-frequency, low-frequency); 0,0 stops */

/* ---- sound device (audio_pulse.c / audio_waveout.c) ----------------------
 * 48 kHz stereo s16; adev_play takes one video frame's worth (960 frames)
 * and never blocks: when the device is too far behind the block is dropped. */
void adev_open(void);
void adev_play(const s16 *pcm);                     /* no-op while the device is closed */
void adev_close(void);

#endif
