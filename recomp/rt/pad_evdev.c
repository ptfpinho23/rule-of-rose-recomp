/* pad_evdev.c — gamepads on Linux (plat.h hpad_*): the kernel's evdev
 * interface, /dev/input/event*, read directly (no libudev, no SDL).
 *
 * Which devices are gamepads
 *   Every event node is classified from its capability bitmaps, along the
 *   lines of udev's input_id builtin and SDL's SDL_EVDEV_GuessDeviceClass:
 *     - it must report BTN_SOUTH (= BTN_GAMEPAD = BTN_A), and
 *     - stick or D-pad axes (ABS_X + ABS_Y, or ABS_HAT0X + ABS_HAT0Y) or the
 *       four BTN_DPAD_* buttons, and
 *     - none of: ordinary keyboard keys, relative (mouse) axes, mouse or
 *       touch / pen buttons, the accelerometer / pointer / button-pad
 *       properties (the motion-sensor and touchpad nodes of a controller).
 *   Virtual devices made by key remappers and the like advertise the whole
 *   key range, BTN_SOUTH included, and usually have low event numbers; they
 *   fail the second or third test.
 *   The bitmaps are read from /sys/class/input/eventN/device/ first (world
 *   readable), so a gamepad whose node cannot be opened can be named in the
 *   log with the reason; without sysfs the node is opened and asked.
 *
 * All gamepads are used
 *   Every node that classifies as a gamepad is opened (up to MAX_PADS) and
 *   the states are merged: buttons OR-ed, each trigger the larger value,
 *   each stick from the device that deflects it most. A node that never
 *   reports anything therefore cannot hide another one, and a controller
 *   that is visible twice (the device itself and a virtual pad made from it
 *   by Steam Input) gives the same result as once. /dev/input is looked
 *   through once per second, also while pads are open (hot-plug); a node is
 *   classified again only when it is new or its inode / change time moved
 *   (udev applying the access rights). Each device is dropped on its own
 *   when reading it fails.
 *   State is read with EVIOCGKEY / EVIOCGABS once per frame and the event
 *   queue is only drained: the kernel answers those from the device's own
 *   state (evdev_handle_get_val), so a device that another program has
 *   grabbed with EVIOCGRAB still reports here.
 *
 * Access rights
 *   udev's default rules give event nodes to root:input, mode 0660; systemd's
 *   70-uaccess.rules tags joysticks ("ID_INPUT_JOYSTICK") so that logind adds
 *   an ACL for the user of the active local session. Without such a session
 *   (no logind, a container, ssh) the open fails with EACCES unless the user
 *   is in the input group; that is logged with the remedy.
 *
 * Log (all lines start with "[pad] evdev:"): one line per node that has
 * joystick-range buttons, whenever its verdict changes — used, skipped and
 * why, cannot be opened and why, disconnected. ROR_PAD_DEBUG=1 adds every
 * node, the capability summary of opened ones and every change of state.
 *
 * Layout, per Documentation/input/gamepad.rst and what the drivers do:
 *   buttons  BTN_SOUTH A, BTN_EAST B, BTN_TL / BTN_TR bumpers, BTN_SELECT,
 *            BTN_START, BTN_THUMBL / BTN_THUMBR, BTN_TL2 / BTN_TR2 (digital
 *            triggers), BTN_DPAD_*. Codes 0x133 / 0x134: xpad, xone, xpadneo
 *            (and the pads that imitate them) report X on 0x133 and Y on
 *            0x134; Sony (vendor 054c) and Nintendo (057e) drivers use them
 *            by position (0x133 = top button, 0x134 = left button), i.e. the
 *            other way round. Either way the left button becomes Square and
 *            the top one Triangle.
 *            Sony pads (hid-playstation: DualSense, DualShock 4; hid-sony:
 *            DualShock 3/4): BTN_SOUTH Cross, BTN_EAST Circle, BTN_NORTH
 *            Triangle, BTN_WEST Square, BTN_TL/TR L1/R1, BTN_TL2/TR2 +
 *            ABS_Z/ABS_RZ L2/R2, BTN_SELECT Share/Create, BTN_START
 *            Options, BTN_THUMBL/R L3/R3, BTN_MODE PS (not used), D-pad on
 *            ABS_HAT0X/Y (DualShock 3: BTN_DPAD_*), sticks ABS_X/Y, ABS_RX/RY.
 *            A pad on the generic HID driver that numbers its buttons
 *            1..10 with nothing in between gets the codes 0x130..0x139 and
 *            no BTN_SELECT / BTN_START; those are taken in the usual order
 *            A B X Y LB RB Back Start LS RS.
 *            BTN_TRIGGER_HAPPY1..4 are the D-pad (left right up down) where
 *            a device has neither a hat nor BTN_DPAD_* (xpad of old kernels).
 *   axes     ABS_X / ABS_Y left stick, ABS_HAT0X / ABS_HAT0Y D-pad. With
 *            ABS_RX / ABS_RY present those are the right stick and ABS_Z /
 *            ABS_RZ the triggers (xpad 0..255, Xbox One / xone / xpadneo
 *            0..1023; scaled from the range the device declares); without
 *            them ABS_Z / ABS_RZ are the right stick (generic HID layout of
 *            Bluetooth pads). ABS_BRAKE / ABS_GAS are triggers where they
 *            exist. L2 / R2 count as pressed from a quarter of the analog
 *            travel (pad.c); BTN_TL2 / BTN_TR2 are used only without analog
 *            triggers. */
