/* padsim.c — test of recomp/rt/pad_evdev.c (and pad.c) against simulated
 * input devices; built and run by padsim.sh.
 *
 * pad_evdev.c is compiled unchanged. The linker's --wrap redirects its
 * calls to opendir / readdir / closedir / stat / fopen / open / close / read
 * / write / ioctl to the functions below, which serve /dev/input/eventN and
 * /sys/class/input/eventN/device/... from a table of devices and pass
 * everything else on. The capability sets, IDs, properties and axis ranges
 * in the table are the ones the kernel drivers register (drivers/input/
 * joystick/xpad.c, drivers/hid/hid-playstation.c, the generic HID mapping in
 * drivers/hid/hid-input.c, the out-of-tree xone driver); the sysfs text is
 * formatted the way drivers/input/input.c prints it.
 *
 *   padsim N        run scenario N (one per process: the code under test keeps its state in statics)
 *   padsim          print the number of scenarios
 * Exit status: 0 if every check of the scenario passed. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/input.h>
#include "rt.h"
#include "plat.h"

void pad_update(u64 frame, u16 kbd, int live);       /* rt/pad.c */

/* ---- simulated devices --------------------------------------------------- */
typedef struct { int code, min, max; } AbsDef;
typedef struct {
    int num; const char *name; unsigned bus, vendor, product;
    const int *keys;                                 /* -1 ends; -2 = every code 1..0x2ff */
    const AbsDef *abs;                               /* code -1 ends */
    const int *rel, *props;                          /* -1 ends */
    int ff;                                          /* FF_RUMBLE */
    int deny;                                        /* 0 openable, 1 read-only, 2 EACCES */
    int no_sysfs, present, gone;
    u64 ino, ctime;
    u8 down[KEY_CNT / 8 + 1]; int val[ABS_CNT];
    int ff_strong, ff_weak, ff_playing, opens;
} Sim;
#define NSIM 12
static Sim sim[NSIM]; static int nsim;
static u64 clock_ns = 5000000000ull, ino_next = 100;

static const int K_END = -1;
static const int keys_all[] = { -2, -1 };
static const int keys_mouse[] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA, -1 };
static const int rel_mouse[] = { REL_X, REL_Y, REL_WHEEL, -1 };
/* xpad.c: xpad_common_btn + xpad360_btn */
static const int keys_xpad[] = { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_START, BTN_SELECT, BTN_THUMBL, BTN_THUMBR, BTN_TL, BTN_TR, BTN_MODE, -1 };
static const AbsDef abs_xpad[] = { { ABS_X, -32768, 32767 }, { ABS_Y, -32768, 32767 }, { ABS_RX, -32768, 32767 }, { ABS_RY, -32768, 32767 },
                                   { ABS_Z, 0, 255 }, { ABS_RZ, 0, 255 }, { ABS_HAT0X, -1, 1 }, { ABS_HAT0Y, -1, 1 }, { -1, 0, 0 } };
/* xone gamepad.c / xpad for Xbox One: triggers 0..1023 */
static const int keys_xone[] = { BTN_MODE, BTN_START, BTN_SELECT, BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR, BTN_THUMBL, BTN_THUMBR, KEY_RECORD, -1 };
static const AbsDef abs_xone[] = { { ABS_X, -32768, 32767 }, { ABS_Y, -32768, 32767 }, { ABS_RX, -32768, 32767 }, { ABS_RY, -32768, 32767 },
                                   { ABS_Z, 0, 1023 }, { ABS_RZ, 0, 1023 }, { ABS_HAT0X, -1, 1 }, { ABS_HAT0Y, -1, 1 }, { -1, 0, 0 } };
/* Xbox pad over Bluetooth on the generic HID mapping: 15 buttons from BTN_GAMEPAD up, right stick on Z / RZ, triggers on BRAKE / GAS */
static const int keys_xbt[] = { 0x130, 0x131, 0x132, 0x133, 0x134, 0x135, 0x136, 0x137, 0x138, 0x139, 0x13a, 0x13b, 0x13c, 0x13d, 0x13e, KEY_RECORD, -1 };
static const AbsDef abs_xbt[] = { { ABS_X, 0, 65535 }, { ABS_Y, 0, 65535 }, { ABS_Z, 0, 65535 }, { ABS_RZ, 0, 65535 },
                                  { ABS_GAS, 0, 1023 }, { ABS_BRAKE, 0, 1023 }, { ABS_HAT0X, -1, 1 }, { ABS_HAT0Y, -1, 1 }, { -1, 0, 0 } };
