#include <vantage/platform/pi4/usb_input.h>
#include <vantage/platform/pi4/usb_disk.h>
#include <stdio.h>
#include <stdlib.h>

int pi4_usb_disk_configure(unsigned int path, const uint8_t* config, size_t bytes) {
    (void)path; (void)config; (void)bytes;
    return HW_UNSUPPORTED;
}
void pi4_usb_disk_detach(void) {}

#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "USB input hub check failed at line %d: %s\n", __LINE__, #test); exit(1); \
} } while (0)

enum { ROOT = 1, PORTS = 4 };
enum { CLOSED, OPEN, ENDPOINT, CONFIGURED, BOOT };
enum { CONNECT = 1, ENABLE = 2, RESET = 16, POWER = 256, LOW = 512, HIGH = 1024 };
enum { KEYBOARD = 1, TIMEOUT_PORT, NESTED_HUB, MOUSE };

static const uint8_t hub_config[] = {
    9, 2, 41, 0, 1, 3, 0, 0xc0, 0,
    9, 4, 0, 0, 1, 9, 0, 1, 0,
    7, 5, 0x81, 3, 1, 0, 12,
    9, 4, 0, 1, 1, 9, 0, 2, 0,
    7, 5, 0x82, 3, 1, 0, 12
};
static const uint8_t boot_config[] = {
    9, 2, 34, 0, 1, 2, 0, 0x80, 50,
    9, 4, 0, 0, 1, 3, 1, 1, 0,
    9, 0x21, 0x11, 1, 0, 1, 0x22, 51, 0,
    7, 5, 0x81, 3, 8, 0, 10
};

typedef struct {
    unsigned int status, changes, phase, resets, opens, closes, disables;
    uint32_t reset_at, completion_seen, connection_seen;
    uint8_t report[8];
    size_t length;
} Child;
static Child children[PORTS + 1];
static uint32_t now = 0xffff0000U, powered_at;
static unsigned int root_attached = 1, root_phase, root_opens, root_closes;
static unsigned int slot_staged, staged_count, temporary_count, final_count, power_count;
static unsigned int reset_port, port_status_reads, early_stall = 1, multi_tt = 1, corrupt_header;

static uint32_t clock_us(void* context) { (void)context; return now; }

int pi4_usb_start(const HwOps* io, Pi4UsbProbe* probe) {
    CHECK(io->time_us == clock_us);
    *probe = (Pi4UsbProbe){.stage = PI4_USB_RUNNING, .ports = 1};
    return HW_OK;
}

int pi4_usb_stop(void) {
    CHECK(0);
    return HW_INVALID;
}

unsigned int pi4_usb_port_count(void) { return 1; }

int pi4_usb_port_connected(unsigned int path) {
    unsigned int port = path >> 8;
    CHECK((path & 255U) == ROOT && port <= PORTS);
    return root_attached && (!port || children[port].phase >= OPEN) ? HW_OK : HW_OFF;
}

int pi4_usb_open_port(unsigned int path) {
    CHECK(path == ROOT && root_attached && root_phase < OPEN && !reset_port);
    root_phase = OPEN;
    root_opens++;
    return HW_OK;
}

int pi4_usb_close_port(unsigned int path) {
    unsigned int port = path >> 8;
    CHECK((path & 255U) == ROOT && port <= PORTS);
    if (port) {
        CHECK(children[port].phase >= OPEN);
        children[port].phase = CLOSED;
        children[port].length = 0;
        children[port].closes++;
        return HW_OK;
    }
    for (unsigned int i = 1; i <= PORTS; i++) {
        CHECK(children[i].phase < OPEN);
        children[i].status &= CONNECT | LOW | HIGH;
        children[i].changes = children[i].status & CONNECT;
    }
    root_phase = CLOSED;
    slot_staged = power_count = reset_port = 0;
    root_closes++;
    return HW_OK;
}