#ifndef _WIN32
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/input.h>
#include "rt.h"
#include "plat.h"

#define BIT(a, n) (((a)[(n) / 8] >> ((n) % 8)) & 1)
#define MAX_PADS  8
#define MAX_NODES 256                                /* event0 .. event255 */

/* What a node can do, from sysfs or from the open device. */
typedef struct {
    u8 ev[EV_CNT / 8 + 1], key[KEY_CNT / 8 + 1], abs[ABS_CNT / 8 + 1], rel[REL_CNT / 8 + 1], prop[INPUT_PROP_CNT / 8 + 1];
    char name[96];
    unsigned bus, vendor, product;
} Caps;

typedef struct {
    int fd, num, rw;
    Caps caps;
    struct input_absinfo ai[ABS_CNT];
    int positional, sequential, happy_dpad, sony, ff_ok, ff_id;
    HostPad last; int have_last;
    u8 last_keys[KEY_CNT / 8 + 1];
} Dev;

enum { V_NEW = 0, V_USED, V_NOT_PAD, V_SKIPPED, V_FILTERED, V_DENIED, V_OPEN_FAILED, V_TOO_MANY };
static struct { u8 present, verdict, open_errno; u64 stamp; } nodes[MAX_NODES];
static Dev devs[MAX_PADS];
static int ndev, select_which, debug = -1, n_nodes, n_denied;
static u64 next_scan;
static char status[200] = "evdev";

/* ---- capabilities -------------------------------------------------------- */
/* A sysfs capability bitmap: hexadecimal words of 64 bits (an unsigned long of the kernel), most significant
 * first, leading zero words left out (input_print_bitmap in drivers/input/input.c). Returns 0 if unreadable. */
