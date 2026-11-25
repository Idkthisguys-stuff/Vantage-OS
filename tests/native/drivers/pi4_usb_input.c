#include <vantage/platform/pi4/usb_input.h>
#include <vantage/platform/pi4/usb_disk.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "USB input check failed at line %d: %s\n", __LINE__, #test); exit(1); \
} } while (0)

/* Port 1 is a non-HID device, port 2 a keyboard, and port 3 a mouse. */
static const uint8_t keyboard_config[] = {
    9, 2, 34, 0, 1, 2, 0, 0x80, 50,
    9, 4, 5, 0, 1, 3, 1, 1, 0,
    9, 0x21, 0x11, 1, 0, 1, 0x22, 51, 0,
    7, 5, 0x81, 3, 8, 0, 10
};
static const uint8_t mouse_config[] = {
    9, 2, 34, 0, 1, 2, 0, 0x80, 50,
    9, 4, 5, 0, 1, 3, 1, 2, 0,
    9, 0x21, 0x11, 1, 0, 1, 0x22, 42, 0,
    7, 5, 0x81, 3, 8, 0, 10
};
static const uint8_t unsupported_config[] = {
    9, 2, 18, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 0, 9, 0, 0, 0
};

enum { CLOSED, OPEN, ENDPOINT, CONFIGURED, BOOT };
typedef struct {
    int connected, phase, opened, opened_closed, transfer_error;
    uint8_t bytes[8];
    size_t length;
} Port;
static Port ports[] = {{0}, {.connected = 1}, {.connected = 1}, {.connected = 1}};
static uint32_t clock_us = 0xffff0000U;
static int running, starts, stops, led_writes;

int pi4_usb_disk_configure(unsigned int path, const uint8_t* config, size_t bytes) {
    (void)path; (void)config; (void)bytes;
    return HW_UNSUPPORTED;
}
void pi4_usb_disk_detach(void) {}

static uint32_t read_clock(void* context) {
    (void)context;
    return clock_us;
}

int pi4_usb_start(const HwOps* io, Pi4UsbProbe* probe) {
    CHECK(io->time_us == read_clock && !running);
    starts++;
    running = 1;
    *probe = (Pi4UsbProbe){.stage = PI4_USB_RUNNING, .ports = 3};
    return HW_OK;
}

int pi4_usb_stop(void) {
    stops++;
    for (unsigned int i = 1; i < sizeof(ports) / sizeof(ports[0]); i++) {
        ports[i].phase = CLOSED;
        ports[i].length = 0;
    }
    running = 0;
    return HW_OK;
}

unsigned int pi4_usb_port_count(void) { return running ? 3U : 0; }

int pi4_usb_port_connected(unsigned int port) {
    CHECK(port && port <= 3);
    return running && ports[port].connected ? HW_OK : HW_OFF;
}

int pi4_usb_open_port(unsigned int port) {
    CHECK(pi4_usb_port_connected(port) == HW_OK && ports[port].phase == CLOSED);
    ports[port].phase = OPEN;
    ports[port].opened++;
    return HW_OK;
}

int pi4_usb_configure_hub(unsigned int root, unsigned int count, unsigned int think_time) {
    (void)root;
    (void)count;
    (void)think_time;
    CHECK(0);
    return HW_INVALID;
}

int pi4_usb_open_child(unsigned int root, unsigned int downstream, unsigned int speed) {
    (void)root;
    (void)downstream;
    (void)speed;
    CHECK(0);
    return HW_INVALID;
}

int pi4_usb_close_port(unsigned int port) {
    CHECK(running && ports[port].phase >= OPEN);
    ports[port].phase = CLOSED;
    ports[port].length = 0;
    ports[port].opened_closed++;
    return HW_OK;
}

int pi4_usb_configure_interrupt_in(unsigned int port, uint8_t endpoint, uint16_t packet,
                                  uint8_t interval) {
    CHECK(port >= 2 && port <= 3 && ports[port].phase == OPEN);
    CHECK(endpoint == 0x81 && packet == 8 && interval == 10);
    ports[port].phase = ENDPOINT;
    return HW_OK;
}

int pi4_usb_control(unsigned int port, const Pi4UsbSetup* setup, void* data, size_t* actual) {
    Port* device = &ports[port];
    CHECK(pi4_usb_port_connected(port) == HW_OK && actual != NULL);
    *actual = 0;
    switch (setup->request) {
    case 6: {
        const uint8_t* configuration = port == 1 ? unsupported_config
            : port == 2 ? keyboard_config : mouse_config;
        CHECK(device->phase == OPEN && data != NULL && setup->request_type == 0x80 &&
              setup->value == 0x0200 && setup->index == 0);
        CHECK(setup->length == 9 || setup->length == configuration[2]);
        memcpy(data, configuration, setup->length);
        *actual = setup->length;
        return HW_OK;
    }
    case 9:
        if (setup->request_type == 0x21) {
            CHECK(port == 2 && device->phase == BOOT && setup->value == 0x0200 &&
                  setup->index == 5 && setup->length == 1 && data);
            CHECK(*(const uint8_t*)data == 2);
            led_writes++;
            return PI4_USB_STALL;
        }
        CHECK(device->phase == ENDPOINT && !setup->request_type && setup->value == 2 &&
              !setup->index && !setup->length && !data);
        device->phase = CONFIGURED;
        return HW_OK;
    case 0x0b:
        CHECK(device->phase == CONFIGURED && setup->request_type == 0x21 && !setup->value &&
              setup->index == 5 && !setup->length && !data);
        device->phase = BOOT;
        return HW_OK;
    case 0x0a:
        CHECK(port == 2 && device->phase == BOOT && setup->request_type == 0x21 && !setup->value &&
              setup->index == 5 && !setup->length && !data);
        return PI4_USB_STALL; /* Remains usable after optional requests stall. */
    }
    CHECK(0);
    return HW_PROTOCOL;
}

