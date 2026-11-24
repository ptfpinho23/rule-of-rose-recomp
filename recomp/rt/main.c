/* main.c — entry point of the recompiled game. */
#include <xmmintrin.h>
#include "rt.h"
#include "iop.h"
#include "plat.h"

void install_signal_handlers(void);
void crash_test(void);
void trace_init(void);
void video_config(int window, int shots, int frames, int nosync);
void video_script(const char *s);
int  video_option(const char *name, const char *value);
int  pad_option(const char *name, const char *value);

#define ISO_NAME "Rule of Rose (Europe) (En,Fr,De,Es,It).iso"
static char exe_dir[900] = ".";
static char iso_buf[1200];

static int readable(const char *p) { FILE *f = plat_fopen(p, "rb"); if (f) fclose(f); return f != 0; }

/* Disc image search: current directory, then the executable's directory and
 * up to four of its parents (so a build inside the source tree finds it). */
static const char *find_iso(void) {
    static const char *names[] = { ISO_NAME, "RuleOfRose.iso", "game.iso" };
    for (int n = 0; n < 3; n++) if (readable(names[n])) return names[n];
    char dir[900]; snprintf(dir, sizeof dir, "%s", exe_dir);
    for (int up = 0; up < 5; up++) {
        for (int n = 0; n < 3; n++) {
            snprintf(iso_buf, sizeof iso_buf, "%s/%s", dir, names[n]);
            if (readable(iso_buf)) return iso_buf;
        }
        char *s = strrchr(dir, '/');
        if (!s || s == dir) break;
        *s = 0;
    }
    return 0;
}

static int is_picture_option(const char *a) {
    static const char *const names[] = { "--scale", "--res", "--aspect", "--filter", "--sharpness", "--brightness", "--contrast",
                                         "--gamma", "--saturation", "--bloom", "--renderer", "--internal", "--vsync", "--fps" };
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++) if (!strcmp(a, names[i])) return 1;
    return 0;
}
static void usage(const char *prog) {
    fprintf(stderr, "usage: %s [--iso path] [--scale 1-4] [--res WxH] [--fullscreen] [--aspect 4:3|stretch]\n"
        "          [--filter nearest|bilinear|fsr|fsr3] [--sharpness 0-100] [--brightness -100..100] [--contrast -100..100]\n"
        "          [--gamma 50-200] [--saturation 0-200] [--bloom 0-100]\n"
        "          [--renderer software|gpu] [--internal 1-8] [--vsync 0|1] [--fps 0|display|25-1000]\n"
        "          [--wide off|16:9|21:9|32:9|W:H|ratio]   (widescreen 3D; ratio 1.0-3.6, 4:3 = off)\n"
        "          [--film-grain 0-100]   (the game's film grain; 100 = original, 0 = off)\n"
        "          [--texture-filter auto|original|smooth]   (GPU renderer: smooth filters distant textures; auto = smooth from --internal 2 on a discrete graphics card)\n"
        "          [--pad auto|off|xinput|playstation] [--pad-deadzone 0-90]   (gamepad; stick dead zone in percent, default 24)\n"
        "          [--no-window] [--no-sync] [--frames N] [--shots N] [--post-shots] [--script s] [--elf path] [--interp] [-v]\n", prog);
}