int pi4_usb_configure_hub(unsigned int root, unsigned int count, unsigned int think_time) {
    CHECK(root == ROOT && root_phase == OPEN && !slot_staged && count == PORTS && think_time == 2);
    slot_staged = 1;
    staged_count++;
    return HW_OK;
}

int pi4_usb_open_child(unsigned int root, unsigned int port, unsigned int speed) {
    CHECK(root == ROOT && root_phase == CONFIGURED && slot_staged && port == reset_port);
    Child* child = &children[port];
    CHECK((child->status & (CONNECT | ENABLE | RESET)) == (CONNECT | ENABLE));
    CHECK(now - child->completion_seen >= 10000U && child->phase < OPEN);
    CHECK(speed == (port == MOUSE ? 2U : port == NESTED_HUB ? 3U : 1U));
    child->phase = OPEN;
    child->opens++;
    reset_port = 0;
    return HW_OK;
}

int pi4_usb_configure_interrupt_in(unsigned int path, uint8_t endpoint, uint16_t packet,
                                  uint8_t interval) {
    unsigned int port = path >> 8;
    CHECK(pi4_usb_port_connected(path) == HW_OK && endpoint == 0x81);
    if (port) {
        CHECK((port == KEYBOARD || port == MOUSE) && children[port].phase == OPEN);
        CHECK(packet == 8 && interval == 10);
        children[port].phase = ENDPOINT;
    } else {
        CHECK(root_phase == OPEN && packet == 1 && interval == 12);
        if (slot_staged) final_count++;
        else {
            CHECK(early_stall && root_opens == 1);
            temporary_count++;
        }
        root_phase = ENDPOINT;
    }
    return HW_OK;
}

static int copy_descriptor(const Pi4UsbSetup* setup, void* data, size_t* actual,
                           const uint8_t* descriptor, size_t bytes) {
    CHECK(data != NULL);
    *actual = setup->length < bytes ? setup->length : bytes;
    memcpy(data, descriptor, *actual);
    return HW_OK;
}

static int get_configuration(unsigned int port, const Pi4UsbSetup* setup, void* data, size_t* actual) {
    CHECK(setup->request_type == 0x80 && !setup->index);
    if (port == KEYBOARD || port == MOUSE) {
        uint8_t descriptor[sizeof(boot_config)];
        memcpy(descriptor, boot_config, sizeof(descriptor));
        if (port == MOUSE) descriptor[16] = 2;
        return copy_descriptor(setup, data, actual, descriptor, sizeof(descriptor));
    }
    uint8_t descriptor[sizeof(hub_config)];
    memcpy(descriptor, hub_config, sizeof(descriptor));
    if (!multi_tt) {
        descriptor[2] = 25;
        descriptor[16] = 0;
    }
    if (!port && corrupt_header && setup->length > 9) descriptor[1] = 7;
    return copy_descriptor(setup, data, actual, descriptor, descriptor[2]);
}

