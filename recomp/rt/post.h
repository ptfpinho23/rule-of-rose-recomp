/* post.h — picture post-processing: scales the GS frame to the output size
 * (nearest, bilinear or AMD FSR 1.0) and applies the colour controls.
 * Platform independent and independent of the rest of the runtime (also
 * built into recomp/tools/posttool). */
#ifndef ROR_POST_H
#define ROR_POST_H
#include <stdint.h>

enum { POST_NEAREST, POST_BILINEAR, POST_FSR };

typedef struct PostSettings {
    int filter;        /* POST_*; default POST_FSR */
    int stretch;       /* 0: keep 4:3 inside the output with black bars; 1: fill the output */
    int sharpness;     /* 0..100, FSR RCAS strength: 100 = 0 stops (sharpest), 0 = RCAS off; default 60 */
    int brightness;    /* -100..100, default 0 */
    int contrast;      /* -100..100, default 0 */
    int gamma;         /* 50..200, 100 = 1.00, above 100 brightens midtones */
    int saturation;    /* 0..200, default 100 */
    int bloom;         /* 0..100, default 0 */
} PostSettings;

void post_defaults(PostSettings *s);
/* Returns 0 if (name, value) is not a valid setting; names are the command
 * line switches without the dashes ("filter", "aspect", "sharpness", ...). */
int  post_set(PostSettings *s, const char *name, const char *value);

/* src: sw*sh pixels 0x00RRGGBB (the frame as the console would show it on a
 * 4:3 screen, whatever its pixel dimensions). dst: dw*dh pixels, completely
 * written (picture plus black bars). The result depends only on the
 * arguments: not on the thread count, the platform or earlier calls. */
void post_run(const uint32_t *src, int sw, int sh, uint32_t *dst, int dw, int dh, const PostSettings *s);

/* Fills the 256-entry brightness/contrast/gamma table post_run applies per
 * channel; returns 0 (table untouched) when the three settings are neutral. */
int  post_lut(const PostSettings *s, uint8_t *lut);

/* Where post_run puts the picture inside a dw*dh output. */
void post_rect(int dw, int dh, const PostSettings *s, int *x, int *y, int *w, int *h);

/* RCAS constant (exp2(-stops)) for a sharpness setting; for the reference check in posttool. */
float post_rcas_con(int sharpness);

#endif
