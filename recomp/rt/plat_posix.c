/* plat_posix.c — plat.h for Linux. */
#ifndef _WIN32
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <signal.h>
#include <ucontext.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "rt.h"
#include "plat.h"

/* <dirent.h> needs kernel headers this toolchain does not have; the three
 * calls used are declared here with glibc's x86-64 layout. */
typedef struct __dirstream DIR;
struct dirent { unsigned long d_ino; long d_off; unsigned short d_reclen; unsigned char d_type; char d_name[256]; };
DIR *opendir(const char *name);
struct dirent *readdir(DIR *dirp);
int closedir(DIR *dirp);

void *__dso_handle = 0;          /* normally from crtbegin.o, which the link line omits; needed by atexit */

/* ---- process ------------------------------------------------------------ */
void plat_init(int *argc, char ***argv) {
    (void)argc; (void)argv;
    setvbuf(stderr, 0, _IOLBF, 0);
}
void plat_exe_dir(char *out, size_t n) {
    ssize_t el = readlink("/proc/self/exe", out, n - 1);
    if (el > 0) { out[el] = 0; char *sl = strrchr(out, '/'); if (sl && sl != out) *sl = 0; }
    else snprintf(out, n, ".");
}
const char *plat_getenv(const char *name) { return getenv(name); }
void plat_setenv(const char *name, const char *value) { setenv(name, value, 1); }
void plat_exit_now(int code) { _exit(code); }

static void (*fault_cb)(const char *, void *, int);
static void (*int_cb)(const char *);
static void on_segv(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    char what[32]; snprintf(what, sizeof what, "signal %d", sig);
    fault_cb(what, si->si_addr, 1);
    _exit(71);
}
static void on_int(int sig) {
    char what[32]; snprintf(what, sizeof what, "signal %d", sig);
    int_cb(what);
    _exit(72);
}

/* ROR_PROF=file: sampling profiler (1 kHz of CPU time); raw program counters
 * are written at exit and resolved by recomp/tools/prof.py. */
static u64 *prof_buf; static size_t prof_n; static const char *prof_path;
#define PROF_MAX (1u << 21)
static void on_prof(int sig, siginfo_t *si, void *ucv) {
    (void)sig; (void)si;
    if (prof_n < PROF_MAX) prof_buf[prof_n++] = (u64)((ucontext_t *)ucv)->uc_mcontext.gregs[REG_RIP];
}
static void prof_write(void) {
    FILE *f = fopen(prof_path, "wb");
    if (f) { fwrite(prof_buf, 8, prof_n, f); fclose(f); }
}
void prof_reset(void) { if (prof_buf) prof_n = 1; }   /* drop samples so far (ROR_PROF_FROM=frame) */
static void prof_init(void) {
    prof_path = getenv("ROR_PROF");
    if (!prof_path) return;
    prof_buf = malloc(PROF_MAX * 8);
    prof_buf[prof_n++] = (u64)&rt_lookup;         /* header: lets prof.py find the load address */
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_prof; sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    sigaction(SIGPROF, &sa, 0);
    struct itimerval it = { { 0, 1000 }, { 0, 1000 } };
    setitimer(ITIMER_PROF, &it, 0);
    atexit(prof_write);
}
void plat_install_handlers(void (*on_fault)(const char *, void *, int), void (*on_interrupt)(const char *)) {
    fault_cb = on_fault; int_cb = on_interrupt;
    signal(SIGINT, on_int); signal(SIGTERM, on_int);
    prof_init();
    static u8 altstack[1 << 16];
    stack_t ss = { .ss_sp = altstack, .ss_size = sizeof altstack };
    sigaltstack(&ss, 0);
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0); sigaction(SIGFPE, &sa, 0);
}

/* ---- guest address space ------------------------------------------------ */
void plat_guest_map(void *base, u64 total, const PlatMap *maps, int n) {
    void *got = mmap(base, total, PROT_NONE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (got != base) { perror("reserve guest address space"); exit(1); }
    for (int i = 0; i < n; i++) {
        u8 *at = (u8 *)base + maps[i].offset;
        if (maps[i].object < 0) {
            if (mmap(at, maps[i].size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) {
                perror("map guest memory"); exit(1);
            }
            continue;
        }
        int first = 1;
        for (int k = 0; k < i; k++) if (maps[k].object == maps[i].object) first = 0;
        if (!first) continue;                       /* mapped together with the first view */
        int fd = memfd_create("ee-mem", 0);
        if (fd < 0 || ftruncate(fd, maps[i].size) < 0) { perror("memfd"); exit(1); }
        for (int k = i; k < n; k++) {
            if (maps[k].object != maps[i].object) continue;
            if (mmap((u8 *)base + maps[k].offset, maps[k].size, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED) { perror("map guest memory"); exit(1); }
        }
        close(fd);
    }
}

/* ---- coroutines --------------------------------------------------------- */
struct PlatCo { ucontext_t uc; void *stack; size_t size; };
PlatCo *plat_co_new(size_t stack_size) {
    PlatCo *co = calloc(1, sizeof *co);
    if (!co) { perror("coroutine"); exit(1); }
    if (stack_size) {
        co->stack = mmap(0, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (co->stack == MAP_FAILED) { perror("coroutine stack"); exit(1); }
        co->size = stack_size;
    }
    return co;
}
void plat_co_start(PlatCo *co, void (*entry)(void)) {
    getcontext(&co->uc);
    co->uc.uc_stack.ss_sp = co->stack;
    co->uc.uc_stack.ss_size = co->size;
    co->uc.uc_link = 0;
    makecontext(&co->uc, entry, 0);
}
void plat_co_switch(PlatCo *from, PlatCo *to) { swapcontext(&from->uc, &to->uc); }
void plat_co_jump(PlatCo *to) { setcontext(&to->uc); abort(); }
void plat_co_free(PlatCo *co) {
    if (!co) return;
    if (co->stack) munmap(co->stack, co->size);
    free(co);
}

/* ---- time --------------------------------------------------------------- */
u64 plat_time_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (u64)t.tv_sec * 1000000000ull + (u64)t.tv_nsec;
}
u64 plat_cpu_ns(void) {
    struct timespec t; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    return (u64)t.tv_sec * 1000000000ull + (u64)t.tv_nsec;
}
void plat_sleep_until(u64 t_ns) {
    struct timespec t = { (time_t)(t_ns / 1000000000ull), (long)(t_ns % 1000000000ull) };
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, 0);
}
void plat_gmtime(s64 t, struct tm *out) { time_t tt = (time_t)t; gmtime_r(&tt, out); }

/* ---- files -------------------------------------------------------------- */
FILE *plat_fopen(const char *path, const char *mode) { return fopen(path, mode); }
int plat_fseek64(FILE *f, u64 offset) { return fseeko(f, (off_t)offset, SEEK_SET); }
int plat_stat(const char *path, PlatStat *st) {
    struct stat s;
    if (stat(path, &s) != 0) return -1;
    st->is_dir = S_ISDIR(s.st_mode); st->size = (u64)s.st_size;
    st->ctime = (s64)s.st_ctime; st->mtime = (s64)s.st_mtime;
    return 0;
}
int plat_mkdir(const char *path) { return mkdir(path, 0755); }
int plat_rmdir(const char *path) { return rmdir(path); }
int plat_unlink(const char *path) { return unlink(path); }
PlatDir *plat_opendir(const char *path) { return (PlatDir *)opendir(path); }
const char *plat_readdir(PlatDir *d) { struct dirent *de = readdir((DIR *)d); return de ? de->d_name : 0; }
void plat_closedir(PlatDir *d) { closedir((DIR *)d); }

#endif
