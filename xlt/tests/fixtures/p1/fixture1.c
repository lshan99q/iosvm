// SPDX-License-Identifier: MIT
/* P1 fixture 1: print one line over PL011 and shut down through PSCI. */
#include "common.h"

int main(void)
{
    uart_init();
    uart_puts("xlt-p1-fixture1\n");
    psci_system_off();
    return 0;
}
