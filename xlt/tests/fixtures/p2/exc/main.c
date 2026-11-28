// SPDX-License-Identifier: MIT
/* P2.2 fixture: EL0->EL1 SVC, lower-EL data abort, invalid-SPSR ERET, DAIF masking. */
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

extern char vector_table[];
extern char user_entry[];
extern char __stack_top[];
void fault_handler_c(uint64_t esr, uint64_t far, uint64_t elr, uint64_t from_lower);
static void el1_report(void);
static void el1_after_illegal(void);

static uint64_t l1[512] __attribute__((aligned(4096)));
static uint64_t l2_uart[512] __attribute__((aligned(4096)));
static uint64_t l2_ram[512] __attribute__((aligned(4096)));
static uint64_t l3_user[512] __attribute__((aligned(4096)));

/* Layout inside the 0x40200000-0x402fffff page run (4 KiB pages). */
#define EL0_CODE_PA 0x40200000ULL
#define EL0_STACK_PA 0x40201000ULL
#define EL0_DATA_PA 0x40202000ULL
#define EL0_PROBE_PA 0x40203000ULL

#define TABLE(pa) (((uint64_t)(pa)) | 0x3ULL)
#define EL1_BLOCK(pa) ((pa) | 0x701ULL)
#define DEVICE_BLOCK(pa) ((pa) | 0x405ULL)
/* AP=00: EL1 RW only, executable at EL1; AP=01: user RW, EL1 XN; AP=11: user RO, exec. */
#define EL1_PAGE(pa) ((pa) | 0x703ULL) /* valid level-3 page, EL1-only */
#define USER_RW_PAGE(pa) ((pa) | 0x743ULL)
#define USER_RO_PAGE(pa) ((pa) | 0x7c3ULL)

static volatile uint64_t g_svc_esr, g_svc_far, g_svc_elr, g_svc_spsr;
static volatile uint64_t g_dabt_esr, g_dabt_far, g_dabt_elr;
static volatile uint64_t g_svc2_seen;
static volatile uint64_t g_illegal_esr, g_illegal_far, g_illegal_elr, g_illegal_spsr, g_illegal_seen;
static volatile uint64_t g_bad_esr;

static void write_elr(uint64_t v)
{
    __asm__ volatile("msr elr_el1, %0" :: "r"(v));
}

static void write_spsr(uint64_t v)
{
    __asm__ volatile("msr spsr_el1, %0" :: "r"(v));
}

static uint64_t read_spsr(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, spsr_el1" : "=r"(v));
    return v;
}

/* Keep the two tables containing the EL0 pages reachable from the handler. */
static void install_probe_page(void)
{
    uint64_t far;
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));
    if ((far & ~0xfffULL) == EL0_PROBE_PA) {
        l3_user[(EL0_PROBE_PA >> 12) & 0x1ff] = USER_RW_PAGE(EL0_PROBE_PA);
    }
    *(volatile uint64_t *)(uintptr_t)EL0_PROBE_PA = 0xfeedfacecafebeefULL;
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("tlbi vmalle1" ::: "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
}

void fault_handler_c(uint64_t esr, uint64_t far, uint64_t elr, uint64_t from_lower)
{
    uint32_t ec = (uint32_t)(esr >> 26);

    if (ec == 0x15) { /* SVC from AArch64 */
        uint32_t imm = (uint32_t)(esr & 0xffffU);
        if (imm == 0x42U) {
            g_svc_esr = esr;
            g_svc_far = far;
            g_svc_elr = elr; /* ELR already points past the SVC */
            g_svc_spsr = read_spsr();
            write_elr(elr);
        } else if (imm == 1U) {
            g_svc2_seen = 1;
            write_elr((uint64_t)el1_report);
            write_spsr(0x3c5ULL); /* EL1h, DAIF masked */
        } else {
            g_bad_esr = esr;
            write_elr((uint64_t)el1_report);
            write_spsr(0x3c5ULL);
        }
        return;
    }

    if (ec == 0x24) { /* data abort from a lower EL */
        g_dabt_esr = esr;
        g_dabt_far = far;
        g_dabt_elr = elr;
        install_probe_page();
        return; /* ELR still points at the faulting load: retry it. */
    }

    if (ec == 0x0e) { /* illegal execution state from an invalid-SPSR ERET */
        g_illegal_esr = esr;
        g_illegal_far = far;
        g_illegal_elr = elr;
        g_illegal_spsr = read_spsr();
        g_illegal_seen = 1;
        write_elr((uint64_t)el1_after_illegal);
        write_spsr(0x3c5ULL);
        return;
    }

    g_bad_esr = esr;
    write_elr((uint64_t)el1_report);
    write_spsr(0x3c5ULL);
}

