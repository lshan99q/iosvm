// SPDX-License-Identifier: MIT
/* P2.1 fixture: stage-1 MMU bring-up, an alias mapping and a recovered data abort. */
#include <stdint.h>

#define UART_BASE 0x09000000ULL
#define UART_DR   0x00ULL
#define UART_FR   0x18ULL
#define UART_IBRD 0x24ULL
#define UART_FBRD 0x28ULL
#define UART_LCRH 0x2cULL
#define UART_CR   0x30ULL
#define UART_FR_TXFF (1u << 5)
#define UART_CR_UARTEN (1u << 0)
#define UART_CR_TXE (1u << 8)
#define UART_CR_RXE (1u << 9)

#define MMIO32(addr) (*(volatile uint32_t *)(uintptr_t)(addr))

static void uart_init(void)
{
    MMIO32(UART_BASE + UART_CR) = 0;
    MMIO32(UART_BASE + UART_IBRD) = 13;
    MMIO32(UART_BASE + UART_FBRD) = 1;
    MMIO32(UART_BASE + UART_LCRH) = 0x70;
    MMIO32(UART_BASE + UART_CR) = UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE;
}

static void uart_putc(char c)
{
    while (MMIO32(UART_BASE + UART_FR) & UART_FR_TXFF) {
    }
    MMIO32(UART_BASE + UART_DR) = (uint32_t)(unsigned char)c;
}

static void uart_puts(const char *s)
{
    while (*s != '\0') {
        uart_putc(*s++);
    }
}

static void uart_hex(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    uart_puts("0x");
    for (int i = 60; i >= 0; i -= 4) {
        uart_putc(digits[(value >> i) & 0xf]);
    }
}

/* Stage-1 tables: 4 KiB granule, 2-3 levels, EL1 only. */
static uint64_t l1[512] __attribute__((aligned(4096)));
static uint64_t l2_uart[512] __attribute__((aligned(4096)));
static uint64_t l2_ram[512] __attribute__((aligned(4096)));
static uint64_t l2_alias[512] __attribute__((aligned(4096)));
static uint64_t l3_alias[512] __attribute__((aligned(4096)));
static uint64_t l3_abort[512] __attribute__((aligned(4096)));

#define RAM_PA 0x40000000ULL
#define DATA_PA 0x40200000ULL
#define ALIAS_VA 0x80080000ULL
#define ABORT_VA 0x70000000ULL

extern char vector_table[];
extern char _start[];
void fault_handler_c(uint64_t esr, uint64_t far, uint64_t elr);

static volatile uint64_t g_esr;
static volatile uint64_t g_far;
static volatile uint64_t g_elr;
static volatile uint64_t g_faults;

/* Normal write-back, inner shareable, AF set; block/page descriptors. */
#define NORMAL_BLOCK(pa) ((pa) | 0x701ULL)
#define NORMAL_PAGE(pa)  ((pa) | 0x703ULL)
#define DEVICE_BLOCK(pa) ((pa) | 0x405ULL)
#define TABLE(pa)        (((uint64_t)(pa)) | 0x3ULL)

void fault_handler_c(uint64_t esr, uint64_t far, uint64_t elr)
{
    g_esr = esr;
    g_far = far;
    g_elr = elr;
    g_faults++;

    /* Install the missing 4 KiB page and retry the faulting load. */
    l2_ram[(ABORT_VA >> 21) & 0x1ff] = TABLE(l3_abort);
    l3_abort[(ABORT_VA >> 12) & 0x1ff] = NORMAL_PAGE(DATA_PA);
    *(volatile uint64_t *)(uintptr_t)DATA_PA = 0x600df00dULL;

    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("tlbi vmalle1" ::: "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
}

static void mmu_setup(void)
{
    l1[0] = TABLE(l2_uart);
    l1[1] = TABLE(l2_ram);
    l1[2] = TABLE(l2_alias);

    /* Device nGnRnE for the PL011 at 0x09000000 (2 MiB block). */
    l2_uart[(0x09000000ULL >> 21) & 0x1ff] = DEVICE_BLOCK(0x09000000ULL);

    /* Identity-map the first 4 MiB of RAM (code, stack, tables, data page). */
    l2_ram[0] = NORMAL_BLOCK(RAM_PA);
    l2_ram[1] = NORMAL_BLOCK(0x40200000ULL);

    /* Alias 0x80080000 -> 0x40200000 through an L3 page. */
    l2_alias[0] = TABLE(l3_alias);
    l3_alias[(ALIAS_VA >> 12) & 0x1ff] = NORMAL_PAGE(DATA_PA);

    /* T0SZ=25 (39-bit VA), TG0=4 KiB, WB/WA walks, inner shareable, IPS=40-bit. */
    uint64_t tcr = 25ULL | (1ULL << 8) | (1ULL << 10) | (3ULL << 12) | (2ULL << 32);

    __asm__ volatile("msr mair_el1, %0" :: "r"(0xffULL)); /* attr0 WB, attr1 nGnRnE */
    __asm__ volatile("msr tcr_el1, %0" :: "r"(tcr));
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"((uint64_t)l1));
    __asm__ volatile("msr vbar_el1, %0" :: "r"((uint64_t)vector_table));
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
    __asm__ volatile("tlbi vmalle1" ::: "memory");
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("isb" ::: "memory");

    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1ULL << 0) | (1ULL << 2) | (1ULL << 12); /* M, C, I */
    __asm__ volatile("msr sctlr_el1, %0" :: "r"(sctlr));
    __asm__ volatile("isb" ::: "memory");
}

static void psci_system_off(void)
{
    register uint64_t x0 __asm__("x0") = 0x84000008ULL;
    register uint64_t x1 __asm__("x1") = 0;
    register uint64_t x2 __asm__("x2") = 0;
    register uint64_t x3 __asm__("x3") = 0;
    __asm__ volatile("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    for (;;) {
        __asm__ volatile("wfe");
    }
}

int main(void)
{
    uart_init();
    uart_puts("xlt-p2.1-start\n");

    mmu_setup();

    *(volatile uint64_t *)(uintptr_t)DATA_PA = 0x1122334455667788ULL;
    uint64_t alias = *(volatile uint64_t *)(uintptr_t)ALIAS_VA;
    uart_puts("alias: pa=");
    uart_hex(DATA_PA);
    uart_puts(" va=");
    uart_hex(ALIAS_VA);
    uart_puts(" val=");
    uart_hex(alias);
    uart_puts("\n");

    uint64_t recovered = *(volatile uint64_t *)(uintptr_t)ABORT_VA;

    uart_puts("abort: esr=");
    uart_hex(g_esr);
    uart_puts(" far=");
    uart_hex(g_far);
    uart_puts(" elr=");
    uart_hex(g_elr);
    uart_puts(" elr_off=");
    uart_hex(g_elr - (uint64_t)_start);
    uart_puts(" recovered=");
    uart_hex(recovered);
    uart_puts(" faults=");
    uart_hex(g_faults);
    uart_puts("\n");

    if (alias == 0x1122334455667788ULL && recovered == 0x600df00dULL && g_faults == 1) {
        uart_puts("xlt-p2.1-ok\n");
    } else {
        uart_puts("xlt-p2.1-fail\n");
    }

    psci_system_off();
    return 0;
}
