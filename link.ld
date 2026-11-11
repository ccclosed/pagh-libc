/* The userland link contract for pagh-libc programs.
 *
 * This mirrors `pagh-riscv/userland/user.ld` (the kernel's own user land
 * fixtures), so a program built here is laid out exactly like the kernel's
 * test images:
 *
 *   - the image starts at 0x1_0000, inside the kernel's user ELF window
 *     (0x10000 .. 0x3fff_0000), away from the kernel identity map at
 *     0x8000_0000 and the MMIO windows;
 *   - `.text.entry` is first, so `_start` is the first byte of the first
 *     PT_LOAD and `ENTRY(_start)` matches the loader's entry contract;
 *   - .text + .rodata are R+X, .data + .bss are R+W. Each group starts on its
 *     own page, so the linker emits two PT_LOAD headers (an RWX single segment
 *     was the old layout and made every link warn);
 *   - no TLS, no `.init_array`, no dynamic sections: one static image.
 *
 * The kernel loader only maps PT_LOAD segments, so nothing outside this
 * script may carry allocated data.
 */
ENTRY(_start)
SECTIONS
{
  . = 0x10000;

  .text : ALIGN(4096) {
    KEEP(*(.text.entry))
    *(.text .text.*)
  }

  . = ALIGN(4096);
  .rodata : ALIGN(16) {
    *(.rodata .rodata.*)
    *(.srodata .srodata.*)
  }

  . = ALIGN(4096);
  .data : ALIGN(16) {
    *(.data .data.*)
    *(.sdata .sdata.*)
  }

  . = ALIGN(4096);
  .bss : ALIGN(16) {
    __bss_start = .;
    *(.bss .bss.*)
    *(.sbss .sbss.*)
    *(COMMON)
    __bss_end = .;
  }

  /DISCARD/ : { *(.eh_frame*) *(.comment*) *(.note*) }
}
PROVIDE(__global_pointer$ = . + 0x800);