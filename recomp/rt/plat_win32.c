/* plat_win32.c — plat.h for Windows (x64, Windows 10 or later).
 *
 * Strings cross this file as UTF-8 and are converted to UTF-16 for the wide
 * Win32/CRT calls, so paths with spaces or non-ASCII characters work for
 * the command line, the disc image, the memory-card directory and $ROR_WAV.
 *
 * Differences from plat_posix.c:
 *  - coroutines are Fibers (the stack is reserved, committed on demand);
 *  - the guest address space is assembled from individual reservations and
 *    file-mapping views instead of one reservation with mappings on top
 *    (Windows cannot map over a reservation without the placeholder API);
 *  - host faults arrive through a vectored exception handler;
 *  - there is no sampling profiler ($ROR_PROF is ignored with a notice). */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <xmmintrin.h>
#include <io.h>
#include "rt.h"
#include "plat.h"

/* ---- UTF-8 <-> UTF-16 ---------------------------------------------------- */
#define WPATH 2048
static wchar_t *to_wide(const char *s, wchar_t *buf, int n) {
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, buf, n)) buf[0] = 0;
    return buf;
}
static char *to_utf8_dup(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, 0, 0, 0, 0);
    char *s = malloc((size_t)(n > 0 ? n : 1));
    if (n <= 0 || !WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, 0, 0)) s[0] = 0;
    return s;
}

/* ---- process ------------------------------------------------------------ */
static void (*fault_cb)(const char *, void *, int);
static void (*int_cb)(const char *);

void plat_init(int *argc, char ***argv) {
    /* no "program has stopped working" dialogs: a crash must end the process */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    int wargc = 0;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv && wargc > 0) {
        char **v = malloc(((size_t)wargc + 1) * sizeof *v);
        for (int i = 0; i < wargc; i++) v[i] = to_utf8_dup(wargv[i]);
        v[wargc] = 0;
        *argc = wargc; *argv = v;
        LocalFree(wargv);
    }
    timeBeginPeriod(1);                      /* 1 ms scheduler granularity for the frame pacing */
    { const char *e = getenv("ROR_AFFINITY"); if (e) SetProcessAffinityMask(GetCurrentProcess(), (DWORD_PTR)strtoull(e, 0, 16)); }     /* test: hexadecimal mask of the logical processors to use */
    plat_thread_role(0);
}
void plat_exe_dir(char *out, size_t n) {
    wchar_t w[WPATH];
    DWORD len = GetModuleFileNameW(0, w, WPATH);
    if (!len || len >= WPATH) { snprintf(out, n, "."); return; }
    char *s = to_utf8_dup(w);
    for (char *p = s; *p; p++) if (*p == '\\') *p = '/';
    char *sl = strrchr(s, '/');
    if (sl && sl != s) *sl = 0;
    snprintf(out, n, "%s", s);
    free(s);
}
static struct EnvEnt { struct EnvEnt *next; char *name, *value; } *env_cache;
const char *plat_getenv(const char *name) {
    for (struct EnvEnt *e = env_cache; e; e = e->next) if (!strcmp(e->name, name)) return e->value;
    wchar_t wn[256]; to_wide(name, wn, 256);
    DWORD n = GetEnvironmentVariableW(wn, 0, 0);
    if (!n) return 0;
    wchar_t *wv = malloc(((size_t)n + 1) * sizeof *wv);
    if (!GetEnvironmentVariableW(wn, wv, n + 1)) { free(wv); return 0; }
    struct EnvEnt *e = malloc(sizeof *e);
    e->name = _strdup(name); e->value = to_utf8_dup(wv); e->next = env_cache; env_cache = e;
    free(wv);
    return e->value;
}
void plat_setenv(const char *name, const char *value) {
    wchar_t wn[256], wv[WPATH];
    SetEnvironmentVariableW(to_wide(name, wn, 256), to_wide(value, wv, WPATH));
    for (struct EnvEnt *e = env_cache; e; e = e->next)
        if (!strcmp(e->name, name)) { e->value = _strdup(value); return; }   /* old value may still be referenced */
    struct EnvEnt *e = malloc(sizeof *e);
    e->name = _strdup(name); e->value = _strdup(value); e->next = env_cache; env_cache = e;
}
void plat_exit_now(int code) {
    TerminateProcess(GetCurrentProcess(), (UINT)code);
    for (;;) Sleep(1000);
}

