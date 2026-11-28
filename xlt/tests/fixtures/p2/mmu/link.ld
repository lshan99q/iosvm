/* SPDX-License-Identifier: MIT */
/* P2.1 fixture: load at 0x40080000 (QEMU virt RAM base + 512 KiB). */
ENTRY(_start)
SECTIONS
{
    . = 0x40080000;
    .text : {
        KEEP(*(.text.start))
        KEEP(*(.text.vectors))
        *(.text*)
    }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    . = ALIGN(16);
    .bss : {
        __bss_start = .;
        *(.bss*) *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }
    . = ALIGN(16);
    __stack_bottom = .;
    . += 0x100000;
    __stack_top = .;
}
