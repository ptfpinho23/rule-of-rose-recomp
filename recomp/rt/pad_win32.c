/* pad_win32.c — gamepad on Windows (plat.h hpad_*): chooses between XInput
 * (pad_xinput.c) and the native reader for Sony pads (pad_hid.c).
 *
 * Only one source is used at a time, so a Sony pad that is also presented
 * as an Xbox pad (Steam Input, DS4Windows) never gives double input.
 *   auto         XInput first: if XInput reports a pad, that one is used and
 *                the HID reader is idle (device closed). Only while XInput
 *                has no pad is a DualShock 4 / DualSense looked for and read
 *                directly. The source in use is kept until it disconnects.
 *   xinput       XInput only.
 *   playstation  the Sony HID reader only. */
#ifdef _WIN32
#include "rt.h"
#include "plat.h"

int  xi_poll(HostPad *st, char *name, size_t n);
const char *xi_status(void);
void xi_rumble(int small, int big);
int  pshid_poll(HostPad *st, char *name, size_t n);
const char *pshid_status(void);
void pshid_rumble(int small, int big);
void pshid_want(int on);

static int sel, cur;                                 /* sel: 0 auto, 1 XInput, 2 PlayStation; cur: 0 none, 1 XInput, 2 HID */

void hpad_select(int which) { sel = which; }
int hpad_poll(HostPad *st, char *name, size_t n) {
    if (sel != 2 && cur != 2) {
        if (xi_poll(st, name, n)) { cur = 1; pshid_want(0); return 1; }
        cur = 0;
    }
    if (sel == 1) return 0;
    pshid_want(1);
    if (pshid_poll(st, name, n)) { cur = 2; return 1; }
    cur = 0;
    return 0;
}
const char *hpad_status(void) {
    static char both[320];
    if (cur == 1 || sel == 1) return xi_status();
    if (cur == 2 || sel == 2) return pshid_status();
    snprintf(both, sizeof both, "%s: no pad; %s", xi_status(), pshid_status());
    return both;
}
const char *hpad_kind(void) { return cur == 2 ? "playstation" : "xbox"; }
void hpad_rumble(int small, int big) {
    if (cur == 1) xi_rumble(small, big); else if (cur == 2) pshid_rumble(small, big);
}
#endif
