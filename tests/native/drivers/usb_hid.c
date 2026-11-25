#include <vantage/drivers/usb_hid.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "USB HID check failed at line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

enum { KEYBOARD_INTERFACE = 32, KEYBOARD_HID = 41, KEYBOARD_OUT = 50, KEYBOARD_IN = 57,
       MOUSE_INTERFACE = 64, MOUSE_HID = 73, MOUSE_IN = 88, CONFIG_LENGTH = 95 };

static const uint8_t composite[] = {
    9, 2, CONFIG_LENGTH, 0, 3, 5, 0, 0x80, 50,
    9, 4, 0, 0, 2, 8, 6, 0x50, 0,
    7, 5, 3, 2, 64, 0, 0,
    7, 5, 0x83, 2, 64, 0, 0,
    9, 4, 1, 0, 2, 3, 1, 1, 0,
    9, 0x21, 0x11, 1, 0, 1, 0x22, 51, 0,
    7, 5, 1, 3, 8, 0, 10,
    7, 5, 0x81, 3, 8, 0, 10,
    9, 4, 2, 0, 1, 3, 1, 2, 0,
    12, 0x21, 0x11, 1, 0, 2, 0x22, 42, 0, 0x23, 4, 0,
    3, 0xff, 7,
    7, 5, 0x82, 3, 4, 0, 5
};

static int discovery(void) {
    uint8_t config[sizeof(composite) + 1] = {0};
    UsbHidBootInterfaces found;
    memcpy(config, composite, sizeof(composite));
    CHECK(usb_hid_find_boot_interfaces(config, sizeof(config), &found) == HW_OK);
    CHECK(found.configuration_value == 5);
    CHECK(found.keyboard.interface_number == 1 && found.keyboard.endpoint == 0x81 &&
          found.keyboard.max_packet_size == 8 && found.keyboard.interval == 10 &&
          found.keyboard.transactions == 1);
    CHECK(found.mouse.interface_number == 2 && found.mouse.endpoint == 0x82 &&
          found.mouse.max_packet_size == 4 && found.mouse.interval == 5 &&
          found.mouse.transactions == 1);

    config[MOUSE_INTERFACE + 7] = 1; /* Two keyboards, pick the first. */
    CHECK(usb_hid_find_boot_interfaces(config, sizeof(config), &found) == HW_OK);
    CHECK(found.keyboard.endpoint == 0x81 && !found.mouse.endpoint);
    config[MOUSE_INTERFACE + 7] = 2;
    config[KEYBOARD_INTERFACE + 6] = 0; /* Report-protocol-only HID. */
    config[MOUSE_IN + 4] = 0;
    config[MOUSE_IN + 5] = 0x0a; /* 512-byte payload, one additional transaction. */
    CHECK(usb_hid_find_boot_interfaces(config, sizeof(config), &found) == HW_OK);
    CHECK(!found.keyboard.endpoint && found.mouse.endpoint == 0x82 &&
          found.mouse.max_packet_size == 512 && found.mouse.transactions == 2);
    config[MOUSE_INTERFACE + 7] = 0;
    CHECK(usb_hid_find_boot_interfaces(config, sizeof(config), &found) == HW_UNSUPPORTED);
    CHECK(!found.configuration_value && !found.keyboard.endpoint && !found.mouse.endpoint);

    const uint8_t alternate[] = {
        9, 2, 43, 0, 1, 2, 0, 0x80, 50,
        9, 4, 0, 0, 0, 0xff, 0, 0, 0,
        9, 4, 0, 1, 1, 3, 1, 1, 0,
        9, 0x21, 0x11, 1, 0, 1, 0x22, 51, 0,
        7, 5, 0x81, 3, 8, 0, 10
    };
    CHECK(usb_hid_find_boot_interfaces(alternate, sizeof(alternate), &found) == HW_UNSUPPORTED);
    return 0;
}