int pi4_usb_interrupt_in(unsigned int port, uint8_t endpoint, void* report,
                         size_t capacity, size_t* actual) {
    Port* device = &ports[port];
    CHECK(pi4_usb_port_connected(port) == HW_OK && device->phase == BOOT);
    CHECK(endpoint == 0x81 && capacity >= sizeof(device->bytes));
    if (device->transfer_error) {
        device->transfer_error = 0;
        return HW_IO;
    }
    if (!device->length) return HW_BUSY;
    *actual = device->length;
    memcpy(report, device->bytes, device->length);
    device->length = 0;
    return HW_OK;
}

static void tick(uint32_t delay) {
    clock_us += delay;
    pi4_usb_input_poll();
}

static void keyboard_report(uint8_t usage) {
    ports[2].bytes[2] = usage;
    ports[2].length = sizeof(ports[2].bytes);
    tick(1000);
}

static void mouse_report(unsigned int button, uint8_t dx, uint8_t dy) {
    ports[3].bytes[0] = (uint8_t)button;
    ports[3].bytes[1] = dx;
    ports[3].bytes[2] = dy;
    ports[3].length = 3;
    tick(1000);
}

static void expect_touch(TouchEventType type, int x, int y) {
    TouchContact contact;
    CHECK(pi4_usb_touch_pending() && pi4_usb_touch_read(&contact));
    CHECK(contact.id == TOUCH_CONTACT_MOUSE && contact.event.type == type);
    CHECK(contact.event.x == x && contact.event.y == y);
}

int main(void) {
    static const HwOps hw = {.time_us = read_clock};
    Pi4UsbProbe probe;
    CHECK(pi4_usb_input_start(&hw, &probe, 480, 800) == HW_OK);
    tick(0);
    CHECK(ports[1].opened == 1 && ports[1].opened_closed == 1);
    tick(100000);
    tick(100000);
    int x, y;
    CHECK(pi4_usb_cursor(&x, &y) && x == 240 && y == 400);
    CHECK(ports[2].phase == BOOT && ports[3].phase == BOOT);

    KeyEvent key;
    keyboard_report(0x04);
    CHECK(pi4_usb_key_pending() && pi4_usb_key_read(&key));
    CHECK(key.type == KEY_TEXT && key.character == 'a');
    tick(499000);
    CHECK(!pi4_usb_key_pending());
    tick(1000);
    CHECK(pi4_usb_key_read(&key) && key.character == 'a');
    keyboard_report(0x39);
    CHECK(led_writes == 1 && !pi4_usb_key_pending());
    for (int i = 0; i < 40; i++) keyboard_report(i % 2 ? 0x04 : 0x05);
    for (int i = 0; i < 32; i++) {
        CHECK(pi4_usb_key_read(&key) && key.character == (uint32_t)(i % 2 ? 'A' : 'B'));
    }
    CHECK(!pi4_usb_key_pending());
    keyboard_report(0x05);
    CHECK(pi4_usb_key_pending());
    ports[2].connected = 0;
    tick(1000);
    CHECK(!pi4_usb_key_pending() && ports[2].opened_closed == 1);

    mouse_report(0, 3, 0xff);
    CHECK(!pi4_usb_touch_pending() && pi4_usb_cursor(&x, &y) && x == 243 && y == 399);
    mouse_report(1, 0, 0);
    expect_touch(TOUCH_EVENT_DOWN, 243, 399);
    mouse_report(1, 1, 0);
    mouse_report(1, 1, 0);
    expect_touch(TOUCH_EVENT_MOVE, 245, 399);
    ports[3].connected = 0;
    tick(1000);
    expect_touch(TOUCH_EVENT_UP, -1, -1);
    CHECK(!pi4_usb_touch_pending() && !pi4_usb_cursor(&x, &y));

    ports[3].connected = 1;
    tick(100000);
    tick(100000); /* A separate scan may probe the adjacent non-HID port first. */
    CHECK(ports[3].opened == 2);
    mouse_report(1, 0, 0);
    CHECK(pi4_usb_touch_pending());
    ports[3].transfer_error = 1;
    tick(1000);
    CHECK(!pi4_usb_touch_pending()); /* Discard an unfinished, unread press. */
    tick(100000);
    tick(100000);
    CHECK(ports[3].opened == 3);
    for (int i = 0; i < 12; i++) {
        mouse_report(1, 0, 0);
        mouse_report(0, 0, 0);
    }
    for (int i = 0; i < 8; i++) {
        expect_touch(TOUCH_EVENT_DOWN, 245, 399);
        expect_touch(TOUCH_EVENT_UP, 245, 399);
    }
    CHECK(!pi4_usb_touch_pending());
    mouse_report(1, 0, 0);
    expect_touch(TOUCH_EVENT_DOWN, 245, 399);
    running = 0;
    tick(1000);
    expect_touch(TOUCH_EVENT_UP, -1, -1);
    tick(999000);
    CHECK(starts == 1);
    tick(1000);
    CHECK(starts == 2 && stops == 1);
    return 0;
}
