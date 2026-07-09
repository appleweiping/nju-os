/* Link user programs as a flat image based at 0x40000000 (AM native user base).
   start.o(.text) is placed first so _start is at exactly 0x40000000, which the
   kernel loader uses as the entry point (no ELF header is kept in the flat
   binary).  .bss follows .data and is zeroed by the loader. */
ENTRY(_start)
SECTIONS {
  . = 0x40000000;
  .text : {
    start.o(.text .text.*)
    *(.text .text.*)
  }
  .rodata : { *(.rodata .rodata.*) }
  .data   : { *(.data .data.*) }
  .bss    : { *(.bss .bss.* COMMON) }
}
