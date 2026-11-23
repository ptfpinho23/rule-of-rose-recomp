/* iop.c — I/O-processor services (RPC servers), implemented natively. */
#include "iop.h"

/* ---- LOADFILE (0x80000006): sceSifLoadModule and friends ---------------- */
static int next_module_id = 30;
static void loadfile_call(RpcCall *rc) {
    switch (rc->fno) {
    case 0: {  /* sceSifLoadModule: { arg_len, modres, path[252], args[252] } -> { id, modres }
                * The module's services are provided natively; loading it only
                * hands out a module id. */
        rt_log("[iop] load module %s\n", (const char *)rc->send + 8);
        wr32(rc->recv, (u32)next_module_id++);
        wr32(rc->recv + 4, 0);
        break;
    }
    case 255:  /* module version tag, checked by the EE library */
        memcpy(rc->recv, "3000", 4);
        break;
    default:
        rt_log("[rpc] loadfile fn=%u first words:", rc->fno);
        for (u32 i = 0; i < rc->send_size && i < 32; i += 4) rt_log(" %08x", rd32(rc->send + i));
        rt_log("\n");
        rt_fatal(rc->ctx, "LOADFILE: unimplemented function %u (send=%u recv=%u)", rc->fno, rc->send_size, rc->recv_size);
    }
}

/* ---- IOP heap (0x80000003): sceSifAllocIopHeap / sceSifFreeIopHeap ------ */
static void iopheap_call(RpcCall *rc) {
    switch (rc->fno) {
    case 1: wr32(rc->recv, iop_alloc(rd32(rc->send))); break;   /* alloc(size) -> addr */
    case 2: wr32(rc->recv, 0); break;                           /* free(addr) -> 0 */
    default:
        rt_fatal(rc->ctx, "IOPHEAP: unimplemented function %u", rc->fno);
    }
}

/* ---- CD/DVD drive (cdvdfsv RPC servers) ---------------------------------
 * 0x80000592 init, 0x80000593 S-commands (drive status), 0x80000595
 * N-commands (reads), 0x80000597 file search, 0x8000059A disk ready. */