static int malformed_configurations(void) {
    static const struct { size_t offset; uint8_t value; } mutations[] = {
        {0, 8}, {1, 1}, {2, 8}, {2, MOUSE_IN + 1}, {2, CONFIG_LENGTH + 1},
        {3, 1}, {4, 2}, {5, 0},
        {9, 0}, {9, 1}, {9, 8}, {10, 2}, {13, 1}, {13, 3},
        {KEYBOARD_INTERFACE + 2, 0}, {KEYBOARD_INTERFACE + 4, 0},
        {KEYBOARD_HID + 1, 0xff}, {KEYBOARD_HID + 5, 0}, {KEYBOARD_HID + 5, 255},
        {KEYBOARD_HID + 6, 0x23}, {KEYBOARD_HID + 7, 0},
        {KEYBOARD_OUT + 2, 0x81}, {KEYBOARD_IN + 2, 0x80}, {KEYBOARD_IN + 2, 0x91},
        {KEYBOARD_IN + 2, 2}, {KEYBOARD_IN + 3, 2}, {KEYBOARD_IN + 4, 0},
        {KEYBOARD_IN + 5, 0x18}, {KEYBOARD_IN + 5, 0x20}, {KEYBOARD_IN + 6, 0},
        {MOUSE_HID, 9}, {MOUSE_IN, 8}, {MOUSE_IN, 6}, {MOUSE_IN + 2, 0x81}
    };
    UsbHidBootInterfaces found;
    uint8_t config[sizeof(composite)];
    for (size_t size = 0; size < sizeof(composite); size++) {
        uint8_t* truncated = malloc(size ? size : 1);
        CHECK(truncated);
        memcpy(truncated, composite, size);
        int result = usb_hid_find_boot_interfaces(truncated, size, &found);
        free(truncated);
        CHECK(result == HW_PROTOCOL);
    }
    CHECK(usb_hid_find_boot_interfaces(composite, sizeof(composite), NULL) == HW_INVALID);
    CHECK(usb_hid_find_boot_interfaces(NULL, sizeof(composite), &found) == HW_INVALID);
    for (size_t i = 0; i < sizeof(mutations) / sizeof(mutations[0]); i++) {
        memcpy(config, composite, sizeof(config));
        CHECK(usb_hid_find_boot_interfaces(config, sizeof(config), &found) == HW_OK);
        config[mutations[i].offset] = mutations[i].value;
        CHECK(usb_hid_find_boot_interfaces(config, sizeof(config), &found) == HW_PROTOCOL);
        CHECK(!found.configuration_value && !found.keyboard.endpoint && !found.mouse.endpoint);
    }
    return 0;
}

static int control_requests(void) {
    uint8_t setup[USB_HID_SETUP_BYTES];
    const uint8_t protocol[] = {0x21, 0x0b, 0, 0, 2, 0, 0, 0};
    const uint8_t idle[] = {0x21, 0x0a, 0, 125, 255, 0, 0, 0};
    const uint8_t leds[] = {0x21, 0x09, 0, 2, 1, 0, 1, 0};
    usb_hid_set_boot_protocol(2, setup);
    CHECK(!memcmp(protocol, setup, sizeof(setup)));
    usb_hid_set_idle(255, 125, setup);
    CHECK(!memcmp(idle, setup, sizeof(setup)));
    usb_hid_set_idle(255, 0, setup);
    CHECK(!setup[3]);
    usb_hid_set_keyboard_leds(1, setup);
    CHECK(!memcmp(leds, setup, sizeof(setup)));
    return 0;
}

static int is_key(KeyEvent event, uint32_t type, uint32_t character) {
    return event.type == type && event.character == character;
}

static int send_keys(UsbHidKeyboard* keyboard, uint32_t now_ms, uint8_t modifiers,
                     uint8_t first, uint8_t second, KeyEvent events[USB_HID_KEY_EVENTS_MAX]) {
    const uint8_t report[] = {modifiers, 0xff, first, second, 0, 0, 0, 0};
    return usb_hid_keyboard_report(keyboard, report, sizeof(report), now_ms, events);
}

