/* pad_ps.c — Sony DualShock 4 / DualSense HID report formats, platform
 * neutral (used by pad_hid.c on Windows; on Linux the kernel drivers decode
 * the pads and pad_evdev.c reads them like any other gamepad).
 *
 * Input reports (byte 0 is the report ID; layouts as in the Linux drivers
 * hid-playstation / hid-sony and SDL's HIDAPI drivers):
 *
 *   DualShock 4, USB, ID 0x01 (64 bytes), and the short Bluetooth report
 *   ID 0x01 that DualShock 4 and DualSense send until the extended mode is
 *   switched on:
 *     [1] LX [2] LY [3] RX [4] RY [5..7] buttons [8] L2 [9] R2
 *   DualShock 4, Bluetooth, ID 0x11 (78 bytes): the same from [3] on:
 *     [3] LX [4] LY [5] RX [6] RY [7..9] buttons [10] L2 [11] R2
 *   DualSense, USB, ID 0x01 (64 bytes):
 *     [1] LX [2] LY [3] RX [4] RY [5] L2 [6] R2 [7] counter [8..10] buttons
 *   DualSense, Bluetooth, ID 0x31 (78 bytes): the same from [2] on:
 *     [2] LX [3] LY [4] RX [5] RY [6] L2 [7] R2 [8] counter [9..11] buttons
 *
 *   buttons[0]: low nibble = D-pad hat (0 up, 1 up-right, ... 7 up-left,
 *               8 released); 0x10 Square, 0x20 Cross, 0x40 Circle, 0x80 Triangle
 *   buttons[1]: 0x01 L1, 0x02 R1, 0x04 L2, 0x08 R2 (digital), 0x10 Share /
 *               Create, 0x20 Options, 0x40 L3, 0x80 R3
 *   buttons[2]: 0x01 PS, 0x02 touchpad click
 *   Sticks are 0..255, centre 128, y down; triggers 0..255.
 * L2 / R2 are taken from the analog bytes (the digital bits switch at the
 * first millimetre). PS and the touchpad click have no PS2 counterpart and
 * are reported separately (not mapped).
 *
 * Output reports, vibration only (the flags select the motors and nothing
 * else, so the light bar and LEDs keep whatever the system set):
 *   DualShock 4 USB  ID 0x05, 32 bytes:  [1] = 0x01 (motors), [4] weak (right), [5] strong (left)
 *   DualShock 4 BT   ID 0x11, 78 bytes:  [1] = 0xC4 (HID + CRC, 4 ms report interval), [3] = 0x01, [6] weak, [7] strong, [74..77] CRC
 *   DualSense USB    ID 0x02, 48 bytes:  [1] = 0x03 (vibration + haptics select), [3] weak, [4] strong
 *   DualSense BT     ID 0x31, 78 bytes:  [1] = sequence << 4 (4 bits, one up per report), [2] = 0x10 (tag),
 *                                       [3] = 0x03, [5] weak, [6] strong, [74..77] CRC
 *                    (struct dualsense_output_report_bt of hid-playstation.c; SDL uses the same since 2026)
 * The DualSense flags 0x03 are the legacy vibration mode, which the kernel driver documents as working on
 * all firmware; the newer COMPATIBLE_VIBRATION2 flag is not used.
 * Bluetooth CRC: CRC-32 over the byte 0xA2 followed by the first 74 report
 * bytes, stored little-endian. */
#include "rt.h"
#include "pad_ps.h"

u32 ps_crc32(u32 crc, const u8 *p, size_t n) {
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc & 1) ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
    }
    return ~crc;
}

static s16 stick_x(int v) { return (s16)(v >= 128 ? (v - 128) * 32767 / 127 : (v - 128) * 256); }
static s16 stick_y(int v) { int r = -(int)stick_x(v); return (s16)(r > 32767 ? 32767 : r); }      /* HostPad: +y up */

static void decode(const u8 *sticks, const u8 *b, int l2, int r2, HostPad *o, int *extra) {
    static const u8 hat[8] = { HPAD_UP, HPAD_UP | HPAD_RIGHT, HPAD_RIGHT, HPAD_RIGHT | HPAD_DOWN,
                               HPAD_DOWN, HPAD_DOWN | HPAD_LEFT, HPAD_LEFT, HPAD_LEFT | HPAD_UP };
    u16 k = (b[0] & 15) < 8 ? hat[b[0] & 15] : 0;
    if (b[0] & 0x10) k |= HPAD_X;                    /* Square   (HostPad is in Xbox terms: X is the left button) */
    if (b[0] & 0x20) k |= HPAD_A;                    /* Cross */
    if (b[0] & 0x40) k |= HPAD_B;                    /* Circle */
    if (b[0] & 0x80) k |= HPAD_Y;                    /* Triangle */
    if (b[1] & 0x01) k |= HPAD_LB;                   /* L1 */
    if (b[1] & 0x02) k |= HPAD_RB;                   /* R1 */
    if (b[1] & 0x10) k |= HPAD_BACK;                 /* Share / Create -> Select */
    if (b[1] & 0x20) k |= HPAD_START;                /* Options -> Start */
    if (b[1] & 0x40) k |= HPAD_LS;                   /* L3 */
    if (b[1] & 0x80) k |= HPAD_RS;                   /* R3 */
    o->buttons = k; o->lt = (u8)l2; o->rt = (u8)r2;
    o->lx = stick_x(sticks[0]); o->ly = stick_y(sticks[1]); o->rx = stick_x(sticks[2]); o->ry = stick_y(sticks[3]);
    if (extra) *extra = ((b[2] & 1) ? PS_EXTRA_PS : 0) | ((b[2] & 2) ? PS_EXTRA_TOUCHPAD : 0);
}

