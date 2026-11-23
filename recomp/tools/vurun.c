/* vurun.c — run one VU1 program start off-line from a ROR_VUDUMP file
 * (vu1_NNNN.bin) with the real interpreter (recomp/rt/vu.c is included),
 * and print every packet it XGKICKs, decoded.
 *   vurun DUMP           print kicked packets
 *   ROR_VUTRACE=0 vurun DUMP    also trace every instruction
 * Build/run with recomp/tools/vurun.sh. */
#include "../rt/vu.c"
u64 plat_time_ns(void) { struct timespec t; timespec_get(&t, TIME_UTC); return (u64)t.tv_sec * 1000000000ull + (u64)t.tv_nsec; }   /* for vu.c */
#include <stdarg.h>
#include <xmmintrin.h>

VU g_vu0;
u64 g_gs_frames, g_gs_prims;
void rt_fatal(Ctx *c, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); exit(1);
}
void rt_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); }

void gif_write_path1(const u8 *mem, u32 addr, u32 mask) {
    static const char *rn[16] = { "PRIM", "RGBAQ", "ST", "UV", "XYZF2", "XYZ2", "TEX0_1", "TEX0_2", "CLAMP_1", "CLAMP_2", "FOG", "-", "XYZF3", "XYZ3", "A+D", "NOP" };
    for (int guard = 0; guard < 1024;) {
        u64 lo, hi; memcpy(&lo, mem + (addr & mask), 8); memcpy(&hi, mem + (addr & mask) + 8, 8);
        u32 nloop = lo & 0x7FFF, eop = (lo >> 15) & 1, flg = (lo >> 58) & 3, nreg = (lo >> 60) & 15;
        if (!nreg) nreg = 16;
        printf("KICK @%03x tag nloop=%u eop=%u pre=%u prim=0x%03x flg=%u nreg=%u regs=%llx\n", (addr & mask) / 16, nloop, eop,
               (u32)(lo >> 46) & 1, (u32)(lo >> 47) & 0x7FF, flg, nreg, (unsigned long long)hi);
        addr += 16; guard++;
        if (flg != 0) { printf("  (non-packed, not decoded)\n"); if (flg == 2) addr += nloop * 16; else addr += ((nloop * nreg + 1) / 2) * 16; }
        else for (u32 l = 0; l < nloop; l++) {
            printf("  %3u:", l);
            for (u32 r = 0; r < nreg; r++, addr += 16, guard++) {
                u32 d = (u32)(hi >> (r * 4)) & 15; u32 w[4]; float f[4];
                memcpy(w, mem + (addr & mask), 16); memcpy(f, w, 16);
                switch (d) {
                case 1: printf(" RGBA(%u %u %u %u)", w[0] & 255, w[1] & 255, w[2] & 255, w[3] & 255); break;
                case 2: printf(" STQ(%.5f %.5f %.6g)", f[0], f[1], f[2]); break;
                case 3: printf(" UV(%u %u)", w[0] & 0x3FFF, w[1] & 0x3FFF); break;
                case 4: case 12: printf(" XYZF%s(%.2f %.2f z=%u f=%u)%s", d == 12 ? "3" : "2", (w[0] & 0xFFFF) / 16.0, (w[1] & 0xFFFF) / 16.0, (w[2] >> 4) & 0xFFFFFF, (w[2] >> 28) | ((w[3] >> 4) & 0xFF), (d == 12 || (w[3] & 0x8000)) ? " ADC" : ""); break;
                case 5: case 13: printf(" XYZ%s(%.2f %.2f z=%u)%s", d == 13 ? "3" : "2", (w[0] & 0xFFFF) / 16.0, (w[1] & 0xFFFF) / 16.0, w[2], (d == 13 || (w[3] & 0x8000)) ? " ADC" : ""); break;
                case 14: printf(" AD[%02x]=%08x%08x", w[2] & 255, w[1], w[0]); break;
                default: printf(" %s[%08x %08x %08x %08x]", rn[d], w[0], w[1], w[2], w[3]);
                }
            }
            printf("\n");
        }
        if (eop) return;
    }
    printf("  (no EOP)\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: vurun DUMP\n"); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    _mm_setcsr(_mm_getcsr() | 0x8040);       /* FTZ | DAZ, as in the runtime's main() */
    vu_init();
    VU *v = &g_vu1; u32 hdr[4], ext[8];
    if (fread(hdr, 4, 4, f) != 4 || fread(v->vf, 16, 32, f) != 32 || fread(v->vi, 4, 16, f) != 16 ||
        fread(&v->acc, 16, 1, f) != 1 || fread(ext, 4, 8, f) != 8 ||
        fread(v->mem, 1, 0x4000, f) != 0x4000 || fread(v->micro, 1, 0x4000, f) != 0x4000) { fprintf(stderr, "short dump\n"); return 1; }
    fclose(f);
    v->q.u = ext[0]; v->p.u = ext[1]; v->i.u = ext[2]; v->r = ext[3]; v->status = ext[4]; v->mac = ext[5]; v->clip = ext[6];
    v->vi_top = hdr[1]; v->vi_itop = hdr[2];
    vu_dump_frame = ~0ull;
    if (vu_trace_k >= 0) vu_force_trace = 1;
    printf("START pc=0x%x top=0x%x itop=0x%x\n", hdr[0], hdr[1], hdr[2]);
    vu_exec(1, hdr[0]);
    return 0;
}
