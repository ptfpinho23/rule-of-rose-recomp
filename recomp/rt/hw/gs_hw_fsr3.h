/* gs_hw_fsr3.h — FSR 3.1 temporal upscaling in the Vulkan layer (gs_hw.c, "FSR 3.1"). */
#ifndef ROR_GS_HW_FSR3_H
#define ROR_GS_HW_FSR3_H
typedef struct HwFsr3_ {
    int color, sx, sy, sw, sh;        /* jittered picture: target and native rectangle */
    int mv;                           /* motion-vector target of the same native size (hw_rt_create_n(.., 1)) */
    int ovl_b, ovl_w;                 /* the 2D layer drawn over black and over white (targets of any scale), or -1 */
    int slot;                         /* 0 / 1: which kept output picture to write */
    int reset, no_reactive;           /* history is not valid (cut); do not hand over the reactive mask */
    float dt_ms, cam_near, cam_far, cam_fovy, unit_m;
} HwFsr3;
void hw_fsr3_want(void);                              /* before hw_start: enable what the FidelityFX backend needs on the device */
int  hw_fsr3_init(void);                              /* after hw_start: load the DLL; 0 (one log line) if FSR 3.1 cannot be used */
int  hw_fsr3_begin(int rw, int rh, int dw, int dh, float *jx, float *jy);   /* context for render -> output size, next jitter (render pixels); 0: not available */
int  hw_fsr3_upscale(const HwFsr3 *p);                /* 0: failed (FSR 3.1 is then off for good) */
void hw_fsr3_show(int slot);                          /* the calling thread's next present shows this slot if its size fits */
int  hw_rt_create_n(int w, int h, int n, int mv);     /* a target at scale n, or (mv) a motion-vector target */
int  hw_ds_create_n(int w, int h, int n);
void hw_rt_clear(int rt, float r, float g, float b, float a);
void hw_mv_params(float zfar, float zinv);            /* GS depth of the far plane and 1 / (near - far) for the motion-vector pass */
#endif
