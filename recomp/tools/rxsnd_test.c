/* rxsnd_test.c — drives recomp/rt/iop_rxsnd.c through the RPC interface the
 * way the game's EE library does, with real bank data, without the game.
 * Build and run with recomp/tools/rxsnd_test.sh. Exits non-zero on failure. */
#include <stdarg.h>
#include <sys/mman.h>
#include "iop.h"

u8 g_iop_ram[IOP_RAM_SIZE];
u64 g_now; int g_verbose;
const RorGame ror_game = { "pal", "SLES-54218", "SLES_542.18", "Europe", 0, 0, 0 };   /* 50 Hz timing */
static u32 heap = 0x100000;
u32 iop_alloc(u32 size) { u32 a = heap; heap += (size + 63) & ~63u; return a; }
u64 kernel_now(Ctx *c) { (void)c; return g_now; }
void rt_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); }
void rt_fatal(Ctx *c, const char *fmt, ...) { (void)c; va_list ap; va_start(ap, fmt); printf("FATAL: "); vprintf(fmt, ap); printf("\n"); va_end(ap); exit(2); }

static u8 sendbuf[0x4000], recvbuf[0x4000];
static s32 rpc(u32 cmd, const void *in, u32 in_size, u32 out_size) {
    RpcCall rc = { .fno = cmd, .send = sendbuf, .send_size = in_size, .recv = recvbuf, .recv_size = out_size + 4 };
    memset(recvbuf, 0, sizeof recvbuf);
    if (in_size) memcpy(sendbuf, in, in_size);
    rxsnd_call(&rc);
    return (s32)rd32(recvbuf);
}
/* a batch of one command, like the EE queue */
static u8 *batch1(u32 cmd, const void *in, u32 in_size, u32 out_size) {
    u8 q[0x1000]; u32 h[3] = { cmd, out_size, in_size };
    memcpy(q, h, 12); if (in_size) memcpy(q + 12, in, in_size);
    memset(q + 12 + in_size, 0, 12);
    rpc(512, q, in_size + 24, out_size);
    return recvbuf + 4;
}
static void frame(void) { g_now += TICKS_PER_FRAME; }
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); exit(1); } } while (0)

static int se_listed(int slot, int *flags) {
    u8 *o = batch1(131, 0, 0, 260);
    for (u32 i = 0; i < rd32(o + 256); i++) if (o[4 * i] == slot) { *flags = o[4 * i + 1]; return 1; }
    return 0;
}

