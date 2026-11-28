/* SPDX-License-Identifier: MIT */
/* P1 fixtures load at 0x40080000, matching the QEMU virt RAM base plus 512 KiB. */
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
