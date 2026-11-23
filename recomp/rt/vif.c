/* vif.c — VIF0/VIF1: the command stream that unpacks vertex data into VU
 * memory, uploads VU micro programs, starts them, and passes GIF data
 * straight through (VIF1 DIRECT). */
#include "gs.h"
#include "vu.h"

typedef struct {
    u32 r[4], c[4], cl, wl, mask, mode, itop, itops, top, tops, base, ofst, dbf, mark, code, err;
    u32 cmd, need, have;        /* command being collected: words needed / held */
    u32 buf[1024 + 8];
    u32 direct_left;            /* DIRECT: quadwords still to pass through */
    u32 qw[4], qw_n;
} Vif;
static Vif vif[2];

/* Debug: ROR_VIFDUMP=<frame> records everything VIF1 is fed for ROR_VIFDUMPN
 * frames (default 3) into ROR_VIFDUMPFILE (default "vifstream.bin"), starting
 * at the first VIF access of that frame: "VIFS", sizeof(Vif), both Vif
 * structs, the VU1 state (vu_write_state), then records
 * { u32 kind (0 = vif_write, 1 = vif_write_reg), n, a, b } [+ a data bytes].
 * recomp/tools/vubench replays such a file. */
void vu_write_state(FILE *f, int n, u32 pc);
static u64 vifdump_frame = ~0ull, vifdump_n = 3; static FILE *vifdump_f; static int vifdump_init;
static void vifdump(u32 kind, int n, u32 a, u32 b, const u8 *data) {
    if (!vifdump_init) {
        const char *e = getenv("ROR_VIFDUMP"); if (e) vifdump_frame = strtoull(e, 0, 0);
        e = getenv("ROR_VIFDUMPN"); if (e) vifdump_n = strtoull(e, 0, 0);
        vifdump_init = 1;
    }
    if (g_gs_frames < vifdump_frame) return;
    if (!vifdump_f) {
        if (g_gs_frames != vifdump_frame) { vifdump_frame = ~0ull; return; }
        const char *name = getenv("ROR_VIFDUMPFILE");
        vifdump_f = fopen(name ? name : "vifstream.bin", "wb");
        if (!vifdump_f) { vifdump_frame = ~0ull; return; }
        u32 sz = (u32)sizeof(Vif);
        fwrite("VIFS", 1, 4, vifdump_f); fwrite(&sz, 4, 1, vifdump_f); fwrite(vif, sizeof(Vif), 2, vifdump_f);
        vu_write_state(vifdump_f, 1, g_vu1.pc);
    }
    if (g_gs_frames >= vifdump_frame + vifdump_n) { fclose(vifdump_f); vifdump_f = 0; vifdump_frame = ~0ull; return; }
    u32 h[4] = { kind, (u32)n, a, b };
    fwrite(h, 4, 4, vifdump_f);
    if (kind == 0) fwrite(data, 1, a, vifdump_f);
}

void vif_reset(int n) { memset(&vif[n], 0, sizeof vif[n]); vif[n].cl = vif[n].wl = 1; }

static inline u16 rd16(const u8 *p) { u16 h; memcpy(&h, p, 2); return h; }
static inline u32 rd32(const u8 *p) { u32 w; memcpy(&w, p, 4); return w; }

static u32 unpack_words(const Vif *v, u32 code) {
    u32 cmd = code >> 24, num = (code >> 16) & 0xFF;
    u32 vn = (cmd >> 2) & 3, vl = cmd & 3;
    if (!num) num = 256;
    u32 vectors = (v->wl <= v->cl || v->wl == 0) ? num : v->cl * (num / v->wl) + (num % v->wl < v->cl ? num % v->wl : v->cl);
    u32 bits = (vn == 3 && vl == 3) ? 16 : (32u >> vl) * (vn + 1);
    return (vectors * bits + 31) / 32;
}

/* UNPACK without mask, addition mode or skipping/filling (CL = WL), not
 * wrapping around the end of VU memory: every vector has data and all four
 * components are written, so each format is one tight loop. Same result as
 * the general loop in do_unpack. Returns 0 if the format has no fast loop. */