static void raw_err(const char *s) {         /* usable with almost no stack and no CRT state */
    DWORD w; WriteFile(GetStdHandle(STD_ERROR_HANDLE), s, (DWORD)strlen(s), &w, 0);
}
static LONG CALLBACK on_exception(EXCEPTION_POINTERS *ep) {
    static volatile LONG in_fault;
    const EXCEPTION_RECORD *r = ep->ExceptionRecord;
    const char *name;
    switch (r->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:      name = "access violation"; break;
    case EXCEPTION_IN_PAGE_ERROR:         name = "in-page error"; break;
    case EXCEPTION_STACK_OVERFLOW:        name = "stack overflow"; break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:   name = "illegal instruction"; break;
    case EXCEPTION_PRIV_INSTRUCTION:      name = "privileged instruction"; break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:    name = "integer divide by zero"; break;
    case EXCEPTION_INT_OVERFLOW:          name = "integer overflow"; break;
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: name = "array bounds exceeded"; break;
    case EXCEPTION_DATATYPE_MISALIGNMENT: name = "misaligned access"; break;
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: case EXCEPTION_FLT_INVALID_OPERATION: case EXCEPTION_FLT_OVERFLOW:
    case EXCEPTION_FLT_UNDERFLOW: case EXCEPTION_FLT_INEXACT_RESULT: case EXCEPTION_FLT_DENORMAL_OPERAND:
    case EXCEPTION_FLT_STACK_CHECK:       name = "floating-point exception"; break;
    case 0xC0000409u /* STATUS_STACK_BUFFER_OVERRUN / fail-fast */: name = "fail-fast"; break;
    default: return EXCEPTION_CONTINUE_SEARCH;   /* not a crash (debugger output, C++ exceptions, ...) */
    }
    if (InterlockedExchange(&in_fault, 1) || r->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        /* a fault while reporting one, or no stack left to format a report */
        raw_err(r->ExceptionCode == EXCEPTION_STACK_OVERFLOW ? "\n[FATAL] host stack overflow\n"
                                                             : "\n[FATAL] fault while reporting a fault\n");
        TerminateProcess(GetCurrentProcess(), 71);
    }
    char what[160];
    u64 pc = (u64)(uintptr_t)r->ExceptionAddress, img = (u64)(uintptr_t)GetModuleHandleW(0);
    snprintf(what, sizeof what, "exception 0x%08lX (%s) at host pc %p (image+0x%llx, thread %lu)",
             (unsigned long)r->ExceptionCode, name, r->ExceptionAddress, (unsigned long long)(pc - img),
             (unsigned long)GetCurrentThreadId());
    int has_addr = (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || r->ExceptionCode == EXCEPTION_IN_PAGE_ERROR)
                   && r->NumberParameters >= 2;
    if (fault_cb) fault_cb(what, has_addr ? (void *)r->ExceptionInformation[1] : 0, has_addr);
    else { raw_err("\n[FATAL] "); raw_err(what); raw_err("\n"); }
    TerminateProcess(GetCurrentProcess(), 71);
    return EXCEPTION_CONTINUE_SEARCH;
}
static BOOL WINAPI on_ctrl(DWORD type) {     /* runs on a thread of its own */
    if (int_cb) int_cb(type == CTRL_C_EVENT ? "Ctrl-C" : type == CTRL_BREAK_EVENT ? "Ctrl-Break" : "console closed");
    TerminateProcess(GetCurrentProcess(), 72);
    return TRUE;
}
void plat_install_handlers(void (*on_fault)(const char *, void *, int), void (*on_interrupt)(const char *)) {
    fault_cb = on_fault; int_cb = on_interrupt;
    AddVectoredExceptionHandler(1, on_exception);
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    if (getenv("ROR_PROF")) fprintf(stderr, "[prof] ROR_PROF is not supported in the Windows build; ignored\n");
}
void prof_reset(void) { }