static int key_mapping(void) {
    static const struct { uint8_t usage, modifiers; uint32_t type, character; } cases[] = {
        {0x04, 0, KEY_TEXT, 'a'}, {0x1d, 0x20, KEY_TEXT, 'Z'},
        {0x1e, 0, KEY_TEXT, '1'}, {0x27, 2, KEY_TEXT, ')'},
        {0x2c, 0, KEY_TEXT, ' '}, {0x31, 0, KEY_TEXT, '\\'},
        {0x31, 2, KEY_TEXT, '|'}, {0x34, 2, KEY_TEXT, '"'}, {0x38, 2, KEY_TEXT, '?'},
        {0x28, 0, KEY_ENTER, 0}, {0x58, 0, KEY_ENTER, 0},
        {0x29, 0, KEY_HOME, 0}, {0x2a, 0, KEY_BACKSPACE, 0},
        {0x06, 1, KEY_CANCEL, 0}, {0x0f, 0x10, KEY_CLEAR, 0},
        {0x54, 0, KEY_TEXT, '/'}, {0x67, 0, KEY_TEXT, '='},
        {0x04, 0x04, KEY_NONE, 0}, {0x04, 0x80, KEY_NONE, 0}, {0x04, 1, KEY_NONE, 0},
        {0x06, 0x41, KEY_NONE, 0}, {0x32, 0, KEY_NONE, 0}, {0x2b, 0, KEY_NONE, 0},
        {0x3a, 0, KEY_NONE, 0}, {0x4f, 0, KEY_NONE, 0}, {0x62, 0, KEY_NONE, 0}
    };
    UsbHidKeyboard keyboard;
    usb_hid_keyboard_reset(&keyboard);
    KeyEvent events[USB_HID_KEY_EVENTS_MAX];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        CHECK(send_keys(&keyboard, 0, 0, 0, 0, events) == 0);
        int count = send_keys(&keyboard, 0, cases[i].modifiers, cases[i].usage, 0, events);
        CHECK(count == (cases[i].type != KEY_NONE));
        if (count) CHECK(is_key(events[0], cases[i].type, cases[i].character));
    }
    CHECK(send_keys(&keyboard, 0, 0, 0x04, 0x39, events) == 1);
    CHECK(is_key(events[0], KEY_TEXT, 'A') && keyboard.leds == USB_HID_LED_CAPS_LOCK);
    CHECK(send_keys(&keyboard, 0, 2, 0x39, 0x05, events) == 1);
    CHECK(is_key(events[0], KEY_TEXT, 'b') && keyboard.leds == USB_HID_LED_CAPS_LOCK);
    CHECK(send_keys(&keyboard, 0, 0, 0x53, 0x62, events) == 1);
    CHECK(is_key(events[0], KEY_TEXT, '0') &&
          keyboard.leds == (USB_HID_LED_CAPS_LOCK | USB_HID_LED_NUM_LOCK));
    CHECK(send_keys(&keyboard, 0, 0, 0x39, 0x47, events) == 0);
    CHECK(keyboard.leds == (USB_HID_LED_NUM_LOCK | USB_HID_LED_SCROLL_LOCK));
    usb_hid_keyboard_reset(&keyboard);
    CHECK(!keyboard.leds && send_keys(&keyboard, 0, 0, 0x04, 0, events) == 1);
    CHECK(is_key(events[0], KEY_TEXT, 'a'));
    return 0;
}