static int unpack_fast(u32 *dst, const u8 *src, u32 num, u32 vn, u32 vl, u32 usn) {
    #define RD16(p) (usn ? (u32)rd16(p) : (u32)(s32)(s16)rd16(p))
    #define RD8(p)  (usn ? (u32)*(p) : (u32)(s32)(s8)*(p))
    switch ((vn << 2) | vl) {
    case 0xC: memcpy(dst, src, num * 16); return 1;                                              /* V4-32 */
    case 0x8: for (u32 k = 0; k < num; k++, dst += 4, src += 12) { memcpy(dst, src, 12); dst[3] = 0; } return 1;
    case 0x4: for (u32 k = 0; k < num; k++, dst += 4, src += 8) { memcpy(dst, src, 8); dst[2] = dst[3] = 0; } return 1;
    case 0x0: for (u32 k = 0; k < num; k++, dst += 4, src += 4) { u32 x; memcpy(&x, src, 4); dst[0] = dst[1] = dst[2] = dst[3] = x; } return 1;
    case 0xD: for (u32 k = 0; k < num; k++, dst += 4, src += 8) { dst[0] = RD16(src); dst[1] = RD16(src + 2); dst[2] = RD16(src + 4); dst[3] = RD16(src + 6); } return 1;
    case 0x9: for (u32 k = 0; k < num; k++, dst += 4, src += 6) { dst[0] = RD16(src); dst[1] = RD16(src + 2); dst[2] = RD16(src + 4); dst[3] = 0; } return 1;
    case 0x5: for (u32 k = 0; k < num; k++, dst += 4, src += 4) { dst[0] = RD16(src); dst[1] = RD16(src + 2); dst[2] = dst[3] = 0; } return 1;
    case 0x1: for (u32 k = 0; k < num; k++, dst += 4, src += 2) { u32 x = RD16(src); dst[0] = dst[1] = dst[2] = dst[3] = x; } return 1;
    case 0xE: for (u32 k = 0; k < num; k++, dst += 4, src += 4) { dst[0] = RD8(src); dst[1] = RD8(src + 1); dst[2] = RD8(src + 2); dst[3] = RD8(src + 3); } return 1;
    case 0xA: for (u32 k = 0; k < num; k++, dst += 4, src += 3) { dst[0] = RD8(src); dst[1] = RD8(src + 1); dst[2] = RD8(src + 2); dst[3] = 0; } return 1;
    case 0x6: for (u32 k = 0; k < num; k++, dst += 4, src += 2) { dst[0] = RD8(src); dst[1] = RD8(src + 1); dst[2] = dst[3] = 0; } return 1;
    case 0x2: for (u32 k = 0; k < num; k++, dst += 4, src += 1) { u32 x = RD8(src); dst[0] = dst[1] = dst[2] = dst[3] = x; } return 1;
    case 0xF:                                                                                    /* V4-5 */
        for (u32 k = 0; k < num; k++, dst += 4, src += 2) {
            u32 w = rd16(src);
            dst[0] = (w & 31) << 3; dst[1] = ((w >> 5) & 31) << 3; dst[2] = ((w >> 10) & 31) << 3; dst[3] = ((w >> 15) & 1) << 7;
        }
        return 1;
    }
    #undef RD16
    #undef RD8
    return 0;
}