/* ---- guest address space ------------------------------------------------ */
#define GRAN 0x10000ull                       /* allocation granularity */
static void map_fail(const char *what, u64 at) {
    fprintf(stderr, "guest address space: %s at %p failed (Windows error %lu)\n", what, (void *)(uintptr_t)at,
            (unsigned long)GetLastError());
    exit(1);
}
void plat_guest_map(void *base, u64 total, const PlatMap *maps, int n) {
    u64 b = (u64)(uintptr_t)base;
    /* The whole range must be free: reserve it in one piece to find out, then
     * release it and rebuild it from reservations (the gaps) and views. */
    void *probe = VirtualAlloc(base, (SIZE_T)total, MEM_RESERVE, PAGE_NOACCESS);
    if (probe != base) {
        MEMORY_BASIC_INFORMATION mi; u64 a = b;
        while (a < b + total && VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi) && mi.State == MEM_FREE)
            a = (u64)(uintptr_t)mi.BaseAddress + mi.RegionSize;
        fprintf(stderr, "cannot reserve the guest address space at %p: address %p is already in use by the process\n",
                base, (void *)(uintptr_t)a);
        exit(1);
    }
    VirtualFree(base, 0, MEM_RELEASE);
    HANDLE obj[16]; int have[16] = { 0 };
    int order[64];
    if (n > 64) { fprintf(stderr, "guest address space: too many mappings\n"); exit(1); }
    for (int i = 0; i < n; i++) order[i] = i;
    for (int i = 1; i < n; i++)               /* by offset */
        for (int k = i; k > 0 && maps[order[k]].offset < maps[order[k - 1]].offset; k--) { int t = order[k]; order[k] = order[k - 1]; order[k - 1] = t; }
    u64 cur = 0;
    for (int i = 0; i <= n; i++) {
        u64 off = i < n ? maps[order[i]].offset : total;
        if (off < cur || (off & (GRAN - 1))) { fprintf(stderr, "guest address space: bad mapping offset 0x%llx\n", (unsigned long long)off); exit(1); }
        if (off > cur && !VirtualAlloc((void *)(uintptr_t)(b + cur), (SIZE_T)(off - cur), MEM_RESERVE, PAGE_NOACCESS))
            map_fail("reserve", b + cur);
        if (i == n) break;
        const PlatMap *m = &maps[order[i]];
        u64 span = ((u64)m->size + GRAN - 1) & ~(GRAN - 1);
        void *at = (void *)(uintptr_t)(b + off);
        if (m->object < 0) {                  /* private: reserve the 64 KiB-rounded block, commit what is asked for */
            if (!VirtualAlloc(at, (SIZE_T)span, MEM_RESERVE, PAGE_NOACCESS)) map_fail("reserve", b + off);
            if (!VirtualAlloc(at, m->size, MEM_COMMIT, PAGE_READWRITE)) map_fail("commit", b + off);
        } else {
            if (m->object >= 16) { fprintf(stderr, "guest address space: bad object\n"); exit(1); }
            if (!have[m->object]) {
                obj[m->object] = CreateFileMappingW(INVALID_HANDLE_VALUE, 0, PAGE_READWRITE, 0, m->size, 0);
                if (!obj[m->object]) map_fail("create memory object", b + off);
                have[m->object] = 1;
            }
            if (MapViewOfFileEx(obj[m->object], FILE_MAP_ALL_ACCESS, 0, 0, m->size, at) != at) map_fail("map view", b + off);
        }
        cur = off + span;
    }
    for (int i = 0; i < 16; i++) if (have[i]) CloseHandle(obj[i]);   /* the views keep the objects alive */
}

/* ---- coroutines ---------------------------------------------------------
 * `own` is the fiber created for the coroutine; `at` is where plat_co_switch
 * last saved a context into the slot (normally the same fiber). A fiber
 * cannot be deleted while it is running, so restarting a coroutine from its
 * own stack parks the old fiber on a list that is emptied later. */
struct PlatCo { void *own, *at; void (*entry)(void); size_t stack; unsigned csr; };
static void *zombies[64]; static int zombie_n;

