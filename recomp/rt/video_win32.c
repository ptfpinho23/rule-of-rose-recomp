/* video_win32.c — the game window on Windows (plat.h vwin_*): a plain
 * Win32 window (or borderless fullscreen), the finished picture copied 1:1. The window
 * belongs to the host thread that runs the guest, which pumps its messages
 * once per frame. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "rt.h"
#include "plat.h"

static HWND hwnd;
static u16 *pad_ptr;
static int want_quit, is_full;
static WINDOWPLACEMENT saved_place = { sizeof(WINDOWPLACEMENT) };
#define WINDOWED_STYLE (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX)

/* Borderless window covering the monitor the window is on, and back. */
static void set_fullscreen(int on) {
    if (on == is_full) return;
    if (on) {
        MONITORINFO mi; mi.cbSize = sizeof mi;
        GetWindowPlacement(hwnd, &saved_place);
        if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return;
        SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongPtrW(hwnd, GWL_STYLE, WINDOWED_STYLE | WS_VISIBLE);
        SetWindowPlacement(hwnd, &saved_place);
        SetWindowPos(hwnd, 0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    is_full = on;
}

static u16 key_bit(WPARAM vk, LPARAM lp) {
    if (vk == VK_SHIFT) vk = MapVirtualKeyW((UINT)((lp >> 16) & 0xFF), MAPVK_VSC_TO_VK_EX);   /* left or right shift */
    switch (vk) {
    case VK_UP: return 0x0010; case VK_RIGHT: return 0x0020; case VK_DOWN: return 0x0040; case VK_LEFT: return 0x0080;
    case 'Z': return 0x4000; case 'X': return 0x2000; case 'A': return 0x8000; case 'S': return 0x1000;   /* cross circle square triangle */
    case VK_RETURN: return 0x0008; case VK_BACK: case VK_RSHIFT: return 0x0001;                             /* start select */
    case 'Q': return 0x0400; case 'W': return 0x0800; case 'E': return 0x0100; case 'R': return 0x0200;   /* L1 R1 L2 R2 */
    }
    return 0;
}
static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE: want_quit = 1; return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (wp == VK_ESCAPE) { want_quit = 1; return 0; }
        if (wp == VK_F11 || (wp == VK_RETURN && (lp & (1 << 29)))) {         /* F11 / Alt+Enter */
            if (!(lp & (1 << 30))) set_fullscreen(!is_full);                 /* not on auto-repeat */
            return 0;
        }
        if (pad_ptr) *pad_ptr &= (u16)~key_bit(wp, lp);
        if (msg == WM_KEYDOWN) return 0;
        break;
    case WM_KEYUP: case WM_SYSKEYUP:
        if (pad_ptr) *pad_ptr |= key_bit(wp, lp);
        if (msg == WM_KEYUP) return 0;
        break;
    case WM_KILLFOCUS: if (pad_ptr) *pad_ptr = 0xFFFF; break;      /* key-up messages go elsewhere now */
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
int vwin_open(int w, int h, int fullscreen) {
    SetProcessDPIAware();                                /* sizes are real pixels; no bitmap stretching by the system */
    HINSTANCE inst = GetModuleHandleW(0);
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = wndproc; wc.hInstance = inst; wc.lpszClassName = L"RuleOfRoseWindow";
    wc.hCursor = LoadCursorW(0, (LPCWSTR)IDC_ARROW); wc.hIcon = LoadIconW(0, (LPCWSTR)IDI_APPLICATION);
    if (!RegisterClassW(&wc)) return 0;
    DWORD style = WINDOWED_STYLE;
    RECT r = { 0, 0, 0, 0 }, wa;
    AdjustWindowRect(&r, style, FALSE);                  /* frame and title bar around the client area */
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0))
        video_fit(&w, &h, (wa.right - wa.left) - (r.right - r.left), (wa.bottom - wa.top) - (r.bottom - r.top));
    r.left = r.top = 0; r.right = w; r.bottom = h;
    AdjustWindowRect(&r, style, FALSE);
    hwnd = CreateWindowExW(0, wc.lpszClassName, L"Rule of Rose", style, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, 0, 0, inst, 0);
    if (!hwnd) return 0;
    /* Started by double-click, the console window exists only for this
     * process: drop it. Started from a shell or with redirected output, the
     * log stays where it is. */
    DWORD pids[2];
    if (GetConsoleProcessList(pids, 2) == 1 && !getenv("ROR_KEEP_CONSOLE")) FreeConsole();
    ShowWindow(hwnd, SW_SHOWNORMAL);
    if (fullscreen) set_fullscreen(1);
    SetForegroundWindow(hwnd);
    return 1;
}
void vwin_pump(u16 *pad_buttons) {
    pad_ptr = pad_buttons;
    MSG m;
    while (PeekMessageW(&m, 0, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) want_quit = 1;
        DispatchMessageW(&m);
    }
    if (want_quit) exit(0);
}
void vwin_size(int *w, int *h) {
    RECT r;
    if (GetClientRect(hwnd, &r)) { *w = r.right; *h = r.bottom; }    /* 0 x 0 while minimised */
}
void vwin_show(const u32 *px, int w, int h) {
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; /* top-down */
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(hwnd);
    SetDIBitsToDevice(dc, 0, 0, (DWORD)w, (DWORD)h, 0, 0, 0, (UINT)h, px, &bi, DIB_RGB_COLORS);
    ReleaseDC(hwnd, dc);
}
void vwin_flush(void) { }
void vwin_native(void **a, void **b) { *a = GetModuleHandleW(0); *b = hwnd; }
int vwin_refresh_hz(void) {
    MONITORINFOEXW mi; DEVMODEW dm;
    memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi; memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm;
    if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), (MONITORINFO *)&mi) &&
        EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) return (int)dm.dmDisplayFrequency;
    return 60;
}
#endif
