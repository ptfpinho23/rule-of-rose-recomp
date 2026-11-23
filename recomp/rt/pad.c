/* pad.c — host gamepad -> the emulated DualShock 2 (iop_dbc.c).
 *
 * The devices themselves are read in pad_evdev.c (Linux) and pad_xinput.c
 * (Windows); both deliver a HostPad in Xbox terms (plat.h). This file maps
 * that to the PS2 pad, merges it with the keyboard and passes the game's
 * vibration requests back.
 *
 * What the game does with the pad (0x101ccc, once per frame): it takes the
 * 16 button bits and the four stick bytes. Each stick axis goes through
 * 0x101e64 / 0x101eec (right X, Y) and 0x101f74 / 0x101ffc (left X, Y):
 *     byte <  96 : (byte - 96) / 96      (-1 at 0)
 *     byte > 160 : (byte - 160) / 95     (+1 at 255)
 *     otherwise  : 0                     (the game's own dead zone, 25 %)
 * and 0x102188 builds the movement vector from the D-pad if a direction is
 * held (exactly -1 / +1) and otherwise from the left stick; a stick axis
 * beyond +-0.7 also counts as that D-pad direction (menus). So the left
 * stick is movement, analog, and the D-pad is the same thing at full tilt.
 *
 * Mapping of a host stick: radial dead zone (--pad-deadzone, percent of full
 * tilt), the rest rescaled to 0..1, and each axis is then placed directly
 * outside the game's dead band (96 - n or 160 + n), so the host dead zone is
 * the only one felt and the whole remaining travel is analog.
 *
 * Test hook: ROR_PAD_FAKE="frame:buttons[:lx:ly:rx:ry[:lt:rt]],..." feeds a
 * synthetic HostPad (buttons = HPAD_* mask in hex, sticks -32768..32767 with
 * +y up, triggers 0..255; each entry holds until the next) through the same
 * mapping. While it is set no real device is read. */
#include <math.h>
#include "rt.h"
#include "plat.h"

void pad_set_state(u16 buttons, u8 rx, u8 ry, u8 lx, u8 ly) __attribute__((weak));
void pad_get_actuators(u8 *small_motor, u8 *big_motor) __attribute__((weak));

#define TRIGGER_ON 64                    /* of 255: L2 / R2 count as pressed from here */

static int mode_auto = 1, mode_sel, deadzone = 24; /* percent; 24 = XInput's suggested left-stick dead zone */

static struct { u64 frame; HostPad st; } fake[512];
static int fake_n = -1, fake_on;
static HostPad fake_cur;

/* --pad auto|off|xinput|playstation, --pad-deadzone 0..90. Returns 0 for a bad value. */
int pad_option(const char *name, const char *value) {
    if (!value) return 0;
    if (!strcmp(name, "pad")) {
        if (!strcmp(value, "auto")) { mode_auto = 1; mode_sel = 0; }
        else if (!strcmp(value, "off")) mode_auto = 0;
        else if (!strcmp(value, "xinput")) { mode_auto = 1; mode_sel = 1; }
        else if (!strcmp(value, "playstation")) { mode_auto = 1; mode_sel = 2; }
        else return 0;
        return 1;
    }
    if (!strcmp(name, "pad-deadzone")) {
        char *e; long n = strtol(value, &e, 10);
        if (!*value || *e || value[0] < '0' || value[0] > '9' || n > 90) return 0;
        deadzone = (int)n; return 1;
    }
    return 0;
}

static void fake_parse(void) {
    const char *s = plat_getenv("ROR_PAD_FAKE");
    fake_n = 0;
    while (s && *s && fake_n < 512) {
        char *e; long v[7] = { 0, 0, 0, 0, 0, 0, 0 };
        u64 frame = strtoull(s, &e, 10);
        if (*e != ':') break;
        v[0] = (long)strtoul(e + 1, &e, 16);
        for (int i = 1; i < 7 && *e == ':'; i++) v[i] = strtol(e + 1, &e, 10);
        HostPad *p = &fake[fake_n].st;
        fake[fake_n++].frame = frame;
        p->buttons = (u16)v[0];
        p->lx = (s16)v[1]; p->ly = (s16)v[2]; p->rx = (s16)v[3]; p->ry = (s16)v[4];
        p->lt = (u8)v[5]; p->rt = (u8)v[6];
        s = (*e == ',') ? e + 1 : e;
        if (s == e && *e) break;
    }
    if (fake_n) rt_log("[pad] ROR_PAD_FAKE: %d synthetic gamepad states; no real gamepad is read\n", fake_n);
}
/* 1 if ROR_PAD_FAKE supplies the gamepad (video.c then calls pad_update on every frame, window or not). */
int pad_fake(void) {
    if (fake_n < 0) fake_parse();
    return fake_n > 0;
}

/* One axis, -1..1 after the dead zone, to the wire byte: the inverse of the
 * game's conversion (see the top of the file). */