static void ensure_fiber(void) {
    if (!IsThreadAFiber() && !ConvertThreadToFiberEx(0, FIBER_FLAG_FLOAT_SWITCH)) {
        fprintf(stderr, "cannot convert the main thread to a fiber (Windows error %lu)\n", (unsigned long)GetLastError());
        exit(1);
    }
}
static void reap(void) {
    void *self = GetCurrentFiber(); int k = 0;
    for (int i = 0; i < zombie_n; i++) { if (zombies[i] == self) zombies[k++] = zombies[i]; else DeleteFiber(zombies[i]); }
    zombie_n = k;
}
static void retire(void *fiber) {
    if (!fiber) return;
    if (fiber != GetCurrentFiber()) { DeleteFiber(fiber); return; }
    if (zombie_n == 64) { fprintf(stderr, "coroutine: too many abandoned stacks\n"); exit(1); }
    zombies[zombie_n++] = fiber;
}
static void CALLBACK fiber_main(void *arg) {
    PlatCo *co = arg;
    _mm_setcsr(co->csr);                      /* like a ucontext made on POSIX, start with the creator's FP mode */
    co->entry();
    fprintf(stderr, "coroutine entry returned\n");
    exit(1);
}
PlatCo *plat_co_new(size_t stack_size) {
    PlatCo *co = calloc(1, sizeof *co);
    if (!co) { fprintf(stderr, "coroutine: out of memory\n"); exit(1); }
    co->stack = stack_size;
    return co;
}
void plat_co_start(PlatCo *co, void (*entry)(void)) {
    ensure_fiber();
    reap();
    retire(co->own);
    co->entry = entry; co->csr = _mm_getcsr();
    co->own = CreateFiberEx(64 << 10, co->stack, FIBER_FLAG_FLOAT_SWITCH, fiber_main, co);
    if (!co->own) { fprintf(stderr, "cannot create a coroutine stack (Windows error %lu)\n", (unsigned long)GetLastError()); exit(1); }
    co->at = co->own;
}
void plat_co_switch(PlatCo *from, PlatCo *to) {
    ensure_fiber();
    from->at = GetCurrentFiber();
    SwitchToFiber(to->at);
}
void plat_co_jump(PlatCo *to) {
    ensure_fiber();
    SwitchToFiber(to->at);
    abort();
}
void plat_co_free(PlatCo *co) {
    if (!co) return;
    retire(co->own);
    free(co);
}

/* ---- time --------------------------------------------------------------- */
u64 plat_time_ns(void) {
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    u64 q = (u64)t.QuadPart, f = (u64)freq.QuadPart;
    return q / f * 1000000000ull + q % f * 1000000000ull / f;
}
u64 plat_cpu_ns(void) {
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    u64 kk = ((u64)k.dwHighDateTime << 32) | k.dwLowDateTime, uu = ((u64)u.dwHighDateTime << 32) | u.dwLowDateTime;
    return (kk + uu) * 100;
}
/* The game thread and, with frame interpolation, the presenter are both busy most of the time. When the
 * scheduler puts them on the two logical processors of one core, each runs at little more than half speed
 * and the game drops into slow motion in heavy scenes (seen in about one run in four on a 12-core
 * processor with two threads per core). Each gets a preferred processor on a core of its own - a hint, not
 * a restriction - and a priority just above normal, so that background work does not push it aside.
 * ROR_NOPIN=1 leaves both to the system. */