static void mmu_setup(void)
{
    l1[0] = TABLE(l2_uart);
    l1[1] = TABLE(l2_ram);

    /* PL011 stays EL1-only. */
    l2_uart[(0x09000000ULL >> 21) & 0x1ff] = DEVICE_BLOCK(0x09000000ULL);

    /* EL1 code/stack/tables: identity block, EL1-only so EL1 can execute it. */
    l2_ram[0] = EL1_BLOCK(0x40000000ULL);
    /* EL0 pages live in the next 2 MiB, mapped with 4 KiB pages. */
    l2_ram[1] = TABLE(l3_user);

    l3_user[(EL0_CODE_PA >> 12) & 0x1ff] = USER_RO_PAGE(EL0_CODE_PA);
    l3_user[(EL0_STACK_PA >> 12) & 0x1ff] = USER_RW_PAGE(EL0_STACK_PA);
    l3_user[(EL0_DATA_PA >> 12) & 0x1ff] = USER_RW_PAGE(EL0_DATA_PA);
    l3_user[(EL0_PROBE_PA >> 12) & 0x1ff] = EL1_PAGE(EL0_PROBE_PA);

    uint64_t tcr = 25ULL | (1ULL << 8) | (1ULL << 10) | (3ULL << 12) | (2ULL << 32);

    __asm__ volatile("msr mair_el1, %0" :: "r"(0xffULL));
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
    sctlr |= (1ULL << 0) | (1ULL << 2) | (1ULL << 12);
    __asm__ volatile("msr sctlr_el1, %0" :: "r"(sctlr));
    __asm__ volatile("isb" ::: "memory");
}

static void enter_el0(void)
{
    __asm__ volatile(
        "mov x2, %0\n"
        "mov x3, %1\n"
        "msr sp_el0, %2\n"
        "msr spsr_el1, %3\n"
        "msr elr_el1, %4\n"
        "eret\n"
        :: "r"(EL0_PROBE_PA), "r"(EL0_DATA_PA),
           "r"(EL0_STACK_PA + 0x1000ULL), "r"(0x3c0ULL), /* EL0t, DAIF masked */
           "r"((uint64_t)user_entry));
    __builtin_unreachable();
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

static void print_record(const char *name, uint64_t esr, uint64_t far, uint64_t elr)
{
    uart_puts(name);
    uart_puts(": esr=");
    uart_hex(esr);
    uart_puts(" far=");
    uart_hex(far);
    uart_puts(" elr=");
    uart_hex(elr);
    uart_puts("\n");
}

static uint64_t read_daif(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, daif" : "=r"(v));
    return v;
}

static void daif_test(void)
{
    uint64_t masked = read_daif();
    __asm__ volatile("msr daifclr, #0xf" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
    uint64_t unmasked = read_daif();
    __asm__ volatile("msr daifset, #0xf" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
    uint64_t remasked = read_daif();
    uart_puts("daif: masked=");
    uart_hex(masked);
    uart_puts(" unmasked=");
    uart_hex(unmasked);
    uart_puts(" remasked=");
    uart_hex(remasked);
    uart_puts("\n");
}

static void illegal_target(void)
{
    uart_puts("illegal: no-trap\n");
    el1_after_illegal();
}

static void illegal_test(void)
{
    write_spsr(0x3c9ULL); /* invalid SPSR: M=EL2h, DAIF masked */
    write_elr((uint64_t)illegal_target);
    __asm__ volatile("eret" ::: "memory");
}

static void el1_report(void)
{
    uart_puts("svc: esr=");
    uart_hex(g_svc_esr);
    uart_puts(" elr=");
    uart_hex(g_svc_elr);
    uart_puts(" spsr=");
    uart_hex(g_svc_spsr);
    uart_puts("\n");
    print_record("dabt", g_dabt_esr, g_dabt_far, g_dabt_elr);
    uart_puts("recovered=");
    uart_hex(*(volatile uint64_t *)(uintptr_t)EL0_DATA_PA);
    uart_puts("\n");
    daif_test();
    illegal_test();
    /* illegal_test either traps (handler jumps to el1_after_illegal) or falls
       through to illegal_target. */
}

static void el1_after_illegal(void)
{
    uart_puts("illegal: esr=");
    uart_hex(g_illegal_esr);
    uart_puts(" elr=");
    uart_hex(g_illegal_elr);
    uart_puts(" spsr=");
    uart_hex(g_illegal_spsr);
    uart_puts(" il=");
    uart_hex((g_illegal_spsr >> 20) & 1ULL);
    uart_puts(" seen=");
    uart_hex(g_illegal_seen);
    uart_puts("\n");

    if (g_svc_esr != 0 && g_dabt_esr != 0 && g_svc2_seen != 0 &&
        *(volatile uint64_t *)(uintptr_t)EL0_DATA_PA == 0xfeedfacecafebeefULL &&
        g_illegal_seen != 0) {
        uart_puts("xlt-p2.2-ok\n");
    } else {
        uart_puts("xlt-p2.2-fail\n");
    }

    psci_system_off();
}

int main(void)
{
    uart_init();
    uart_puts("xlt-p2.2-start\n");
    mmu_setup();
    enter_el0();
    return 0;
}
