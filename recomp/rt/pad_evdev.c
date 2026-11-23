/* pad_evdev.c — gamepad on Linux (plat.h hpad_*): the kernel's evdev
 * interface, /dev/input/event*, read directly (no libudev, no SDL).
 *
 * A device is a gamepad if it reports BTN_SOUTH (= BTN_GAMEPAD, = BTN_A);
 * that is read from /sys/class/input/eventN/device/capabilities/key first,
 * so keyboards and mice (which a normal user may not open) are not touched
 * and a gamepad that cannot be opened can be named in the log. The device
 * with the lowest event number is used. While there is none, /dev/input is
 * looked through once per second (hot-plug). State is read with EVIOCGKEY /
 * EVIOCGABS once per frame; the event queue is only drained.
 *
 * Layout, per Documentation/input/gamepad.rst and what the drivers do:
 *   buttons  BTN_SOUTH A, BTN_EAST B, BTN_TL / BTN_TR bumpers, BTN_SELECT,
 *            BTN_START, BTN_THUMBL / BTN_THUMBR, BTN_TL2 / BTN_TR2 (digital
 *            triggers), BTN_DPAD_*. Codes 0x133 / 0x134: xpad (and the pads
 *            that imitate it) report X on 0x133 and Y on 0x134; Sony
 *            (vendor 054c) and Nintendo (057e) drivers use them by position
 *            (0x133 = top button, 0x134 = left button), i.e. the other way
 *            round. Either way the left button becomes Square and the top
 *            one Triangle.
 *            Sony pads (hid-playstation: DualSense, DualShock 4; hid-sony:
 *            DualShock 3/4): BTN_SOUTH Cross, BTN_EAST Circle, BTN_NORTH
 *            Triangle, BTN_WEST Square, BTN_TL/TR L1/R1, BTN_TL2/TR2 +
 *            ABS_Z/ABS_RZ L2/R2, BTN_SELECT Share/Create, BTN_START
 *            Options, BTN_THUMBL/R L3/R3, BTN_MODE PS (not used), D-pad on
 *            ABS_HAT0X/Y (DualShock 3: BTN_DPAD_*), sticks ABS_X/Y, ABS_RX/RY.
 *   axes     ABS_X / ABS_Y left stick, ABS_HAT0X / ABS_HAT0Y D-pad. With
 *            ABS_RX / ABS_RY present those are the right stick and ABS_Z /
 *            ABS_RZ the triggers; without them ABS_Z / ABS_RZ are the right
 *            stick. ABS_BRAKE / ABS_GAS are triggers where they exist.
 *            L2 / R2 count as pressed from a quarter of the analog travel
 *            (pad.c); BTN_TL2 / BTN_TR2 are used only without analog triggers. */
#ifndef _WIN32
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include "rt.h"
#include "plat.h"

#define BIT(a, n) (((a)[(n) / 8] >> ((n) % 8)) & 1)

static int fd = -1, positional, ff_ok, ff_id = -1, warned, select_which;
static u8 has_abs[ABS_CNT / 8 + 1];
static struct input_absinfo ainfo[ABS_CNT];
static u64 next_scan;
static int is_sony;
static char dev_name[128], status[160] = "evdev";

/* Bit `bit` of a sysfs capability bitmap: hexadecimal words of 64 bits, most significant first. */
static int sysfs_bit(const char *path, int bit) {
    char buf[1024]; FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f);
    buf[n] = 0;
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == ' ')) buf[--n] = 0;
    int word = bit / 64;
    char *end = buf + n;
    for (;;) {                                       /* walk the words from the right */
        char *s = end;
        while (s > buf && s[-1] != ' ') s--;
        if (s == end) return 0;
        if (word-- == 0) return (int)((strtoull(s, 0, 16) >> (bit % 64)) & 1);
        if (s == buf) return 0;
        end = s - 1; *end = 0;
    }
}
static void close_dev(void) { if (fd >= 0) close(fd); fd = -1; ff_id = -1; ff_ok = 0; }