/* a HID pad with ten buttons numbered 1..10 */
static const int keys_seq[] = { 0x130, 0x131, 0x132, 0x133, 0x134, 0x135, 0x136, 0x137, 0x138, 0x139, -1 };
/* hid-playstation.c: ps_gamepad_buttons, ps_gamepad_create, ps_sensors_create, ps_touchpad_create */
static const int keys_ps[] = { BTN_WEST, BTN_NORTH, BTN_EAST, BTN_SOUTH, BTN_TL, BTN_TR, BTN_TL2, BTN_TR2, BTN_SELECT, BTN_START, BTN_THUMBL, BTN_THUMBR, BTN_MODE, -1 };
static const AbsDef abs_ps[] = { { ABS_X, 0, 255 }, { ABS_Y, 0, 255 }, { ABS_RX, 0, 255 }, { ABS_RY, 0, 255 }, { ABS_Z, 0, 255 }, { ABS_RZ, 0, 255 },
                                 { ABS_HAT0X, -1, 1 }, { ABS_HAT0Y, -1, 1 }, { -1, 0, 0 } };
static const AbsDef abs_ps_motion[] = { { ABS_X, -32768, 32767 }, { ABS_Y, -32768, 32767 }, { ABS_Z, -32768, 32767 },
                                        { ABS_RX, -2097152, 2097152 }, { ABS_RY, -2097152, 2097152 }, { ABS_RZ, -2097152, 2097152 }, { -1, 0, 0 } };
static const int props_motion[] = { INPUT_PROP_ACCELEROMETER, -1 };
static const int keys_ps_touch[] = { BTN_LEFT, BTN_TOUCH, BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP, -1 };
static const AbsDef abs_ps_touch[] = { { ABS_X, 0, 1919 }, { ABS_Y, 0, 1079 }, { ABS_MT_SLOT, 0, 1 }, { ABS_MT_POSITION_X, 0, 1919 }, { ABS_MT_POSITION_Y, 0, 1079 }, { -1, 0, 0 } };
static const int props_touch[] = { INPUT_PROP_POINTER, INPUT_PROP_BUTTONPAD, -1 };
static const AbsDef abs_none[] = { { -1, 0, 0 } };

static Sim *add(int num, const char *name, unsigned bus, unsigned vendor, unsigned product, const int *keys, const AbsDef *abs,
                const int *rel, const int *props, int ff) {
    Sim *s = &sim[nsim++];
    memset(s, 0, sizeof *s);
    s->num = num; s->name = name; s->bus = bus; s->vendor = vendor; s->product = product;
    s->keys = keys; s->abs = abs ? abs : abs_none; s->rel = rel ? rel : &K_END; s->props = props ? props : &K_END; s->ff = ff;
    s->present = 1; s->ino = ino_next++; s->ctime = clock_ns;
    return s;
}
/* sticks rest in the middle of their range, triggers at the bottom; z_is_stick: ABS_Z / ABS_RZ are the right stick */
static void rest(Sim *s, int z_is_stick) {
    for (const AbsDef *a = s->abs; a->code >= 0; a++) {
        int stick = a->code == ABS_X || a->code == ABS_Y || a->code == ABS_RX || a->code == ABS_RY || (z_is_stick && (a->code == ABS_Z || a->code == ABS_RZ));
        s->val[a->code] = a->min < 0 ? 0 : stick ? (a->min + a->max + 1) / 2 : a->min;
    }
}
static Sim *add_keyd(int num)      { return add(num, "keyd virtual keyboard", 0x03, 0x0fac, 0x0ade, keys_all, 0, 0, 0, 0); }
static Sim *add_keyd_ptr(int num)  { return add(num, "keyd virtual pointer", 0x03, 0x0fac, 0x1ade, keys_mouse, 0, rel_mouse, 0, 0); }
static Sim *add_mouse(int num)     { return add(num, "Logitech USB Optical Mouse", 0x03, 0x046d, 0xc077, keys_mouse, 0, rel_mouse, 0, 0); }
static Sim *add_xpad(int num)      { Sim *s = add(num, "Microsoft X-Box 360 pad", 0x03, 0x045e, 0x028e, keys_xpad, abs_xpad, 0, 0, 1); rest(s, 0); return s; }
static Sim *add_xone(int num)      { Sim *s = add(num, "Microsoft Xbox Controller", 0x03, 0x045e, 0x0b12, keys_xone, abs_xone, 0, 0, 1); rest(s, 0); return s; }
static Sim *add_xbt(int num)       { Sim *s = add(num, "Xbox Wireless Controller", 0x05, 0x045e, 0x0b13, keys_xbt, abs_xbt, 0, 0, 1); rest(s, 1); return s; }
static Sim *add_seq(int num)       { Sim *s = add(num, "USB Gamepad", 0x03, 0x0079, 0x0006, keys_seq, abs_ps, 0, 0, 0); rest(s, 0); return s; }
static Sim *add_steam(int num)     { Sim *s = add(num, "Microsoft X-Box 360 pad 0", 0x03, 0x28de, 0x11ff, keys_xpad, abs_xpad, 0, 0, 1); rest(s, 0); return s; }
static Sim *add_dualsense(int num) {
    Sim *s = add(num, "Sony Interactive Entertainment DualSense Wireless Controller", 0x03, 0x054c, 0x0ce6, keys_ps, abs_ps, 0, 0, 1); rest(s, 0);
    add(num + 1, "Sony Interactive Entertainment DualSense Wireless Controller Motion Sensors", 0x03, 0x054c, 0x0ce6, &K_END, abs_ps_motion, 0, props_motion, 0);
    add(num + 2, "Sony Interactive Entertainment DualSense Wireless Controller Touchpad", 0x03, 0x054c, 0x0ce6, keys_ps_touch, abs_ps_touch, 0, props_touch, 0);
    return s;
}
static void press(Sim *s, int code, int on) { if (on) s->down[code / 8] |= (u8)(1 << (code % 8)); else s->down[code / 8] &= (u8)~(1 << (code % 8)); }

