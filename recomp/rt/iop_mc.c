/* iop_mc.c — memory card service (MCSERV, RPC 0x80000400).
 *
 * One formatted 8 MB PS2 card sits in slot 1 (port 0); its contents are
 * ordinary files under a host directory (default "saves/mc0", env
 * ROR_MC_DIR). Slot 2 is empty. ROR_NO_MC=1 removes the card (the test
 * scripts in NOTES.md were recorded without one).
 *
 * Requests use the two layouts of Sony's libmc:
 *   mcDescParam (48 bytes): { fd, port, slot, size, offset, origin, buffer, param, data[16] }
 *   mcNameParam (0x414):    { port, slot, flags, maxent, table/curdir, name[1024] }
 * The RPC result word is the function's return value; reads write straight
 * into EE memory and clear the library's "unaligned head/tail" record. */
#include "iop.h"
#include "plat.h"

enum { MC_OK = 0, MC_CHANGED = -1, MC_NOFORMAT = -2, MC_FULL = -3, MC_NOENT = -4, MC_DENIED = -5,
       MC_NOTEMPTY = -6, MC_TOOMANY = -7 };
#define MC_FREE_CLUSTERS 8000
#define MAX_MCFD 16

static const char *mc_root(void) { const char *e = plat_getenv("ROR_MC_DIR"); return e ? e : "saves/mc0"; }
static char curdir[512] = "/";
static int announced;                      /* first GetInfo reports "card changed" */
static struct { FILE *f; } mfd[MAX_MCFD];
static struct { PlatDir *d; char dir[1024]; char pat[256]; int dots; } listing;

static void mkdirs(const char *path) {
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) if (PLAT_IS_SEP(*p)) { char sep = *p; *p = 0; plat_mkdir(tmp); *p = sep; }
    plat_mkdir(tmp);
}
/* Card path (absolute or relative to the current directory) -> host path.
 * ".." components are resolved inside the card; nothing escapes the root. */
static void host_path(const char *name, char *out, size_t n) {
    char full[1100], norm[1100] = "";
    if (name[0] == '/') snprintf(full, sizeof full, "%s", name);
    else snprintf(full, sizeof full, "%s/%s", curdir, name);
    for (char *tok = strtok(full, "/"); tok; tok = strtok(0, "/")) {
        if (!strcmp(tok, ".")) continue;
        if (!strcmp(tok, "..")) { char *s = strrchr(norm, '/'); if (s) *s = 0; continue; }
        strncat(norm, "/", sizeof norm - strlen(norm) - 1);
        strncat(norm, tok, sizeof norm - strlen(norm) - 1);
    }
    snprintf(out, n, "%s%s", mc_root(), norm);
}
static int glob_match(const char *p, const char *s) {
    if (!*p) return !*s;
    if (*p == '*') return glob_match(p + 1, s) || (*s && glob_match(p, s + 1));
    if (*s && (*p == '?' || *p == *s)) return glob_match(p + 1, s + 1);
    return 0;
}
static void put_time(u8 *p, s64 t) {            /* sceMcStDateTime */
    struct tm tm; plat_gmtime(t, &tm);
    p[0] = 0; p[1] = (u8)tm.tm_sec; p[2] = (u8)tm.tm_min; p[3] = (u8)tm.tm_hour; p[4] = (u8)tm.tm_mday; p[5] = (u8)(tm.tm_mon + 1);
    u16 y = (u16)(tm.tm_year + 1900); memcpy(p + 6, &y, 2);
}
static void put_entry(u32 ee, const char *name, const PlatStat *st) {   /* sceMcTblGetDir, 64 bytes */
    u8 e[64] = { 0 };
    put_time(e, st->ctime); put_time(e + 8, st->mtime);
    wr32(e + 16, st->is_dir ? 0 : (u32)st->size);
    u16 attr = st->is_dir ? 0x8427 : 0x8497; memcpy(e + 20, &attr, 2);
    strncpy((char *)e + 32, name, 31);
    memcpy(gptr(ee), e, 64);
}