int main(int argc, char **argv) {
    const char *elf = 0;
    const char *insn = "build/recomp/gen/insn.bin";
    int window = 1, nosync = 0, shots = 0, frames = 0;
    plat_init(&argc, &argv);
    { int wide_set(const char *v); const char *w = plat_getenv("ROR_WIDE"); if (w && !wide_set(w)) fprintf(stderr, "ROR_WIDE: bad value %s\n", w); }
    const char *iso = plat_getenv("ROR_ISO");
    plat_exe_dir(exe_dir, sizeof exe_dir);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--interp")) g_use_interp = 1;
        else if (!strcmp(argv[i], "-v")) g_verbose++;
        else if (!strcmp(argv[i], "--elf") && i + 1 < argc) elf = argv[++i];
        else if (!strcmp(argv[i], "--iso") && i + 1 < argc) iso = argv[++i];
        else if (!strcmp(argv[i], "--no-window")) window = 0;
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) video_script(argv[++i]);
        else if (!strcmp(argv[i], "--no-sync")) nosync = 1;
        else if (!strcmp(argv[i], "--fullscreen") || !strcmp(argv[i], "--post-shots")) video_option(argv[i] + 2, 0);
        else if (is_picture_option(argv[i]) && i + 1 < argc) {
            if (!video_option(argv[i] + 2, argv[i + 1])) { fprintf(stderr, "%s: bad value for %s: %s\n", argv[0], argv[i], argv[i + 1]); usage(argv[0]); return 2; }
            i++;
        }
        else if ((!strcmp(argv[i], "--pad") || !strcmp(argv[i], "--pad-deadzone")) && i + 1 < argc) {
            if (!pad_option(argv[i] + 2, argv[i + 1])) { fprintf(stderr, "%s: bad value for %s: %s\n", argv[0], argv[i], argv[i + 1]); usage(argv[0]); return 2; }
            i++;
        }
        else if (!strcmp(argv[i], "--wide") && i + 1 < argc) {
            int wide_set(const char *v);
            if (!wide_set(argv[++i])) { fprintf(stderr, "%s: bad value for --wide: %s\n", argv[0], argv[i]); usage(argv[0]); return 2; }
        }
        else if (!strcmp(argv[i], "--texture-filter") && i + 1 < argc) {
            void gs_texture_filter(int mode); const char *v = argv[++i];
            if (!strcmp(v, "original")) gs_texture_filter(0); else if (!strcmp(v, "smooth")) gs_texture_filter(1); else if (!strcmp(v, "auto")) gs_texture_filter(-1);
            else { fprintf(stderr, "%s: bad value for --texture-filter: %s\n", argv[0], v); usage(argv[0]); return 2; }
        }
        else if (!strcmp(argv[i], "--film-grain") && i + 1 < argc) {
            char *e; long n = strtol(argv[++i], &e, 10);
            if (!*argv[i] || *e || n < 0 || n > 100) { fprintf(stderr, "%s: bad value for --film-grain: %s\n", argv[0], argv[i]); usage(argv[0]); return 2; }
            { void gs_film_grain(int pct); gs_film_grain((int)n); }
        }
        else if (!strcmp(argv[i], "--shots") && i + 1 < argc) shots = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else { usage(argv[0]); return 2; }
    }
    _mm_setcsr(_mm_getcsr() | 0x8040);       /* FTZ | DAZ: no denormals on the EE */
    if (plat_getenv("ROR_PAD_SELFTEST")) { int ps_selftest(void); return ps_selftest() ? 1 : 0; }   /* rt/pad_ps.c: report parsers */
    install_signal_handlers();
    trace_init();
    mem_init();
    crash_test();
    if (!iso) iso = find_iso();
    if (!iso) {
        fprintf(stderr, "Rule of Rose: disc image not found.\n"
                "Put \"%s\" (or RuleOfRose.iso) next to this program,\nor pass it with --iso PATH.\n", ISO_NAME);
        return 1;
    }
    if (!plat_getenv("ROR_MC_DIR")) {         /* saves live next to the program */
        static char mc[1000]; snprintf(mc, sizeof mc, "%s/saves/mc0", exe_dir);
        plat_setenv("ROR_MC_DIR", mc);
    }
    u32 entry;
    if (elf) mem_load_elf(elf, &entry);
    else {                                    /* the game's data segments come off the disc */
        u32 lsn, size;
        if (!iso_open(iso)) { fprintf(stderr, "cannot open disc image: %s\n", iso); return 1; }
        if (!iso_find("\\SLES_542.18;1", &lsn, &size)) {
            fprintf(stderr, "%s: this is not the European Rule of Rose disc (SLES-54218)\n", iso); return 1;
        }
        u8 *b = malloc(((size_t)size + 2047) & ~(size_t)2047);
        iso_read(lsn, (size + 2047) / 2048, b);
        mem_load_elf_buf(b, size, "SLES_542.18", &entry);
        free(b);
    }
    if (g_use_interp) interp_init(insn);
    video_config(window, shots, frames, nosync);
    hw_init();
    iop_init(iso);
    kernel_init(entry);
    rt_log("[boot] entry 0x%08x, %s mode\n", entry, g_use_interp ? "interpreter" : "recompiled");
    kernel_run();
}