static int key_state_and_repeat(void) {
    UsbHidKeyboard keyboard = {0};
    KeyEvent events[USB_HID_KEY_EVENTS_MAX], repeated;
    const uint8_t six_keys[] = {0, 0, 4, 5, 6, 7, 8, 9};
    const uint8_t oversized[9] = {0};
    CHECK(usb_hid_keyboard_report(&keyboard, six_keys, sizeof(six_keys), 0, events) == 6);
    for (unsigned int i = 0; i < USB_HID_KEY_EVENTS_MAX; i++) {
        CHECK(is_key(events[i], KEY_TEXT, 'a' + i));
    }
    CHECK(send_keys(&keyboard, 100, 0, 9, 4, events) == 0);
    CHECK(!usb_hid_keyboard_repeat(&keyboard, 499, &repeated));
    CHECK(usb_hid_keyboard_repeat(&keyboard, 500, &repeated) && is_key(repeated, KEY_TEXT, 'f'));
    CHECK(!usb_hid_keyboard_repeat(&keyboard, 549, &repeated));
    CHECK(usb_hid_keyboard_repeat(&keyboard, 800, &repeated));
    CHECK(!usb_hid_keyboard_repeat(&keyboard, 800, &repeated));
    CHECK(send_keys(&keyboard, 801, 0, 4, 4, events) == 0);
    CHECK(!usb_hid_keyboard_repeat(&keyboard, 1400, &repeated));
    CHECK(send_keys(&keyboard, 1401, 0, 0, 0, events) == 0);
    CHECK(send_keys(&keyboard, 1402, 0, 0x04, 0x04, events) == 1);

    const uint8_t rollover[] = {2, 0, 1, 1, 1, 1, 1, 1};
    CHECK(usb_hid_keyboard_report(&keyboard, rollover, sizeof(rollover), 1450, events) == HW_PROTOCOL);
    CHECK(!usb_hid_keyboard_repeat(&keyboard, 2000, &repeated));
    CHECK(send_keys(&keyboard, 2001, 0, 0x04, 0x02, events) == HW_PROTOCOL);
    CHECK(send_keys(&keyboard, 2002, 0, 0x03, 0x39, events) == HW_PROTOCOL && !keyboard.leds);
    CHECK(usb_hid_keyboard_report(&keyboard, six_keys, sizeof(six_keys) - 1, 2003, events) == HW_PROTOCOL);
    CHECK(usb_hid_keyboard_report(&keyboard, oversized, sizeof(oversized), 2004, events) == HW_PROTOCOL);
    CHECK(usb_hid_keyboard_report(&keyboard, NULL, 0, 2005, events) == HW_PROTOCOL);
    CHECK(send_keys(&keyboard, 2100, 0, 0x04, 0, events) == 0);
    CHECK(!usb_hid_keyboard_repeat(&keyboard, 2599, &repeated));
    CHECK(usb_hid_keyboard_repeat(&keyboard, 2600, &repeated) && is_key(repeated, KEY_TEXT, 'a'));
    CHECK(send_keys(&keyboard, 2601, 2, 0x04, 0, events) == 0);
    CHECK(!usb_hid_keyboard_repeat(&keyboard, 3100, &repeated));
    CHECK(usb_hid_keyboard_repeat(&keyboard, 3101, &repeated) && is_key(repeated, KEY_TEXT, 'A'));

    usb_hid_keyboard_reset(&keyboard);
    CHECK(!usb_hid_keyboard_repeat(&keyboard, 4000, &repeated));
    CHECK(send_keys(&keyboard, 4000, 0, 0x28, 0x29, events) == 2);
    CHECK(!usb_hid_keyboard_repeat(&keyboard, 5000, &repeated));
    CHECK(send_keys(&keyboard, 5001, 1, 0x06, 0x0f, events) == 2);
    CHECK(!usb_hid_keyboard_repeat(&keyboard, 6000, &repeated));

    const uint32_t before_wrap = (uint32_t)-200;
    CHECK(send_keys(&keyboard, before_wrap, 0, 0x2a, 0, events) == 1);
    CHECK(!usb_hid_keyboard_repeat(&keyboard, 299, &repeated));
    CHECK(usb_hid_keyboard_repeat(&keyboard, 300, &repeated) && is_key(repeated, KEY_BACKSPACE, 0));
    CHECK(send_keys(&keyboard, 350, 0, 0, 0, events) == 0);
    CHECK(!usb_hid_keyboard_repeat(&keyboard, 350, &repeated));
    return 0;
}

static int is_touch(TouchContact contact, TouchEventType type, int x, int y, uint32_t id) {
    return contact.event.type == type && contact.event.x == x && contact.event.y == y && contact.id == id;
}

