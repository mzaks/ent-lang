/* Where the processor's program lies: from 0x10000 on, the code that
   starts it first (PicoRV32's dhrystone/start.S), then everything else;
   `end` is where the program's own memory begins. */
ENTRY(start)
SECTIONS {
  . = 0x10000;
  .text : { *start.o(.text*) *(.text*) }
  .rodata : { *(.rodata*) *(.srodata*) }
  .data : { *(.data*) *(.sdata*) }
  .bss : { *(.bss*) *(.sbss*) *(COMMON) }
  end = .;
  /DISCARD/ : { *(.riscv.attributes) *(.comment) *(.note*) *(.eh_frame*) }
}
