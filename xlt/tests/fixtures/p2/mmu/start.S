// SPDX-License-Identifier: MIT
// P2.1 fixture entry point and exception vectors; MMU bring-up lives in mmu.c.
    .section .text.start, "ax"
    .global _start
_start:
    mrs     x0, mpidr_el1
    and     x0, x0, #0xff
    cbnz    x0, 1f
    ldr     x0, =__stack_top
    mov     sp, x0
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
2:
    cmp     x0, x1
    b.hs    3f
    str     xzr, [x0], #8
    b       2b
3:
    bl      main
    /* main should not return; stop the machine if it does. */
    ldr     x0, =0x84000008
    mov     x1, #0
    mov     x2, #0
    mov     x3, #0
    hvc     #0
1:
    wfe
    b       1b

/* Vector table: only the current-EL-with-SP_ELx synchronous slot is used. */
    .section .text.vectors, "ax"
    .balign 2048
    .global vector_table
vector_table:
    b       unhandled_exception
    .balign 0x80
    b       unhandled_exception
    .balign 0x80
    b       unhandled_exception
    .balign 0x80
    b       unhandled_exception
    .balign 0x80
    b       sync_current_elx
    .balign 0x80
    b       unhandled_exception
    .balign 0x80
    b       unhandled_exception
    .balign 0x80
    b       unhandled_exception
    .balign 0x80
    b       unhandled_exception
    .balign 0x80
    b       unhandled_exception
    .balign 0x80
    b       unhandled_exception
    .balign 0x80
    b       unhandled_exception
    .balign 0x80
    b       unhandled_exception
    .balign 0x80
    b       unhandled_exception
    .balign 0x80
    b       unhandled_exception
    .balign 0x80

sync_current_elx:
    sub     sp, sp, #256
    stp     x0, x1, [sp, #0]
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30, [sp, #240]
    mrs     x0, esr_el1
    mrs     x1, far_el1
    mrs     x2, elr_el1
    bl      fault_handler_c
    ldp     x0, x1, [sp, #0]
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldp     x8, x9, [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldr     x30, [sp, #240]
    add     sp, sp, #256
    eret

unhandled_exception:
    wfe
    b       unhandled_exception
