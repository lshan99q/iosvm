// SPDX-License-Identifier: MIT
// P1 fixture entry point: configure SP, zero .bss, call main and stop through PSCI.
    .section .text.start
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
    ldr     x0, =0x84000008
    mov     x1, #0
    mov     x2, #0
    mov     x3, #0
    hvc     #0
1:
    wfe
    b       1b
