/* gs_hw.h — Vulkan layer of the GS hardware renderer (see gs_hw.c). It knows
 * images, pipelines and draws; everything about GS memory lives in gs.c. */
#ifndef ROR_GS_HW_H
#define ROR_GS_HW_H
#include <stdint.h>

typedef struct HwVertex_ { float x, y, z, fog; float s, t, q; float r, g, b, a; float sx0, sy0, dudx, dvdy; } HwVertex;   /* native pixels; colour 0..255 */

enum { HWB_OFF, HWB_MIX, HWB_ADD, HWB_REVSUB };          /* fixed-function blend: Cs*F + Cd*(1-F), Cs*F + Cd, Cd - Cs*F */
enum { HWF_TME = 1, HWF_TEXEL = 2, HWF_BILIN = 4, HWF_TCC = 8, HWF_TFX_SHIFT = 4, HWF_FGE = 64, HWF_ATST_SHIFT = 7, HWF_ATE = 1024,
       HWF_PASS_SHIFT = 11, HWF_FBA = 8192, HWF_SHBLEND = 16384, HWF_DATE = 32768, HWF_DATM = 65536, HWF_COLCLAMP = 131072,
       HWF_WMS_SHIFT = 18, HWF_WMT_SHIFT = 20, HWF_TEX24 = 1 << 22, HWF_AEM = 1 << 23, HWF_SPRITE = 1 << 24,
       HWF_MIP = 1 << 25 /* set by the layer itself: minified texels are filtered from mip levels */ };
/* (HwState.afail 4 / 5, motion-vector targets only: draw the failing / the passing fragments as reactive values) */
typedef struct HwState_ {
    int rt, ds;                       /* colour and depth target */
    int sx0, sy0, sx1, sy1;           /* scissor, native pixels, inclusive */
    int tex_kind, tex;                /* 0 none, 1 texture, 2 render target (sampled at device resolution) */
    uint32_t flags; int aref; uint32_t fogcol, blend, fbmsk;
    int region[4]; float texw, texh, zscale, zmax, ta0;
    uint8_t blend_mode, blend_const, fix, cmask, ztest /* 0 off, 1 always, 2 gequal, 3 greater */, zwrite;
    uint8_t afail;                    /* 1 FB_ONLY, 2 ZB_ONLY, 3 RGB_ONLY: failing fragments drawn in a second pass per primitive */
    uint8_t need_dst;                 /* fragment shader reads the destination (copy made per primitive) */
} HwState;

int  hw_start(int scale);                              /* 0: no usable Vulkan device */
/* Presentation. a, b: Display* and Window (X11) or HINSTANCE and HWND (Windows). */
int  hw_swap_attach(void *a, void *b, int vsync);     /* vsync 0 off, 1 on without blocking, 2 on and blocking (FIFO); returns 0 if no swapchain is possible */
void hw_present_opts(float rcas, int bloom);          /* RCAS constant for filter 2 (0 = off) and bloom strength 0..100 */
void hw_grab_request(void);                           /* copy the next presented image ... */
const uint32_t *hw_grab_result(int *w, int *h, int *bgra);   /* ... and fetch it after that hw_present (null if not available) */
void hw_set_lut(const uint8_t *lut);                  /* 256-entry colour table for hw_present, or null for none */
/* Show native rect (sx, sy, sw, sh) of a target in window rect (dx, dy, dw, dh) of a ww x wh window; rest black. */
/* The same picture hw_present would put in the window, rendered off screen and read back (RGBA, ww x wh). */
const uint32_t *hw_present_pixels(int rt, int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh, int ww, int wh, int filter /* 0 nearest, 1 bilinear, 2 FSR 1.0, 3 = 2 unless hw_fsr3_show was called */, int sat64);
void hw_present(int rt, int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh, int ww, int wh, int filter /* 0 nearest, 1 bilinear, 2 FSR 1.0 */, int sat64);
const char *hw_device_name(void);
int  hw_device_discrete(void);                         /* 1: a discrete graphics card (not integrated graphics, not a software driver) */
int  hw_rt_create(int w, int h);                      /* native size; contents undefined */
int  hw_ds_create(int w, int h);                      /* depth, cleared to 0 */
void hw_ds_clear(int ds);
void hw_rt_upload(int rt, int x, int y, int w, int h, const uint32_t *rgba);     /* native rect -> N x N blocks */
void hw_rt_download(int rt, int x, int y, int w, int h, uint32_t *rgba);        /* native rect; waits for the GPU */
int  hw_rt_copy_async(int rt, int x, int y, int w, int h);                       /* queued native read; returns a ticket */
void hw_ticket_read(int ticket, uint32_t *rgba);      /* valid after the next hw_flush */
const uint32_t *hw_rt_read_scaled(int rt, int x, int y, int w, int h);           /* native rect at device resolution (RGBA) */
void hw_rt_rows_from_depth(int rt, int dy, int rows, int ds, const uint32_t *table, int page0, int bw);   /* see gs_hw.c */
void hw_rt_copy_rows(int src, int sy, int dst, int dy, int rows);                /* native rows, full width, src -> dst */
void hw_thread_init(void);                            /* a second thread calls this before using the layer */
void hw_rt_save(int rt, int slot, int final);         /* keep a copy of a target: 4 slots each of "start of frame" and "final" */
void hw_rt_load(int dst, int src, int slot, int final);   /* a kept copy of target src into target dst (any thread) */
void hw_rt_restore(int rt, int slot);
void hw_ds_save(int ds, int slot);                    /* the same for a depth buffer ("start of frame" only) */
void hw_ds_load(int dst, int src, int slot);
void hw_keep_generation(void);                        /* one more retained draw list exists: textures given up stay alive for 5 of these */
int  hw_rt_count(void);
void hw_tex_filter(int smooth);                       /* 1: textures created from now on get mip levels and are filtered when minified (--texture-filter smooth) */
int  hw_tex_create(int w, int h, const uint32_t *rgba);
void hw_tex_destroy(int tex);                         /* deferred until the GPU is idle */
void hw_draw(const HwState *s, const HwVertex *v, int n);                        /* triangle list */
void hw_flush_at(int site);                           /* submit everything and wait (site: caller's line, for statistics) */
#define hw_flush() hw_flush_at(__LINE__)
extern int hw_flush_sites[16], hw_flush_counts[16];   /* game thread: submits per call site since the counts were last cleared */
extern uint64_t hw_stat_draws, hw_stat_copies, hw_stat_flushes, hw_stat_wait_ns;   /* wait_ns: game thread waiting for the GPU */
#include "gs_hw_fsr3.h"
#endif
