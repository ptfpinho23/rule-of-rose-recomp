/* Linker script for the Rule of Rose native recompile.
 *
 * The recompiled MIPS->x86-64 code (ror_native.o's .text) must sit at 0x100000
 * (its original MIPS virtual addresses) and occupies ~6.3 MB. Everything else
 * (libc, libgcc, the host code, data, bss) goes after it starting at 0x800000
 * so nothing overlaps.
 *
 * We link fully statically with -nostdlib, providing _start ourselves, so we
 * also need to define the standard CRT array sections that libc's init/fini
 * helpers reference (preinit/init/fini arrays, ehdr).
 */
ENTRY(_start)
/* __ehdr_start: absolute address of the real ELF header (start of the executable
 * at vaddr 0x70000000). Referenced by libc's dl-support.o and dl-reloc-static-pie.o
 * (and by glibc's __libc_setup_tls). Defined at top level so it is ABSOLUTE. */
__ehdr_start = 0x70000000;
SECTIONS
{
  /* ---- the recompiled code, at a HIGH base ---------------------------- */
  /* The recompiled MIPS->x86-64 code is placed at 0x70000000 (well above the
   * game's original address space 0x100000-0x5BAE94 and above glibc's
   * static-TLS region). This keeps the game data at its original low
   * addresses (so the recompiled code's lui+addiu-formed pointers, rebased by
   * REBASE in translate.py, land correctly) and avoids any overlap between
   * the 6.3 MB recompiled .text and the game data / TLS. */
  . = 0x70000000;
  .text : {
    KEEP (*(.text._ror_entry .text.ror_native*))
    *(.text)
  }

  /* ---- the linker's interposition PLT stubs (.iplt) ------------------- */
  /* The linker emits .iplt (jmp *got) stubs for interposed libc symbols and
   * places them right after .text by default. The recompiled game code must
   * NEVER fall through into .iplt, so we push it to a high address well clear
   * of .text, with a guard gap. .text (recompiled code) ends ~0x7045xxxx; we
   * start .iplt at 0x70800000 leaving a >5 MB gap. */
  . = 0x70800000;
  . = ALIGN(4096);
  .iplt : {
    LONG(0) LONG(0) LONG(0)   /* guard: 3 x int3 if ever executed */
    *(.iplt)
  }
  . = ALIGN(4096);
  .plt : { *(.plt) }

  /* ---- everything else, after the recompiled text --------------------- */
  . = 0x71000000;
  . = ALIGN(4096);
  .preinit_array : {
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(SORT_BY_INIT_PRIORITY(.preinit_array.*)))
    KEEP (*(.preinit_array))
    PROVIDE_HIDDEN (__preinit_array_end = .);
  }
  . = ALIGN(4096);
  .init_array : {
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*)))
    KEEP (*(.init_array))
    PROVIDE_HIDDEN (__init_array_end = .);
  }
  . = ALIGN(4096);
  .fini_array : {
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
    KEEP (*(.fini_array))
    PROVIDE_HIDDEN (__fini_array_end = .);
  }
  . = ALIGN(4096);
  .ehdr_frame : {
    /* The LONG(0) is a guard byte if ever executed. __ehdr_start is defined as a
     * top-level ABSOLUTE symbol (below) pointing at the real ELF header, so
     * glibc's __libc_setup_tls can walk the program headers to find PT_TLS. */
    LONG(0)
    __ehdr_end = .;
  }
  . = ALIGN(4096);
  . = ALIGN(4096);
  .got : { *(.got .igot .igot.plt) }
  .got.plt : { *(.got.plt) }
  . = ALIGN(4096);
  .plt : { *(.plt) }
  /* host + libc + libgcc code */
  .text2 : { *(.text .text.*) }
  . = ALIGN(4096);
  .rodata : { *(.rodata .rodata.*) *(.gnu.warning .GCC.command_line) }
  . = ALIGN(4096);
  .data : { *(.data .data.*) *(.tdata) }
  . = ALIGN(4096);
  .bss : {
    *(.tdata)
    *(COMMON)
    *(.bss .bss.*)
  }
  . = ALIGN(4096);
  .eh_frame : { *(.eh_frame) }
  . = ALIGN(4096);
  _end = .;
  . = ALIGN(4096);
  /DISCARD/ : {
    *(.comment) *(.note*) *(.interp) *(.hash) *(.gnu.hash)
    *(.dynsym) *(.dynstr) *(.rela.dyn) *(.rel.dyn)
    *(.eh_frame_hdr)
  }
}