static int sysfs_bits(int num, const char *file, u8 *out, size_t out_n) {
    char path[96], buf[1024]; snprintf(path, sizeof path, "/sys/class/input/event%d/device/%s", num, file);
    FILE *f = fopen(path, "r");
    memset(out, 0, out_n);
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f);
    buf[n] = 0;
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == ' ')) buf[--n] = 0;
    char *end = buf + n;
    for (size_t word = 0; end > buf; word++) {       /* walk the words from the right */
        char *s = end;
        while (s > buf && s[-1] != ' ') s--;
        u64 v = strtoull(s, 0, 16);
        for (size_t b = 0; b < 8; b++) if (word * 8 + b < out_n) out[word * 8 + b] = (u8)(v >> (8 * b));
        if (s == buf) break;
        end = s - 1; *end = 0;
    }
    return 1;
}
static int sysfs_text(int num, const char *file, char *out, size_t out_n) {
    char path[96]; snprintf(path, sizeof path, "/sys/class/input/event%d/device/%s", num, file);
    FILE *f = fopen(path, "r");
    out[0] = 0;
    if (!f) return 0;
    size_t n = fread(out, 1, out_n - 1, f); fclose(f);
    out[n] = 0;
    while (n && (out[n - 1] == '\n' || out[n - 1] == ' ')) out[--n] = 0;
    return 1;
}
static int caps_from_sysfs(int num, Caps *c) {
    char t[16];
    memset(c, 0, sizeof *c);
    if (!sysfs_bits(num, "capabilities/key", c->key, sizeof c->key)) return 0;
    sysfs_bits(num, "capabilities/ev", c->ev, sizeof c->ev);
    sysfs_bits(num, "capabilities/abs", c->abs, sizeof c->abs);
    sysfs_bits(num, "capabilities/rel", c->rel, sizeof c->rel);
    sysfs_bits(num, "properties", c->prop, sizeof c->prop);
    sysfs_text(num, "name", c->name, sizeof c->name);
    if (sysfs_text(num, "id/bustype", t, sizeof t)) c->bus = (unsigned)strtoul(t, 0, 16);
    if (sysfs_text(num, "id/vendor", t, sizeof t)) c->vendor = (unsigned)strtoul(t, 0, 16);
    if (sysfs_text(num, "id/product", t, sizeof t)) c->product = (unsigned)strtoul(t, 0, 16);
    return 1;
}
static int caps_from_fd(int fd, Caps *c) {
    struct input_id id;
    memset(c, 0, sizeof *c); memset(&id, 0, sizeof id);
    if (ioctl(fd, EVIOCGBIT(0, sizeof c->ev), c->ev) < 0) return 0;
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof c->key), c->key);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof c->abs), c->abs);
    ioctl(fd, EVIOCGBIT(EV_REL, sizeof c->rel), c->rel);
    ioctl(fd, EVIOCGPROP(sizeof c->prop), c->prop);
    if (ioctl(fd, EVIOCGNAME(sizeof c->name - 1), c->name) < 0) c->name[0] = 0;
    if (ioctl(fd, EVIOCGID, &id) >= 0) { c->bus = id.bustype; c->vendor = id.vendor; c->product = id.product; }
    return 1;
}
/* Buttons of the joystick / gamepad ranges: only nodes with one of these are worth a log line. */
static int joystick_buttons(const Caps *c) {
    int n = 0;
    for (int k = BTN_JOYSTICK; k < BTN_DIGI; k++) n += BIT(c->key, k);
    for (int k = BTN_DPAD_UP; k <= BTN_DPAD_RIGHT; k++) n += BIT(c->key, k);
    return n;
}
/* 0 if the node is a gamepad this file can read, otherwise the reason it is not. */
static const char *why_not_gamepad(const Caps *c) {
    static const u16 kbd[] = { KEY_LEFTCTRL, KEY_CAPSLOCK, KEY_NUMLOCK, KEY_INSERT, KEY_MUTE, KEY_CALC, KEY_FILE,
                               KEY_MAIL, KEY_PLAYPAUSE, KEY_BRIGHTNESSDOWN };     /* udev's "well known keyboard keys" */
    int nk = 0;
    if (BIT(c->prop, INPUT_PROP_ACCELEROMETER)) return "motion sensor of a controller";
    if (BIT(c->prop, INPUT_PROP_POINTER) || BIT(c->prop, INPUT_PROP_BUTTONPAD) || BIT(c->prop, INPUT_PROP_TOPBUTTONPAD) ||
        BIT(c->prop, INPUT_PROP_SEMI_MT) || BIT(c->prop, INPUT_PROP_POINTING_STICK)) return "touchpad or pointer";
    if (!BIT(c->key, BTN_SOUTH)) return joystick_buttons(c) ? "joystick buttons but no gamepad layout (no BTN_SOUTH)" : "no gamepad buttons";
    for (unsigned i = 0; i < sizeof kbd / sizeof *kbd; i++) nk += BIT(c->key, kbd[i]);
    if (nk >= 4 || (BIT(c->key, KEY_A) && BIT(c->key, KEY_SPACE) && BIT(c->key, KEY_ENTER)))
        return "has keyboard keys too (a keyboard or a virtual device)";
    if (BIT(c->rel, REL_X) && BIT(c->rel, REL_Y)) return "has mouse axes too";
    if (BIT(c->key, BTN_LEFT) || BIT(c->key, BTN_TOUCH) || BIT(c->key, BTN_TOOL_FINGER) || BIT(c->key, BTN_TOOL_PEN) || BIT(c->key, BTN_STYLUS))
        return "has mouse or touch buttons too";
    if (!(BIT(c->abs, ABS_X) && BIT(c->abs, ABS_Y)) && !(BIT(c->abs, ABS_HAT0X) && BIT(c->abs, ABS_HAT0Y)) &&
        !(BIT(c->key, BTN_DPAD_UP) && BIT(c->key, BTN_DPAD_DOWN) && BIT(c->key, BTN_DPAD_LEFT) && BIT(c->key, BTN_DPAD_RIGHT)))
        return "gamepad buttons but no stick or D-pad axes";
    return 0;
}

