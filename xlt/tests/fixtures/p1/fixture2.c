// SPDX-License-Identifier: MIT
/* P1 fixture 2: exercise PL011 FR read-back, RAM store/load and a system-register read. */
#include "common.h"

int main(void)
{
    uart_init();

    uint32_t fr = uart_fr();

    volatile uint64_t *cell = (volatile uint64_t *)(uintptr_t)0x40090000ULL;
    *cell = 0x1122334455667788ULL;
    uint64_t value = *cell;

    uint64_t current_el;
    asm volatile("mrs %0, CurrentEL" : "=r"(current_el));

    uart_puts("xlt-p1-fixture2 fr=");
    uart_hex(fr);
    uart_puts(" ram=");
    uart_hex(value);
    uart_puts(" el=");
    uart_hex(current_el);
    uart_puts("\n");

    psci_system_off();
    return 0;
}