int ps_parse(int model, int bt, const u8 *d, int n, HostPad *out, int *extra) {
    if (n < 1) return 0;
    if (model == PS_DS4) {
        if (d[0] == 0x01 && n >= 10) { decode(d + 1, d + 5, d[8], d[9], out, extra); return 1; }      /* USB, or short Bluetooth */
        if (d[0] == 0x11 && bt && n >= 12) { decode(d + 3, d + 7, d[10], d[11], out, extra); return 1; }
    } else if (model == PS_DUALSENSE) {
        if (d[0] == 0x01 && !bt && n >= 11) { decode(d + 1, d + 8, d[5], d[6], out, extra); return 1; }
        if (d[0] == 0x01 && bt && n >= 10) { decode(d + 1, d + 5, d[8], d[9], out, extra); return 1; } /* short Bluetooth */
        if (d[0] == 0x31 && bt && n >= 12) { decode(d + 2, d + 9, d[6], d[7], out, extra); return 1; }
    }
    return 0;
}

static int bt_crc(u8 *out) {
    u8 t[75];
    t[0] = 0xA2;                                     /* Bluetooth HID header: DATA | output */
    memcpy(t + 1, out, 74);
    u32 c = ps_crc32(0, t, 75);
    out[74] = (u8)c; out[75] = (u8)(c >> 8); out[76] = (u8)(c >> 16); out[77] = (u8)(c >> 24);
    return 78;
}
int ps_rumble_report(int model, int bt, int small, int big, u8 *out) {
    memset(out, 0, 78);
    if (model == PS_DS4) {
        if (!bt) { out[0] = 0x05; out[1] = 0x01; out[4] = (u8)small; out[5] = (u8)big; return 32; }
        out[0] = 0x11; out[1] = 0xC4; out[3] = 0x01; out[6] = (u8)small; out[7] = (u8)big;
        return bt_crc(out);
    }
    if (!bt) { out[0] = 0x02; out[1] = 0x03; out[3] = (u8)small; out[4] = (u8)big; return 48; }
    static unsigned seq;
    out[0] = 0x31; out[1] = (u8)((seq++ & 15) << 4); out[2] = 0x10; out[3] = 0x03; out[5] = (u8)small; out[6] = (u8)big;
    return bt_crc(out);
}