/* ---- log ----------------------------------------------------------------- */
static void say(int num, const Caps *c, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void say(int num, const Caps *c, const char *fmt, ...) {
    char t[400]; va_list ap; va_start(ap, fmt); vsnprintf(t, sizeof t, fmt, ap); va_end(ap);
    if (c) rt_log("[pad] evdev: /dev/input/event%d \"%s\" %04x:%04x: %s\n", num, c->name[0] ? c->name : "?", c->vendor, c->product, t);
    else rt_log("[pad] evdev: /dev/input/event%d: %s\n", num, t);
}
static void dump_caps(const Dev *d) {
    char t[900]; size_t n = 0;
    n += (size_t)snprintf(t + n, sizeof t - n, "bus %04x, keys", d->caps.bus);
    for (int k = 0x100; k < KEY_CNT && n < sizeof t - 8; k++) if (BIT(d->caps.key, k)) n += (size_t)snprintf(t + n, sizeof t - n, " %x", k);
    n += (size_t)snprintf(t + n, sizeof t - n, "; axes");
    for (int a = 0; a < ABS_CNT && n < sizeof t - 40; a++)
        if (BIT(d->caps.abs, a)) n += (size_t)snprintf(t + n, sizeof t - n, " %x[%d..%d]", a, d->ai[a].minimum, d->ai[a].maximum);
    say(d->num, &d->caps, "%s", t);
}

/* ---- devices ------------------------------------------------------------- */
static void drop_dev(int i, const char *why) {
    Dev *d = &devs[i];
    say(d->num, &d->caps, "%s", why);
    close(d->fd);
    nodes[d->num].verdict = V_NEW; nodes[d->num].stamp = 0;
    memmove(d, d + 1, (size_t)(ndev - i - 1) * sizeof *d);
    ndev--;
}
/* Look at node `num` and open it if it is a gamepad. Returns the verdict. */
static int consider(int num) {
    char path[64]; Caps c; int known = caps_from_sysfs(num, &c), f = -1, rw = 1;
    const char *why = known ? why_not_gamepad(&c) : 0;
    snprintf(path, sizeof path, "/dev/input/event%d", num);
    if (why) {                                       /* sysfs already says no: do not touch the node */
        if (debug || joystick_buttons(&c)) say(num, &c, "skipped, %s", why);
        return joystick_buttons(&c) ? V_SKIPPED : V_NOT_PAD;
    }
    f = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);                     /* writing is only needed for vibration */
    if (f < 0) { rw = 0; f = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC); }
    if (f < 0) {
        int e = errno;
        nodes[num].open_errno = (u8)e;
        if (!known) {                                /* nothing is known about it; keyboards and mice end here for a normal user */
            if (debug) say(num, 0, "cannot be opened (%s) and has no readable sysfs entry", strerror(e));
            return V_NOT_PAD;
        }
        if (e == EACCES || e == EPERM)
            say(num, &c, "is a gamepad but cannot be opened: permission denied for %s. Fix: log out and in again, or "
                "'sudo usermod -aG input $USER' and log in again, or a udev rule with TAG+=\"uaccess\" for this device", path);
        else say(num, &c, "is a gamepad but cannot be opened: %s", strerror(e));
        return e == EACCES || e == EPERM ? V_DENIED : V_OPEN_FAILED;
    }
    if (!caps_from_fd(f, &c)) { close(f); return V_NOT_PAD; }            /* the device's own answer counts */
    if ((why = why_not_gamepad(&c)) != 0) {
        if (debug || joystick_buttons(&c)) say(num, &c, "skipped, %s", why);
        close(f);
        return joystick_buttons(&c) ? V_SKIPPED : V_NOT_PAD;
    }
    if ((select_which == 1 && c.vendor == 0x054c) || (select_which == 2 && c.vendor != 0x054c)) {
        say(num, &c, "skipped by --pad %s", select_which == 1 ? "xinput (a Sony device)" : "playstation (not a Sony device)");
        close(f); return V_FILTERED;
    }
    if (ndev >= MAX_PADS) { say(num, &c, "skipped, %d gamepads are in use already", MAX_PADS); close(f); return V_TOO_MANY; }
    int at = ndev;                                   /* keep the list in event-number order */
    while (at > 0 && devs[at - 1].num > num) { devs[at] = devs[at - 1]; at--; }
    Dev *d = &devs[at]; ndev++;
    memset(d, 0, sizeof *d);
    d->fd = f; d->num = num; d->rw = rw; d->caps = c; d->ff_id = -1;
    for (int a = 0; a < ABS_CNT; a++)
        if (BIT(d->caps.abs, a) && ioctl(f, EVIOCGABS(a), &d->ai[a]) < 0) d->caps.abs[a / 8] &= (u8)~(1 << (a % 8));
    d->sony = c.vendor == 0x054c;
    d->positional = c.vendor == 0x054c || c.vendor == 0x057e;
    d->sequential = !BIT(c.key, BTN_SELECT) && !BIT(c.key, BTN_START) && !BIT(c.key, BTN_THUMBL) && !BIT(c.key, BTN_MODE) &&
                    BIT(c.key, BTN_C) && BIT(c.key, BTN_Z) && BIT(c.key, BTN_TL2) && BIT(c.key, BTN_TR2);
    d->happy_dpad = !BIT(c.abs, ABS_HAT0X) && !BIT(c.key, BTN_DPAD_UP) && BIT(c.key, BTN_TRIGGER_HAPPY1) && BIT(c.key, BTN_TRIGGER_HAPPY4);
    u8 ffb[FF_CNT / 8 + 1]; memset(ffb, 0, sizeof ffb);
    d->ff_ok = rw && ioctl(f, EVIOCGBIT(EV_FF, sizeof ffb), ffb) >= 0 && BIT(ffb, FF_RUMBLE);
    say(num, &d->caps, "used as gamepad (%s layout%s, %s)", d->sony ? "playstation" : "xbox", d->sequential ? ", buttons in sequence" : "",
        d->ff_ok ? "vibration" : rw ? "no vibration" : "opened read-only: no vibration");
    if (debug) dump_caps(d);
    return V_USED;
}
static void scan(void) {
    u8 seen[MAX_NODES / 8]; struct dirent *e; DIR *dir = opendir("/dev/input");
    memset(seen, 0, sizeof seen);
    n_nodes = n_denied = 0;
    if (!dir) { snprintf(status, sizeof status, "evdev: /dev/input does not exist"); return; }
    while ((e = readdir(dir))) {
        char *end; long n;
        if (strncmp(e->d_name, "event", 5) || e->d_name[5] < '0' || e->d_name[5] > '9') continue;
        n = strtol(e->d_name + 5, &end, 10);
        if (!*end && n >= 0 && n < MAX_NODES) seen[n / 8] |= (u8)(1 << (n % 8));
    }
    closedir(dir);
    for (int n = 0; n < MAX_NODES; n++) {            /* ascending, so the log and the device list are in a fixed order */
        if (!BIT(seen, n)) { nodes[n].present = 0; nodes[n].verdict = V_NEW; nodes[n].stamp = 0; continue; }
        n_nodes++;
        if (nodes[n].verdict != V_USED) {
            char path[64]; struct stat st; u64 stamp = 1;
            snprintf(path, sizeof path, "/dev/input/event%d", n);
            if (stat(path, &st) == 0) stamp = (u64)st.st_ino * 1000003u + (u64)st.st_ctim.tv_sec * 1000000007u + (u64)st.st_ctim.tv_nsec + 1;
            /* again only if the node is new or was changed (udev sets the access rights a moment after creating it),
             * or a place among the open devices has become free */
            if (!nodes[n].present || nodes[n].stamp != stamp || (nodes[n].verdict == V_TOO_MANY && ndev < MAX_PADS)) {
                nodes[n].stamp = stamp;
                nodes[n].verdict = (u8)consider(n);
            }
        }
        nodes[n].present = 1;
        if (nodes[n].verdict == V_DENIED) n_denied++;
    }
    if (ndev) snprintf(status, sizeof status, "evdev, %d device%s, /dev/input/event%d %04x:%04x", ndev, ndev > 1 ? "s" : "", devs[0].num, devs[0].caps.vendor, devs[0].caps.product);
    else if (n_denied) snprintf(status, sizeof status, "evdev: %d gamepad%s present but not accessible, see the lines above", n_denied, n_denied > 1 ? "s" : "");
    else snprintf(status, sizeof status, "evdev: %d input devices in /dev/input, no gamepad among them", n_nodes);
}

