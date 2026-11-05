#include "../../includes/io.h"
#include <stdio.h>

static unsigned int accesses;
static unsigned int fsel1;

void mmio_write(long reg, unsigned int value) {
    accesses++;
    if ((unsigned long)reg == MMIO_BASE + 0x200004u) fsel1 = value;
}

unsigned int mmio_read(long reg) {
    accesses++;
    return (unsigned long)reg == MMIO_BASE + 0x200004u ? fsel1 : 0;
}

int main(void) {
    unsigned int before;
    uart_init();
    if (((fsel1 >> 12) & 7) != 2 || ((fsel1 >> 15) & 7) != 2) {
        fputs("UART pins 14 and 15 were not configured as ALT5\n", stderr);
        return 1;
    }
    before = accesses;
    gpio_function(59, 2);
    gpio_function(14, 8);
    if (accesses != before) return 1;
    gpio_function(58, 2);
    if (accesses != before + 2) return 1;
    puts("PASS: UART pins, highest allowed pin, invalid pin and invalid field value");
    return 0;
}
