// SPDX-License-Identifier: MIT
/* Minimal PL011 console and PSCI helpers for the P1 boot fixtures. */
#ifndef XLT_P1_COMMON_H
#define XLT_P1_COMMON_H

#include <stdint.h>

#define PL011_BASE 0x09000000ULL
#define PL011_DR 0x00
#define PL011_FR 0x18
#define PL011_IBRD 0x24
#define PL011_FBRD 0x28
#define PL011_LCRH 0x2c
#define PL011_CR 0x30
#define PL011_FR_TXFF (1u << 5)
#define PL011_CR_UARTEN (1u << 0)
#define PL011_CR_TXE (1u << 8)
#define PL011_CR_RXE (1u << 9)

static inline void mmio_write32(uint64_t addr, uint32_t value)
{
    *(volatile uint32_t *)(uintptr_t)addr = value;
}

static inline uint32_t mmio_read32(uint64_t addr)
{
    return *(volatile uint32_t *)(uintptr_t)addr;
}

static inline void uart_init(void)
{
    mmio_write32(PL011_BASE + PL011_CR, 0);
    mmio_write32(PL011_BASE + PL011_IBRD, 13);
    mmio_write32(PL011_BASE + PL011_FBRD, 1);
    mmio_write32(PL011_BASE + PL011_LCRH, 0x70);
    mmio_write32(PL011_BASE + PL011_CR, PL011_CR_UARTEN | PL011_CR_TXE | PL011_CR_RXE);
}

static inline uint32_t uart_fr(void)
{
    return mmio_read32(PL011_BASE + PL011_FR);
}

static inline void uart_putc(char c)
{
    while (uart_fr() & PL011_FR_TXFF) {
    }
    mmio_write32(PL011_BASE + PL011_DR, (uint32_t)(unsigned char)c);
}

static inline void uart_puts(const char *s)
{
    while (*s != '\0') {
        uart_putc(*s++);
    }
}

static inline void uart_hex(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    uart_puts("0x");
    for (int i = 60; i >= 0; i -= 4) {
        uart_putc(digits[(value >> i) & 0xf]);
    }
}

static inline void psci_system_off(void)
{
    register uint64_t x0 asm("x0") = 0x84000008ULL;
    register uint64_t x1 asm("x1") = 0;
    register uint64_t x2 asm("x2") = 0;
    register uint64_t x3 asm("x3") = 0;
    asm volatile("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    for (;;) {
        asm volatile("wfe");
    }
}

#endif /* XLT_P1_COMMON_H */