static int has_key(const Sim *s, int code) {
    for (const int *k = s->keys; *k != -1; k++) if (*k == code || (*k == -2 && code >= 1 && code < 0x300)) return 1;
    return 0;
}
static int in_list(const int *l, int code) { for (; *l >= 0; l++) if (*l == code) return 1; return 0; }
static const AbsDef *abs_def(const Sim *s, int code) { for (const AbsDef *a = s->abs; a->code >= 0; a++) if (a->code == code) return a; return 0; }
/* capability bitmap of event type `type` */
static void bits_of(const Sim *s, int type, u8 *out, size_t n) {
    memset(out, 0, n);
#define SET(b) do { if ((size_t)(b) / 8 < n) out[(b) / 8] |= (u8)(1 << ((b) % 8)); } while (0)
    if (type == 0) {
        SET(EV_SYN);
        if (*s->keys != -1) SET(EV_KEY);
        if (s->abs->code >= 0) SET(EV_ABS);
        if (*s->rel >= 0) SET(EV_REL);
        if (s->ff) SET(EV_FF);
    } else if (type == EV_KEY) { for (int k = 0; k < KEY_CNT; k++) if (has_key(s, k)) SET(k); }
    else if (type == EV_ABS) { for (const AbsDef *a = s->abs; a->code >= 0; a++) SET(a->code); }
    else if (type == EV_REL) { for (const int *r = s->rel; *r >= 0; r++) SET(*r); }
    else if (type == EV_FF) { if (s->ff) SET(FF_RUMBLE); }
    else if (type == -1) { for (const int *p = s->props; *p >= 0; p++) SET(*p); }
#undef SET
}
/* drivers/input/input.c input_print_bitmap: words of an unsigned long, highest first, leading zero words skipped */
static void sysfs_bitmap(const u8 *bits, size_t n, char *out, size_t out_n) {
    size_t len = 0; int started = 0;
    for (int w = (int)(n / 8) - 1; w >= 0; w--) {
        u64 v = 0; for (int b = 7; b >= 0; b--) v = v << 8 | bits[w * 8 + b];
        if (!v && !started) continue;
        len += (size_t)snprintf(out + len, out_n - len, "%s%llx", started ? " " : "", (unsigned long long)v);
        started = 1;
    }
    if (!started) len += (size_t)snprintf(out + len, out_n - len, "0");
    snprintf(out + len, out_n - len, "\n");
}

static Sim *by_path(const char *path, const char *prefix, const char **rest_out) {
    size_t pl = strlen(prefix);
    if (strncmp(path, prefix, pl)) return 0;
    char *end; long n = strtol(path + pl, &end, 10);
    if (end == path + pl) return 0;
    if (rest_out) *rest_out = end;
    for (int i = 0; i < nsim; i++) if (sim[i].present && sim[i].num == n) return &sim[i];
    return 0;
}

