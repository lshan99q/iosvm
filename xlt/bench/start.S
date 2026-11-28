// SPDX-License-Identifier: MIT
// Bare-metal AArch64 entry for the QEMU TCG / XLT benchmark harness.
// QEMU loads this ELF via -kernel on -M virt (RAM at 0x40000000).
    .section .text.start
    .global _start
_start:
    mrs     x0, mpidr_el1
    and     x0, x0, #0xff
    cbnz    x0, 1f
    ldr     x0, =__stack_top
    mov     sp, x0
    // zero .bss
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
2:  cmp     x0, x1
    b.hs    3f
    str     xzr, [x0], #8
    b       2b
3:  bl      main
    mov     x0, #0x18          // SYS_EXIT
    mov     x1, #0             // ADP_Stopped_ApplicationExit
    hlt     #0xf000
1:  wfe
    b       1b