static u8 axis_byte(double v) {
    int n;
    if (v < 0) { n = (int)(-v * 96.0 + 0.5); if (n > 96) n = 96; return n ? (u8)(96 - n) : 0x80; }
    n = (int)(v * 95.0 + 0.5); if (n > 95) n = 95;
    return n ? (u8)(160 + n) : 0x80;
}
/* Host stick (+y up) to the two wire bytes (0 = left / up, 0x80 = centre). */
static void map_stick(int x, int y, u8 *bx, u8 *by) {
    double fx = x / 32767.0, fy = -y / 32767.0, dz = deadzone / 100.0;
    double m = sqrt(fx * fx + fy * fy);
    *bx = *by = 0x80;
    if (m <= dz || m <= 0.0) return;
    double k = ((m > 1.0 ? 1.0 : m) - dz) / (1.0 - dz) / m;
    fx *= k; fy *= k;
    *bx = axis_byte(fx < -1.0 ? -1.0 : fx > 1.0 ? 1.0 : fx);
    *by = axis_byte(fy < -1.0 ? -1.0 : fy > 1.0 ? 1.0 : fy);
}
/* Host buttons to the PS2 button word, bit set = pressed (the wire is the inverse). */
static u16 map_buttons(const HostPad *h) {
    static const struct { u16 host, ps2; } tab[] = {
        { HPAD_BACK, 0x0001 }, { HPAD_LS, 0x0002 }, { HPAD_RS, 0x0004 }, { HPAD_START, 0x0008 },
        { HPAD_UP, 0x0010 }, { HPAD_RIGHT, 0x0020 }, { HPAD_DOWN, 0x0040 }, { HPAD_LEFT, 0x0080 },
        { HPAD_LB, 0x0400 }, { HPAD_RB, 0x0800 },                                   /* L1 R1 */
        { HPAD_Y, 0x1000 }, { HPAD_B, 0x2000 }, { HPAD_A, 0x4000 }, { HPAD_X, 0x8000 },   /* triangle circle cross square */
    };
    u16 b = 0;
    for (unsigned i = 0; i < sizeof tab / sizeof *tab; i++) if (h->buttons & tab[i].host) b |= tab[i].ps2;
    if (h->lt >= TRIGGER_ON) b |= 0x0100;                                           /* L2 */
    if (h->rt >= TRIGGER_ON) b |= 0x0200;                                           /* R2 */
    return b;
}

static void rumble_off(void) { hpad_rumble(0, 0); }

/* Once per frame: combine the keyboard word (active low) with the gamepad
 * and hand the result to the emulated pad. `live` = a real device may be
 * read (a window is open and the run is not scripted). */
void pad_update(u64 frame, u16 kbd, int live) {
    static int started, connected, announced, rum_small = -1, rum_big = -1, rum_hooked;
    static char name[128];
    HostPad hp; int have = 0, real = 0;
    if (pad_fake()) {
        for (int i = 0; i < fake_n; i++) if (fake[i].frame == frame) { fake_cur = fake[i].st; fake_on = 1; }
        if (fake_on) { hp = fake_cur; have = 1; }
    } else if (!live || !mode_auto) {
        if (!started) rt_log(mode_auto ? "[pad] scripted run: no gamepad is read\n" : "[pad] gamepad input is off (--pad off)\n");
    } else {
        /* The start-up line is held back for half a second: devices are looked for in the background on Windows. */
        char now[128] = "";
        if (!started) hpad_select(mode_sel);
        have = real = hpad_poll(&hp, now, sizeof now);
        if (have && (!connected || strcmp(now, name))) {
            snprintf(name, sizeof name, "%s", now);
            rt_log("[pad] %s: %s (%s; kind %s; stick dead zone %d %%)\n", announced ? "gamepad connected" : "gamepad", name, hpad_status(), hpad_kind(), deadzone);
            rum_small = rum_big = -1; announced = 1;
        } else if (!have && connected) rt_log("[pad] gamepad disconnected: %s\n", name);
        else if (!have && !announced && started >= 25) {
            rt_log("[pad] no gamepad found (%s); keyboard only until one is connected\n", hpad_status());
            announced = 1;
        }
        connected = have;
    }
    if (started < 1000) started++;
    u16 b = kbd; u8 lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
    if (have) {
        b &= (u16)~map_buttons(&hp);
        map_stick(hp.lx, hp.ly, &lx, &ly);
        map_stick(hp.rx, hp.ry, &rx, &ry);
    }
    if (pad_set_state) pad_set_state(b, rx, ry, lx, ly);
    if (real && pad_get_actuators) {                 /* vibration: the game's last request, sent when it changes */
        u8 s, g; pad_get_actuators(&s, &g);
        if (s != rum_small || g != rum_big) {
            if (!rum_hooked && (s || g)) { atexit(rumble_off); rum_hooked = 1; }
            if (rum_small >= 0 || s || g) hpad_rumble(s ? 255 : 0, g);
            rum_small = s; rum_big = g;
        }
    }
}