static int mouse_reports(void) {
    UsbHidMouse mouse;
    UsbHidMouseEvent event;
    TouchContact contact;
    const uint32_t id = 37;
    const uint8_t move[] = {0xf8, 0xfe, 0xff, 7, 6, 5, 4, 3};
    const uint8_t press[] = {USB_HID_MOUSE_LEFT | USB_HID_MOUSE_RIGHT, 0, 0};
    const uint8_t drag[] = {USB_HID_MOUSE_LEFT | USB_HID_MOUSE_MIDDLE, 127, 127};
    const uint8_t release[] = {USB_HID_MOUSE_RIGHT, 0x81, 0};
    const uint8_t oversized[9] = {0};

    usb_hid_mouse_init(&mouse, 10, 20, 6, 7);
    CHECK(usb_hid_mouse_report(&mouse, move, sizeof(move), &event) == 1);
    CHECK(event.x == 4 && event.y == 6 && event.moved && !event.buttons && !event.changed);
    CHECK(!usb_hid_mouse_to_touch(&event, id, &contact));
    CHECK(usb_hid_mouse_report(&mouse, press, sizeof(press), &event) == 1);
    CHECK(event.buttons == press[0] && event.changed == press[0] && !event.moved);
    CHECK(usb_hid_mouse_to_touch(&event, id, &contact) && is_touch(contact, TOUCH_EVENT_DOWN, 4, 6, id));
    CHECK(usb_hid_mouse_report(&mouse, press, sizeof(press), &event) == 0);
    CHECK(usb_hid_mouse_report(&mouse, drag, sizeof(drag), &event) == 1);
    CHECK(event.buttons == drag[0] && event.changed == (USB_HID_MOUSE_RIGHT | USB_HID_MOUSE_MIDDLE));
    CHECK(usb_hid_mouse_to_touch(&event, id, &contact) && is_touch(contact, TOUCH_EVENT_MOVE, 9, 19, id));
    CHECK(usb_hid_mouse_report(&mouse, drag, sizeof(drag), &event) == 0);

    CHECK(usb_hid_mouse_report(&mouse, release, 2, &event) == HW_PROTOCOL);
    CHECK(usb_hid_mouse_report(&mouse, oversized, sizeof(oversized), &event) == HW_PROTOCOL);
    CHECK(usb_hid_mouse_report(&mouse, NULL, 3, &event) == HW_PROTOCOL);
    CHECK(mouse.buttons == drag[0] && mouse.x == 9 && mouse.y == 19);
    CHECK(usb_hid_mouse_report(&mouse, release, sizeof(release), &event) == 1);
    CHECK(usb_hid_mouse_to_touch(&event, id, &contact) && is_touch(contact, TOUCH_EVENT_UP, 0, 19, id));
    CHECK(usb_hid_mouse_disconnect(&mouse, &event) && event.changed == USB_HID_MOUSE_RIGHT);
    CHECK(!usb_hid_mouse_to_touch(&event, id, &contact));
    CHECK(usb_hid_mouse_report(&mouse, press, sizeof(press), &event) == 1);
    CHECK(usb_hid_mouse_disconnect(&mouse, &event) && !event.buttons && event.changed == press[0]);
    CHECK(usb_hid_mouse_to_touch(&event, id, &contact) && is_touch(contact, TOUCH_EVENT_UP, 0, 19, id));
    CHECK(!usb_hid_mouse_disconnect(&mouse, &event));

    usb_hid_mouse_init(&mouse, INT_MAX, 0, INT_MAX, INT_MIN);
    CHECK(mouse.x == INT_MAX - 1 && mouse.y == 0);
    CHECK(usb_hid_mouse_report(&mouse, drag, sizeof(drag), &event) == 1 && !event.moved);
    CHECK(event.x == INT_MAX - 1 && event.y == 0);
    return 0;
}

int main(void) {
    if (discovery() || malformed_configurations() || control_requests() || key_mapping() ||
        key_state_and_repeat() || mouse_reports()) return 1;
    puts("USB boot HID: all 6 test groups passed");
    return 0;
}
