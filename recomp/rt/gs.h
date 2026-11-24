/* gs.h — Graphics Synthesizer: software implementation. */
#ifndef ROR_GS_H
#define ROR_GS_H
#include "rt.h"

void gs_init(void);
void gs_write_reg(u32 reg, u64 data);                 /* general registers (via GIF) */
void gs_write_hwreg(const u8 *data, u32 bytes);       /* image data, host -> local */
u64  gs_read_priv(u32 addr);                          /* privileged registers 0x12000000.. */
void gs_write_priv(u32 addr, u64 v);
void gs_vblank(int start);
void gs_set_q(float q);

/* Display output: converts the current display buffer to 32-bit XRGB.
 * Returns 0 if no display is enabled. */
int  gs_get_frame(u32 **pixels, int *w, int *h);

/* Complete GS state to/from a file (used by the ROR_GSCAP capture and by
 * recomp/tools/gsreplay). Returns 1 on success. */
int  gs_snapshot(FILE *f, int save);

/* Renderer selection, before gs_init: gpu = 1 asks for the Vulkan renderer at
 * `internal` times the PS2 resolution (1..8). If no usable device is found
 * the software renderer is used (one log line says so). */
void gs_renderer(int gpu, int internal);
/* Frame skipping: while want is set, whole frames the game draws are left out
 * (at most two in a row, and only frames that start by clearing the picture);
 * the display keeps the newest complete picture. See "frame skipping" in gs.c. */
void gs_frameskip(int want);
void gs_frameskip_stats(u64 *skipped, u64 *frames);   /* frames left out / frames the game finished, so far */
void gs_film_grain(int pct);                          /* --film-grain 0..100 (100 = the game's own strength; both renderers) */
int  gs_hw_active(void);                              /* 1 when the GPU renderer is drawing */
/* --filter fsr3, before gs_init: ask for FSR 3.1 temporal upscaling on the GPU
 * renderer (AMD FidelityFX DLL beside the program; Windows). The scaling
 * filter number is then 3; where FSR 3.1 is not available (one log line says
 * why) 3 behaves as 2. Without a window, gs_hw_fsr3_rect tells every frame
 * where the picture goes in the output (the upscaler works across frames). */
void gs_fsr3(int want);                    /* 0 no, 1 FSR 3.1, 2 DLSS (NVIDIA Streamline DLLs beside the program; then FSR 3.1, then FSR 1.0) */
/* --framegen, before gs_init: 1 = AMD FSR 3 frame generation on the pictures of
 * the temporal upscaler (AMD FidelityFX DLL), 2 = DLSS Frame Generation (only
 * reported: see "DLSS" in hw/gs_hw.c). Where it cannot run: one log line, off. */
void gs_framegen(int kind);
void gs_hw_fsr3_rect(int dx, int dy, int dw, int dh, int ww, int wh);
/* Presentation through a swapchain on the game window (a, b: Display* and
 * Window, or HINSTANCE and HWND). Returns 0 if that is not possible; the
 * picture can then still be fetched with gs_get_frame. */
int  gs_hw_attach(void *a, void *b, int vsync);
/* Show the displayed picture in rect (dx, dy, dw, dh) of a ww x wh window.
 * lut: 256-entry colour table or null; sat64: saturation * 64 / 100. Returns
 * 0 if the display is off. */
/* `linear` is the scaling filter: 0 nearest, 1 bilinear, 3 FSR 3.1 (see gs_fsr3), 2 FSR 1.0 (EASU up /
 * area filter down, then RCAS). gs_hw_present_opts sets the RCAS constant
 * (post_rcas_con; 0 = no sharpening) and the bloom strength (0..100). */
void gs_hw_present_opts(float rcas, int bloom);
int  gs_hw_show(int dx, int dy, int dw, int dh, int ww, int wh, int linear, const u8 *lut, int sat64);
/* Frame interpolation (after a successful gs_hw_attach with vsync mode 2 or 0):
 * a presenter thread shows in-between pictures; fps > 0 limits the rate. The
 * game thread then calls gs_hw_vblank once per emulated vblank (ideal_ns: the
 * vblank's time on the plat_time_ns clock) instead of gs_hw_show. */
void gs_hw_interp(int fps, int display_hz);
void gs_hw_vblank(int dx, int dy, int dw, int dh, int ww, int wh, int linear, const u8 *lut, int sat64, u64 ideal_ns);
/* The same picture, rendered off screen and read back as ww x wh 0x00RRGGBB pixels (screenshots, tests). */
int  gs_hw_show_pixels(int dx, int dy, int dw, int dh, int ww, int wh, int linear, const u8 *lut, int sat64, u32 **pixels);

extern u32 g_gs_imr;
extern u64 g_gs_prims, g_gs_frames;
extern u64 g_gs_frame_id;       /* identifies the picture gs_get_frame last returned: unchanged = the same pixels again */

/* gif.c */
enum { GIF_PATH1, GIF_PATH2, GIF_PATH3 };
void gif_write(int path, const u8 *data, u32 bytes);  /* whole quadwords */
/* Path 1 (VU1 XGKICK): packet inside VU memory, wraps at `mask`+1.
 * Stops at the end-of-packet tag. */
void gif_write_path1(const u8 *mem, u32 addr, u32 mask);
void gif_reset(void);

#endif