static int open_dev(int num) {
    char path[64], sys[96]; u8 keys[KEY_CNT / 8 + 1], ffb[FF_CNT / 8 + 1]; struct input_id id;
    snprintf(sys, sizeof sys, "/sys/class/input/event%d/device/capabilities/key", num);
    if (sysfs_bit(sys, BTN_SOUTH) == 0) return 0;    /* not a gamepad (unknown: open it and ask) */
    snprintf(path, sizeof path, "/dev/input/event%d", num);
    int rw = 1, f = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);          /* writing is only needed for vibration */
    if (f < 0) { rw = 0; f = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC); }
    if (f < 0) {
        if (errno == EACCES && sysfs_bit(sys, BTN_SOUTH) == 1 && !warned) {
            warned = 1;
            rt_log("[pad] a gamepad is present at %s but cannot be opened (permission denied): "
                   "the user needs read access, e.g. membership of the 'input' group\n", path);
        }
        return 0;
    }
    memset(keys, 0, sizeof keys); memset(has_abs, 0, sizeof has_abs); memset(ffb, 0, sizeof ffb); memset(&id, 0, sizeof id);
    if (ioctl(f, EVIOCGBIT(EV_KEY, sizeof keys), keys) < 0 || !BIT(keys, BTN_SOUTH)) { close(f); return 0; }
    ioctl(f, EVIOCGBIT(EV_ABS, sizeof has_abs), has_abs);
    for (int a = 0; a < ABS_CNT; a++) {
        memset(&ainfo[a], 0, sizeof ainfo[a]);
        if (BIT(has_abs, a) && ioctl(f, EVIOCGABS(a), &ainfo[a]) < 0) has_abs[a / 8] &= (u8)~(1 << (a % 8));
    }
    /* The same controller may show up as further devices (hid-playstation / hid-sony: "... Motion Sensors" with
     * the accelerometer property, "... Touchpad" as a pointer / button pad). Those report no BTN_SOUTH and stop
     * above; the properties are checked as well in case a driver ever adds buttons there. */
    u8 props[INPUT_PROP_CNT / 8 + 1]; memset(props, 0, sizeof props);
    if (ioctl(f, EVIOCGPROP(sizeof props), props) >= 0 &&
        (BIT(props, INPUT_PROP_ACCELEROMETER) || BIT(props, INPUT_PROP_POINTER) || BIT(props, INPUT_PROP_BUTTONPAD))) { close(f); return 0; }
    ioctl(f, EVIOCGID, &id);
    if ((select_which == 1 && id.vendor == 0x054c) || (select_which == 2 && id.vendor != 0x054c)) { close(f); return 0; }
    is_sony = id.vendor == 0x054c;
    positional = id.vendor == 0x054c || id.vendor == 0x057e;
    ff_ok = rw && ioctl(f, EVIOCGBIT(EV_FF, sizeof ffb), ffb) >= 0 && BIT(ffb, FF_RUMBLE);
    ff_id = -1;
    dev_name[0] = 0;
    if (ioctl(f, EVIOCGNAME(sizeof dev_name - 1), dev_name) < 0 || !dev_name[0]) snprintf(dev_name, sizeof dev_name, "gamepad");
    fd = f;
    snprintf(status, sizeof status, "evdev, %s, %04x:%04x, %s", path, id.vendor, id.product, ff_ok ? "vibration" : "no vibration");
    return 1;
}
static void scan(void) {
    DIR *d = opendir("/dev/input");
    if (!d) { snprintf(status, sizeof status, "evdev: /dev/input does not exist"); return; }
    snprintf(status, sizeof status, "evdev: no device in /dev/input reports gamepad buttons");
    u8 seen[32]; memset(seen, 0, sizeof seen);       /* event0..event255, tried in ascending order */
    struct dirent *e;
    while ((e = readdir(d))) {
        char *end; long n;
        if (strncmp(e->d_name, "event", 5) || e->d_name[5] < '0' || e->d_name[5] > '9') continue;
        n = strtol(e->d_name + 5, &end, 10);
        if (!*end && n >= 0 && n < 256) seen[n / 8] |= (u8)(1 << (n % 8));
    }
    closedir(d);
    for (int n = 0; n < 256 && fd < 0; n++) if (BIT(seen, n)) open_dev(n);
}

static int key(const u8 *keys, int code) { return BIT(keys, code); }
/* Current value of an axis scaled to lo..hi (0 if the device has no such axis). */
static int axis(int code, int lo, int hi) {
    struct input_absinfo a;
    if (!BIT(has_abs, code) || ioctl(fd, EVIOCGABS(code), &a) < 0) return lo < 0 ? 0 : lo;
    s64 min = ainfo[code].minimum, max = ainfo[code].maximum, v = a.value;
    if (max <= min) return lo < 0 ? 0 : lo;
    if (v < min) v = min; else if (v > max) v = max;
    return (int)(lo + (v - min) * (hi - lo) / (max - min));
}
static s16 flip(int v) { return (s16)(v <= -32767 ? 32767 : -v); }      /* evdev +y is down */

