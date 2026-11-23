/* iso.c — read-only access to the game disc image (ISO9660, 2048-byte
 * sectors). */
#include <ctype.h>
#include "iop.h"
#include "plat.h"

static FILE *iso;
static u32 root_lba, root_size;

int iso_open(const char *path) {
    if (iso) { fclose(iso); iso = 0; }
    iso = plat_fopen(path, "rb");
    if (!iso) return 0;
    u8 pvd[2048];
    if (!iso_read(16, 1, pvd) || memcmp(pvd + 1, "CD001", 5) != 0) { fclose(iso); iso = 0; return 0; }
    root_lba = rd32(pvd + 156 + 2);
    root_size = rd32(pvd + 156 + 10);
    return 1;
}

int iso_read(u32 lsn, u32 count, void *dst) {
    if (!iso) return 0;
    if (plat_fseek64(iso, (u64)lsn * 2048) != 0) return 0;
    size_t got = fread(dst, 2048, count, iso);
    if (got < count) memset((u8 *)dst + got * 2048, 0, (count - got) * 2048);   /* past end: zeros */
    return 1;
}

/* Compare a path component with a directory entry name, ignoring case and
 * the ";1" version suffix on either side. */
static int name_eq(const char *want, size_t wn, const u8 *have, size_t hn) {
    while (wn && (want[wn - 1] == '1' || want[wn - 1] == ';') && memchr(want, ';', wn)) { if (want[wn - 1] == ';') { wn--; break; } wn--; }
    const u8 *semi = memchr(have, ';', hn);
    if (semi) hn = (size_t)(semi - have);
    if (hn && have[hn - 1] == '.') hn--;
    if (wn && want[wn - 1] == '.') wn--;
    if (wn != hn) return 0;
    for (size_t i = 0; i < wn; i++) if (toupper((u8)want[i]) != toupper(have[i])) return 0;
    return 1;
}

/* Look up a file by path ("\DIR\FILE.EXT;1", '/' also accepted). */
int iso_find(const char *path, u32 *lsn, u32 *size) {
    if (!iso) return 0;
    u32 dir_lba = root_lba, dir_size = root_size;
    const char *p = path;
    for (;;) {
        while (*p == '\\' || *p == '/') p++;
        if (!*p) return 0;
        const char *e = p;
        while (*e && *e != '\\' && *e != '/') e++;
        int last = (*e == 0);
        u8 *dir = malloc(dir_size + 2048);
        iso_read(dir_lba, (dir_size + 2047) / 2048, dir);
        int found = 0;
        for (u32 off = 0; off < dir_size;) {
            u8 len = dir[off];
            if (len == 0) { off = (off + 2048) & ~2047u; continue; }   /* next sector */
            const u8 *r = dir + off;
            int is_dir = (r[25] & 2) != 0;
            if (r[32] && !(r[32] == 1 && r[33] <= 1) && name_eq(p, (size_t)(e - p), r + 33, r[32]) && is_dir == !last) {
                if (last) { *lsn = rd32(r + 2); *size = rd32(r + 10); free(dir); return 1; }
                dir_lba = rd32(r + 2); dir_size = rd32(r + 10);
                found = 1;
                break;
            }
            off += len;
        }
        free(dir);
        if (!found) return 0;
        p = e;
    }
}
