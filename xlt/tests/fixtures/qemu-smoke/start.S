// SPDX-License-Identifier: MIT
// P1 smoke payload: print a line on the QEMU virt PL011 UART, then power the
// machine off through PSCI SYSTEM_OFF. This is the unpatched-QEMU baseline for
// the M2 accelerator: the same image must behave identically under -accel tcg
// and, later, under -accel xlt.

    .section .text
    .global _start
    .type   _start, %function
_start:
    movz    x0, #0x0900, lsl #16        // PL011 base address 0x0900_0000
    adrp    x1, message
    add     x1, x1, :lo12:message
1:  ldrb    w2, [x1], #1                // next character, or NUL at the end
    cbz     w2, 3f

2:  ldr     w3, [x0, #0x18]              // UARTFR
    tbnz    w3, #5, 2b                  // wait while TXFF (bit 5) is set
    strb    w2, [x0]                    // UARTDR
    b       1b

3:  mov     x1, xzr                    // PSCI SYSTEM_OFF takes no arguments
    mov     x2, xzr
    mov     x3, xzr
    movz    x0, #0x0008                 // 0x8400_0008 = SYSTEM_OFF (32-bit)
    movk    x0, #0x8400, lsl #16
    hvc     #0                          // QEMU virt uses the HVC conduit for -kernel

4:  wfi                                 // not reached when SYSTEM_OFF is handled
    b       4b
    .size   _start, . - _start

    .section .rodata
    .balign 4
message:
    .asciz  "xlt-p1-ok\n"