void mc_call(RpcCall *rc) {
    const u8 *a = rc->send;
    s32 res = MC_OK;
    if (rc->fno == 0xFE) {                       /* init -> { result, mcserv version, mcman version } */
        mkdirs(mc_root());
        wr32(rc->recv, 0); wr32(rc->recv + 4, 0x0220); wr32(rc->recv + 8, 0x0220);
        return;
    }
    int name_form = (rc->fno == 0x02 || rc->fno == 0x0C || rc->fno == 0x0D || rc->fno == 0x0E || rc->fno == 0x0F);
    u32 port = rd32(a + (name_form ? 0 : 4));
    if (getenv("ROR_NO_MC")) port = 1;           /* test switch: behave as if no card is inserted */
    if (rc->fno == 0x01) {                       /* sceMcGetInfo: { type, free, format } -> *param */
        u32 param = rd32(a + 28);
        if (port != 0) { if (param) { p32(param, 0); p32(param + 4, 0); p32(param + 8, 0); } wr32(rc->recv, (u32)-10); return; }
        if (param) { p32(param, 2); p32(param + 4, MC_FREE_CLUSTERS); p32(param + 8, 1); }
        res = announced ? MC_OK : MC_CHANGED; announced = 1;
        wr32(rc->recv, (u32)res);
        return;
    }
    if (port != 0 && (name_form || rc->fno == 0x10 || rc->fno == 0x11 || rc->fno == 0x12)) { wr32(rc->recv, (u32)-10); return; }
    char hp[1200];
    switch (rc->fno) {
    case 0x02: {                                 /* open / mkdir: flags 1 read, 2 write, 0x200 create, 0x400 truncate, 0x40 mkdir */
        u32 flags = rd32(a + 8); const char *name = (const char *)a + 20;
        host_path(name, hp, sizeof hp);
        if (flags & 0x40) { res = (plat_mkdir(hp) == 0) ? MC_OK : MC_NOENT; break; }   /* -4: already exists / no parent */
        int fd = -1;
        for (int i = 0; i < MAX_MCFD; i++) if (!mfd[i].f) { fd = i; break; }
        if (fd < 0) { res = MC_TOOMANY; break; }
        PlatStat st; int exists = plat_stat(hp, &st) == 0;
        if (exists && st.is_dir) { res = MC_NOENT; break; }
        const char *mode = !exists ? ((flags & 0x200) ? "w+b" : 0) : (flags & 0x400) ? "w+b" : (flags & 2) ? "r+b" : "rb";
        FILE *f = mode ? plat_fopen(hp, mode) : 0;
        if (!f) { res = MC_NOENT; break; }
        mfd[fd].f = f; res = fd;
        rt_log("[mc] open %s flags=0x%x -> %d\n", name, flags, fd);
        break;
    }
    case 0x03: { u32 fd = rd32(a); if (fd < MAX_MCFD && mfd[fd].f) { fclose(mfd[fd].f); mfd[fd].f = 0; } else res = MC_NOENT; break; }
    case 0x04: {                                 /* seek(fd, offset, origin) -> position */
        u32 fd = rd32(a); if (fd >= MAX_MCFD || !mfd[fd].f) { res = MC_NOENT; break; }
        fseek(mfd[fd].f, (s32)rd32(a + 16), (int)rd32(a + 20) == 0 ? SEEK_SET : rd32(a + 20) == 1 ? SEEK_CUR : SEEK_END);
        res = (s32)ftell(mfd[fd].f); break;
    }
    case 0x05: {                                 /* read(fd, buffer, size) */
        u32 fd = rd32(a), size = rd32(a + 12), buf = rd32(a + 24), param = rd32(a + 28);
        if (fd >= MAX_MCFD || !mfd[fd].f) { res = MC_NOENT; break; }
        if ((buf & 0x1FFFFFFFu) + (u64)size > RAM_SIZE) rt_fatal(rc->ctx, "mcRead buffer outside RAM");
        res = (s32)fread(gptr(buf), 1, size, mfd[fd].f);
        if (param) { p32(param, 0); p32(param + 4, 0); }
        break;
    }
    case 0x06: {                                 /* write: `origin` head bytes inline in data[], then `size` bytes at buffer */
        u32 fd = rd32(a), size = rd32(a + 12), head = rd32(a + 20), buf = rd32(a + 24);
        if (fd >= MAX_MCFD || !mfd[fd].f) { res = MC_NOENT; break; }
        if (head > 16) head = 16;
        if ((buf & 0x1FFFFFFFu) + (u64)size > RAM_SIZE) rt_fatal(rc->ctx, "mcWrite buffer outside RAM");
        res = (s32)fwrite(a + 32, 1, head, mfd[fd].f);
        if (size) res += (s32)fwrite(gptr(buf), 1, size, mfd[fd].f);
        break;
    }
    case 0x0A: { u32 fd = rd32(a); if (fd < MAX_MCFD && mfd[fd].f) fflush(mfd[fd].f); else res = MC_NOENT; break; }
    case 0x0C: {                                 /* chdir(newdir) and report the current one to *curdir */
        const char *name = (const char *)a + 20; u32 out = rd32(a + 16);
        if (name[0]) {
            PlatStat st; host_path(name, hp, sizeof hp);
            if (plat_stat(hp, &st) != 0 || !st.is_dir) { res = MC_NOENT; break; }
            const char *rel = hp + strlen(mc_root());
            snprintf(curdir, sizeof curdir, "%s", *rel ? rel : "/");
        }
        if (out) strcpy(gptr(out), curdir);
        break;
    }
    case 0x0D: {                                 /* getdir(name pattern, mode, maxent, table) -> entries written */
        u32 mode = rd32(a + 8), maxent = rd32(a + 12), table = rd32(a + 16); const char *name = (const char *)a + 20;
        if (mode == 0) {
            char pat[1100]; snprintf(pat, sizeof pat, "%s", name);
            char *slash = strrchr(pat, '/');
            const char *leaf = slash ? slash + 1 : pat;
            snprintf(listing.pat, sizeof listing.pat, "%s", leaf);
            if (slash) { *(slash + (slash == pat ? 1 : 0)) = 0; } else pat[0] = 0;
            host_path(slash ? pat : "", listing.dir, sizeof listing.dir);
            if (listing.d) plat_closedir(listing.d);
            listing.d = plat_opendir(listing.dir);
            listing.dots = 0;
            if (!listing.d) { res = MC_NOENT; break; }
        }
        if (!listing.d) { res = 0; break; }
        int n = 0;
        const char *de;
        while ((u32)n < maxent && (de = plat_readdir(listing.d))) {
            if (!glob_match(listing.pat, de)) continue;
            char p2[1400]; PlatStat st;
            snprintf(p2, sizeof p2, "%s/%s", listing.dir, de);
            if (plat_stat(p2, &st) != 0) continue;
            put_entry(table + (u32)n * 64, de, &st);
            n++;
        }
        if ((u32)n < maxent) { plat_closedir(listing.d); listing.d = 0; }
        res = n;
        break;
    }
    case 0x0E: res = MC_OK; break;               /* set file info (attributes/times): nothing to store */
    case 0x0F: {                                 /* delete */
        host_path((const char *)a + 20, hp, sizeof hp);
        PlatStat st;
        if (plat_stat(hp, &st) != 0) res = MC_NOENT;
        else if (st.is_dir) res = plat_rmdir(hp) == 0 ? MC_OK : MC_NOTEMPTY;
        else res = plat_unlink(hp) == 0 ? MC_OK : MC_DENIED;
        break;
    }
    case 0x10: res = MC_OK; break;               /* format: the card is always formatted */
    case 0x12: res = MC_FREE_CLUSTERS; break;    /* get free entry space */
    case 0x14: res = MC_OK; break;               /* change thread priority */
    default:
        rt_log("[mc] fn=0x%x send=%u first words: %08x %08x %08x %08x\n", rc->fno, rc->send_size, rd32(a), rd32(a + 4), rd32(a + 8), rd32(a + 12));
        rt_fatal(rc->ctx, "memory card function 0x%x not implemented", rc->fno);
    }
    wr32(rc->recv, (u32)res);
}
