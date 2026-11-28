/* SPDX-License-Identifier: MIT */
/* P2.1 semihosting fixture: load at 0x40080000. */
ENTRY(_start)
SECTIONS
{
    . = 0x40080000;
    .text : { *(.text.start) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) }
}
