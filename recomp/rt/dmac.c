/* dmac.c — EE DMA controller. A transfer runs to completion when the
 * channel is started (STR set); the channel interrupt is raised right after.
 * Source-chain tags are followed for VIF0, VIF1, GIF and toSPR. */
#include "gs.h"
#include "vu.h"

typedef struct { u32 chcr, madr, qwc, tadr, asr[2], sadr; } Chan;
static Chan ch[10];
static u32 d_ctrl, d_pcr, d_sqwc, d_rbsr, d_rbor, d_enable = 0x1201;
extern u32 g_dmac_mask;

static int chan_of(u32 a) {
    switch (a & 0xFF00) {
    case 0x8000: return 0; case 0x9000: return 1; case 0xA000: return 2;
    case 0xB000: return 3; case 0xB400: return 4; case 0xC000: return 5;
    case 0xC400: return 6; case 0xC800: return 7; case 0xD000: return 8; case 0xD400: return 9;
    }
    return -1;
}
static u8 *dma_ptr(u32 addr) {
    if (addr & 0x80000000u) return HPTR(0x70000000u + (addr & 0x3FF0));
    return HPTR(addr & 0x01FFFFF0u);
}
static void sink(int n, const u8 *p, u32 bytes) {
    switch (n) {
    case 0: vif_write(0, p, bytes); break;
    case 1: vif_write(1, p, bytes); break;
    case 2: gif_write(GIF_PATH3, p, bytes); break;
    case 9: memcpy(HPTR(0x70000000u + (ch[9].sadr & 0x3FF0)), p, bytes);
            ch[9].sadr = (ch[9].sadr + bytes) & 0x3FFF; break;
    default: break;
    }
}

int  ipu_out_pull(u8 *qw);
void ipu_input_ready(void);
static int ch4_end;

static void finish(int n) { ch[n].chcr &= ~0x100u; dmac_raise(n); }

/* IPU output -> memory (channel 3, normal mode). */
void dmac_ipu_out_kick(void) {
    Chan *c = &ch[3];
    while ((c->chcr & 0x100) && c->qwc && ipu_out_pull(dma_ptr(c->madr))) { c->madr += 16; c->qwc--; }
    if ((c->chcr & 0x100) && c->qwc == 0) finish(3);
}
/* Next quadword of IPU input from channel 4 (normal or source chain). */
int dmac_ipu_in_pull(u8 *qw) {
    Chan *c = &ch[4];
    int chain = ((c->chcr >> 2) & 3) == 1;
    for (;;) {
        if (!(c->chcr & 0x100)) return 0;
        if (c->qwc) break;
        if (!chain || ch4_end) { ch4_end = 0; finish(4); return 0; }
        u64 tag; memcpy(&tag, dma_ptr(c->tadr), 8);
        u32 id = (tag >> 28) & 7, addr = (u32)(tag >> 32);
        c->qwc = tag & 0xFFFF;
        c->chcr = (c->chcr & 0xFFFF) | ((u32)tag & 0xFFFF0000u);
        switch (id) {
        case 0: c->madr = addr; c->tadr += 16; ch4_end = 1; break;
        case 1: c->madr = c->tadr + 16; c->tadr = c->madr + c->qwc * 16; break;
        case 2: c->madr = c->tadr + 16; c->tadr = addr; break;
        case 3: case 4: c->madr = addr; c->tadr += 16; break;
        default: c->madr = c->tadr + 16; ch4_end = 1; break;
        }
        if (((tag >> 31) & 1) && (c->chcr & 0x80)) ch4_end = 1;
    }
    memcpy(qw, dma_ptr(c->madr), 16);
    c->madr += 16; c->qwc--;
    if (c->qwc == 0 && (!chain || ch4_end)) { ch4_end = 0; finish(4); }
    return 1;
}