/* Current value of a trigger or hat axis scaled to lo..hi (0 if the device has no such axis). */
static int axis(Dev *d, int code, int lo, int hi, int *gone) {
    struct input_absinfo a;
    if (!BIT(d->caps.abs, code)) return lo < 0 ? 0 : lo;
    if (ioctl(d->fd, EVIOCGABS(code), &a) < 0) { *gone = 1; return lo < 0 ? 0 : lo; }
    s64 min = d->ai[code].minimum, max = d->ai[code].maximum, v = a.value;
    if (max <= min) return lo < 0 ? 0 : lo;
    if (v < min) v = min; else if (v > max) v = max;
    return (int)(lo + (v - min) * (hi - lo) / (max - min));
}
/* A stick axis to -32768..32767 around the middle of its declared range (0..255 rests at 128, 0..65535 at 32768). */
static int stick(Dev *d, int code, int *gone) {
    struct input_absinfo a;
    if (!BIT(d->caps.abs, code)) return 0;
    if (ioctl(d->fd, EVIOCGABS(code), &a) < 0) { *gone = 1; return 0; }
    s64 min = d->ai[code].minimum, max = d->ai[code].maximum, v = a.value;
    if (max <= min) return 0;
    s64 mid = (min + max + 1) >> 1;
    if (v < min) v = min; else if (v > max) v = max;
    return (int)(v >= mid ? (max > mid ? (v - mid) * 32767 / (max - mid) : 0) : (v - mid) * 32768 / (mid - min));
}
static s16 flip(int v) { return (s16)(v <= -32767 ? 32767 : -v); }      /* evdev +y is down */

