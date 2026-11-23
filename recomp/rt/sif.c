/* sif.c — the SIF link between the EE and the I/O processor, at the level of
 * the sifcmd / sifrpc protocol.
 *
 * The game's EE-side SIF library is real recompiled code. It sends command
 * packets with SifSetDma and expects replies to arrive in its packet buffer,
 * announced by the SIF0 DMA interrupt. The IOP side here is native: commands
 * are decoded, RPC binds/calls are dispatched to the service table in
 * iop.c, and replies are queued and delivered one per interrupt. */
#include "rt.h"
#include "iop.h"

#define SIF_CMD_CHANGE_SADDR 0x80000000u
#define SIF_CMD_SET_SREG     0x80000001u
#define SIF_CMD_INIT_CMD     0x80000002u
#define SIF_CMD_RESET_CMD    0x80000003u
#define SIF_CMD_RPC_END      0x80000008u
#define SIF_CMD_RPC_BIND     0x80000009u
#define SIF_CMD_RPC_CALL     0x8000000Au
#define SIF_CMD_RPC_RDATA    0x8000000Cu

#define IOP_CMDBUF   0x00010000u      /* IOP-side command packet buffer */
#define SIF_DELAY    3000             /* ticks between packet deliveries */
#define SM_FLAGS     0x00070000u      /* SIFINIT | CMDINIT | BOOTEND */

u8 g_iop_ram[IOP_RAM_SIZE];
static u32 iop_heap = 0x00100000u;    /* bump allocator for service buffers */

static u32 ee_pktbuf;                 /* where the EE receives packets */
static u32 sif_reg[8];                /* MAINADDR, SUBADDR, MSFLAG, SMFLAG, ... */
static u32 sif_sysreg[32];
static u32 dma_id = 1;

typedef struct Pending {
    u8  pkt[128]; u32 psize;
    u8 *data; u32 dsize, ddest;       /* extra data copied to EE memory first */
    u64 not_before;
    struct Pending *next;
} Pending;
static Pending *q_head, *q_tail;
static int deliver_scheduled;

u32 iop_alloc(u32 size) {
    u32 a = iop_heap;
    iop_heap += (size + 0x3F) & ~0x3Fu;
    if (iop_heap > IOP_RAM_SIZE) rt_fatal(0, "IOP heap exhausted");
    return a;
}

static void deliver(void *arg);
static void schedule_deliver(u64 when) {
    if (deliver_scheduled) return;
    deliver_scheduled = 1;
    kernel_schedule(when, deliver, 0);
}
static void deliver(void *arg) {
    (void)arg;
    deliver_scheduled = 0;
    Pending *p = q_head;
    if (!p) return;
    /* previous packet not yet taken by the EE handler, or nowhere to put it */
    if ((g_dmac_stat & (1u << DMAC_SIF0)) || !ee_pktbuf || p->not_before > g_now) {
        schedule_deliver((p->not_before > g_now ? p->not_before : g_now) + SIF_DELAY);
        return;
    }
    q_head = p->next;
    if (!q_head) q_tail = 0;
    if (p->dsize) memcpy(gptr(p->ddest), p->data, p->dsize);
    memcpy(gptr(ee_pktbuf), p->pkt, p->psize);
    free(p->data);
    free(p);
    dmac_raise(DMAC_SIF0);
    if (q_head) schedule_deliver(g_now + SIF_DELAY);
}

/* Queue a command packet for the EE. `payload` follows the 16-byte header. */
void sif_send_cmd(u32 cid, u32 opt, const void *payload, u32 payload_size,
                  const void *data, u32 dsize, u32 ddest, u64 delay) {
    Pending *p = calloc(1, sizeof *p);
    p->psize = 16 + payload_size;
    if (p->psize > sizeof p->pkt) rt_fatal(0, "SIF packet too large");
    u32 hdr[4] = { p->psize | (dsize << 8), ddest, cid, opt };
    memcpy(p->pkt, hdr, 16);
    if (payload_size) memcpy(p->pkt + 16, payload, payload_size);
    if (dsize) { p->data = malloc(dsize); memcpy(p->data, data, dsize); p->dsize = dsize; p->ddest = ddest; }
    p->not_before = g_now + delay;
    if (q_tail) q_tail->next = p; else q_head = p;
    q_tail = p;
    schedule_deliver(g_now + (delay > SIF_DELAY ? delay : SIF_DELAY));
}

static void rpc_end(const u32 *req, u32 cid, u32 server, u32 buff, u32 cbuff,
                    const void *data, u32 dsize, u32 ddest, u64 delay) {
    /* rec_id, pkt_addr, rpc_id, client, cid, server, buff, cbuff */
    u32 body[8] = { req[4], req[5], req[6], req[7], cid, server, buff, cbuff };
    sif_send_cmd(SIF_CMD_RPC_END, 0, body, sizeof body, data, dsize, ddest, delay);
}