#define CD_TICKS_PER_SECTOR 4000
static void cdvd_init_call(RpcCall *rc) {
    /* sceCdInit(mode) -> { result, cdvdfsv version, cdvdman version, debug } */
    wr32(rc->recv, 0); wr32(rc->recv + 4, 0x0225); wr32(rc->recv + 8, 0x0225); wr32(rc->recv + 12, 0);
}
static void cdvd_ready_call(RpcCall *rc) { wr32(rc->recv, 2); }   /* SCECdComplete */
static void cdvd_scmd_call(RpcCall *rc) {
    switch (rc->fno) {
    case 0x01: {  /* sceCdReadClock -> { result, stat, sec, min, hour, pad, day, month, year } (BCD) */
        static const u8 clock[8] = { 0, 0x00, 0x00, 0x12, 0, 0x01, 0x01, 0x06 };
        wr32(rc->recv, 1); memcpy(rc->recv + 4, clock, 8); break;
    }
    case 0x03: wr32(rc->recv, 0x14); break;     /* sceCdGetDiskType: PS2 DVD */
    case 0x04: wr32(rc->recv, 0); break;        /* sceCdGetError: none */
    case 0x05: wr32(rc->recv, 1); wr32(rc->recv + 4, 0); break;  /* sceCdTrayReq: ok, no change */
    case 0x0C: wr32(rc->recv, 0x0A); break;     /* sceCdStatus: spinning, paused */
    case 0x16: wr32(rc->recv, 1); break;        /* sceCdBreak */
    case 0x1F: wr32(rc->recv, 1); wr32(rc->recv + 4, 0); break;  /* sceCdCancelPOffRdy */
    case 0x22: wr32(rc->recv, 1); break;        /* sceCdMmode */
    case 0x27: wr32(rc->recv, 1); wr32(rc->recv + 4, 0); wr32(rc->recv + 8, 0); break;  /* sceCdReadDvdDualInfo: single layer */
    case 0x23: wr32(rc->recv, 1); break;        /* sceCdChangeThreadPriority */
    default:
        rt_fatal(rc->ctx, "CDVD S-command 0x%x not implemented (send=%u recv=%u)", rc->fno, rc->send_size, rc->recv_size);
    }
}
static void cdvd_ncmd_call(RpcCall *rc) {
    switch (rc->fno) {
    case 0x01: case 0x03: {  /* sceCdRead / sceCdReadDVDV: { lsn, sectors, buf, mode, intr_data, readpos } */
        u32 lsn = rd32(rc->send), n = rd32(rc->send + 4), buf = rd32(rc->send + 8);
        u32 intr = rd32(rc->send + 16), readpos = rd32(rc->send + 20);
        if (g_verbose) rt_log("[cdvd] read lsn=%u sectors=%u -> 0x%08x\n", lsn, n, buf);
        if ((buf & 0x1FFFFFFFu) + (u64)n * 2048 > RAM_SIZE) rt_fatal(rc->ctx, "sceCdRead buffer 0x%x+%u sectors outside RAM", buf, n);
        iso_read(lsn, n, gptr(buf));
        /* The data is delivered whole, so the EE-side fix-up record for the
         * unaligned head/tail carries no bytes. */
        if (intr) { p32(intr, 0); p32(intr + 4, 0); }
        if (readpos) p32(readpos, n * 2048);
        rc->delay = (u64)n * CD_TICKS_PER_SECTOR;
        break;
    }
    case 0x05: case 0x06: case 0x07: case 0x08:   /* seek / standby / stop / pause */
        break;
    case 0x0E: wr32(rc->recv, 2); break;          /* sceCdDiskReady */
    default:
        rt_fatal(rc->ctx, "CDVD N-command 0x%x not implemented (send=%u recv=%u)", rc->fno, rc->send_size, rc->recv_size);
    }
}
static void cdvd_search_call(RpcCall *rc) {
    /* sceCdSearchFile: { sceCdlFILE file; char name[256]; void *dest } -> found
     * sceCdlFILE = { lsn, size, name[16], date[8] } (+ flag in newer libs) */
    u32 fsize = rc->send_size - 260;
    const char *name = (const char *)rc->send + fsize;
    u32 dest = rd32(rc->send + rc->send_size - 4);
    u32 lsn, size;
    int ok = iso_find(name, &lsn, &size);
    rt_log("[cdvd] search \"%s\" -> %s", name, ok ? "found" : "NOT FOUND");
    if (ok) {
        rt_log(" lsn=%u size=%u\n", lsn, size);
        u8 f[36] = { 0 };
        wr32(f, lsn); wr32(f + 4, size);
        const char *base = strrchr(name, '\\'); base = base ? base + 1 : name;
        strncpy((char *)f + 8, base, 15);
        memcpy(gptr(dest), f, fsize < sizeof f ? fsize : sizeof f);
    } else rt_log("\n");
    wr32(rc->recv, (u32)ok);
}

/* ---- FILEIO (0x80000001), libkernl 3.0 protocol -------------------------
 * Every request starts with { sema, result_ptr, result_size } followed by
 * the arguments. The RPC itself only acknowledges (recv word != 0). The
 * result arrives later: the IOP writes
 *     { sema, function, result_ptr, result_size, result bytes... }
 * into the reply buffer the EE registered at init, then sends SIF command
 * 0x80000011; the game's handler copies the result out and signals `sema`.
 * (Layouts taken from the game's own sceOpen/sceRead/sceLseek/sceClose and
 * its 0x80000011 handler at 0x263cb8.) Only the disc device exists. */
#define SIF_CMD_FILEIO_DONE 0x80000011u
#define MAX_FDS 32
static struct { int used; u32 lsn, size, pos; } fds[MAX_FDS];
static u32 fio_reply_buf;