/* Read one device. Returns 0 if it has gone away. */
static int read_dev(Dev *d, HostPad *st) {
    u8 keys[KEY_CNT / 8 + 1]; int gone = 0;
    for (;;) {                                       /* drain the queue; the state is asked for below */
        struct input_event ev[32];
        ssize_t r = read(d->fd, ev, sizeof ev);
        if (r > 0) continue;
        if (r < 0 && (errno == EAGAIN || errno == EINTR)) break;
        return 0;                                    /* ENODEV: unplugged */
    }
    memset(keys, 0, sizeof keys);
    if (ioctl(d->fd, EVIOCGKEY(sizeof keys), keys) < 0) return 0;
    u16 b = 0;
    if (d->sequential) {
        static const u16 seq[10] = { HPAD_A, HPAD_B, HPAD_X, HPAD_Y, HPAD_LB, HPAD_RB, HPAD_BACK, HPAD_START, HPAD_LS, HPAD_RS };
        for (int i = 0; i < 10; i++) if (BIT(keys, BTN_SOUTH + i)) b |= seq[i];
    } else {
        if (BIT(keys, BTN_SOUTH)) b |= HPAD_A;
        if (BIT(keys, BTN_EAST)) b |= HPAD_B;
        if (BIT(keys, 0x133)) b |= d->positional ? HPAD_Y : HPAD_X;
        if (BIT(keys, 0x134)) b |= d->positional ? HPAD_X : HPAD_Y;
        if (BIT(keys, BTN_TL)) b |= HPAD_LB;
        if (BIT(keys, BTN_TR)) b |= HPAD_RB;
        if (BIT(keys, BTN_SELECT)) b |= HPAD_BACK;
        if (BIT(keys, BTN_START)) b |= HPAD_START;
        if (BIT(keys, BTN_THUMBL)) b |= HPAD_LS;
        if (BIT(keys, BTN_THUMBR)) b |= HPAD_RS;
    }
    if (BIT(keys, BTN_DPAD_UP)) b |= HPAD_UP;
    if (BIT(keys, BTN_DPAD_DOWN)) b |= HPAD_DOWN;
    if (BIT(keys, BTN_DPAD_LEFT)) b |= HPAD_LEFT;
    if (BIT(keys, BTN_DPAD_RIGHT)) b |= HPAD_RIGHT;
    if (d->happy_dpad) {
        if (BIT(keys, BTN_TRIGGER_HAPPY1)) b |= HPAD_LEFT;
        if (BIT(keys, BTN_TRIGGER_HAPPY2)) b |= HPAD_RIGHT;
        if (BIT(keys, BTN_TRIGGER_HAPPY3)) b |= HPAD_UP;
        if (BIT(keys, BTN_TRIGGER_HAPPY4)) b |= HPAD_DOWN;
    }
    int hx = axis(d, ABS_HAT0X, -1, 1, &gone), hy = axis(d, ABS_HAT0Y, -1, 1, &gone);
    if (hx < 0) b |= HPAD_LEFT; else if (hx > 0) b |= HPAD_RIGHT;
    if (hy < 0) b |= HPAD_UP; else if (hy > 0) b |= HPAD_DOWN;
    int lt = 0, rt = 0, analog_l = BIT(d->caps.abs, ABS_BRAKE), analog_r = BIT(d->caps.abs, ABS_GAS), v;
    st->lx = (s16)stick(d, ABS_X, &gone); st->ly = flip(stick(d, ABS_Y, &gone));
    if (BIT(d->caps.abs, ABS_RX) && BIT(d->caps.abs, ABS_RY)) {
        st->rx = (s16)stick(d, ABS_RX, &gone); st->ry = flip(stick(d, ABS_RY, &gone));
        lt = axis(d, ABS_Z, 0, 255, &gone); rt = axis(d, ABS_RZ, 0, 255, &gone);
        analog_l |= BIT(d->caps.abs, ABS_Z); analog_r |= BIT(d->caps.abs, ABS_RZ);
    } else {
        st->rx = (s16)stick(d, ABS_Z, &gone); st->ry = flip(stick(d, ABS_RZ, &gone));
    }
    if ((v = axis(d, ABS_BRAKE, 0, 255, &gone)) > lt) lt = v;
    if ((v = axis(d, ABS_GAS, 0, 255, &gone)) > rt) rt = v;
    if (!d->sequential) {
        if (!analog_l && BIT(keys, BTN_TL2)) lt = 255;   /* the digital bits only where there is no analog trigger */
        if (!analog_r && BIT(keys, BTN_TR2)) rt = 255;   /* (they switch at the first millimetre) */
    }
    st->buttons = b; st->lt = (u8)lt; st->rt = (u8)rt;
    if (gone) return 0;
    if (debug && (!d->have_last || memcmp(st, &d->last, sizeof *st) || memcmp(keys, d->last_keys, sizeof keys))) {
        char t[300]; size_t n = 0; t[0] = 0;
        for (int k = 0x100; k < KEY_CNT && n < sizeof t - 8; k++) if (BIT(keys, k)) n += (size_t)snprintf(t + n, sizeof t - n, " %x", k);
        char r[300]; size_t m = 0; r[0] = 0;
        for (int a = 0; a < ABS_CNT && m < sizeof r - 24; a++) {
            struct input_absinfo ai;
            if (BIT(d->caps.abs, a) && ioctl(d->fd, EVIOCGABS(a), &ai) >= 0) m += (size_t)snprintf(r + m, sizeof r - m, " %x=%d", a, ai.value);
        }
        rt_log("[pad] evdev: event%d state: buttons %04x lt %d rt %d lx %d ly %d rx %d ry %d; keys down:%s; axes:%s\n", d->num, st->buttons, st->lt, st->rt,
               st->lx, st->ly, st->rx, st->ry, t[0] ? t : " none", r);
    }
    d->last = *st; d->have_last = 1; memcpy(d->last_keys, keys, sizeof keys);
    return 1;
}
static long reach(int x, int y) { return (long)x * x + (long)y * y; }