/* src = the packed data that follows the command word. */
static void do_unpack(int n, Vif *v, u32 code, const u8 *src) {
    VU *vu = n ? &g_vu1 : &g_vu0;
    u32 cmd = code >> 24, num = (code >> 16) & 0xFF, imm = code & 0xFFFF;
    u32 vn = (cmd >> 2) & 3, vl = cmd & 3, m = (cmd >> 4) & 1, usn = (imm >> 14) & 1;
    u32 addr = (imm & 0x3FF) + ((imm & 0x8000) ? v->tops : 0);
    if (!num) num = 256;
    u32 bitpos = 0, cyc = 0;
    u32 cl = v->cl, wl = v->wl ? v->wl : 256;
    if (!m && v->mode == 0 && cl == wl) {
        u32 at = (addr * 16) & vu->mem_mask;
        if (at + num * 16 <= vu->mem_mask + 1 && unpack_fast((u32 *)(vu->mem + at), src, num, vn, vl, usn)) return;
    }
    for (u32 written = 0; written < num;) {
        int has_data = (wl <= cl) ? 1 : (cyc < cl);
        if (wl <= cl && cyc >= wl) {               /* skip part of the cycle */
            addr += cl - wl; cyc = 0;
            continue;
        }
        u32 d[4] = { 0, 0, 0, 0 };
        if (has_data) {
            if (vn == 3 && vl == 3) {
                u32 p = bitpos / 8; u32 w = src[p] | (src[p + 1] << 8); bitpos += 16;
                d[0] = (w & 31) << 3; d[1] = ((w >> 5) & 31) << 3; d[2] = ((w >> 10) & 31) << 3; d[3] = ((w >> 15) & 1) << 7;
            } else {
                for (u32 i = 0; i <= vn; i++) {
                    u32 p = bitpos / 8, x;
                    if (vl == 0) { memcpy(&x, src + p, 4); bitpos += 32; }
                    else if (vl == 1) { u16 h; memcpy(&h, src + p, 2); x = usn ? h : (u32)(s32)(s16)h; bitpos += 16; }
                    else { u8 b = src[p]; x = usn ? b : (u32)(s32)(s8)b; bitpos += 8; }
                    d[i] = x;
                }
                if (vn == 0) d[1] = d[2] = d[3] = d[0];
            }
        }
        u32 row = cyc < 3 ? cyc : 3;
        u32 *dst = (u32 *)(vu->mem + ((addr * 16) & vu->mem_mask));
        for (u32 i = 0; i < 4; i++) {
            u32 mk = m ? (v->mask >> (row * 8 + i * 2)) & 3 : 0;
            u32 val;
            if (!has_data && mk == 0) mk = 1;      /* fill write without data: row register */
            switch (mk) {
            case 0:
                val = d[i];
                if (v->mode == 1) val += v->r[i];
                else if (v->mode == 2) { val += v->r[i]; v->r[i] = val; }
                break;
            case 1: val = v->r[i]; break;
            case 2: val = v->c[row]; break;
            default: continue;                     /* write-protected */
            }
            dst[i] = val;
        }
        addr++; written++; cyc++;
        if (wl > cl && cyc >= wl) cyc = 0;
    }
}

static void start_micro(int n, Vif *v, u32 addr, int cont) {
    if (n) {
        v->top = v->tops; v->itop = v->itops;
        if (v->dbf) { v->tops = v->base; v->dbf = 0; } else { v->tops = v->base + v->ofst; v->dbf = 1; }
        g_vu1.vi_top = v->top; g_vu1.vi_itop = v->itop;
    } else { v->itop = v->itops; g_vu0.vi_itop = v->itop; }
    vu_exec(n, cont ? 0xFFFFFFFFu : addr);
}

/* One complete command: w = its words (command word first), not necessarily aligned. */
static void command(int n, Vif *v, const u8 *w) {
    u32 code = rd32(w), cmd = (code >> 24) & 0x7F, imm = code & 0xFFFF, num = (code >> 16) & 0xFF;
    v->code = code;
    if (cmd >= 0x60) { do_unpack(n, v, code, w + 4); return; }
    switch (cmd) {
    case 0x00: break;
    case 0x01: v->cl = imm & 0xFF; v->wl = imm >> 8; break;
    case 0x02: v->ofst = imm & 0x3FF; v->dbf = 0; v->tops = v->base; break;
    case 0x03: v->base = imm & 0x3FF; break;
    case 0x04: v->itops = imm & 0x3FF; break;
    case 0x05: v->mode = imm & 3; break;
    case 0x06: break;                              /* MSKPATH3: path 3 is processed immediately */
    case 0x07: v->mark = imm; break;
    case 0x10: case 0x11: case 0x13: break;        /* FLUSHE / FLUSH / FLUSHA: nothing is pending */
    case 0x14: case 0x15: start_micro(n, v, imm * 8, 0); break;
    case 0x17: start_micro(n, v, 0, 1); break;
    case 0x20: v->mask = rd32(w + 4); break;
    case 0x30: memcpy(v->r, w + 4, 16); break;
    case 0x31: memcpy(v->c, w + 4, 16); break;
    case 0x4A: {
        VU *vu = n ? &g_vu1 : &g_vu0;
        u32 bytes = (num ? num : 256) * 8, at = (imm * 8) & vu->micro_mask;
        if (at + bytes <= vu->micro_mask + 1) memcpy(vu->micro + at, w + 4, bytes);
        else for (u32 i = 0; i < bytes; i += 4) memcpy(vu->micro + ((at + i) & vu->micro_mask), w + 4 + i, 4);
        vu_micro_changed(n);
        break;
    }
    default:
        rt_fatal(0, "VIF%d: unknown command 0x%02x (code 0x%08x)", n, cmd, code);
    }
}

