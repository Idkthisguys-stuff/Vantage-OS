#include <vantage/drivers/bluetooth.h>
#include <vantage/platform/pi4/drivers.h>
#include <vantage/platform/pi4/io.h>

/* Pi 4B routes the radio's H4 link over PL011 UART0, independently of the
 * mini-UART console. The radio's ROM default is 115200 baud with flow control. */
#define UART0 0xfe201000ULL
#define UART_DATA 0x00U
#define UART_ERRORS 0x04U
#define UART_FLAGS 0x18U
#define UART_INT_DIVISOR 0x24U
#define UART_FRAC_DIVISOR 0x28U
#define UART_LINE 0x2cU
#define UART_CONTROL 0x30U
#define UART_IRQ_MASK 0x38U
#define UART_IRQ_CLEAR 0x44U
#define UART_DMA 0x48U
#define UART_ID0 0xfe0U
#define UART_ID1 0xfe4U
#define UART_BUSY (1U << 3)
#define UART_RX_EMPTY (1U << 4)
#define UART_TX_FULL (1U << 5)
#define UART_FIFO_8N1 ((3U << 5) | (1U << 4))
#define UART_CTS_ENABLE (1U << 15)
#define UART_H4_CONTROL (UART_CTS_ENABLE | (1U << 14) | (1U << 9) | (1U << 8) | 1U)
#define UART_BAUD 115200U
#define UART_CLOCK_ID 2U
#define TAG_CLOCK_RATE 0x00030002U
#define TAG_SET_GPIO 0x00038041U
#define BT_ENABLE_GPIO 128U

static BtHci controller;
static int32_t startup_error;

#if VANTAGE_RADIO_DIAGNOSTICS
static uint32_t diagnostic_rx_bytes;
#endif

static void uart_write(const HwOps* io, unsigned int reg, uint32_t value) {
    io->write(io->context, UART0 + reg, value);
}

static uint32_t uart_read(const HwOps* io, unsigned int reg) {
    return io->read(io->context, UART0 + reg);
}

static int send_packet(void* context, const uint8_t* data, size_t length) {
    const HwOps* io = context;
    uint32_t start = io->time_us(io->context);
    for (size_t i = 0; i < length; i++) {
        uint32_t elapsed = io->time_us(io->context) - start;
        if (elapsed >= 20000U ||
            hw_wait(io, UART0 + UART_FLAGS, UART_TX_FULL, 0, 20000U - elapsed) != HW_OK)
            return BT_TIMEOUT;
        uart_write(io, UART_DATA, data[i]);
    }
#if VANTAGE_RADIO_DIAGNOSTICS
    uint32_t opcode = (uint32_t)data[1] | ((uint32_t)data[2] << 8);
    int result = hw_wait(io, UART0 + UART_FLAGS, UART_BUSY, 0, 20000U);
    uart_writeText(result == HW_OK ? "[RADIO] BT UART sent opcode " : "[RADIO] BT UART TX stalled on opcode ");
    uart_writeHex(opcode);
    uart_writeText(" flags=");
    uart_writeHex(uart_read(io, UART_FLAGS));
    uart_writeText("\n");
    if (result != HW_OK) return BT_TIMEOUT;
#endif
    return BT_OK;
}

static int receive_byte(void* context, uint8_t* result) {
    const HwOps* io = context;
    if (uart_read(io, UART_FLAGS) & UART_RX_EMPTY) return 0;
    uint32_t data = uart_read(io, UART_DATA);
    if (data & 0xf00U) {
        uart_write(io, UART_ERRORS, 0xff);
        return BT_IO;
    }
#if VANTAGE_RADIO_DIAGNOSTICS
    diagnostic_rx_bytes++;
#endif
    *result = (uint8_t)data;
    return 1;
}

static uint32_t time_us_cb(void* context) {
    const HwOps* io = context;
    return io->time_us(io->context);
}

static int set_radio_power(const HwOps* io, uint32_t enabled) {
    uint32_t gpio[2] = { BT_ENABLE_GPIO, enabled };
    int result = io->property(io->context, TAG_SET_GPIO, gpio, 2);
    if (result == HW_TIMEOUT) return BT_TIMEOUT;
    if (result != HW_OK || (gpio[0] != 0 && gpio[0] != BT_ENABLE_GPIO)) return BT_IO;
    return BT_OK;
}