static void run(int n) {
    Chan *c = &ch[n];
    u32 mode = (c->chcr >> 2) & 3;
    if ((n == 3 || n == 4) && g_verbose) { static u32 k; if (k++ < 60) rt_log("[dmac] ch%d start chcr=%x madr=%x qwc=%x tadr=%x\n", n, c->chcr, c->madr, c->qwc, c->tadr); }
    if (n == 3) { dmac_ipu_out_kick(); return; }       /* fromIPU: filled as the IPU produces output */
    if (n == 4) { ch4_end = 0; ipu_input_ready(); return; }   /* toIPU: pulled by the IPU as it consumes */
    if (n == 8) {                               /* fromSPR: scratchpad -> memory */
        memcpy(dma_ptr(c->madr), HPTR(0x70000000u + (c->sadr & 0x3FF0)), (size_t)c->qwc * 16);
        c->madr += c->qwc * 16; c->sadr = (c->sadr + c->qwc * 16) & 0x3FFF; c->qwc = 0;
    } else if (n == 5 || n == 6 || n == 7) {
        /* SIF channels are driven by the SIF layer (SifSetDma), not from here */
    } else if (n == 1 && !(c->chcr & 1)) {
        rt_log("[dmac] VIF1 read-back transfer requested (not implemented)\n");
        c->qwc = 0;
    } else if (mode != 1) {                            /* normal / interleave */
        sink(n, dma_ptr(c->madr), c->qwc * 16);
        c->madr += c->qwc * 16; c->qwc = 0;
    } else {                                           /* source chain */
        int asp = (c->chcr >> 4) & 3, tte = (c->chcr >> 6) & 1, tie = (c->chcr >> 7) & 1;
        if (c->qwc) {                                  /* resume a tag that was in progress */
            sink(n, dma_ptr(c->madr), c->qwc * 16);
            c->madr += c->qwc * 16; c->qwc = 0;
        }
        for (int guard = 0; guard < 1000000; guard++) {
            const u8 *t = dma_ptr(c->tadr);
            u64 tag; memcpy(&tag, t, 8);
            u32 qwc = tag & 0xFFFF, id = (tag >> 28) & 7, irq = (tag >> 31) & 1, addr = (u32)(tag >> 32);
            int end = 0;
            c->chcr = (c->chcr & 0xFFFF) | ((u32)tag & 0xFFFF0000u);
            if (tte && (n == 0 || n == 1)) sink(n, t + 8, 8);
            switch (id) {
            case 0: c->madr = addr; c->tadr += 16; end = 1; break;                 /* refe */
            case 1: c->madr = c->tadr + 16; c->tadr = c->madr + qwc * 16; break;   /* cnt */
            case 2: c->madr = c->tadr + 16; c->tadr = addr; break;                 /* next */
            case 3: case 4: c->madr = addr; c->tadr += 16; break;                  /* ref / refs */
            case 5:                                                               /* call */
                c->madr = c->tadr + 16;
                if (asp < 2) c->asr[asp++] = c->madr + qwc * 16;
                c->tadr = addr; break;
            case 6:                                                               /* ret */
                c->madr = c->tadr + 16;
                if (asp > 0) c->tadr = c->asr[--asp]; else end = 1;
                break;
            default: c->madr = c->tadr + 16; end = 1; break;                       /* end */
            }
            sink(n, dma_ptr(c->madr), qwc * 16);
            c->madr += qwc * 16;
            if (end || (irq && tie)) break;
        }
        c->chcr = (c->chcr & ~0x30u) | ((u32)asp << 4);
    }
    c->chcr &= ~0x100u;
    dmac_raise(n);
}

u32 dmac_read(u32 a) {
    switch (a) {
    case 0x1000E000: return d_ctrl;
    case 0x1000E010: return g_dmac_stat | (g_dmac_mask << 16);
    case 0x1000E020: return d_pcr;
    case 0x1000E030: return d_sqwc;
    case 0x1000E040: return d_rbsr;
    case 0x1000E050: return d_rbor;
    case 0x1000F520: return d_enable;
    }
    int n = chan_of(a);
    if (n < 0) return 0;
    Chan *c = &ch[n];
    switch (a & 0xFF) {
    case 0x00: return c->chcr; case 0x10: return c->madr; case 0x20: return c->qwc;
    case 0x30: return c->tadr; case 0x40: return c->asr[0]; case 0x50: return c->asr[1];
    case 0x80: return c->sadr;
    }
    return 0;
}
void dmac_write(u32 a, u32 v) {
    switch (a) {
    case 0x1000E000: d_ctrl = v; return;
    case 0x1000E010:                                   /* status: write 1 clears; mask: write 1 toggles */
        g_dmac_stat &= ~(v & 0xFFFF);
        g_dmac_mask ^= (v >> 16) & 0x3FF;
        return;
    case 0x1000E020: d_pcr = v; return;
    case 0x1000E030: d_sqwc = v; return;
    case 0x1000E040: d_rbsr = v; return;
    case 0x1000E050: d_rbor = v; return;
    case 0x1000F590: {                                 /* D_ENABLEW: bit 16 suspends all transfers */
        u32 was = d_enable;
        d_enable = v;
        if ((was & 0x10000) && !(v & 0x10000) && (d_ctrl & 1))
            for (int i = 0; i < 10; i++) if (ch[i].chcr & 0x100) run(i);   /* start what was armed meanwhile */
        return;
    }
    }
    int n = chan_of(a);
    if (n < 0) return;
    Chan *c = &ch[n];
    switch (a & 0xFF) {
    case 0x00:
        if ((n == 3 || n == 4) && g_verbose) { static u32 k; if (k++ < 40) rt_log("[dmac] ch%d chcr write %x (ctrl=%x enable=%x)\n", n, v, d_ctrl, d_enable); }
        c->chcr = v;
        if ((v & 0x100) && (d_ctrl & 1) && !(d_enable & 0x10000)) run(n);
        break;
    case 0x10: c->madr = v; break;
    case 0x20: c->qwc = v & 0xFFFF; break;
    case 0x30: c->tadr = v; break;
    case 0x40: c->asr[0] = v; break;
    case 0x50: c->asr[1] = v; break;
    case 0x80: c->sadr = v; break;
    }
}

void dmac_dump(void) {
    for (int i = 0; i < 10; i++)
        fprintf(stderr, "  dma ch%d: chcr=%08x madr=%08x qwc=%04x tadr=%08x\n", i, ch[i].chcr, ch[i].madr, ch[i].qwc, ch[i].tadr);
}
