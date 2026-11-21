/* Earlier versions used the CC0 rpi4-osdev GPIO and serial examples.
 * Register layout follows the BCM2711 peripheral specification. */

#include <vantage/platform/pi4/io.h>

enum {
    PERIPHERAL_BASE = MMIO_BASE,
    IO_GPFSEL0 = PERIPHERAL_BASE + 0x200000,
    IO_GPSET0 = PERIPHERAL_BASE + 0x20001c,
    IO_GPCLR0 = PERIPHERAL_BASE + 0x200028,
    GPPUPPDN0 = PERIPHERAL_BASE + 0x2000e4,
};

enum {
    GPIO_MAX_PIN = 57,
    GPIO_FUNC_ALT5 = 2,
};

enum {
    pull_none = 0,
};

#ifndef VANTAGE_SIM
// Host driver tests supply MMIO functions without accessing physical registers.
void mmio_write(long reg, unsigned int val) {
    *(volatile unsigned int *)reg = val;
}

unsigned int mmio_read(long reg) {
    return *(volatile unsigned int *)reg;
}
#endif

static void gpio_configure(unsigned int pin, unsigned int value, long base, unsigned int width) {
    unsigned int mask = (1U << width) - 1U;
    if (pin > GPIO_MAX_PIN || value > mask) return;
    unsigned int pins_per_register = 32U / width;
    unsigned int shift = (pin % pins_per_register) * width;
    long reg = base + (long)((pin / pins_per_register) * 4);
    mmio_write(reg, (mmio_read(reg) & ~(mask << shift)) | (value << shift));
}

static void gpio_write_level(unsigned int pin, unsigned int value, long base) {
    /* SET and CLR are write-only strobes. Zero leaves a pin unchanged. */
    if (pin <= GPIO_MAX_PIN && value == 1)
        mmio_write(base + (long)((pin / 32U) * 4), 1U << (pin % 32U));
}

unsigned int gpio_set(unsigned int pin_number, unsigned int value) {
    gpio_write_level(pin_number, value, (long)IO_GPSET0);
    return 0;
}

unsigned int gpio_clear(unsigned int pin_number, unsigned int value) {
    gpio_write_level(pin_number, value, (long)IO_GPCLR0);
    return 0;
}

unsigned int gpio_pull(unsigned int pin_number, unsigned int value) {
    gpio_configure(pin_number, value, (long)GPPUPPDN0, 2);
    return 0;
}

unsigned int gpio_function(unsigned int pin_number, unsigned int value) {
    gpio_configure(pin_number, value, (long)IO_GPFSEL0, 3);
    return 0;
}

void gpio_useAsAlt5(unsigned int pin_number) {
    gpio_pull(pin_number, pull_none);
    gpio_function(pin_number, GPIO_FUNC_ALT5);
}

// UART

enum {
    AUX_BASE = PERIPHERAL_BASE + 0x215000,
    AUX_ENABLES = AUX_BASE + 4,
    AUX_MU_IO_REG = AUX_BASE + 64,
    AUX_MU_IER_REG = AUX_BASE + 68,
    AUX_MU_IIR_REG = AUX_BASE + 72,
    AUX_MU_LCR_REG = AUX_BASE + 76,
    AUX_MU_MCR_REG = AUX_BASE + 80,
    AUX_MU_LSR_REG = AUX_BASE + 84,
    AUX_MU_CNTL_REG = AUX_BASE + 96,
    AUX_MU_BAUD_REG = AUX_BASE + 104,
    AUX_UART_CLOCK = 500000000,
    UART_MAX_QUEUE = 16 * 1024
};

#define AUX_MU_BAUD(baud) ((AUX_UART_CLOCK/(baud*8))-1)

void uart_init(void) {
    mmio_write(AUX_ENABLES, mmio_read(AUX_ENABLES) | 1U);
    mmio_write(AUX_MU_CNTL_REG, 0);
    mmio_write(AUX_MU_IER_REG, 0);
    /* Set eight data bits, no modem control, and clear both FIFOs. */
    mmio_write(AUX_MU_LCR_REG, 3);
    mmio_write(AUX_MU_MCR_REG, 0);
    mmio_write(AUX_MU_IIR_REG, 0xc6);
    mmio_write(AUX_MU_BAUD_REG, AUX_MU_BAUD(115200));
    gpio_useAsAlt5(14);
    gpio_useAsAlt5(15);
    mmio_write(AUX_MU_CNTL_REG, 3);
}

unsigned int uart_isWriteByteReady() {
    return mmio_read(AUX_MU_LSR_REG) & 0x20; 
}

void uart_writeByteBlockingActual(unsigned char ch) {
    while (!uart_isWriteByteReady()); 
    mmio_write(AUX_MU_IO_REG, (unsigned int)ch);
}

static void uart_console_char(unsigned char ch) {
    if (ch == '\n') uart_writeByteBlockingActual('\r');
    uart_writeByteBlockingActual(ch);
}

void uart_writeText(const char *buffer) {
    for (; *buffer; buffer++) uart_console_char((unsigned char)*buffer);
}

void uart_writeHex(unsigned int d) {
    uart_writeText("0x");

    for (int shift = 28; shift >= 0; shift -= 4) {
        unsigned int n = (d >> shift) & 0xF;
        uart_writeByteBlockingActual((unsigned char)(n < 10 ? '0' + n : 'A' + (n - 10)));
    }
}

char* uart_convertHexToString(unsigned int hex) {
    static char buf[2];
    buf[0] = (char)hex;
    buf[1] = '\0';

    return buf;
}
void uart_write_bytes(const char* buffer, size_t length) {
    for (size_t i = 0; i < length; i++) uart_console_char((unsigned char)buffer[i]);
}

uint32_t uart_read_byte(uint8_t* byte) {
    uint32_t status;
    if (!byte) return 0;
    status = mmio_read(AUX_MU_LSR_REG);
    if (status & 2) {
        if (status & 1) (void)mmio_read(AUX_MU_IO_REG);
        return 2;
    }
    if (!(status & 1)) return 0;
    *byte = (uint8_t)mmio_read(AUX_MU_IO_REG);
    return 1;
}

void uart_writeHex64(uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";

    uart_writeText("0x");

    for (int shift = 60; shift >= 0; shift -= 4) {
        uart_writeByteBlockingActual(
            digits[(value >> shift) & 0xF]
        );
    }
}