static int radio_start(const HwOps* io, uint32_t control) {
    if (!hw_ops_valid(io) || !io->property || !io->owned(io->context))
        return BT_UNAVAILABLE;
    if ((uart_read(io, UART_ID0) & 0xffU) != 0x11U ||
        (uart_read(io, UART_ID1) & 0xffU) != 0x10U) return BT_UNAVAILABLE;

    uint32_t rate[2] = { UART_CLOCK_ID, 0 };
    int result = io->property(io->context, TAG_CLOCK_RATE, rate, 2);
    if (result == HW_TIMEOUT) return BT_TIMEOUT;
    if (result != HW_OK || rate[0] != UART_CLOCK_ID) return BT_IO;
    uint64_t divisor = ((uint64_t)rate[1] * 4 + UART_BAUD / 2) / UART_BAUD;
    if (divisor < 64 || divisor > 0x3fffffU) return BT_PROTOCOL;

    result = set_radio_power(io, 0);
    if (result != BT_OK) return result;
    if (hw_delay(io, 5000U) != HW_OK) return BT_TIMEOUT;
    /* Disable transmission before draining an old boot UART configuration. */
    uart_write(io, UART_CONTROL, 0);
    uart_write(io, UART_LINE, 0);
    if (hw_wait(io, UART0 + UART_FLAGS, UART_BUSY, 0, 20000U) != HW_OK) return BT_TIMEOUT;
    uart_write(io, UART_IRQ_MASK, 0);
    uart_write(io, UART_IRQ_CLEAR, 0x7ffU);
    uart_write(io, UART_DMA, 0);
    uart_write(io, UART_INT_DIVISOR, (uint32_t)(divisor >> 6));
    uart_write(io, UART_FRAC_DIVISOR, (uint32_t)(divisor & 63U));
    uart_write(io, UART_LINE, UART_FIFO_8N1);
    for (unsigned int pin = 30; pin <= 33; pin++) {
        gpio_pull(pin, 0);
        gpio_function(pin, 7);  /* ALT3, UART0 CTS/RTS/TX/RX. */
    }
    uart_write(io, UART_CONTROL, control);
    result = set_radio_power(io, 1);
    if (result != BT_OK) return result;
    if (hw_delay(io, 120000U) != HW_OK) return BT_TIMEOUT;

    /* Ignore a bounded amount of pre-reset UART input. */
    for (unsigned int i = 0; i < 64 && !(uart_read(io, UART_FLAGS) & UART_RX_EMPTY); i++)
        (void)uart_read(io, UART_DATA);
    uart_write(io, UART_ERRORS, 0xff);
    BtH4Io transport = { (void*)io, send_packet, receive_byte, time_us_cb };
    return bt_hci_init(&controller, &transport);
}

#if VANTAGE_RADIO_DIAGNOSTICS
static int diagnostic_start(const HwOps* io, uint32_t control, const char* label) {
    uart_writeText("[RADIO] BT ");
    uart_writeText(label);
    uart_writeText(" start\n");
    diagnostic_rx_bytes = 0;
    int result = radio_start(io, control);
    if (result == BT_OK) {
        uint32_t start = io->time_us(io->context);
        while (controller.status.state == BT_BOOTING &&
               io->time_us(io->context) - start < 4000000U) bt_hci_poll(&controller);
        if (controller.status.state != BT_READY)
            result = controller.status.error ? controller.status.error : BT_TIMEOUT;
    }
    uart_writeText("[RADIO] BT ");
    uart_writeText(label);
    uart_writeText(" result=");
    uart_writeHex((uint32_t)result);
    uart_writeText(" rx_bytes=");
    uart_writeHex(diagnostic_rx_bytes);
    uart_writeText("\n");
    return result;
}
#endif

int pi4_bt_start(const HwOps* io) {
#if VANTAGE_RADIO_DIAGNOSTICS
    int initial = diagnostic_start(io, UART_H4_CONTROL & ~UART_CTS_ENABLE, "CTS disabled");
    int result = diagnostic_start(io, UART_H4_CONTROL, "CTS enabled");
    if (result != BT_OK && initial == BT_OK)
        result = diagnostic_start(io, UART_H4_CONTROL & ~UART_CTS_ENABLE, "restore CTS disabled");
#else
    int result = radio_start(io, UART_H4_CONTROL);
#endif
    startup_error = result == BT_OK ? 0 : result;
    return result;
}

void pi4_bt_poll(void) {
    if (!startup_error) bt_hci_poll(&controller);
}

void pi4_bt_status(BtStatus* status) {
    if (startup_error) *status = (BtStatus){ .state = BT_FAILED, .error = startup_error };
    else bt_hci_status(&controller, status);
}

int pi4_bt_scan_start(void) {
    return startup_error ? startup_error : bt_hci_scan_start(&controller);
}

int pi4_bt_device(uint32_t index, BtDevice* device) {
    return startup_error ? startup_error : bt_hci_device(&controller, index, device);
}
