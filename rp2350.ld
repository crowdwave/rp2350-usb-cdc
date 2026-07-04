/* rp2350.ld — RP2350 (Pico 2 W). XIP flash @ 0x10000000 (4 MB), SRAM @ 0x20000000 (512 KB main bank).
 * The bootrom enters via the Cortex-M vector table at the image start (SP@+0, reset@+4); a valid 20-byte
 * IMAGE_DEF block must appear within the first 4 KB (RP2350 datasheet 5.9.5). No RP2040-style boot2 needed. */
MEMORY
{
  FLASH (rx)  : ORIGIN = 0x10000000, LENGTH = 4096K
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 512K
}
_estack = ORIGIN(RAM) + LENGTH(RAM);          /* full descending stack from top of SRAM */

SECTIONS
{
  .flash_begin : {
    KEEP(*(.isr_vector))        /* vector table AT 0x10000000 — bootrom takes SP+reset from here */
    KEEP(*(.image_def))         /* RP2350 IMAGE_DEF metadata block (must be within first 4 KB) */
  } > FLASH

  .text : {
    *(.text*)
    *(.rodata*)
    . = ALIGN(4);
    _etext = .;                 /* LMA source for .data */
  } > FLASH

  _sidata = LOADADDR(.data);
  .data : {
    . = ALIGN(4); _sdata = .;
    *(.data*)
    . = ALIGN(4); _edata = .;
  } > RAM AT > FLASH

  .bss (NOLOAD) : {
    . = ALIGN(4); _sbss = .;
    *(.bss*)
    *(COMMON)
    . = ALIGN(4); _ebss = .;
  } > RAM
}
