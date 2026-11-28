// SPDX-License-Identifier: MIT
// P2.1 semihosting smoke: SYS_WRITE0 then SYS_EXIT through HLT #0xf000.
    .section .text.start, "ax"
    .global _start
_start:
    adr x1, message
    mov x0, #4                  // SYS_WRITE0
    hlt #0xf000
    mov x0, #0x18               // SYS_EXIT
    mov x1, #0
    hlt #0xf000
1:  b 1b

    .section .rodata
message:
    .asciz "xlt-p2-semihost-ok\n"
