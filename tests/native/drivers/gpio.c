#include <vantage/platform/pi4/io.h>
#include <stdio.h>

static unsigned int accesses;
static unsigned int fsel1;
static unsigned int fsel5;
static unsigned int set_mask, clear_mask, strobe_reads;

void mmio_write(long reg, unsigned int value) {
    accesses++;
    if ((unsigned long)reg == MMIO_BASE + 0x200004u) fsel1 = value;
    if ((unsigned long)reg == MMIO_BASE + 0x200014u) fsel5 = value;
    if ((unsigned long)reg == MMIO_BASE + 0x200020u) set_mask |= value;
    if ((unsigned long)reg == MMIO_BASE + 0x20002cu) clear_mask |= value;
}

unsigned int mmio_read(long reg) {
    accesses++;
    if ((unsigned long)reg == MMIO_BASE + 0x200020u || (unsigned long)reg == MMIO_BASE + 0x20002cu)
        strobe_reads++;
    if ((unsigned long)reg == MMIO_BASE + 0x200004u) return fsel1;
    return (unsigned long)reg == MMIO_BASE + 0x200014u ? fsel5 : 0;
}

int main(void) {
    unsigned int before;
    uart_init();
    if (((fsel1 >> 12) & 7) != 2 || ((fsel1 >> 15) & 7) != 2) {
        fputs("UART pins 14 and 15 were not configured as ALT5\n", stderr);
        return 1;
    }
    before = accesses;
    gpio_function(58, 2);
    gpio_pull(58, 0);
    gpio_set(58, 1);
    gpio_clear(58, 1);
    gpio_function(59, 2);
    gpio_function(14, 8);
    if (accesses != before) return 1;
    gpio_function(57, 2);
    if (accesses != before + 2 || ((fsel5 >> 21) & 7) != 2) return 1;
    gpio_pull(57, 0);
    gpio_set(57, 1);
    gpio_clear(57, 1);
    gpio_set(57, 0);
    if (accesses != before + 6 || strobe_reads ||
        set_mask != (1U << 25) || clear_mask != (1U << 25)) return 1;
    puts("PASS: UART pins 14/15, GPIO 57 accepted, GPIO 58 rejected by all helpers, invalid field value");
    return 0;
}
