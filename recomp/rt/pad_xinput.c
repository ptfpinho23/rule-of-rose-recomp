/* pad_xinput.c — gamepad on Windows, XInput part (xi_*; pad_win32.c chooses
 * between this and the Sony HID reader in pad_hid.c).
 *
 * The DLL is loaded at run time (xinput1_4.dll, Windows 8+; then
 * xinput1_3.dll, the DirectX redistributable; then xinput9_1_0.dll, Vista+),
 * so the .exe has no import for it and starts without it. XInput covers
 * Xbox pads and whatever Steam Input / DS4Windows present as one. The first
 * connected slot of the four is used. */
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
static int loaded, slot = -1;
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

int xi_poll(HostPad *st, char *name, size_t n) {
    XState xs;
    if (!loaded) load();
    if (!x_get) return 0;
    memset(&xs, 0, sizeof xs);
    if (slot >= 0 && x_get((DWORD)slot, &xs) != ERROR_SUCCESS) slot = -1;
    if (slot < 0) {                                  /* asking an empty slot is slow: look once per second */
        u64 now = plat_time_ns();
        if (now < next_scan) return 0;
        next_scan = now + 1000000000ull;
        for (int i = 0; i < 4 && slot < 0; i++) { memset(&xs, 0, sizeof xs); if (x_get((DWORD)i, &xs) == ERROR_SUCCESS) slot = i; }
        if (slot < 0) return 0;
    }
    st->buttons = xs.Gamepad.wButtons & 0xF3FF;      /* the HPAD_* bits are XInput's */
    st->lt = xs.Gamepad.bLeftTrigger; st->rt = xs.Gamepad.bRightTrigger;
    st->lx = xs.Gamepad.sThumbLX; st->ly = xs.Gamepad.sThumbLY; st->rx = xs.Gamepad.sThumbRX; st->ry = xs.Gamepad.sThumbRY;
    snprintf(name, n, "XInput controller %d", slot + 1);
    return 1;
}
/* PS2 big motor -> the low-frequency (left) motor, small motor -> the high-frequency (right) one. */
void xi_rumble(int small, int big) {
    if (slot < 0 || !x_set) return;
    XVibration v = { (WORD)(big * 257), (WORD)(small * 257) };
    x_set((DWORD)slot, &v);
}
#endif