int hpad_poll(HostPad *st, char *name, size_t n) {
    u64 now = plat_time_ns();
    if (debug < 0) { const char *e = plat_getenv("ROR_PAD_DEBUG"); debug = e && *e && strcmp(e, "0"); }
    if (now >= next_scan) { next_scan = now + 1000000000ull; scan(); }
    memset(st, 0, sizeof *st);
    for (int i = 0; i < ndev; ) {
        HostPad h; memset(&h, 0, sizeof h);
        if (!read_dev(&devs[i], &h)) { drop_dev(i, "disconnected"); next_scan = 0; continue; }
        st->buttons |= h.buttons;
        if (h.lt > st->lt) st->lt = h.lt;
        if (h.rt > st->rt) st->rt = h.rt;
        if (reach(h.lx, h.ly) > reach(st->lx, st->ly)) { st->lx = h.lx; st->ly = h.ly; }
        if (reach(h.rx, h.ry) > reach(st->rx, st->ry)) { st->rx = h.rx; st->ry = h.ry; }
        i++;
    }
    if (!ndev) return 0;
    if (ndev == 1) snprintf(name, n, "%s", devs[0].caps.name[0] ? devs[0].caps.name : "gamepad");
    else snprintf(name, n, "%s and %d more", devs[0].caps.name[0] ? devs[0].caps.name : "gamepad", ndev - 1);
    return 1;
}
const char *hpad_status(void) { return status; }
const char *hpad_kind(void) { return ndev && devs[0].sony ? "playstation" : "xbox"; }
void hpad_select(int which) { select_which = which; }