/* ---- the wrapped calls --------------------------------------------------- */
DIR *__real_opendir(const char *); struct dirent *__real_readdir(DIR *); int __real_closedir(DIR *);
int __real_stat(const char *, struct stat *); FILE *__real_fopen(const char *, const char *);
int __real_open(const char *, int, ...); int __real_close(int); ssize_t __real_read(int, void *, size_t);
ssize_t __real_write(int, const void *, size_t); int __real_ioctl(int, unsigned long, ...);

static int dir_pos, dir_token, no_dev_input;
DIR *__wrap_opendir(const char *path) {
    if (strcmp(path, "/dev/input")) return __real_opendir(path);
    if (no_dev_input) { errno = ENOENT; return 0; }
    dir_pos = -3; return (DIR *)&dir_token;
}
struct dirent *__wrap_readdir(DIR *d) {
    static struct dirent e;
    if (d != (DIR *)&dir_token) return __real_readdir(d);
    for (;;) {
        int p = dir_pos++;
        if (p == -3) { strcpy(e.d_name, "."); return &e; }
        if (p == -2) { strcpy(e.d_name, "mice"); return &e; }
        if (p == -1) { strcpy(e.d_name, "js0"); return &e; }
        if (p >= nsim) return 0;
        if (!sim[nsim - 1 - p].present) continue;    /* in descending order: the code must not rely on the order */
        snprintf(e.d_name, sizeof e.d_name, "event%d", sim[nsim - 1 - p].num);
        return &e;
    }
}
int __wrap_closedir(DIR *d) { return d == (DIR *)&dir_token ? 0 : __real_closedir(d); }
int __wrap_stat(const char *path, struct stat *st) {
    if (strncmp(path, "/dev/input/event", 16)) return __real_stat(path, st);
    Sim *s = by_path(path, "/dev/input/event", 0);
    if (!s) { errno = ENOENT; return -1; }
    memset(st, 0, sizeof *st);
    st->st_ino = s->ino; st->st_ctim.tv_sec = (time_t)(s->ctime / 1000000000ull); st->st_ctim.tv_nsec = (long)(s->ctime % 1000000000ull);
    return 0;
}
FILE *__wrap_fopen(const char *path, const char *mode) {
    const char *rest_ = 0; char text[2048]; u8 bits[KEY_CNT / 8 + 8];
    if (strncmp(path, "/sys/class/input/event", 22)) return __real_fopen(path, mode);
    Sim *s = by_path(path, "/sys/class/input/event", &rest_);
    if (!s || s->no_sysfs) { errno = ENOENT; return 0; }
    memset(bits, 0, sizeof bits);
    if (!strcmp(rest_, "/device/capabilities/key")) { bits_of(s, EV_KEY, bits, 96); sysfs_bitmap(bits, 96, text, sizeof text); }
    else if (!strcmp(rest_, "/device/capabilities/ev")) { bits_of(s, 0, bits, 8); sysfs_bitmap(bits, 8, text, sizeof text); }
    else if (!strcmp(rest_, "/device/capabilities/abs")) { bits_of(s, EV_ABS, bits, 8); sysfs_bitmap(bits, 8, text, sizeof text); }
    else if (!strcmp(rest_, "/device/capabilities/rel")) { bits_of(s, EV_REL, bits, 8); sysfs_bitmap(bits, 8, text, sizeof text); }
    else if (!strcmp(rest_, "/device/properties")) { bits_of(s, -1, bits, 8); sysfs_bitmap(bits, 8, text, sizeof text); }
    else if (!strcmp(rest_, "/device/name")) snprintf(text, sizeof text, "%s\n", s->name);
    else if (!strcmp(rest_, "/device/id/bustype")) snprintf(text, sizeof text, "%04x\n", s->bus);
    else if (!strcmp(rest_, "/device/id/vendor")) snprintf(text, sizeof text, "%04x\n", s->vendor);
    else if (!strcmp(rest_, "/device/id/product")) snprintf(text, sizeof text, "%04x\n", s->product);
    else { errno = ENOENT; return 0; }
    char *copy = strdup(text);
    return fmemopen(copy, strlen(copy), "r");
}
#define FD_BASE 1000
static Sim *by_fd(int fd) { return fd >= FD_BASE && fd < FD_BASE + NSIM ? &sim[fd - FD_BASE] : 0; }
int __wrap_open(const char *path, int flags, ...) {
    if (strncmp(path, "/dev/input/event", 16)) { va_list ap; va_start(ap, flags); int m = va_arg(ap, int); va_end(ap); return __real_open(path, flags, m); }
    Sim *s = by_path(path, "/dev/input/event", 0);
    if (!s) { errno = ENOENT; return -1; }
    if (s->gone) { errno = ENODEV; return -1; }
    if (s->deny == 2 || (s->deny == 1 && (flags & O_ACCMODE) != O_RDONLY)) { errno = EACCES; return -1; }
    s->opens++;
    return FD_BASE + (int)(s - sim);
}
int __wrap_close(int fd) { Sim *s = by_fd(fd); if (!s) return __real_close(fd); s->opens--; return 0; }
ssize_t __wrap_read(int fd, void *buf, size_t n) {
    Sim *s = by_fd(fd);
    if (!s) return __real_read(fd, buf, n);
    errno = s->gone || !s->present ? ENODEV : EAGAIN; return -1;
}
ssize_t __wrap_write(int fd, const void *buf, size_t n) {
    Sim *s = by_fd(fd);
    if (!s) return __real_write(fd, buf, n);
    const struct input_event *ev = buf;
    if (s->gone) { errno = ENODEV; return -1; }
    if (n == sizeof *ev && ev->type == EV_FF) s->ff_playing = ev->value;
    return (ssize_t)n;
}
int __wrap_ioctl(int fd, unsigned long req, ...) {
    va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap);
    Sim *s = by_fd(fd);
    if (!s) return __real_ioctl(fd, req, arg);
    unsigned nr = _IOC_NR(req), size = _IOC_SIZE(req);
    if (s->gone || !s->present) { errno = ENODEV; return -1; }
    if (_IOC_TYPE(req) != 'E') { errno = EINVAL; return -1; }
    if (nr == 0x02) { struct input_id *id = arg; id->bustype = (u16)s->bus; id->vendor = (u16)s->vendor; id->product = (u16)s->product; id->version = 0x0110; return 0; }
    if (nr == 0x06) { snprintf(arg, size, "%s", s->name); return (int)strlen(arg) + 1; }
    if (nr == 0x09) { bits_of(s, -1, arg, size); return (int)size; }
    if (nr == 0x18) { memset(arg, 0, size); memcpy(arg, s->down, size < sizeof s->down ? size : sizeof s->down); return (int)size; }
    if (nr >= 0x20 && nr < 0x40) { bits_of(s, (int)nr - 0x20, arg, size); return (int)size; }
    if (nr >= 0x40 && nr < 0x80 && (_IOC_DIR(req) & _IOC_READ)) {
        const AbsDef *a = abs_def(s, (int)nr - 0x40); struct input_absinfo *ai = arg;
        memset(ai, 0, sizeof *ai);
        if (a) { ai->minimum = a->min; ai->maximum = a->max; ai->value = s->val[a->code]; }
        return 0;
    }
    if (nr == 0x80) { struct ff_effect *e = arg; if (!s->ff) { errno = EINVAL; return -1; } if (e->id < 0) e->id = 0;
                      s->ff_strong = e->u.rumble.strong_magnitude; s->ff_weak = e->u.rumble.weak_magnitude; return 0; }
    errno = EINVAL; return -1;
}