/* ROR_VUSTAT (see vu.c): time in vif_write(1), with the VU1 programs it starts and the GS work of DIRECT taken out. */
extern int g_vu_stat; extern u64 g_vu_stat_ticks[5]; void vu_stat_report(void);
static void vif_write1(int n, const u8 *data, u32 bytes);
static __attribute__((noinline)) void gif_write_timed(const u8 *data, u32 bytes) {
    u64 t0 = __builtin_ia32_rdtsc();
    gif_write(GIF_PATH2, data, bytes);
    g_vu_stat_ticks[3] += __builtin_ia32_rdtsc() - t0;
}
#define GIF2(data, bytes) do { if (UNLIKELY(g_vu_stat)) gif_write_timed(data, bytes); else gif_write(GIF_PATH2, data, bytes); } while (0)
void vif_write(int n, const u8 *data, u32 bytes) {
    if (LIKELY(!g_vu_stat) || !n) { vif_write1(n, data, bytes); return; }
    vu_stat_report();
    u64 t0 = __builtin_ia32_rdtsc(), e0 = g_vu_stat_ticks[0];
    vif_write1(n, data, bytes);
    g_vu_stat_ticks[2] += __builtin_ia32_rdtsc() - t0;
    g_vu_stat_ticks[3] += g_vu_stat_ticks[0] - e0;          /* VU1 programs started from here */
}
static void vif_write1(int n, const u8 *data, u32 bytes) {
    Vif *v = &vif[n];
    if (UNLIKELY(vifdump_frame != ~0ull || !vifdump_init)) vifdump(0, n, bytes, 0, data);
    for (u32 i = 0; i + 4 <= bytes;) {
        if (v->direct_left) {                       /* DIRECT payload -> GIF path 2 */
            if (v->qw_n == 0 && bytes - i >= 16) {  /* whole quadwords: straight from the source */
                u32 q = (bytes - i) / 16;
                if (q > v->direct_left) q = v->direct_left;
                GIF2(data + i, q * 16);
                v->direct_left -= q; i += q * 16;
                continue;
            }
            v->qw[v->qw_n++] = rd32(data + i); i += 4;
            if (v->qw_n == 4) { GIF2((const u8 *)v->qw, 16); v->qw_n = 0; v->direct_left--; }
            continue;
        }
        u32 w = rd32(data + i);
        if (v->have == 0) {
            u32 cmd = (w >> 24) & 0x7F, imm = w & 0xFFFF, num = (w >> 16) & 0xFF;
            if (cmd == 0x50 || cmd == 0x51) {
                v->code = w; v->direct_left = imm ? imm : 65536; v->qw_n = 0;
                i += 4;
                continue;
            }
            u32 need = 1;
            if (cmd == 0x20) need = 2;
            else if (cmd == 0x30 || cmd == 0x31) need = 5;
            else if (cmd == 0x4A) need = 1 + (num ? num : 256) * 2;
            else if (cmd >= 0x60) need = 1 + unpack_words(v, w & 0x7FFFFFFFu);
            if (bytes - i >= need * 4) {            /* the whole command is here: run it in place */
                command(n, v, data + i);
                i += need * 4;
                continue;
            }
            v->need = need;
        }
        v->buf[v->have++] = w; i += 4;              /* a command split across writes is collected first */
        if (v->have == v->need) { v->have = 0; command(n, v, (const u8 *)v->buf); }
    }
}

u32 vif_read_reg(int n, u32 off) {
    Vif *v = &vif[n];
    switch (off) {
    case 0x00: return (v->dbf << 7);              /* STAT: idle */
    case 0x20: return v->err;
    case 0x30: return v->mark;
    case 0x40: return v->cl | (v->wl << 8);
    case 0x50: return v->mode;
    case 0x70: return v->mask;
    case 0x80: return v->code;
    case 0x90: return v->itops;
    case 0xA0: return v->base;
    case 0xB0: return v->ofst;
    case 0xC0: return v->tops;
    case 0xD0: return v->itop;
    case 0xE0: return v->top;
    }
    if (off >= 0x100 && off < 0x140) return v->r[(off >> 4) & 3];
    if (off >= 0x140 && off < 0x180) return v->c[(off >> 4) & 3];
    return 0;
}
void vif_write_reg(int n, u32 off, u32 val) {
    Vif *v = &vif[n];
    if (UNLIKELY(vifdump_frame != ~0ull || !vifdump_init)) vifdump(1, n, off, val, 0);
    switch (off) {
    case 0x10: if (val & 1) vif_reset(n); break;   /* FBRST */
    case 0x20: v->err = val; break;
    case 0x30: v->mark = val; break;
    }
}
