/* gif.c — GIF packet decoding: GIFtags and their PACKED / REGLIST / IMAGE
 * payloads, turned into GS register writes. The three paths (VU1 XGKICK,
 * VIF1 DIRECT, GIF DMA/FIFO) keep separate tag state. */
#include "gs.h"

typedef struct { u32 nloop, nreg, flg, reg_idx, eop; u64 regs; u32 st_q; } Path;
static Path paths[3];
int g_gif_path;                 /* path of the packet being decoded (GIF_PATH1 = VU1 XGKICK); read by gs.c for --wide */

void gif_reset(void) { memset(paths, 0, sizeof paths); }

static void packed(Path *p, u32 desc, u64 lo, u64 hi) {
    switch (desc) {
    case 0x0: gs_write_reg(0x00, lo); break;
    case 0x1: gs_write_reg(0x01, (lo & 0xFF) | ((lo >> 24) & 0xFF00) | ((hi & 0xFF) << 16) |
                                 (((hi >> 32) & 0xFF) << 24) | ((u64)p->st_q << 32)); break;
    case 0x2: p->st_q = (u32)hi; gs_write_reg(0x02, lo); break;
    case 0x3: gs_write_reg(0x03, (lo & 0x3FFF) | (((lo >> 32) & 0x3FFF) << 16)); break;
    case 0x4: gs_write_reg((hi >> 47) & 1 ? 0x0C : 0x04,
                           (lo & 0xFFFF) | (((lo >> 32) & 0xFFFF) << 16) | (((hi >> 4) & 0xFFFFFF) << 32) |
                           (((hi >> 36) & 0xFF) << 56)); break;
    case 0x5: gs_write_reg((hi >> 47) & 1 ? 0x0D : 0x05,
                           (lo & 0xFFFF) | (((lo >> 32) & 0xFFFF) << 16) | ((hi & 0xFFFFFFFFull) << 32)); break;
    case 0xA: gs_write_reg(0x0A, ((hi >> 36) & 0xFF) << 56); break;
    case 0xE: gs_write_reg((u32)(hi & 0xFF), lo); break;
    case 0xF: break;
    default:  gs_write_reg(desc, lo); break;
    }
}

/* Consume one quadword. Returns 1 when it completed a packet (EOP). */
static int feed(Path *p, const u8 *qw) {
    u64 lo, hi;
    memcpy(&lo, qw, 8); memcpy(&hi, qw + 8, 8);
    if (p->nloop == 0) {                               /* a GIFtag */
        p->nloop = lo & 0x7FFF; p->eop = (lo >> 15) & 1;
        p->flg = (lo >> 58) & 3; p->nreg = (lo >> 60) & 15;
        if (!p->nreg) p->nreg = 16;
        p->regs = hi; p->reg_idx = 0;
        { float one = 1.0f; memcpy(&p->st_q, &one, 4); }
        if (p->flg == 0 && ((lo >> 46) & 1)) gs_write_reg(0x00, (lo >> 47) & 0x7FF);
        return p->nloop == 0 && p->eop;
    }
    switch (p->flg) {
    case 0:
        packed(p, (u32)(p->regs >> (p->reg_idx * 4)) & 15, lo, hi);
        if (++p->reg_idx == p->nreg) { p->reg_idx = 0; p->nloop--; }
        break;
    case 1:
        for (int h = 0; h < 2 && p->nloop; h++) {
            u32 desc = (u32)(p->regs >> (p->reg_idx * 4)) & 15;
            u64 d = h ? hi : lo;
            if (desc == 0x2) p->st_q = 0;              /* reglist ST has no Q */
            if (desc < 0xE) gs_write_reg(desc, d);
            if (++p->reg_idx == p->nreg) { p->reg_idx = 0; p->nloop--; }
        }
        break;
    default:
        gs_write_hwreg(qw, 16);
        p->nloop--;
        break;
    }
    return p->nloop == 0 && p->eop;
}

void gif_write(int path, const u8 *data, u32 bytes) {
    Path *p = &paths[path];
    g_gif_path = path;
    for (u32 i = 0; i + 16 <= bytes;) {
        if (p->nloop && p->flg == 2) {                 /* IMAGE data: hand over all of it that is here */
            u32 n = (bytes - i) / 16;
            if (n > p->nloop) n = p->nloop;
            gs_write_hwreg(data + i, n * 16);
            p->nloop -= n; i += n * 16;
        } else { feed(p, data + i); i += 16; }
    }
}
void gif_write_path1(const u8 *mem, u32 addr, u32 mask) {
    Path *p = &paths[GIF_PATH1];
    g_gif_path = GIF_PATH1;
    p->nloop = 0;
    for (u32 n = 0; n <= mask / 16 + 1; n++) {
        if (feed(p, mem + (addr & mask & ~15u))) return;
        addr += 16;
    }
    p->nloop = 0;                                      /* ran off the memory: drop */
}