static void handle_cmd(Ctx *c, const u8 *pkt, u32 size) {
    u32 w[32] = { 0 };
    memcpy(w, pkt, size < sizeof w ? size : sizeof w);
    u32 cid = w[2], opt = w[3];
    if (g_verbose) rt_log("[sif] EE cmd 0x%08x opt=%u size=%u\n", cid, opt, size);
    switch (cid) {
    case SIF_CMD_CHANGE_SADDR:
        ee_pktbuf = w[4];
        break;
    case SIF_CMD_SET_SREG:
        if (w[4] < 32) sif_sysreg[w[4]] = w[5];
        break;
    case SIF_CMD_INIT_CMD:
        if (opt == 0) ee_pktbuf = w[4];
        else {                                   /* RPC init: answer "sreg 0 = 1" */
            u32 body[2] = { 0, 1 };
            sif_send_cmd(SIF_CMD_SET_SREG, 0, body, sizeof body, 0, 0, 0, 0);
        }
        break;
    case SIF_CMD_RESET_CMD:
        /* IOP reboot with a new module image: services are native, so the
         * "reboot" completes immediately (BOOTEND stays set). */
        rt_log("[sif] IOP reset requested: \"%.*s\"\n", (int)(size - 24), (const char *)pkt + 24);
        iop_reset();
        break;
    case SIF_CMD_RPC_BIND: {
        u32 sid = w[8];
        IopServer *s = iop_find_server(sid);
        if (!s) {
            rt_log("[sif] bind to unknown RPC server 0x%08x\n", sid);
            rpc_end(w, SIF_CMD_RPC_BIND, 0, 0, 0, 0, 0, 0, 0);
            break;
        }
        if (!s->buf) { s->buf = iop_alloc(s->buf_size ? s->buf_size : 0x1000); s->handle = iop_alloc(0x40); }
        if (g_verbose) rt_log("[sif] bind 0x%08x (%s)\n", sid, s->name);
        rpc_end(w, SIF_CMD_RPC_BIND, s->handle, s->buf, s->buf, 0, 0, 0, 0);
        break;
    }
    case SIF_CMD_RPC_CALL: {
        IopServer *s = iop_server_by_handle(w[13]);
        if (!s) rt_fatal(c, "SIF RPC call to unknown server handle 0x%x", w[13]);
        RpcCall rc = { .fno = w[8], .send = g_iop_ram + s->buf, .send_size = w[9],
                       .ee_recv = w[10], .recv_size = w[11], .ctx = c, .server = s };
        static u8 result[0x4000];
        if (rc.recv_size > sizeof result) rt_fatal(c, "RPC %s: recv_size 0x%x too large", s->name, rc.recv_size);
        memset(result, 0, rc.recv_size);
        rc.recv = result;
        if (g_verbose) rt_log("[rpc] %s fn=%u send=%u recv=%u\n", s->name, rc.fno, rc.send_size, rc.recv_size);
        s->call(&rc);
        if (rc.no_reply) break;
        if (w[12])                               /* rmode: caller wants an END packet */
            rpc_end(w, SIF_CMD_RPC_CALL, 0, 0, 0, result, rc.recv_size, rc.ee_recv, rc.delay);
        else if (rc.recv_size) memcpy(gptr(rc.ee_recv), result, rc.recv_size);
        break;
    }
    case SIF_CMD_RPC_RDATA: {                    /* sceSifGetOtherData */
        u32 src = w[8], dest = w[9], n = w[10];
        if (src + n > IOP_RAM_SIZE) rt_fatal(c, "SIF RDATA outside IOP RAM (0x%x+0x%x)", src, n);
        rpc_end(w, SIF_CMD_RPC_RDATA, 0, 0, 0, g_iop_ram + src, n, dest, 0);
        break;
    }
    default:
        if (!iop_custom_cmd(c, cid, pkt, size))
            rt_fatal(c, "unhandled SIF command 0x%08x (size %u)", cid, size);
    }
}

/* SifSetDma(SifDmaTransfer_t *sdd, int count): { src, dest, size, attr } */
u32 sif_set_dma(Ctx *c, u32 sdd, u32 count) {
    for (u32 i = 0; i < count; i++) {
        u32 src = g32(sdd + i * 16), dest = g32(sdd + i * 16 + 4) & 0x1FFFFFFFu;
        u32 size = g32(sdd + i * 16 + 8);
        if (dest == IOP_CMDBUF) { handle_cmd(c, gptr(src), size); continue; }
        if (dest + size > IOP_RAM_SIZE) rt_fatal(c, "SifSetDma to IOP 0x%x+0x%x outside IOP RAM", dest, size);
        memcpy(g_iop_ram + dest, gptr(src), size);
    }
    return dma_id++ ? dma_id : (dma_id = 2);
}
u32 sif_get_reg(u32 r) {
    if (r & 0x80000000u) return sif_sysreg[r & 31];
    switch (r) {
    case 2: return IOP_CMDBUF;                   /* SUBADDR: IOP command buffer */
    case 4: return SM_FLAGS;                     /* SMFLAG */
    }
    return r < 8 ? sif_reg[r] : 0;
}
void sif_set_reg(u32 r, u32 v) {
    if (r & 0x80000000u) sif_sysreg[r & 31] = v;
    else if (r < 8) sif_reg[r] = v;
}

void sif_dump(void) {
    int n = 0; for (Pending *p = q_head; p; p = p->next) n++;
    fprintf(stderr, "  sif: %d packets queued, deliver_scheduled=%d, ee_pktbuf=%08x", n, deliver_scheduled, ee_pktbuf);
    if (q_head) { u32 cid; memcpy(&cid, q_head->pkt + 8, 4); fprintf(stderr, ", head cid=%08x not_before=%llu now=%llu", cid, (unsigned long long)q_head->not_before, (unsigned long long)g_now); }
    fprintf(stderr, "\n");
}