int hpad_poll(HostPad *st, char *name, size_t n) {
    u8 keys[KEY_CNT / 8 + 1];
    if (fd < 0) {
        u64 now = plat_time_ns();
        if (now < next_scan) return 0;
        next_scan = now + 1000000000ull;
        scan();
        if (fd < 0) return 0;
    }
    for (;;) {                                       /* drain the queue; the state is asked for below */
        struct input_event ev[32];
        ssize_t r = read(fd, ev, sizeof ev);
        if (r > 0) continue;
        if (r < 0 && (errno == EAGAIN || errno == EINTR)) break;
        close_dev(); return 0;                       /* ENODEV: unplugged */
    }
    memset(keys, 0, sizeof keys);
    if (ioctl(fd, EVIOCGKEY(sizeof keys), keys) < 0) { close_dev(); return 0; }
    u16 b = 0;
    if (key(keys, BTN_SOUTH)) b |= HPAD_A;
    if (key(keys, BTN_EAST)) b |= HPAD_B;
    if (key(keys, 0x133)) b |= positional ? HPAD_Y : HPAD_X;
    if (key(keys, 0x134)) b |= positional ? HPAD_X : HPAD_Y;
    if (key(keys, BTN_TL)) b |= HPAD_LB;
    if (key(keys, BTN_TR)) b |= HPAD_RB;
    if (key(keys, BTN_SELECT)) b |= HPAD_BACK;
    if (key(keys, BTN_START)) b |= HPAD_START;
    if (key(keys, BTN_THUMBL)) b |= HPAD_LS;
    if (key(keys, BTN_THUMBR)) b |= HPAD_RS;
    if (key(keys, BTN_DPAD_UP)) b |= HPAD_UP;
    if (key(keys, BTN_DPAD_DOWN)) b |= HPAD_DOWN;
    if (key(keys, BTN_DPAD_LEFT)) b |= HPAD_LEFT;
    if (key(keys, BTN_DPAD_RIGHT)) b |= HPAD_RIGHT;
    int hx = axis(ABS_HAT0X, -1, 1), hy = axis(ABS_HAT0Y, -1, 1);
    if (hx < 0) b |= HPAD_LEFT; else if (hx > 0) b |= HPAD_RIGHT;
    if (hy < 0) b |= HPAD_UP; else if (hy > 0) b |= HPAD_DOWN;
    int lt = 0, rt = 0, analog_l = BIT(has_abs, ABS_BRAKE), analog_r = BIT(has_abs, ABS_GAS);
    st->lx = (s16)axis(ABS_X, -32768, 32767); st->ly = flip(axis(ABS_Y, -32768, 32767));
    if (BIT(has_abs, ABS_RX) && BIT(has_abs, ABS_RY)) {
        st->rx = (s16)axis(ABS_RX, -32768, 32767); st->ry = flip(axis(ABS_RY, -32768, 32767));
        lt = axis(ABS_Z, 0, 255); rt = axis(ABS_RZ, 0, 255);
        analog_l |= BIT(has_abs, ABS_Z); analog_r |= BIT(has_abs, ABS_RZ);
    } else {
        st->rx = (s16)axis(ABS_Z, -32768, 32767); st->ry = flip(axis(ABS_RZ, -32768, 32767));
    }
    int v;
    if ((v = axis(ABS_BRAKE, 0, 255)) > lt) lt = v;
    if ((v = axis(ABS_GAS, 0, 255)) > rt) rt = v;
    if (!analog_l && key(keys, BTN_TL2)) lt = 255;   /* the digital bits only where there is no analog trigger */
    if (!analog_r && key(keys, BTN_TR2)) rt = 255;   /* (they switch at the first millimetre) */
    st->buttons = b; st->lt = (u8)lt; st->rt = (u8)rt;
    snprintf(name, n, "%s", dev_name);
    return 1;
}
const char *hpad_status(void) { return status; }
const char *hpad_kind(void) { return fd >= 0 && is_sony ? "playstation" : "xbox"; }
void hpad_select(int which) { select_which = which; }

/* FF_RUMBLE: strong = the low-frequency motor (PS2 big motor), weak = the high-frequency one. */
void hpad_rumble(int small, int big) {
    if (fd < 0 || !ff_ok) return;
    struct input_event ev; memset(&ev, 0, sizeof ev);
    ev.type = EV_FF;
    if (!small && !big) {
        if (ff_id >= 0) { ev.code = (u16)ff_id; ev.value = 0; if (write(fd, &ev, sizeof ev) < 0) ff_ok = 0; }
        return;
    }
    struct ff_effect e; memset(&e, 0, sizeof e);
    e.type = FF_RUMBLE; e.id = (s16)ff_id;           /* -1: new effect; afterwards the same one is updated */
    e.u.rumble.strong_magnitude = (u16)(big * 257); e.u.rumble.weak_magnitude = (u16)(small * 257);
    e.replay.length = 0;                             /* until stopped */
    if (ioctl(fd, EVIOCSFF, &e) < 0) { ff_ok = 0; return; }
    ff_id = e.id;
    ev.code = (u16)ff_id; ev.value = 1;
    if (write(fd, &ev, sizeof ev) < 0) ff_ok = 0;
}
#endif
