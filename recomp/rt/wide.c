/* wide.c - widescreen (Hor+) for the 3D scenes: --wide 16:9 | 21:9 | 32:9 | W:H | ratio.
 *
 * The GS picture stays 512x448; it is rendered with a wider horizontal field
 * of view and meant to be shown stretched to the wide aspect (post_rect asks
 * wide_present for the aspect of the picture on display).
 *
 * 3D: the game builds every camera in one function (0x228b80 on the European
 * disc; ror_game.fn_camera) with a0 = renderer, a1 = frustum,
 * a2 = world-to-view matrix. a1 points to floats
 *     [0] near  [1] far  [2] left  [3] right  [4] bottom  [5] top   (on the near plane)
 *     [6] [7] depth range   [8..11] viewport x0, x1, y0, y1 (0, 511, 0, 447)
 *     [12] [13] GS XYOFFSET in pixels (1792, 1824)
 * and derives everything from it into a per-camera block that goes to VU1
 * (world-to-screen and world-to-clip matrices, screen scale/offset, scissor)
 * and is used for culling on the EE. wide_hook runs that function on a copy
 * of the frustum whose left/right are scaled around their centre by
 * (wide aspect) / (4/3). The game's own frustum structure is not changed.
 *
 * 2D: see wide_squeeze / wide_flip below. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"
#include "gs.h"

#include <math.h>
/* The last 4:3 camera the game built: near, far, vertical field of view
 * (radians), GS depth value at the near and at the far plane. Until one is
 * seen: the values every gameplay camera of the opening had (near 5, far
 * 2000, top/bottom +-2.21484 on the near plane, depth 65530 .. 1). */
static float g_cam[5] = { 5.0f, 2000.0f, 0.83f, 65530.0f, 1.0f };
void wide_frustum(float *cam) { memcpy(cam, g_cam, sizeof g_cam); }

static double g_wide;            /* target aspect / (4/3); 0 = off */
static double g_aspect;          /* target aspect; 0 = off */
#define WIDE_MAGIC 0x57494445u   /* marks the frustum copy of the nested call */

/* "16:9", "21:9", "32:9", "W:H" or a ratio such as 2.3704; "4:3" / "off" = off.
 * Limit 3.6 (32:9 = 3.556): beyond about 4.4 the clip volume kept at its 4:3
 * view-space size would come inside the screen. */
int wide_set(const char *v) {
    double a = 0, b = 0; char ch = 0;
    if (!v || !*v) return 0;
    if (!strcmp(v, "off")) { g_wide = g_aspect = 0; return 1; }
    int n = sscanf(v, "%lf%c%lf", &a, &ch, &b);
    if (n == 3 && ch == ':' && b > 0) a /= b; else if (n != 1) return 0;
    if (a < 1.0 || a > 3.6) return 0;
    g_aspect = a; g_wide = a / (4.0 / 3.0);
    if (g_wide < 1.0005) g_wide = g_aspect = 0;
    return 1;
}
double wide_aspect(void) { return g_aspect; }      /* 0: 4:3 */

/* ---- "3D on screen" latch -----------------------------------------------------
 * Measured order of a game frame N (one flip per 2 vblanks in gameplay):
 *   interval k  : EE builds the cameras (0x228b80) and display lists of N
 *   flip k+1    : DISPFB shows frame N-1; the lists of N are then sent
 *   interval k+1: GS draws N into the back buffer (while the EE builds N+1)
 *   flip k+2    : DISPFB shows frame N
 * The game builds its cameras during the movies too, so a frame counts as 3D
 * only when its cameras were widened AND VU1 (PATH1) drew into a display
 * buffer during it.
 *   cam_seen     cameras of the frame being built were widened
 *   draw_cam     same, for the frame being drawn
 *   p1_now       PATH1 primitives seen in the frame being drawn
 *   prev3d       the previous drawn frame was 3D
 *   present_wide the frame on display is 3D: show it at the wide aspect
 * 2D is squeezed when the previous frame was 3D or 3D was already drawn in
 * this one (the first / last frame of a 3D section can be off for one frame). */
static int cam_seen, draw_cam, p1_now, prev3d, present_wide;
int g_wide_want_p1;                                 /* gs.c: report the next PATH1 primitive (wide_path1) */
static u32 disp_fbp[2] = { ~0u, ~0u };
void wide_flip(u32 fbp) {                           /* gs.c: the displayed buffer changed (fbp in blocks) */
    if (g_wide > 0 && p1_now != present_wide) {     /* the picture changes shape: worth a line (also shows a flickering latch) */
        static int n;
        if (n < 400) { n++; rt_log("[wide] vblank %llu: picture %s\n", (unsigned long long)g_gs_frames, p1_now ? "wide (3D)" : "4:3"); }
    }
    present_wide = prev3d = p1_now; p1_now = 0;
    draw_cam = cam_seen; cam_seen = 0;
    g_wide_want_p1 = g_wide > 0 && draw_cam;
    disp_fbp[1] = disp_fbp[0]; disp_fbp[0] = fbp;
}
void wide_path1(u32 fbp) {                          /* gs.c: a PATH1 primitive while g_wide_want_p1 */
    if (fbp != disp_fbp[0] && fbp != disp_fbp[1]) return;
    p1_now = 1; g_wide_want_p1 = 0;
}
/* gs.c, per PATH2/PATH3 primitive: 1 = squeeze x around the screen centre by *inv */
int wide_squeeze(u32 fbp, float *inv) {
    if (g_wide <= 0 || !(prev3d || p1_now)) return 0;
    if (fbp != disp_fbp[0] && fbp != disp_fbp[1]) return 0;      /* render-to-texture targets stay as they are */
    *inv = (float)(1.0 / g_wide);
    return 1;
}
/* post.c: aspect of the picture now on display, as num:den */
void wide_present(int *num, int *den) {
    if (g_wide > 0 && present_wide) { *num = (int)(g_aspect * 3000.0 + 0.5); *den = 3000; }
    else { *num = 4; *den = 3; }
}

