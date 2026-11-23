/* pad_hid.c — Sony pads on Windows, read directly as HID devices (pshid_*;
 * XInput does not see them). DualShock 4 (054c:05c4, 054c:09cc, and the USB
 * wireless adapter 054c:0ba0), DualSense (054c:0ce6), DualSense Edge
 * (054c:0df2), by USB or Bluetooth. Report formats: pad_ps.c.
 *
 * Everything that can wait happens on one background thread: looking for
 * the device (SetupAPI, once per second while there is none), opening it,
 * switching a Bluetooth pad to its extended reports (reading the
 * calibration feature report does that), reading input reports (overlapped)
 * and writing vibration reports. The game thread only copies the last
 * decoded state under a lock and posts vibration requests.
 *
 * setupapi.dll and hid.dll are loaded at run time from System32, so the
 * .exe has no import for them. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <stdarg.h>
#include "rt.h"
#include "plat.h"
#include "pad_ps.h"

static HDEVINFO (WINAPI *sd_GetClassDevs)(const GUID *, PCWSTR, HWND, DWORD);
static BOOL (WINAPI *sd_EnumInterfaces)(HDEVINFO, PSP_DEVINFO_DATA, const GUID *, DWORD, PSP_DEVICE_INTERFACE_DATA);
static BOOL (WINAPI *sd_InterfaceDetail)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA, PSP_DEVICE_INTERFACE_DETAIL_DATA_W, DWORD, PDWORD, PSP_DEVINFO_DATA);
static BOOL (WINAPI *sd_Destroy)(HDEVINFO);
static BOOLEAN (NTAPI *hd_GetAttributes)(HANDLE, PHIDD_ATTRIBUTES);
static BOOLEAN (NTAPI *hd_GetPreparsedData)(HANDLE, PHIDP_PREPARSED_DATA *);
static BOOLEAN (NTAPI *hd_FreePreparsedData)(PHIDP_PREPARSED_DATA);
static NTSTATUS (NTAPI *hd_GetCaps)(PHIDP_PREPARSED_DATA, PHIDP_CAPS);

static const GUID hid_guid = { 0x4D1E55B2, 0xF16F, 0x11CF, { 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };
static const struct { u16 pid; int model, dongle; const char *name; } known[] = {
    { 0x05C4, PS_DS4, 0, "DualShock 4" }, { 0x09CC, PS_DS4, 0, "DualShock 4" },
    { 0x0BA0, PS_DS4, 1, "DualShock 4 (USB wireless adapter)" },
    { 0x0CE6, PS_DUALSENSE, 0, "DualSense" }, { 0x0DF2, PS_DUALSENSE, 0, "DualSense Edge" },
};

/* shared with the game thread, under `lock` */
static CRITICAL_SECTION lock;
static HostPad last;
static int connected;
static char dev_name[64], status[160] = "HID: not looked yet";
static int want_small, want_big, rumble_dirty;
/* thread control */
static HANDLE thread, wake, flushed;
static volatile LONG wanted;
static int started, exit_hooked;
/* owned by the thread */
static HANDLE dev = INVALID_HANDLE_VALUE;
static int model, is_bt, is_dongle, can_write, in_len, out_len, feat_len;

static void set_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_status(const char *fmt, ...) {
    char t[160]; va_list ap; va_start(ap, fmt); vsnprintf(t, sizeof t, fmt, ap); va_end(ap);
    EnterCriticalSection(&lock); memcpy(status, t, sizeof status); LeaveCriticalSection(&lock);
}
static int load_dlls(void) {
    HMODULE s = LoadLibraryExA("setupapi.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32), h = LoadLibraryExA("hid.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!s || !h) return 0;
    sd_GetClassDevs = (void *)GetProcAddress(s, "SetupDiGetClassDevsW");
    sd_EnumInterfaces = (void *)GetProcAddress(s, "SetupDiEnumDeviceInterfaces");
    sd_InterfaceDetail = (void *)GetProcAddress(s, "SetupDiGetDeviceInterfaceDetailW");
    sd_Destroy = (void *)GetProcAddress(s, "SetupDiDestroyDeviceInfoList");
    hd_GetAttributes = (void *)GetProcAddress(h, "HidD_GetAttributes");
    hd_GetPreparsedData = (void *)GetProcAddress(h, "HidD_GetPreparsedData");
    hd_FreePreparsedData = (void *)GetProcAddress(h, "HidD_FreePreparsedData");
    hd_GetCaps = (void *)GetProcAddress(h, "HidP_GetCaps");
    return sd_GetClassDevs && sd_EnumInterfaces && sd_InterfaceDetail && sd_Destroy && hd_GetAttributes &&
           hd_GetPreparsedData && hd_FreePreparsedData && hd_GetCaps;
}

