/* pad_xinput.c — gamepad on Windows, XInput part (xi_*; pad_win32.c chooses
 * between this and the Sony HID reader in pad_hid.c).
 *
 * The DLL is loaded at run time (xinput1_4.dll, Windows 8+; then
 * xinput1_3.dll, the DirectX redistributable; then xinput9_1_0.dll, Vista+),
 * so the .exe has no import for it and starts without it. XInput covers
 * Xbox pads and whatever Steam Input / DS4Windows present as one. All four
 * slots are read and merged. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "rt.h"
#include "plat.h"

typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XGamepad;
typedef struct { DWORD dwPacketNumber; XGamepad Gamepad; } XState;
typedef struct { WORD wLeftMotorSpeed, wRightMotorSpeed; } XVibration;

static DWORD (WINAPI *x_get)(DWORD, XState *);
static DWORD (WINAPI *x_set)(DWORD, XVibration *);
static int loaded, slots;                           /* slots: bit i = slot i has a pad */
static u64 next_scan;
static char status[96] = "XInput";

static void load(void) {
    static const char *const names[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    loaded = 1;
    for (int i = 0; i < 3; i++) {
        HMODULE m = LoadLibraryExA(names[i], 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!m) continue;
        x_get = (DWORD (WINAPI *)(DWORD, XState *))(void *)GetProcAddress(m, "XInputGetState");
        x_set = (DWORD (WINAPI *)(DWORD, XVibration *))(void *)GetProcAddress(m, "XInputSetState");
        if (x_get) { snprintf(status, sizeof status, "XInput, %s", names[i]); return; }
        FreeLibrary(m);
    }
    x_get = 0; x_set = 0;
    snprintf(status, sizeof status, "XInput is not available: no xinput1_4.dll, xinput1_3.dll or xinput9_1_0.dll");
}
const char *xi_status(void) { if (!loaded) load(); return status; }

static long reach(int x, int y) { return (long)x * x + (long)y * y; }
/* All four slots are used and merged (buttons OR-ed, each trigger the larger value, each stick from the pad that
 * deflects it most), so a slot held by a device that never reports anything cannot hide the pad in another one. */
int xi_poll(HostPad *st, char *name, size_t n) {
    XState xs; u64 now = plat_time_ns();
    if (!loaded) load();
    if (!x_get) return 0;
    int look = now >= next_scan;                     /* asking an empty slot is slow: those once per second */
    if (look) next_scan = now + 1000000000ull;
    memset(st, 0, sizeof *st);
    for (int i = 0; i < 4; i++) {
        if (!(slots & (1 << i)) && !look) continue;
        memset(&xs, 0, sizeof xs);
        if (x_get((DWORD)i, &xs) != ERROR_SUCCESS) { slots &= ~(1 << i); continue; }
        slots |= 1 << i;
        st->buttons |= xs.Gamepad.wButtons & 0xF3FF; /* the HPAD_* bits are XInput's */
        if (xs.Gamepad.bLeftTrigger > st->lt) st->lt = xs.Gamepad.bLeftTrigger;
        if (xs.Gamepad.bRightTrigger > st->rt) st->rt = xs.Gamepad.bRightTrigger;
        if (reach(xs.Gamepad.sThumbLX, xs.Gamepad.sThumbLY) > reach(st->lx, st->ly)) { st->lx = xs.Gamepad.sThumbLX; st->ly = xs.Gamepad.sThumbLY; }
        if (reach(xs.Gamepad.sThumbRX, xs.Gamepad.sThumbRY) > reach(st->rx, st->ry)) { st->rx = xs.Gamepad.sThumbRX; st->ry = xs.Gamepad.sThumbRY; }
    }
    if (!slots) return 0;
    size_t len = (size_t)snprintf(name, n, "XInput controller");
    for (int i = 0, first = 1; i < 4 && len < n; i++)
        if (slots & (1 << i)) { len += (size_t)snprintf(name + len, n - len, "%s%d", first ? " " : "+", i + 1); first = 0; }
    return 1;
}
/* PS2 big motor -> the low-frequency (left) motor, small motor -> the high-frequency (right) one. */
void xi_rumble(int small, int big) {
    if (!x_set) return;
    XVibration v = { (WORD)(big * 257), (WORD)(small * 257) };
    for (int i = 0; i < 4; i++) if (slots & (1 << i)) x_set((DWORD)i, &v);
}
#endif