/* cpu.h TRACE_FN: entry of the camera builder. Returns 1 when the call has been done here. */
int wide_hook(Ctx *c, u32 entry) {
    if (entry != ror_game.fn_camera) return 0;
    {   /* FSR 3.1 (gs.c): the main camera's planes and depth range, widescreen or not */
        float f[8]; memcpy(f, HPTR(c->r[5].u32[0]), sizeof f);
        if (f[0] > 0 && f[1] > f[0] && f[5] > f[4] && f[3] > f[2] && (f[3] - f[2]) / (f[5] - f[4]) > 1.30f && (f[3] - f[2]) / (f[5] - f[4]) < 1.37f && f[6] > f[7] && f[7] > 0) {
            g_cam[0] = f[0]; g_cam[1] = f[1]; g_cam[2] = 2.0f * atanf((f[5] - f[4]) * 0.5f / f[0]); g_cam[3] = f[6]; g_cam[4] = f[7];
        }
        static int dbg = -1; if (dbg < 0) dbg = getenv("ROR_FRUSTUMLOG") != 0;
        if (dbg) { float g[16]; memcpy(g, HPTR(c->r[5].u32[0]), sizeof g); static float lf[16]; static int n;
            if (memcmp(g, lf, sizeof g) && n < 200) { n++; memcpy(lf, g, sizeof g); rt_log("[frustum] vblank %llu: near %g far %g l %g r %g b %g t %g  z %g %g  vp %g %g %g %g  off %g %g  [14] %g [15] %g\n", (unsigned long long)g_gs_frames, g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]); } }
    }
    if (g_wide <= 0) return 0;
    u32 a0 = c->r[4].u32[0], a1 = c->r[5].u32[0], sp = c->r[29].u32[0];
    u32 magic; memcpy(&magic, HPTR(sp + 64), 4);
    if (a1 == sp && magic == WIDE_MAGIC) return 0;                 /* our own nested call: run the body */
    float f[16]; memcpy(f, HPTR(a1), sizeof f);
    float l = f[2], r = f[3], b = f[4], t = f[5];
    /* only cameras that fill the 4:3 screen (not the square 256x256 shadow-texture camera) */
    if (!((t - b) > 0 && (r - l) > 0 && (r - l) / (t - b) > 1.30f && (r - l) / (t - b) < 1.37f)) return 0;
    cam_seen = 1;
    float mid = (l + r) * 0.5f, half = (r - l) * 0.5f * (float)g_wide;
    f[2] = mid - half; f[3] = mid + half;
    /* nested call with the widened copy of the frustum on the guest stack */
    u64 old_sp = c->r[29].u64[0];
    u32 nsp = sp - 80;
    u32 save[20]; memcpy(save, HPTR(nsp), sizeof save);   /* below sp is dead by the ABI; kept anyway */
    memcpy(HPTR(nsp), f, 64); magic = WIDE_MAGIC; memcpy(HPTR(nsp + 64), &magic, 4);
    c->r[29].u64[0] = (u64)(s64)(s32)nsp; c->r[5].u64[0] = (u64)(s64)(s32)nsp;
    /* 0x228b80 also derives two volumes around the screen from pixel limits: VU1's
     * clip volume (screen edge + k * (guard edge - screen edge), k = float at
     * renderer + 0x12128, 0.35 in the bus stop scene: +-863 px from the centre)
     * and the guard band (GS x/y 64 .. 4031.94, floats at ror_game.guard_band,
     * 0x32a5e4 / 0x32a5e8 on the European disc:
     * +-1984 px). Both are fixed in pixels, so with a wider frustum they open up
     * in view space, and polygons reaching behind the camera then pass
     * unclipped (seen at 16:9 and 21:9: a ground polygon with wrapped negative
     * Z drawn across the picture). Their half-widths are divided by the
     * widening for the duration of the call, which keeps them where they are
     * at 4:3 in view space. */
    float k0, g0[2], k1, g1[2];
    memcpy(&k0, HPTR(a0 + 0x12128), 4); memcpy(g0, HPTR(ror_game.guard_band), 8);
    half = (256.0f + k0 * (1984.0f - 256.0f)) / (float)g_wide;
    k1 = (half - 256.0f) / (1984.0f - 256.0f); if (k1 < 0.02f) k1 = 0.02f;
    half = 1984.0f / (float)g_wide; g1[0] = 2048.0f - half; g1[1] = 2048.0f + half;
    { static int keep = -1; if (keep < 0) keep = getenv("ROR_WIDE_NOCLIPFIX") != 0; if (keep) { k1 = k0; g1[0] = g0[0]; g1[1] = g0[1]; } }   /* test: volumes as the game sets them */
    memcpy(HPTR(a0 + 0x12128), &k1, 4); memcpy(HPTR(ror_game.guard_band), g1, 8);
    rt_lookup(c, ror_game.fn_camera)(c);        /* the generated function; its hook sees the marked copy and runs the body */
    memcpy(HPTR(a0 + 0x12128), &k0, 4); memcpy(HPTR(ror_game.guard_band), g0, 8);
    memcpy(HPTR(nsp), save, sizeof save);
    c->r[29].u64[0] = old_sp;                           /* a1 is caller-saved; nothing to restore */
    return 1;
}