/* Read feature report `id` (what HidD_GetFeature does, but overlapped and with a time limit). */
static void get_feature(HANDLE h, int id) {
    static u8 f[4096]; OVERLAPPED ov; DWORD n = 0;
    DWORD len = feat_len > 64 ? (feat_len > 4096 ? 4096 : (DWORD)feat_len) : 64;
    memset(f, 0, sizeof f); f[0] = (u8)id;
    memset(&ov, 0, sizeof ov); ov.hEvent = CreateEventW(0, TRUE, FALSE, 0);
    if (!DeviceIoControl(h, 0x000B0192 /* IOCTL_HID_GET_FEATURE */, f, len, f, len, &n, &ov) && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, 1000) != WAIT_OBJECT_0) CancelIoEx(h, &ov);
        GetOverlappedResult(h, &ov, &n, TRUE);
    }
    CloseHandle(ov.hEvent);
}
static void drop_device(const char *why) {
    if (dev != INVALID_HANDLE_VALUE) { CancelIo(dev); CloseHandle(dev); dev = INVALID_HANDLE_VALUE; }
    EnterCriticalSection(&lock); connected = 0; LeaveCriticalSection(&lock);
    if (why) set_status("HID: %s", why);
}
/* Is this interface one of the pads? Opens it for reading (and writing, for vibration). */
static int try_path(const WCHAR *path) {
    WCHAR low[512]; size_t n = 0;
    for (; path[n] && n < 511; n++) low[n] = (path[n] >= L'A' && path[n] <= L'Z') ? (WCHAR)(path[n] + 32) : path[n];
    low[n] = 0;
    if (!wcsstr(low, L"054c")) return 0;             /* the vendor ID is part of the path, USB and Bluetooth alike */
    HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);     /* no access: attributes only */
    if (h == INVALID_HANDLE_VALUE) return 0;
    HIDD_ATTRIBUTES at; memset(&at, 0, sizeof at); at.Size = sizeof at;
    int k = -1;
    if (hd_GetAttributes(h, &at) && at.VendorID == 0x054C)
        for (unsigned i = 0; i < sizeof known / sizeof *known; i++) if (known[i].pid == at.ProductID) k = (int)i;
    CloseHandle(h);
    if (k < 0) return 0;
    can_write = 1;
    h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, 0);
    if (h == INVALID_HANDLE_VALUE) {
        can_write = 0;
        h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, 0);
    }
    if (h == INVALID_HANDLE_VALUE) {
        set_status("HID: %s found but it cannot be opened (error %lu; another program may hold it exclusively)", known[k].name, GetLastError());
        return -1;
    }
    PHIDP_PREPARSED_DATA pp = 0; HIDP_CAPS caps; memset(&caps, 0, sizeof caps);
    if (!hd_GetPreparsedData(h, &pp) || hd_GetCaps(pp, &caps) != HIDP_STATUS_SUCCESS) { if (pp) hd_FreePreparsedData(pp); CloseHandle(h); return 0; }
    hd_FreePreparsedData(pp);
    in_len = caps.InputReportByteLength; out_len = caps.OutputReportByteLength; feat_len = caps.FeatureReportByteLength;
    if (in_len < 10 || in_len > 4096) { CloseHandle(h); return 0; }
    model = known[k].model; is_dongle = known[k].dongle;
    if (is_dongle) {                                 /* the adapter reports all the time; is a pad linked to it? */
        static u8 pb[4096]; OVERLAPPED ov; DWORD got = 0; int ok = 0;
        memset(&ov, 0, sizeof ov); ov.hEvent = CreateEventW(0, TRUE, FALSE, 0);
        if (ReadFile(h, pb, (DWORD)in_len, &got, &ov)) ok = 1;
        else if (GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ov.hEvent, 200) != WAIT_OBJECT_0) CancelIoEx(h, &ov);
            ok = GetOverlappedResult(h, &ov, &got, TRUE) != 0;
        }
        CloseHandle(ov.hEvent);
        if (!ok || got < 32 || pb[0] != 0x01 || (pb[31] & 0x04)) { CloseHandle(h); return -2; }      /* no: look further */
    }
    /* Bluetooth: the path carries the Bluetooth HID service UUID; its reports are longer than the 64 bytes of USB */
    is_bt = !is_dongle && (wcsstr(low, L"00001124-0000-1000-8000-00805f9b34fb") != 0 || in_len > 64);
    dev = h;
    EnterCriticalSection(&lock); snprintf(dev_name, sizeof dev_name, "%s", known[k].name); LeaveCriticalSection(&lock);
    set_status("HID, %s%s", is_bt ? "Bluetooth" : "USB", can_write ? "" : ", read-only: no vibration");
    if (is_bt) {                                     /* ask for a calibration report: the pad then sends its full reports */
        get_feature(dev, 0x05);                      /* DualSense, and DualShock 4 over Bluetooth */
        if (model == PS_DS4) get_feature(dev, 0x02);
    }
    return 1;
}
static void find_device(void) {
    HDEVINFO set = sd_GetClassDevs(&hid_guid, 0, 0, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) { set_status("HID: device list not available (error %lu)", GetLastError()); return; }
    /* Goes on past a pad that cannot be opened and past a wireless adapter with no pad linked: there may be another pad. */
    int count = 0, r = 0, failed = 0, idle_adapter = 0;
    for (DWORD i = 0; r <= 0; i++) {
        SP_DEVICE_INTERFACE_DATA ifd; memset(&ifd, 0, sizeof ifd); ifd.cbSize = sizeof ifd;
        if (!sd_EnumInterfaces(set, 0, &hid_guid, i, &ifd)) break;
        static union { SP_DEVICE_INTERFACE_DETAIL_DATA_W d; u8 raw[2048]; } det;
        memset(&det, 0, sizeof det); det.d.cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!sd_InterfaceDetail(set, &ifd, &det.d, sizeof det - 2, 0, 0)) continue;
        count++;
        r = try_path(det.d.DevicePath);
        if (r == -1) failed = 1; else if (r == -2) idle_adapter = 1;
    }
    sd_Destroy(set);
    if (r > 0 || failed) return;                     /* status already says which pad, or why it could not be opened */
    if (idle_adapter) set_status("HID: %d devices, a DualShock 4 USB wireless adapter with no pad linked to it", count);
    else set_status("HID: %d devices, no DualShock 4 / DualSense among them", count);
}

