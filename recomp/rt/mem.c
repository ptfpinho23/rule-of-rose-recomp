/* mem.c — guest address space.
 *
 * 4 GiB reserved at MEMBASE. The 32 MiB of main RAM is one shared memory
 * object mapped at every address the EE can see it through (kuseg, the two
 * uncached mirrors, kseg0, kseg1). The 16 KiB scratchpad lives at
 * 0x70000000. Everything else stays PROT_NONE so a stray access faults and
 * is reported with the guest address. */
#include "rt.h"
#include "plat.h"

/* The two ELF32 structures needed (little-endian, as in <elf.h>). */
typedef struct { u8 e_ident[16]; u16 e_type, e_machine; u32 e_version, e_entry, e_phoff, e_shoff, e_flags;
                 u16 e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; } Elf32_Ehdr;
typedef struct { u32 p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align; } Elf32_Phdr;
#define ELFMAG "\177ELF"
#define PT_LOAD 1

void mem_init(void) {
    /* Main RAM at every address the EE can see it through; the boot ROM
     * window (4 MiB at physical 0x1FC00000) and its mirrors; the scratchpad. */
    static const PlatMap maps[] = {
        { 0x00000000u, RAM_SIZE, 0 }, { 0x20000000u, RAM_SIZE, 0 }, { 0x30000000u, RAM_SIZE, 0 },
        { 0x80000000u, RAM_SIZE, 0 }, { 0xA0000000u, RAM_SIZE, 0 },
        { 0x1FC00000u, 0x400000, 1 }, { 0x9FC00000u, 0x400000, 1 }, { 0xBFC00000u, 0x400000, 1 },
        { 0x70000000u, 0x4000, -1 },
    };
    plat_guest_map((void *)MEMBASE, 0x100000000ull, maps, (int)(sizeof maps / sizeof *maps));
    /* No BIOS image is used; the ROM page only carries the two fields games
     * read to identify the console: the ROM date word at +0x100 and the
     * maker string at +0x108. */
    u32 rom_date = 0x20040614u;
    memcpy(HPTR(0x1FC00100u), &rom_date, 4);
    memcpy(HPTR(0x1FC00108u), "Sony Computer Entertainment Inc.", 33);
}

/* Load an ELF image held in memory (the game executable read off the disc). */
void mem_load_elf_buf(const u8 *b, size_t n, const char *what, u32 *entry) {
    Elf32_Ehdr eh;
    if (n < sizeof eh || memcmp(b, ELFMAG, 4) != 0) { fprintf(stderr, "%s: not an ELF\n", what); exit(1); }
    memcpy(&eh, b, sizeof eh);
    for (int i = 0; i < eh.e_phnum; i++) {
        Elf32_Phdr ph;
        size_t o = eh.e_phoff + (size_t)i * eh.e_phentsize;
        if (o + sizeof ph > n) { fprintf(stderr, "%s: truncated\n", what); exit(1); }
        memcpy(&ph, b + o, sizeof ph);
        if (ph.p_type != PT_LOAD) continue;
        if ((u64)ph.p_vaddr + ph.p_memsz > RAM_SIZE || ph.p_filesz > ph.p_memsz) { fprintf(stderr, "%s: segment outside RAM\n", what); exit(1); }
        if ((u64)ph.p_offset + ph.p_filesz > n) { fprintf(stderr, "%s: truncated\n", what); exit(1); }
        memcpy(HPTR(ph.p_vaddr), b + ph.p_offset, ph.p_filesz);
        memset(HPTR(ph.p_vaddr + ph.p_filesz), 0, ph.p_memsz - ph.p_filesz);
    }
    *entry = eh.e_entry;
}
void mem_load_elf(const char *path, u32 *entry) {
    FILE *f = plat_fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); s64 n = ftell(f); fseek(f, 0, SEEK_SET);
    u8 *b = malloc((size_t)n);
    if (n <= 0 || fread(b, 1, (size_t)n, f) != (size_t)n) { perror(path); exit(1); }
    fclose(f);
    mem_load_elf_buf(b, (size_t)n, path, entry);
    free(b);
}
