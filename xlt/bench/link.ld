/* SPDX-License-Identifier: MIT */
/* Bare-metal AArch64 image: QEMU -M virt RAM starts at 0x40000000, the ELF loads at 0x40080000. */
ENTRY(_start)
SECTIONS
{
    . = 0x40080000;
    .text : { *(.text.start) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    . = ALIGN(16);
    .bss : { __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(16); __bss_end = .; }
    . = ALIGN(16);
    __stack_bottom = .;
    . += 0x100000;
    __stack_top = .;
}