/* Returns 0 if the device should be given up (so that another pad can be looked for). */
static int take_report(const u8 *d, int n) {
    HostPad h;
    if (is_dongle && d[0] == 0x01 && n >= 32 && (d[31] & 0x04)) return 0;        /* adapter: its pad is gone */
    if (!ps_parse(model, is_bt, d, n, &h, 0)) return 1;
    EnterCriticalSection(&lock); last = h; connected = 1; LeaveCriticalSection(&lock);
    return 1;
}
static void send_rumble(OVERLAPPED *ov) {
    static u8 buf[4096]; u8 rep[78]; int s, b; DWORD done = 0;
    EnterCriticalSection(&lock); s = want_small; b = want_big; rumble_dirty = 0; LeaveCriticalSection(&lock);
    if (dev != INVALID_HANDLE_VALUE && can_write) {
        int n = ps_rumble_report(model, is_bt, s, b, rep);
        int len = out_len > n ? (out_len > 4096 ? 4096 : out_len) : n;   /* Windows wants the full report length */
        memset(buf, 0, sizeof buf); memcpy(buf, rep, (size_t)n);
        ResetEvent(ov->hEvent);
        DWORD err = 0;
        if (!WriteFile(dev, buf, (DWORD)len, &done, ov)) {
            err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                if (WaitForSingleObject(ov->hEvent, 200) != WAIT_OBJECT_0) CancelIoEx(dev, ov);
                err = GetOverlappedResult(dev, ov, &done, TRUE) ? 0 : GetLastError();
            }
        }
        static DWORD last_err;                       /* one log line per distinct error */
        if (err && err != last_err) { last_err = err; rt_log("[pad] HID: vibration write failed (error %lu, len %d)\n", err, len); }
    }
    SetEvent(flushed);
}
static DWORD WINAPI reader(void *arg) {
    static u8 buf[4096];
    OVERLAPPED rd, wr; int pending = 0, quiet = 0;
    (void)arg;
    memset(&rd, 0, sizeof rd); memset(&wr, 0, sizeof wr);
    rd.hEvent = CreateEventW(0, TRUE, FALSE, 0); wr.hEvent = CreateEventW(0, TRUE, FALSE, 0);
    if (!load_dlls()) { set_status("HID: setupapi.dll / hid.dll not available"); return 0; }
    for (;;) {
        if (!wanted) {                               /* XInput has the pad: stay out of the way */
            if (dev != INVALID_HANDLE_VALUE) {
                DWORD n; CancelIo(dev); if (pending) GetOverlappedResult(dev, &rd, &n, TRUE);
                pending = 0; drop_device("idle (XInput is in use)");
            }
            WaitForSingleObject(wake, 1000);
            continue;
        }
        if (dev == INVALID_HANDLE_VALUE) {
            find_device(); pending = 0; quiet = 0;
            if (dev == INVALID_HANDLE_VALUE) { WaitForSingleObject(wake, 1000); continue; }
        }
        DWORD n = 0; int got = 0;
        if (!pending) {
            ResetEvent(rd.hEvent);
            if (ReadFile(dev, buf, (DWORD)in_len, &n, &rd)) got = 1;
            else if (GetLastError() == ERROR_IO_PENDING) pending = 1;
            else { drop_device("the pad went away"); continue; }
        }
        if (pending) {
            HANDLE hs[2] = { rd.hEvent, wake };
            DWORD w = WaitForMultipleObjects(2, hs, FALSE, 500);
            if (w == WAIT_OBJECT_0) {
                pending = 0;
                if (GetOverlappedResult(dev, &rd, &n, FALSE)) got = 1;
                else { drop_device("the pad went away"); continue; }
            } else if (w == WAIT_TIMEOUT && ++quiet >= 4) {              /* nothing for 2 s: not connected (e.g. pad asleep) */
                EnterCriticalSection(&lock); connected = 0; LeaveCriticalSection(&lock);
            }
        }
        if (got) {
            quiet = 0;
            if (n > 0 && !take_report(buf, (int)n)) { drop_device("the pad linked to the wireless adapter went away"); WaitForSingleObject(wake, 1000); continue; }
        }
        if (rumble_dirty) send_rumble(&wr);
    }
}