static void fio_reply(RpcCall *rc, s32 result, u64 delay) {
    u8 r[36] = { 0 };
    wr32(r, rd32(rc->send)); wr32(r + 4, rc->fno);
    wr32(r + 8, rd32(rc->send + 4)); wr32(r + 12, rd32(rc->send + 8));
    wr32(r + 16, (u32)result);          /* +20.. : no unaligned head/tail bytes */
    sif_send_cmd(SIF_CMD_FILEIO_DONE, 0, 0, 0, r, sizeof r, fio_reply_buf, delay);
    wr32(rc->recv, 1);                  /* request accepted */
}
static void fileio_call(RpcCall *rc) {
    const u8 *a = rc->send;
    switch (rc->fno) {
    case 0: {   /* open: +12 mode, +16 perm, +20 name[1024] */
        const char *name = (const char *)a + 20;
        const char *path = strchr(name, ':');
        u32 lsn, size;
        int fd = -1;
        if (path && !strncmp(name, "cdrom", 5) && iso_find(path + 1, &lsn, &size)) {
            for (int i = 2; i < MAX_FDS; i++) if (!fds[i].used) { fd = i; break; }
            if (fd >= 0) { fds[fd].used = 1; fds[fd].lsn = lsn; fds[fd].size = size; fds[fd].pos = 0; }
        }
        rt_log("[fileio] open \"%s\" mode=0x%x -> %d\n", name, rd32(a + 12), fd);
        fio_reply(rc, fd >= 0 ? fd : -2 /* ENOENT */, 0);
        break;
    }
    case 1: {   /* close: +12 fd */
        u32 fd = rd32(a + 12);
        int ok = fd < MAX_FDS && fds[fd].used;
        if (ok) fds[fd].used = 0;
        fio_reply(rc, ok ? 0 : -9, 0);
        break;
    }
    case 2: {   /* read: +12 fd, +16 buffer, +20 size */
        u32 fd = rd32(a + 12), ptr = rd32(a + 16), n = rd32(a + 20);
        if (fd >= MAX_FDS || !fds[fd].used) { fio_reply(rc, -9, 0); break; }
        if (n > fds[fd].size - fds[fd].pos) n = fds[fd].size - fds[fd].pos;
        if ((ptr & 0x1FFFFFFFu) + (u64)n > RAM_SIZE) rt_fatal(rc->ctx, "sceRead buffer 0x%x+0x%x outside RAM", ptr, n);
        u32 first = fds[fd].pos / 2048, last = (fds[fd].pos + n + 2047) / 2048;
        u8 *tmp = malloc((size_t)(last - first) * 2048 + 1);
        iso_read(fds[fd].lsn + first, last - first, tmp);
        memcpy(gptr(ptr), tmp + fds[fd].pos % 2048, n);
        free(tmp);
        fds[fd].pos += n;
        if (g_verbose) rt_log("[fileio] read fd=%u %u bytes -> 0x%08x\n", fd, n, ptr);
        fio_reply(rc, (s32)n, (u64)(last - first) * CD_TICKS_PER_SECTOR);
        break;
    }
    case 4: {   /* lseek: +12 fd, +16 offset, +20 whence */
        u32 fd = rd32(a + 12); s32 off = (s32)rd32(a + 16); u32 whence = rd32(a + 20);
        if (fd >= MAX_FDS || !fds[fd].used) { fio_reply(rc, -9, 0); break; }
        s64 pos = whence == 0 ? off : whence == 1 ? (s64)fds[fd].pos + off : (s64)fds[fd].size + off;
        if (pos < 0) pos = 0;
        if (pos > fds[fd].size) pos = fds[fd].size;
        fds[fd].pos = (u32)pos;
        fio_reply(rc, (s32)pos, 0);
        break;
    }
    case 255:   /* init: { reply buffer 0, reply buffer 1 } -> module version tag */
        fio_reply_buf = rd32(a);
        memcpy(rc->recv, "3000", 4);
        break;
    default:
        rt_fatal(rc->ctx, "FILEIO function %u not implemented (send=%u recv=%u)", rc->fno, rc->send_size, rc->recv_size);
    }
}