int main(int argc, char **argv) {
    CHECK(argc == 2, "usage: rxsnd_test BANK.SE");
    void *ram = mmap((void *)MEMBASE, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    CHECK(ram == (void *)MEMBASE, "mmap");
    FILE *f = fopen(argv[1], "rb"); CHECK(f, "open %s", argv[1]);
    u8 *bank = gptr(0x100000); size_t blen = fread(bank, 1, 0x800000, f); fclose(f);
    u32 hsize = rd32(bank + 8), body = (hsize + 15) & ~15u, bsize = rd32(bank + 0x14 + 4);
    CHECK(body + bsize == blen, "bank layout");

    /* --- heap --- */
    u32 a3[3] = { 0, 64, 0 };
    u32 p1 = (u32)rpc(32, a3, 12, 0), p2 = (u32)rpc(32, a3, 12, 0);
    CHECK(p1 && p2 && p1 != p2, "alloc");
    u32 q = 2; u32 free0 = (u32)rpc(34, &q, 4, 0);
    rpc(33, &p2, 4, 0);
    CHECK((u32)rpc(34, &q, 4, 0) == free0 + 0x100, "free returns memory");
    CHECK((u32)rpc(32, a3, 12, 0) == p2, "first fit reuses the freed block");

    /* --- XVAB open: header to IOP RAM, cmd 80, body EE->SPU (type 3), poll 65 --- */
    a3[1] = (hsize + 63) & ~63u;
    u32 hdr = (u32)rpc(32, a3, 12, 0); CHECK(hdr, "header alloc");
    memcpy(g_iop_ram + hdr, bank, hsize);
    u32 o80[3] = { 5, hdr, 0x5010 };
    CHECK(rpc(80, o80, 12, 0) == 5, "XVAB open");
    CHECK(rpc(80, o80, 12, 0) == -1, "XVAB open twice");
    u8 x64[16] = { 3, 1 }; wr32(x64 + 4, 0x5010); wr32(x64 + 8, 0x100000 + body); wr32(x64 + 12, bsize);
    s32 id = rpc(64, x64, 16, 0); CHECK(id >= 0, "transfer id");
    CHECK(rpc(65, &id, 4, 0) == 0, "transfer done");

    /* --- SE: play (slot 63, XVAB 5, group 0, index 0), poll until not active, stop, wait until unlisted --- */
    int fl = 0;
    u8 play[4] = { 63, 5, 0, 0 };
    batch1(128, play, 4, 0);
    CHECK(se_listed(63, &fl) && fl == 3, "SE listed as 0x03 right after play (flags %x)", fl);
    int n;
    for (n = 0; n < 3000; n++) { frame(); CHECK(se_listed(63, &fl), "SE vanished without stop"); if (!(fl & 2)) break; }
    CHECK(n < 3000, "SE never finished");
    CHECK(n > 9, "SE finished implausibly fast (%d frames)", n);
    printf("SE (0,0) finished after %d frames, flags 0x%02x\n", n, fl);
    u32 stop[3] = { 63, 0, 1 };
    batch1(130, stop, 12, 0);
    for (n = 0; n < 100 && se_listed(63, &fl); n++) frame();
    CHECK(n < 100, "SE slot not released after stop");
    printf("SE slot released %d frames after stop\n", n);
    /* slot is reusable; a missing SE ends at once but stays listed until stopped */
    u8 bad[4] = { 63, 5, 200, 200 };
    batch1(128, bad, 4, 0); frame(); frame();
    CHECK(se_listed(63, &fl) && !(fl & 2), "unregistered SE reported finished");
    stop[2] = 2; batch1(130, stop, 12, 0); frame(); frame();
    CHECK(!se_listed(63, &fl), "kill-stop releases the slot");
    CHECK(rpc(81, &o80[0], 4, 0) == 0, "XVAB close with no users");
    CHECK(rpc(81, &o80[0], 4, 0) == 1, "XVAB close twice");

    /* --- ADPCM stream: mono, 48 kHz, 4096-byte IOP ring, 8192-byte SPU ring --- */
    a3[1] = 4096; u32 ring = (u32)rpc(32, a3, 12, 0); CHECK(ring, "ring alloc");
    u8 op[24] = { 7, 0, 1, 0 }; s16 prio = -1, pitch = 4096;
    memcpy(op + 4, &prio, 2); memcpy(op + 6, &pitch, 2); wr32(op + 8, 4096); wr32(op + 12, 8192);
    wr32(op + 16, ring); wr32(op + 20, 0x100000);
    batch1(160, op, 24, 0);
    u8 *st = batch1(163, 0, 0, 772);
    CHECK(rd32(st + 768) == 1 && st[0] == 7 && st[1] == 1 && rd32(st + 4) == 4096, "ADPCM status after open");
    /* feed like the EE: fill whatever is free each frame, flag 3 on the last block of the SPU ring */
    u32 total = 0, target = 64 * 1024, ee_wr = 0, spu_wr = 0; u64 played = 0; int ended = 0, playing_seen = 0;
    for (n = 0; n < 3000; n++) {
        st = batch1(163, 0, 0, 772);
        CHECK(rd32(st + 768) == 1, "ADPCM slot listed");
        if (st[1] & 2) playing_seen = 1;
        if (st[1] & 4) break;
        if (rd32(st + 8) != 0xFFFFFFFFu) { u64 p = (u64)rd32(st + 8) << 32 | rd32(st + 12); CHECK(p >= played, "position went back"); played = p; }
        u32 room = rd32(st + 4) & ~63u;
        if (!ended && room) {
            u32 nb = room; int last = 0;
            if (total + nb >= target) { nb = target - total; last = 1; }
            for (u32 i = 0; i < nb; i += 16) {
                u8 blk[16] = { 0 };
                if ((spu_wr + 16) % 8192 == 0) blk[1] = 3;
                if (last && i + 16 == nb) blk[1] = 1;
                memcpy(g_iop_ram + ring + ee_wr, blk, 16);
                ee_wr = (ee_wr + 16) % 4096; spu_wr = (spu_wr + 16) % 8192;
            }
            u8 fd[8] = { 7, (u8)last }; wr32(fd + 4, nb);
            batch1(161, fd, 8, 0);
            total += nb; ended = last;
        }
        frame();
    }
    CHECK(n < 3000, "ADPCM stream never finished");
    CHECK(playing_seen, "ADPCM stream never reported playing");
    /* 65536 bytes at 27428.6 bytes/s = 2.39 s = 119 frames */
    printf("ADPCM stream: %u bytes finished after %d frames, last position %llu\n", total, n, (unsigned long long)played);
    CHECK(n >= 115 && n <= 135, "ADPCM stream duration %d frames, expected about 120", n);
    CHECK(played + 4096 >= target && played <= target, "ADPCM final position %llu", (unsigned long long)played);
    u32 cl[3] = { 7, 0, 0 };
    batch1(162, cl, 12, 0); frame();
    st = batch1(163, 0, 0, 772);
    CHECK(rd32(st + 768) == 0, "ADPCM slot gone after close");

    /* --- PCM stream: mode 0, 4 blocks --- */
    u8 po[12] = { 3, 0 }; wr32(po + 4, ring); wr32(po + 8, 2);
    batch1(176, po, 12, 0);
    u8 pf[12] = { 3, 2 }; wr32(pf + 4, 10); wr32(pf + 8, 11);
    batch1(177, pf, 12, 0);
    u32 pe[3] = { 3, 5, 0 }; batch1(178, pe, 12, 0);
    int tags = 0;
    for (n = 0; n < 50; n++) {
        frame(); st = batch1(179, 0, 0, 388);
        CHECK(rd32(st + 384) == 1 && st[0] == 3, "PCM listed");
        if ((st[1] & 2) && rd32(st + 4) == 10) tags |= 1;
        if ((st[1] & 2) && rd32(st + 4) == 11) tags |= 2;
        if (st[1] & 4) break;
    }
    CHECK(n < 50 && tags == 3, "PCM stream played both tags and finished (n=%d tags=%d)", n, tags);
    printf("PCM stream finished after %d frames\n", n);
    u32 pc[3] = { 3, 0, 0 }; batch1(178, pc, 12, 0); frame();
    st = batch1(179, 0, 0, 388); CHECK(rd32(st + 384) == 0, "PCM slot gone after close");

    /* --- status blocks with nothing active, shutdown --- */
    CHECK(rd32(batch1(114, 0, 0, 260) + 256) == 0, "BGM status empty");
    CHECK(rpc(17, 0, 0, 0) == 0, "cmd 17 first answer");
    printf("all checks passed\n");
    return 0;
}