static int root_control(const Pi4UsbSetup* setup, void* data, size_t* actual) {
    unsigned int port = setup->index;
    CHECK(port <= PORTS);
    if (setup->request == 6 && setup->value == 0x0100) {
        const uint8_t device[] = {18, 1, 0, 2, 9, 0, multi_tt ? 2 : 1, 64, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        CHECK(root_phase == OPEN && setup->request_type == 0x80 && !port && setup->length == 18);
        return copy_descriptor(setup, data, actual, device, sizeof(device));
    }
    if (setup->request == 6 && setup->value == 0x2900) {
        const uint8_t descriptor[] = {9, 0x29, PORTS, 0x49, 0, 25, 0, 0, 0xff};
        CHECK(setup->request_type == 0xa0 && !port && setup->length >= 9);
        if (root_phase == OPEN && early_stall) return PI4_USB_STALL;
        CHECK(root_phase == OPEN || root_phase == CONFIGURED);
        return copy_descriptor(setup, data, actual, descriptor, sizeof(descriptor));
    }
    if (setup->request == 9) {
        CHECK(root_phase == ENDPOINT && setup->value == 3 && !setup->request_type &&
              !port && !setup->length && !data);
        root_phase = CONFIGURED;
        return HW_OK;
    }

    CHECK(root_phase == CONFIGURED && slot_staged);
    Child* child = &children[port];
    if (!setup->request) {
        CHECK(setup->request_type == (port ? 0xa3 : 0xa0) && !setup->value && setup->length == 4);
        if (port) {
            CHECK(power_count == PORTS && now - powered_at >= 50000U);
            port_status_reads++;
            if (child->changes & CONNECT) child->connection_seen = now;
            if (child->changes & RESET) child->completion_seen = now;
        }
        const uint8_t status[] = {
            (uint8_t)child->status, (uint8_t)(child->status >> 8), (uint8_t)child->changes, 0
        };
        return copy_descriptor(setup, data, actual, status, sizeof(status));
    }
    CHECK(port && setup->request_type == 0x23 && !setup->length && !data);
    if (setup->request == 3 && setup->value == 8) {
        CHECK(!(child->status & POWER) && port == ++power_count);
        child->status |= POWER;
        powered_at = now;
    } else if (setup->request == 3 && setup->value == 4) {
        CHECK(!reset_port && (child->status & CONNECT) && now - child->connection_seen >= 100000U);
        reset_port = port;
        child->status = (child->status & ~(unsigned int)ENABLE) | RESET;
        child->reset_at = now;
        child->resets++;
    } else if (setup->request == 1 && setup->value == 1) {
        child->status &= ~(unsigned int)(ENABLE | RESET);
        child->disables++;
        if (reset_port == port) reset_port = 0;
    } else {
        CHECK(setup->request == 1 && setup->value >= 16 && setup->value <= 20);
        child->changes &= ~(1U << (setup->value - 16));
    }
    return HW_OK;
}

int pi4_usb_control(unsigned int path, const Pi4UsbSetup* setup, void* data, size_t* actual) {
    unsigned int port = path >> 8;
    CHECK(pi4_usb_port_connected(path) == HW_OK && actual);
    *actual = 0;
    if (setup->request == 6 && setup->value == 0x0200)
        return get_configuration(port, setup, data, actual);
    if (!port) return root_control(setup, data, actual);

    Child* child = &children[port];
    CHECK(!setup->index && !setup->length && !data);
    if (setup->request == 9) {
        CHECK(!setup->request_type && setup->value == 2 && child->phase == ENDPOINT);
        child->phase = CONFIGURED;
    } else if (setup->request == 11) {
        CHECK(setup->request_type == 0x21 && !setup->value && child->phase == CONFIGURED);
        child->phase = BOOT;
    } else {
        CHECK(port == KEYBOARD && child->phase == BOOT && setup->request_type == 0x21 &&
              setup->request == 10 && !setup->value);
    }
    return HW_OK;
}

int pi4_usb_interrupt_in(unsigned int path, uint8_t endpoint, void* data,
                         size_t capacity, size_t* actual) {
    CHECK(pi4_usb_port_connected(path) == HW_OK && endpoint == 0x81);
    unsigned int port = path >> 8;
    if (port) {
        Child* child = &children[port];
        CHECK(child->phase == BOOT && capacity >= 8);
        if (!child->length) return HW_BUSY;
        *actual = child->length;
        memcpy(data, child->report, child->length);
        child->length = 0;
        return HW_OK;
    }
    CHECK(root_phase == CONFIGURED && slot_staged && capacity >= 1);
    uint8_t bitmap = 0;
    for (unsigned int i = 1; i <= PORTS; i++) {
        Child* child = &children[i];
        if (i != TIMEOUT_PORT && (child->status & RESET) && now - child->reset_at >= 20000U) {
            child->status = (child->status & ~(unsigned int)RESET) | ENABLE;
            child->changes |= RESET;
        }
        if (child->changes) bitmap |= (uint8_t)(1U << i);
    }
    if (!bitmap) return HW_BUSY;
    *(uint8_t*)data = bitmap;
    *actual = 1;
    return HW_OK;
}

static void tick(uint32_t delay) {
    now += delay;
    pi4_usb_input_poll();
}

static void ticks(unsigned int count) {
    for (unsigned int i = 0; i < count; i++) tick(10000);
}

static void set_connection(unsigned int port, int connected) {
    Child* child = &children[port];
    if (connected) child->status |= CONNECT;
    else child->status &= ~(unsigned int)(CONNECT | ENABLE | RESET);
    child->changes |= CONNECT;
}

int main(void) {
    static const HwOps io = {.time_us = clock_us};
    Pi4UsbProbe probe;
    set_connection(KEYBOARD, 1);
    set_connection(TIMEOUT_PORT, 1);
    set_connection(NESTED_HUB, 1);
    children[NESTED_HUB].status |= HIGH;
    children[MOUSE].status |= LOW;
    CHECK(pi4_usb_input_start(&io, &probe, 480, 800) == HW_OK);
    tick(0);
    CHECK(root_opens == 2 && root_closes == 1 && temporary_count == 1 && final_count == 1);
    tick(49000);
    CHECK(!port_status_reads);
    tick(1000);
    CHECK(port_status_reads == 1);
    ticks(80);
    CHECK(children[KEYBOARD].phase == BOOT && children[KEYBOARD].opens == 1);
    CHECK(children[TIMEOUT_PORT].resets == 1 && children[TIMEOUT_PORT].disables == 1);
    CHECK(!children[TIMEOUT_PORT].opens && root_closes == 1);
    CHECK(children[NESTED_HUB].opens == 1 && children[NESTED_HUB].closes == 1);

    children[KEYBOARD].report[2] = 0x04;
    children[KEYBOARD].length = 8;
    tick(1000);
    KeyEvent key;
    CHECK(pi4_usb_key_read(&key) && key.type == KEY_TEXT && key.character == 'a');
    set_connection(MOUSE, 1);
    ticks(5);
    set_connection(MOUSE, 0);
    ticks(1);
    set_connection(MOUSE, 1);
    ticks(8);
    CHECK(!children[MOUSE].resets);
    ticks(15);
    CHECK(children[MOUSE].phase == BOOT);

    children[MOUSE].report[0] = 1;
    children[MOUSE].length = 3;
    tick(1000);
    TouchContact contact;
    CHECK(pi4_usb_touch_read(&contact) && contact.event.type == TOUCH_EVENT_DOWN);
    CHECK(contact.id == TOUCH_CONTACT_MOUSE);
    set_connection(MOUSE, 0);
    ticks(1);
    CHECK(pi4_usb_touch_read(&contact) && contact.event.type == TOUCH_EVENT_UP);
    CHECK(contact.event.x == -1 && contact.event.y == -1);
    CHECK(children[MOUSE].closes == 1 && children[KEYBOARD].phase == BOOT && root_closes == 1);
    CHECK(children[TIMEOUT_PORT].resets == 1);

    children[KEYBOARD].report[2] = 0x05;
    children[KEYBOARD].length = 8;
    tick(1000);
    CHECK(pi4_usb_key_pending());
    root_attached = 0;
    ticks(1);
    CHECK(children[KEYBOARD].closes == 1 && root_closes == 2 && !pi4_usb_key_pending());

    multi_tt = early_stall = 0;
    corrupt_header = root_attached = 1;
    ticks(10);
    CHECK(root_opens == 3 && root_closes == 3 && staged_count == 1);
    root_attached = 0;
    ticks(10);
    corrupt_header = 0;
    root_attached = 1;
    ticks(10);
    CHECK(root_opens == 4 && root_closes == 3 && staged_count == 2 && final_count == 2);
    return 0;
}