/* FF_RUMBLE on every open pad that has it: strong = the low-frequency motor (PS2 big motor), weak = the high-frequency one. */
void hpad_rumble(int small, int big) {
    for (int i = 0; i < ndev; i++) {
        Dev *d = &devs[i];
        struct input_event ev; memset(&ev, 0, sizeof ev);
        if (!d->ff_ok) continue;
        ev.type = EV_FF;
        if (!small && !big) {
            if (d->ff_id >= 0) { ev.code = (u16)d->ff_id; ev.value = 0; if (write(d->fd, &ev, sizeof ev) < 0) d->ff_ok = 0; }
            continue;
        }
        struct ff_effect e; memset(&e, 0, sizeof e);
        e.type = FF_RUMBLE; e.id = (s16)d->ff_id;    /* -1: new effect; afterwards the same one is updated */
        e.u.rumble.strong_magnitude = (u16)(big * 257); e.u.rumble.weak_magnitude = (u16)(small * 257);
        e.replay.length = 0;                         /* until stopped */
        if (ioctl(d->fd, EVIOCSFF, &e) < 0) { d->ff_ok = 0; continue; }
        d->ff_id = e.id;
        ev.code = (u16)d->ff_id; ev.value = 1;
        if (write(d->fd, &ev, sizeof ev) < 0) d->ff_ok = 0;
    }
}
#endif