/* Services that are known but not written yet: binding succeeds so the boot
 * sequence is visible; the first call stops with the details. */
static void todo_call(RpcCall *rc) {
    rt_log("[rpc] %s fn=%u send_size=%u recv_size=%u, first words:", rc->server->name, rc->fno, rc->send_size, rc->recv_size);
    for (u32 i = 0; i < rc->send_size && i < 48; i += 4) rt_log(" %08x", rd32(rc->send + i));
    rt_log("\n");
    rt_fatal(rc->ctx, "RPC service %s (0x%08x) is not implemented yet", rc->server->name, rc->server->sid);
}

static IopServer servers[] = {
    { 0x0000D000u, "rxsnddrv", rxsnd_call, 0x4000 },
    { 0x80001300u, "dbcman", dbc_call, 0x2100 },
    { 0x80000003u, "iopheap", iopheap_call, 0x100 },
    { 0x80000006u, "loadfile", loadfile_call, 0x400 },
    { 0x80000001u, "fileio", fileio_call, 0x1000 },
    { 0x80000592u, "cdvd-init", cdvd_init_call, 0x100 },
    { 0x80000593u, "cdvd-scmd", cdvd_scmd_call, 0x1000 },
    { 0x80000595u, "cdvd-ncmd", cdvd_ncmd_call, 0x1000 },
    { 0x80000597u, "cdvd-search", cdvd_search_call, 0x400 },
    { 0x8000059Au, "cdvd-ready", cdvd_ready_call, 0x100 },
    { 0x8000059Cu, "cdvd-ready2", cdvd_ready_call, 0x100 },   /* sceCdDiskReady, non-blocking */
    { 0x80000100u, "pad-0", todo_call, 0x400 },
    { 0x80000101u, "pad-1", todo_call, 0x400 },
    { 0x8000010Fu, "pad-ext", todo_call, 0x400 },
    { 0x80000400u, "mcserv", mc_call, 0x1000 },
    { 0x80000701u, "sdrdrv", todo_call, 0x1000 },
};
#define NSERVERS (sizeof servers / sizeof *servers)

/* The controller modules register several server ids in 0x800013xx (one
 * per function group); all of them go to dbc_call, which tells them apart by
 * rc->server->sid. */
static IopServer dyn_servers[32];
static unsigned n_dyn;

IopServer *iop_find_server(u32 sid) {
    for (unsigned i = 0; i < NSERVERS; i++) if (servers[i].sid == sid) return &servers[i];
    for (unsigned i = 0; i < n_dyn; i++) if (dyn_servers[i].sid == sid) return &dyn_servers[i];
    if ((sid & 0xFFFFFF00u) == 0x80001300u && n_dyn < 32) {
        dyn_servers[n_dyn] = (IopServer){ sid, "dbcman", dbc_call, 0x2100, 0, 0 };
        return &dyn_servers[n_dyn++];
    }
    return 0;
}
IopServer *iop_server_by_handle(u32 handle) {
    for (unsigned i = 0; i < NSERVERS; i++) if (servers[i].handle && servers[i].handle == handle) return &servers[i];
    for (unsigned i = 0; i < n_dyn; i++) if (dyn_servers[i].handle && dyn_servers[i].handle == handle) return &dyn_servers[i];
    return 0;
}
int iop_custom_cmd(Ctx *c, u32 cid, const u8 *pkt, u32 size) { (void)c; (void)cid; (void)pkt; (void)size; return 0; }
void iop_reset(void) { memset(fds, 0, sizeof fds); }
void iop_init(const char *iso_path) {
    if (!iso_open(iso_path)) { fprintf(stderr, "cannot open disc image: %s\n", iso_path); exit(1); }
}