/* ---- self-test: constructed sample reports through ps_parse ------------- */
static void put(u8 *d, int at_sticks, int at_buttons, int at_l2) {
    /* LX 0 (full left), LY 255 (full down), RX 255, RY 64; hat 1 (up-right), Cross + Triangle;
     * L1, Options, R3 (+ digital R2); PS; L2 = 30, R2 = 200 */
    d[at_sticks] = 0; d[at_sticks + 1] = 255; d[at_sticks + 2] = 255; d[at_sticks + 3] = 64;
    d[at_buttons] = 0x01 | 0x20 | 0x80; d[at_buttons + 1] = 0x01 | 0x20 | 0x80 | 0x08; d[at_buttons + 2] = 0x01;
    d[at_l2] = 30; d[at_l2 + 1] = 200;
}
static int check(const char *what, int model, int bt, const u8 *d, int n, int idle) {
    HostPad h; int extra = 0;
    memset(&h, 0xEE, sizeof h);
    if (!ps_parse(model, bt, d, n, &h, &extra)) { printf("%-34s NOT DECODED\n", what); return 1; }
    printf("%-34s buttons %04x  lt %3d rt %3d  lx %6d ly %6d rx %6d ry %6d  extra %d", what, h.buttons, h.lt, h.rt, h.lx, h.ly, h.rx, h.ry, extra);
    int ok;
    if (idle) ok = h.buttons == 0 && h.lt == 0 && h.rt == 0 && h.lx == 0 && h.ly == 0 && h.rx == 0 && h.ry == 0 && extra == 0;
    else ok = h.buttons == (HPAD_UP | HPAD_RIGHT | HPAD_A | HPAD_Y | HPAD_LB | HPAD_START | HPAD_RS) && h.lt == 30 && h.rt == 200 &&
              h.lx == -32768 && h.ly == -32767 && h.rx == 32767 && h.ry == 16384 && extra == PS_EXTRA_PS;
    printf("  %s\n", ok ? "ok" : "WRONG");
    return !ok;
}
static void hex(const char *what, const u8 *p, int n) {
    printf("%-34s %d bytes:", what, n);
    for (int i = 0; i < 12; i++) printf(" %02x", p[i]);
    if (n == 78) printf(" ... crc %02x %02x %02x %02x", p[74], p[75], p[76], p[77]);
    printf("\n");
}
int ps_selftest(void) {
    int bad = 0; u8 d[80]; HostPad h;
    /* idle reports: sticks centred, hat released */
    static const u8 ds4_idle[64] = { 0x01, 0x80, 0x80, 0x80, 0x80, 0x08, 0x00, 0xFC, 0x00, 0x00 };
    static const u8 dse_idle[64] = { 0x01, 0x80, 0x80, 0x80, 0x80, 0x00, 0x00, 0x2A, 0x08, 0x00, 0x00, 0x00 };
    bad += check("DualShock 4 USB 0x01 idle", PS_DS4, 0, ds4_idle, 64, 1);
    bad += check("DualSense USB 0x01 idle", PS_DUALSENSE, 0, dse_idle, 64, 1);
    memset(d, 0, sizeof d); d[0] = 0x01; put(d, 1, 5, 8);
    bad += check("DualShock 4 USB 0x01", PS_DS4, 0, d, 64, 0);
    bad += check("DualShock 4 Bluetooth 0x01 short", PS_DS4, 1, d, 10, 0);
    bad += check("DualSense Bluetooth 0x01 short", PS_DUALSENSE, 1, d, 10, 0);
    memset(d, 0, sizeof d); d[0] = 0x11; d[1] = 0xC0; put(d, 3, 7, 10);
    bad += check("DualShock 4 Bluetooth 0x11", PS_DS4, 1, d, 78, 0);
    memset(d, 0, sizeof d); d[0] = 0x01; put(d, 1, 8, 5);
    bad += check("DualSense USB 0x01", PS_DUALSENSE, 0, d, 64, 0);
    memset(d, 0, sizeof d); d[0] = 0x31; put(d, 2, 9, 6);
    bad += check("DualSense Bluetooth 0x31", PS_DUALSENSE, 1, d, 78, 0);
    /* reports that must be refused */
    memset(d, 0, sizeof d); d[0] = 0x11;
    if (ps_parse(PS_DS4, 0, d, 78, &h, 0) || ps_parse(PS_DUALSENSE, 0, d, 78, &h, 0) || ps_parse(PS_DS4, 1, d, 5, &h, 0)) { printf("unexpected report accepted  WRONG\n"); bad++; }
    else printf("%-34s refused  ok\n", "wrong ID / short report");
    /* CRC-32 check value, and the Bluetooth output reports verified with an independent pass */
    u32 c = ps_crc32(0, (const u8 *)"123456789", 9);
    printf("%-34s %08x  %s\n", "crc32(\"123456789\")", c, c == 0xCBF43926u ? "ok" : "WRONG"); bad += c != 0xCBF43926u;
    static const struct { const char *what; int model, bt, len; } o[4] = {
        { "rumble DualShock 4 USB 0x05", PS_DS4, 0, 32 }, { "rumble DualShock 4 Bluetooth 0x11", PS_DS4, 1, 78 },
        { "rumble DualSense USB 0x02", PS_DUALSENSE, 0, 48 }, { "rumble DualSense Bluetooth 0x31", PS_DUALSENSE, 1, 78 } };
    for (int i = 0; i < 4; i++) {
        int n = ps_rumble_report(o[i].model, o[i].bt, 255, 128, d);
        hex(o[i].what, d, n);
        static const u8 want_head[4][8] = { { 0x05, 0x01, 0, 0, 0xFF, 0x80, 0, 0 }, { 0x11, 0xC4, 0, 0x01, 0, 0, 0xFF, 0x80 },
                                            { 0x02, 0x03, 0, 0xFF, 0x80, 0, 0, 0 }, { 0x31, 0x00, 0x10, 0x03, 0, 0xFF, 0x80, 0 } };
        int ok = n == o[i].len && !memcmp(d, want_head[i], 8);
        if (!ok) printf("%-34s unexpected length or layout  WRONG\n", "");
        if (n == 78) {
            static const u8 hdr = 0xA2;              /* checked the other way round: chained */
            u32 want = ps_crc32(ps_crc32(0, &hdr, 1), d, 74), got = (u32)d[74] | (u32)d[75] << 8 | (u32)d[76] << 16 | (u32)d[77] << 24;
            ok = ok && want == got;
            printf("%-34s crc over a2 + 74 bytes %08x, in report %08x  %s\n", "", want, got, want == got ? "ok" : "WRONG");
        }
        bad += !ok;
    }
    {   /* DualSense Bluetooth: the sequence number goes up by one per report, 4 bits */
        u8 a[78], b[78]; ps_rumble_report(PS_DUALSENSE, 1, 0, 0, a);
        for (int i = 0; i < 15; i++) ps_rumble_report(PS_DUALSENSE, 1, 0, 0, b);
        int ok = a[1] == 0x10 && b[1] == 0x00 && a[2] == 0x10;
        printf("%-34s second report %02x, seventeenth %02x  %s\n", "DualSense Bluetooth sequence byte", a[1], b[1], ok ? "ok" : "WRONG"); bad += !ok;
    }
    printf("pad self-test: %s\n", bad ? "FAILED" : "all ok");
    return bad;
}