static void at_exit(void) {                          /* stop the motors before the process goes away */
    if (!connected) return;
    EnterCriticalSection(&lock); want_small = want_big = 0; rumble_dirty = 1; LeaveCriticalSection(&lock);
    ResetEvent(flushed); SetEvent(wake);
    WaitForSingleObject(flushed, 150);
}
void pshid_want(int on) {
    if (!started) {
        if (!on) return;
        started = 1;
        InitializeCriticalSection(&lock);
        wake = CreateEventW(0, FALSE, FALSE, 0); flushed = CreateEventW(0, TRUE, FALSE, 0);
        thread = CreateThread(0, 0, reader, 0, 0, 0);
        if (!thread) snprintf(status, sizeof status, "HID: reader thread could not be started");
    }
    if ((LONG)on != wanted) { wanted = on; SetEvent(wake); }
}
int pshid_poll(HostPad *st, char *name, size_t n) {
    int c;
    if (!started) return 0;
    EnterCriticalSection(&lock);
    c = connected; if (c) { *st = last; snprintf(name, n, "%s", dev_name); }
    LeaveCriticalSection(&lock);
    return c;
}
const char *pshid_status(void) {
    static char copy[160];
    if (!started) return status;
    EnterCriticalSection(&lock); memcpy(copy, status, sizeof copy); LeaveCriticalSection(&lock);
    return copy;
}
void pshid_rumble(int small, int big) {
    if (!started) return;
    if ((small || big) && !exit_hooked) { exit_hooked = 1; atexit(at_exit); }
    EnterCriticalSection(&lock); want_small = small; want_big = big; rumble_dirty = 1; LeaveCriticalSection(&lock);
    SetEvent(wake);
}
#endif