void plat_thread_role(int role) {
    if (getenv("ROR_NOPIN")) return;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION info[256]; DWORD len = sizeof info; DWORD_PTR pm = 0, sm = 0;
    if (!GetLogicalProcessorInformation(info, &len) || !GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm)) return;
    int first[64], n = 0;                         /* first usable logical processor of every core */
    for (DWORD i = 0; i < len / sizeof info[0] && n < 64; i++) {
        if (info[i].Relationship != RelationProcessorCore) continue;
        DWORD_PTR m = info[i].ProcessorMask & pm;
        for (int b = 0; b < 64; b++) if (m & ((DWORD_PTR)1 << b)) { first[n++] = b; break; }
    }
    if (n < 4) return;                            /* few cores: nothing to gain by choosing */
    SetThreadIdealProcessor(GetCurrentThread(), (DWORD)first[role == 0 ? n / 2 : n / 2 + 1]);     /* away from the first cores, where the system's own work lands */
}
void plat_sleep_until(u64 t_ns) {
    /* A high-resolution waitable timer where the system has one (Windows 10
     * 1803+), otherwise Sleep() at the 1 ms granularity set in plat_init. */
    static __thread HANDLE timer; static __thread int tried;      /* one timer per thread: a shared one would be re-armed under a sleeper */
    if (!tried) { tried = 1; timer = CreateWaitableTimerExW(0, 0, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS); }
    for (;;) {
        u64 now = plat_time_ns();
        if (now >= t_ns) return;
        u64 left = t_ns - now;
        if (timer) {
            LARGE_INTEGER due; due.QuadPart = -(LONGLONG)(left / 100 ? left / 100 : 1);
            if (SetWaitableTimer(timer, &due, 0, 0, 0, FALSE)) { WaitForSingleObject(timer, INFINITE); continue; }
        }
        if (left >= 2000000) Sleep((DWORD)(left / 1000000) - 1); else Sleep(0);
    }
}
void plat_gmtime(s64 t, struct tm *out) {
    __time64_t tt = (__time64_t)t;
    if (_gmtime64_s(out, &tt) != 0) memset(out, 0, sizeof *out);
}

/* ---- files -------------------------------------------------------------- */
FILE *plat_fopen(const char *path, const char *mode) {
    wchar_t wp[WPATH], wm[16];
    return _wfopen(to_wide(path, wp, WPATH), to_wide(mode, wm, 16));
}
int plat_fseek64(FILE *f, u64 offset) { return _fseeki64(f, (__int64)offset, SEEK_SET); }
static s64 unix_time(FILETIME ft) {
    u64 t = ((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;     /* 100 ns units since 1601 */
    return (s64)(t / 10000000ull) - 11644473600ll;
}
int plat_stat(const char *path, PlatStat *st) {
    wchar_t wp[WPATH]; WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(to_wide(path, wp, WPATH), GetFileExInfoStandard, &d)) return -1;
    st->is_dir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    st->size = ((u64)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    st->ctime = unix_time(d.ftCreationTime); st->mtime = unix_time(d.ftLastWriteTime);
    return 0;
}
int plat_mkdir(const char *path)  { wchar_t wp[WPATH]; return CreateDirectoryW(to_wide(path, wp, WPATH), 0) ? 0 : -1; }
int plat_rmdir(const char *path)  { wchar_t wp[WPATH]; return RemoveDirectoryW(to_wide(path, wp, WPATH)) ? 0 : -1; }
int plat_unlink(const char *path) { wchar_t wp[WPATH]; return DeleteFileW(to_wide(path, wp, WPATH)) ? 0 : -1; }
struct PlatDir { HANDLE h; int first; WIN32_FIND_DATAW fd; char name[800]; };
PlatDir *plat_opendir(const char *path) {
    char pat[WPATH]; wchar_t wp[WPATH];
    snprintf(pat, sizeof pat, "%s/*", path);
    PlatDir *d = malloc(sizeof *d);
    d->h = FindFirstFileW(to_wide(pat, wp, WPATH), &d->fd);
    if (d->h == INVALID_HANDLE_VALUE) { free(d); return 0; }
    d->first = 1;
    return d;
}
const char *plat_readdir(PlatDir *d) {
    if (!d->first && !FindNextFileW(d->h, &d->fd)) return 0;
    d->first = 0;
    if (!WideCharToMultiByte(CP_UTF8, 0, d->fd.cFileName, -1, d->name, sizeof d->name, 0, 0)) d->name[0] = 0;
    return d->name;
}
void plat_closedir(PlatDir *d) { FindClose(d->h); free(d); }

#endif

#ifdef _WIN32
/* win_compat.h */
long ror_win_sysconf(int name) {
    if (name == _SC_NPROCESSORS_ONLN) { SYSTEM_INFO si; GetSystemInfo(&si); return (long)si.dwNumberOfProcessors; }
    return -1;
}
#endif