/* ---- what the runtime provides ------------------------------------------- */
static char logbuf[16384]; static size_t loglen;
void rt_log(const char *fmt, ...) {
    char t[1024]; va_list ap; va_start(ap, fmt); vsnprintf(t, sizeof t, fmt, ap); va_end(ap);
    printf("    | %s", t);
    loglen += (size_t)snprintf(logbuf + loglen, sizeof logbuf - loglen, "%s", t);
}
u64 plat_time_ns(void) { return clock_ns; }
#ifndef PADSIM_SYSTEM_CRT
void *__dso_handle = 0;                             /* as in rt/plat_posix.c: the link line has no crtbegin.o */
#endif
static int want_debug;
const char *plat_getenv(const char *name) { return want_debug && !strcmp(name, "ROR_PAD_DEBUG") ? "1" : 0; }
static u16 ps2_buttons; static u8 ps2_sticks[4]; static u8 act_small, act_big;
void pad_set_state(u16 buttons, u8 rx, u8 ry, u8 lx, u8 ly) { ps2_buttons = buttons; ps2_sticks[0] = rx; ps2_sticks[1] = ry; ps2_sticks[2] = lx; ps2_sticks[3] = ly; }
void pad_get_actuators(u8 *small_motor, u8 *big_motor) { *small_motor = act_small; *big_motor = act_big; }

