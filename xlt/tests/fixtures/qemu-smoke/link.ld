/* SPDX-License-Identifier: MIT */
/* Link the QEMU virt P1 smoke payload at the start of guest RAM. */
ENTRY(_start)

SECTIONS
{
    . = 0x40080000;

    .text : {
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    .data : {
        *(.data .data.*)
    }

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
        *(.ARM.exidx .ARM.extab)
    }
}
