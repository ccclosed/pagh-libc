ENTRY(_start)
SECTIONS
{
  . = 0x10000;
  .text : { *(.text.entry) *(.text*) }
  .rodata : ALIGN(16) { *(.rodata*) *(.srodata*) }
  .data : ALIGN(16) { *(.data*) *(.sdata*) }
  .bss : ALIGN(16) { __bss_start = .; *(.bss*) *(.sbss*) *(COMMON) __bss_end = .; }
  /DISCARD/ : { *(.eh_frame*) *(.comment*) *(.note*) }
}
PROVIDE(__global_pointer$ = . + 0x800);