/* ---- checks -------------------------------------------------------------- */
static int fails;
static void ok(int cond, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void ok(int cond, const char *fmt, ...) {
    char t[400]; va_list ap; va_start(ap, fmt); vsnprintf(t, sizeof t, fmt, ap); va_end(ap);
    printf("  %-4s %s\n", cond ? "ok" : "FAIL", t);
    fails += !cond;
}
static HostPad hp; static char hp_name[128]; static int hp_have;
static void frame(void) { clock_ns += 20000000ull; memset(&hp, 0, sizeof hp); hp_name[0] = 0; hp_have = hpad_poll(&hp, hp_name, sizeof hp_name); }
static void later(void) { clock_ns += 1100000000ull; frame(); }       /* past the once-per-second look at /dev/input */
static void expect(const char *what, int have, unsigned buttons, int lt, int rt, int lx, int ly, int rx, int ry) {
    int good = hp_have == have && (!have || (hp.buttons == buttons && hp.lt == lt && hp.rt == rt && hp.lx == lx && hp.ly == ly && hp.rx == rx && hp.ry == ry));
    if (have) ok(good, "%s: pad %d \"%s\" buttons %04x lt %d rt %d lx %d ly %d rx %d ry %d   (expected buttons %04x lt %d rt %d lx %d ly %d rx %d ry %d)",
                 what, hp_have, hp_name, hp.buttons, hp.lt, hp.rt, hp.lx, hp.ly, hp.rx, hp.ry, buttons, lt, rt, lx, ly, rx, ry);
    else ok(good, "%s: pad %d   (expected none)", what, hp_have);
}
static void logged(const char *text) { ok(strstr(logbuf, text) != 0, "log contains \"%s\"", text); }
static void not_logged(const char *text) { ok(strstr(logbuf, text) == 0, "log does not contain \"%s\"", text); }

/* ---- scenarios ----------------------------------------------------------- */
static void s_virtual_keyboard_first(void) {
    puts("virtual keyboard and pointer of a key remapper at event2/3, a mouse, wired Xbox 360 pad (xpad) at event17");
    add_keyd(2); add_keyd_ptr(3); add_mouse(4); Sim *x = add_xpad(17);
    frame(); expect("at rest", 1, 0, 0, 0, 0, 0, 0, 0);
    ok(strstr(hp_name, "X-Box") != 0, "the device in use is the Xbox pad (name \"%s\")", hp_name);
    press(x, BTN_A, 1); x->val[ABS_HAT0Y] = -1; x->val[ABS_Z] = 200; x->val[ABS_X] = -32768; x->val[ABS_RY] = 32767;
    frame(); expect("A + D-pad up + LT 200 + left stick left + right stick down", 1, HPAD_A | HPAD_UP, 200, 0, -32768, 0, 0, -32767);
    puts("  the same through pad.c to the PS2 pad, and a vibration request back:");
    act_small = 1; act_big = 128;
    for (int i = 0; i < 30; i++) { clock_ns += 20000000ull; pad_update(100 + (u64)i, 0xFFFF, 1); }
    ok(ps2_buttons == 0xBEEF && ps2_sticks[2] == 0x00 && ps2_sticks[3] == 0x80 && ps2_sticks[0] == 0x80 && ps2_sticks[1] == 0xFF,
       "PS2 word %04x (expected beef: Cross, Up, L2 pressed), sticks LX %02x LY %02x RX %02x RY %02x (expected 00 80 80 ff)",
       ps2_buttons, ps2_sticks[2], ps2_sticks[3], ps2_sticks[0], ps2_sticks[1]);
    ok(x->ff_playing == 1 && x->ff_strong == 128 * 257 && x->ff_weak == 65535, "vibration effect on the pad: playing %d strong %d weak %d (expected 1 32896 65535)",
       x->ff_playing, x->ff_strong, x->ff_weak);
    logged("[pad] gamepad: Microsoft X-Box 360 pad");
}
static void s_xone(void) {
    puts("Xbox One / Series pad (xpad or xone: triggers 0..1023)");
    Sim *x = add_xone(9);
    press(x, BTN_X, 1); press(x, BTN_Y, 1); press(x, BTN_SELECT, 1); press(x, BTN_THUMBR, 1);
    x->val[ABS_Z] = 512; x->val[ABS_RZ] = 1023; x->val[ABS_RY] = -32768; x->val[ABS_HAT0X] = 1;
    frame(); expect("X + Y + View + RS + D-pad right, LT half, RT full, right stick up", 1, HPAD_X | HPAD_Y | HPAD_BACK | HPAD_RS | HPAD_RIGHT, 127, 255, 0, 0, 0, 32767);
}
static void s_bluetooth_generic(void) {
    puts("Xbox pad over Bluetooth on the generic HID mapping (right stick on Z/RZ, triggers on BRAKE/GAS 0..1023)");
    Sim *x = add_xbt(21);
    frame(); expect("at rest", 1, 0, 0, 0, 0, 0, 0, 0);
    press(x, 0x133, 1); press(x, 0x13a, 1); press(x, 0x13b, 1); press(x, 0x136, 1);
    x->val[ABS_Z] = 65535; x->val[ABS_RZ] = 0; x->val[ABS_BRAKE] = 1023; x->val[ABS_GAS] = 300; x->val[ABS_X] = 0; x->val[ABS_HAT0Y] = 1;
    frame(); expect("X + View + Menu + LB + D-pad down, LT full, RT 300/1023, left stick left, right stick up-right", 1,
                    HPAD_X | HPAD_BACK | HPAD_START | HPAD_LB | HPAD_DOWN, 255, 74, -32768, 0, 32767, 32767);
}
static void s_sequential(void) {
    puts("HID pad with ten buttons numbered in sequence (codes 0x130..0x139, no BTN_START)");
    Sim *x = add_seq(6);
    press(x, 0x132, 1); press(x, 0x137, 1); press(x, 0x134, 1); press(x, 0x139, 1);
    frame(); expect("buttons 3, 5, 8, 10 = X, LB, Start, RS", 1, HPAD_X | HPAD_LB | HPAD_START | HPAD_RS, 0, 0, 0, 0, 0, 0);
    logged("buttons in sequence");
}
static void s_dualsense(void) {
    puts("DualSense (hid-playstation): gamepad node, motion sensors node, touchpad node");
    Sim *d = add_dualsense(20);
    press(d, BTN_NORTH, 1); press(d, BTN_WEST, 1); press(d, BTN_TL2, 1); d->val[ABS_Z] = 30; d->val[ABS_RZ] = 255; d->val[ABS_Y] = 0;
    frame(); expect("Triangle + Square, L2 touched (30), R2 full, left stick up", 1, HPAD_Y | HPAD_X, 30, 255, 0, 32767, 0, 0);
    ok(!strcmp(hpad_kind(), "playstation"), "kind \"%s\" (expected playstation)", hpad_kind());
    ok(sim[0].opens == 1 && sim[1].opens == 0 && sim[2].opens == 0, "open handles: gamepad %d, motion sensors %d, touchpad %d (expected 1 0 0)", sim[0].opens, sim[1].opens, sim[2].opens);
}
static void s_permission(void) {
    puts("Xbox pad whose node cannot be opened (no access rights), then the rights arrive");
    Sim *x = add_xpad(5); x->deny = 2; press(x, BTN_B, 1);
    frame(); expect("no access", 0, 0, 0, 0, 0, 0, 0, 0);
    logged("permission denied for /dev/input/event5");
    logged("usermod -aG input");
    size_t before = loglen; later(); later();
    ok(loglen == before, "the line is not repeated while nothing changes");
    x->deny = 0; x->ctime = clock_ns;                 /* udev applies the ACL: the node's change time moves */
    later(); expect("after access was granted", 1, HPAD_B, 0, 0, 0, 0, 0, 0);
}
static void s_hotplug(void) {
    puts("two pads; a third is plugged in; one is unplugged; the others go on; replug under a new number");
    Sim *x = add_xpad(17); Sim *d = add_dualsense(20);
    press(x, BTN_A, 1); press(d, BTN_EAST, 1); d->val[ABS_X] = 255; x->val[ABS_X] = -9000;
    frame(); expect("xpad A + small stick, DualSense Circle + full stick", 1, HPAD_A | HPAD_B, 0, 0, 32767, 0, 0, 0);
    Sim *o = add_xone(30); press(o, BTN_START, 1);
    frame(); expect("third pad just plugged in, not looked for yet", 1, HPAD_A | HPAD_B, 0, 0, 32767, 0, 0, 0);
    later(); expect("a second later", 1, HPAD_A | HPAD_B | HPAD_START, 0, 0, 32767, 0, 0, 0);
    x->gone = 1; x->present = 0;
    frame(); expect("xpad unplugged", 1, HPAD_B | HPAD_START, 0, 0, 32767, 0, 0, 0);
    logged("/dev/input/event17 \"Microsoft X-Box 360 pad\" 045e:028e: disconnected");
    d->gone = o->gone = 1; d->present = d[1].present = d[2].present = o->present = 0;
    frame(); expect("all unplugged", 0, 0, 0, 0, 0, 0, 0, 0);
    Sim *y = add_xpad(23); press(y, BTN_Y, 1);
    later(); expect("xpad again, now event23", 1, HPAD_Y, 0, 0, 0, 0, 0, 0);
}
static void s_steam(void) {
    puts("physical pad that stays silent (as if another program swallowed its input) plus Steam's virtual pad made from it");
    Sim *x = add_xpad(17); Sim *v = add_steam(25);
    press(v, BTN_B, 1); v->val[ABS_RX] = 32767; v->val[ABS_RZ] = 90;
    frame(); expect("only the virtual pad reports", 1, HPAD_B, 0, 90, 0, 0, 32767, 0);
    press(x, BTN_B, 1); x->val[ABS_RX] = 32767; x->val[ABS_RZ] = 90;
    frame(); expect("both report the same", 1, HPAD_B, 0, 90, 0, 0, 32767, 0);
}
static void s_filter(void) {
    puts("--pad playstation with an Xbox pad and a DualSense connected");
    Sim *x = add_xpad(17); Sim *d = add_dualsense(20);
    hpad_select(2); press(x, BTN_A, 1); press(d, BTN_WEST, 1);
    frame(); expect("only the DualSense counts (Square)", 1, HPAD_X, 0, 0, 0, 0, 0, 0);
    logged("skipped by --pad playstation");
}
static void s_filter_xinput(void) {
    puts("--pad xinput with an Xbox pad and a DualSense connected");
    Sim *x = add_xpad(17); Sim *d = add_dualsense(20);
    hpad_select(1); press(x, BTN_A, 1); press(d, BTN_WEST, 1);
    frame(); expect("only the Xbox pad counts (A)", 1, HPAD_A, 0, 0, 0, 0, 0, 0);
}
static void s_no_sysfs(void) {
    puts("no readable sysfs entries; a keyboard-like virtual device first; the pad can only be opened read-only");
    Sim *k = add_keyd(1); Sim *x = add_xpad(3); k->no_sysfs = x->no_sysfs = 1; x->deny = 1; press(x, BTN_TR, 1);
    frame(); expect("RB", 1, HPAD_RB, 0, 0, 0, 0, 0, 0);
    logged("opened read-only: no vibration");
}
static void s_nothing(void) {
    puts("keyboard-like virtual device, a mouse and a node with gamepad buttons but no axes; then no /dev/input at all");
    add_keyd(2); add_mouse(4); add(7, "Gaming Keyboard Consumer Control", 0x03, 0x1b1c, 0x1b2d, keys_seq, 0, 0, 0, 0);
    frame(); expect("no pad", 0, 0, 0, 0, 0, 0, 0, 0);
    ok(strstr(hpad_status(), "3 input devices in /dev/input, no gamepad") != 0, "status \"%s\"", hpad_status());
    logged("skipped, has keyboard keys too");
    logged("skipped, gamepad buttons but no stick or D-pad axes");
    not_logged("Logitech");
    no_dev_input = 1; later();
    ok(strstr(hpad_status(), "/dev/input does not exist") != 0, "status \"%s\"", hpad_status());
}
static void s_debug(void) {
    puts("ROR_PAD_DEBUG=1: capability summary and state changes");
    want_debug = 1; add_mouse(4); Sim *x = add_xpad(17);
    frame(); press(x, BTN_A, 1); frame(); frame();
    logged("keys 130 131 133 134 136 137 13a 13b 13c 13d 13e; axes 0[-32768..32767]");
    logged("event17 state: buttons 1000");
    logged("Logitech USB Optical Mouse\" 046d:c077: skipped, no gamepad buttons");
}

static const struct { const char *name; void (*run)(void); } scenarios[] = {
    { "virtual-keyboard-first", s_virtual_keyboard_first }, { "xbox-one", s_xone }, { "xbox-bluetooth-generic", s_bluetooth_generic },
    { "sequential-buttons", s_sequential }, { "dualsense", s_dualsense }, { "permission", s_permission }, { "hot-plug", s_hotplug },
    { "steam-virtual-pad", s_steam }, { "filter-playstation", s_filter }, { "filter-xinput", s_filter_xinput }, { "no-sysfs", s_no_sysfs },
    { "no-gamepad", s_nothing }, { "debug", s_debug },
};
int main(int argc, char **argv) {
    int n = (int)(sizeof scenarios / sizeof *scenarios);
    if (argc < 2) { printf("%d\n", n); return 0; }
    int i = atoi(argv[1]);
    if (i < 1 || i > n) return 2;
    printf("--- %d %s: ", i, scenarios[i - 1].name);
    scenarios[i - 1].run();
    printf("  => %s\n", fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}
