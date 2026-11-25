#include <vantage/drivers/usb_hid.h>

/* USB 2.0 section 9.6 and HID 1.11 sections 4, 6 and 7. */
enum {
    USB_DESC_CONFIGURATION = 2, USB_DESC_INTERFACE = 4, USB_DESC_ENDPOINT = 5,
    HID_DESC_CLASS = 0x21, HID_DESC_REPORT = 0x22,
    HID_CLASS = 3, HID_SUBCLASS_BOOT = 1, HID_BOOT_KEYBOARD = 1, HID_BOOT_MOUSE = 2,
    USB_ENDPOINT_IN = 0x80, USB_ENDPOINT_INTERRUPT = 3,
    HID_SET_REPORT = 9, HID_SET_IDLE = 10, HID_SET_PROTOCOL = 11
};

typedef struct {
    UsbHidBootInterface in;
    uint32_t endpoints;
    uint8_t protocol, alternate, remaining_endpoints, has_hid, has_out;
} HidInterfaceScan;

static uint16_t read_le16(const uint8_t* data) {
    return (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
}

static int valid_hid_descriptor(const uint8_t* descriptor, uint8_t length) {
    if (length < 9 || !descriptor[5] || length != 6U + 3U * descriptor[5]) return 0;
    return descriptor[6] == HID_DESC_REPORT && read_le16(descriptor + 7) != 0;
}

static int scan_endpoint(HidInterfaceScan* scan, const uint8_t* descriptor,
                         uint32_t* default_endpoints) {
    uint8_t address = descriptor[2];
    if ((address & 0x70U) || !(address & 0x0fU)) return 0;
    unsigned int index = (address & 0x0fU) + ((address & USB_ENDPOINT_IN) ? 16U : 0U);
    uint32_t bit = (uint32_t)1 << index;
    if (scan->endpoints & bit) return 0;
    scan->endpoints |= bit;
    if (!scan->alternate) {
        if (*default_endpoints & bit) return 0;
        *default_endpoints |= bit;
    }
    if (!scan->protocol) return 1;

    uint16_t packet = read_le16(descriptor + 4);
    unsigned int payload = packet & 0x07ffU;
    unsigned int additional = (packet >> 11) & 3U;
    if (!scan->has_hid || (descriptor[3] & 3U) != USB_ENDPOINT_INTERRUPT ||
        (packet & 0xe000U) || !payload || payload > 1024U || additional == 3U ||
        !descriptor[6]) return 0;

    if (address & USB_ENDPOINT_IN) {
        if (scan->in.endpoint) return 0;
        scan->in.endpoint = address;
        scan->in.interval = descriptor[6];
        scan->in.max_packet_size = (uint16_t)payload;
        scan->in.transactions = (uint8_t)(additional + 1U);
    } else {
        if (scan->has_out) return 0;
        scan->has_out = 1;
    }
    return 1;
}

static int finish_interface(const HidInterfaceScan* scan, UsbHidBootInterfaces* found) {
    if (scan->remaining_endpoints) return 0;
    if (!scan->protocol) return 1;
    if (!scan->in.endpoint) return 0;
    UsbHidBootInterface* match = scan->protocol == HID_BOOT_KEYBOARD ?
                                &found->keyboard : &found->mouse;
    if (!match->endpoint) *match = scan->in;
    return 1;
}

int usb_hid_find_boot_interfaces(const uint8_t* config, size_t bytes,
                                UsbHidBootInterfaces* interfaces) {
    if (!interfaces) return HW_INVALID;
    *interfaces = (UsbHidBootInterfaces){0};
    if (!config) return HW_INVALID;
    if (bytes < 9 || config[0] < 9 || config[1] != USB_DESC_CONFIGURATION || !config[5])
        return HW_PROTOCOL;
    size_t total = read_le16(config + 2);
    if (total > bytes || total < config[0]) return HW_PROTOCOL;

    UsbHidBootInterfaces found = {0};
    HidInterfaceScan scan = {0};
    uint8_t defaults[32] = {0}; /* One bit for each possible interface number. */
    uint32_t default_endpoints = 0;
    unsigned int interface_count = 0;
    found.configuration_value = config[5];
    for (size_t offset = config[0]; offset < total;) {
        const uint8_t* descriptor = config + offset;
        uint8_t length = descriptor[0];
        if (length < 2 || length > total - offset) return HW_PROTOCOL;
        switch (descriptor[1]) {
        case USB_DESC_CONFIGURATION:
            return HW_PROTOCOL;
        case USB_DESC_INTERFACE: {
            if (length < 9 || !finish_interface(&scan, &found)) return HW_PROTOCOL;
            scan = (HidInterfaceScan){0};
            uint8_t number = descriptor[2];
            scan.alternate = descriptor[3];
            scan.remaining_endpoints = descriptor[4];
            if (scan.alternate) break;
            uint8_t mask = (uint8_t)(1U << (number % 8U));
            if (defaults[number / 8U] & mask) return HW_PROTOCOL;
            defaults[number / 8U] |= mask;
            interface_count++;
            if (descriptor[5] == HID_CLASS && descriptor[6] == HID_SUBCLASS_BOOT &&
                (descriptor[7] == HID_BOOT_KEYBOARD || descriptor[7] == HID_BOOT_MOUSE)) {
                scan.protocol = descriptor[7];
                scan.in.interface_number = number;
            }
            break;
        }
        case HID_DESC_CLASS:
            if (!scan.protocol) break;
            if (scan.has_hid || !valid_hid_descriptor(descriptor, length)) return HW_PROTOCOL;
            scan.has_hid = 1;
            break;
        case USB_DESC_ENDPOINT:
            if (length < 7 || !scan.remaining_endpoints ||
                !scan_endpoint(&scan, descriptor, &default_endpoints)) return HW_PROTOCOL;
            scan.remaining_endpoints--;
            break;
        default:
            break;
        }
        offset += length;
    }
    if (!finish_interface(&scan, &found) || interface_count != config[4]) return HW_PROTOCOL;
    if (!found.keyboard.endpoint && !found.mouse.endpoint) return HW_UNSUPPORTED;
    *interfaces = found;
    return HW_OK;
}

static void class_request(uint8_t interface_number, uint8_t request, uint8_t value_high,
                          uint8_t bytes, uint8_t setup[USB_HID_SETUP_BYTES]) {
    setup[0] = 0x21; /* Host to device, class, interface. */
    setup[1] = request;
    setup[2] = 0;
    setup[3] = value_high;
    setup[4] = interface_number;
    setup[5] = 0;
    setup[6] = bytes;
    setup[7] = 0;
}

void usb_hid_set_boot_protocol(uint8_t interface_number, uint8_t setup[USB_HID_SETUP_BYTES]) {
    class_request(interface_number, HID_SET_PROTOCOL, 0, 0, setup);
}

void usb_hid_set_idle(uint8_t interface_number, uint8_t duration, uint8_t setup[USB_HID_SETUP_BYTES]) {
    class_request(interface_number, HID_SET_IDLE, duration, 0, setup);
}

void usb_hid_set_keyboard_leds(uint8_t interface_number, uint8_t setup[USB_HID_SETUP_BYTES]) {
    class_request(interface_number, HID_SET_REPORT, 2, 1, setup);
}

/* Keyboard/Keypad usages from USB HID Usage Tables, section 10. */
enum {
    KEYBOARD_A = 0x04, KEYBOARD_C = 0x06, KEYBOARD_L = 0x0f, KEYBOARD_Z = 0x1d,
    KEYBOARD_1 = 0x1e, KEYBOARD_0 = 0x27, KEYBOARD_ENTER = 0x28, KEYBOARD_ESCAPE = 0x29,
    KEYBOARD_BACKSPACE = 0x2a, KEYBOARD_SPACE = 0x2c, KEYBOARD_SLASH = 0x38,
    KEYBOARD_CAPS_LOCK = 0x39, KEYBOARD_SCROLL_LOCK = 0x47, KEYPAD_NUM_LOCK = 0x53,
    KEYPAD_DIVIDE = 0x54, KEYPAD_ADD = 0x57, KEYPAD_ENTER = 0x58, KEYPAD_1 = 0x59,
    KEYPAD_0 = 0x62, KEYPAD_DECIMAL = 0x63, KEYPAD_EQUAL = 0x67,
    MOD_CONTROL = 0x11, MOD_SHIFT = 0x22, MOD_ALT_GUI = 0xcc
};

static KeyEvent translate_key(uint8_t usage, uint8_t modifiers, uint8_t leds) {
    static const char digits[] = "1234567890";
    static const char shifted_digits[] = "!@#$%^&*()";
    static const char punctuation[] = " -=[]\\\0;'`,./";
    static const char shifted_punctuation[] = " _+{}|\0:\"~<>?";
    static const char keypad_operators[] = "/*-+";
    static const char keypad_numbers[] = "1234567890.";
    KeyEvent none = {KEY_NONE, 0};
    if (modifiers & MOD_ALT_GUI) return none;
    if (modifiers & MOD_CONTROL) {
        if (usage == KEYBOARD_C) return (KeyEvent){KEY_CANCEL, 0};
        if (usage == KEYBOARD_L) return (KeyEvent){KEY_CLEAR, 0};
        return none;
    }

    int shift = (modifiers & MOD_SHIFT) != 0;
    char character = 0;
    if (usage >= KEYBOARD_A && usage <= KEYBOARD_Z) {
        int upper = shift != ((leds & USB_HID_LED_CAPS_LOCK) != 0);
        character = (char)((upper ? 'A' : 'a') + usage - KEYBOARD_A);
    } else if (usage >= KEYBOARD_1 && usage <= KEYBOARD_0) {
        character = (shift ? shifted_digits : digits)[usage - KEYBOARD_1];
    } else if (usage >= KEYBOARD_SPACE && usage <= KEYBOARD_SLASH) {
        character = (shift ? shifted_punctuation : punctuation)[usage - KEYBOARD_SPACE];
    } else if (usage >= KEYPAD_DIVIDE && usage <= KEYPAD_ADD) {
        character = keypad_operators[usage - KEYPAD_DIVIDE];
    } else if (usage >= KEYPAD_1 && usage <= KEYPAD_DECIMAL && (leds & USB_HID_LED_NUM_LOCK)) {
        character = keypad_numbers[usage - KEYPAD_1];
    } else if (usage == KEYPAD_EQUAL) character = '=';
    if (character) return (KeyEvent){KEY_TEXT, (uint32_t)character};

    if (usage == KEYBOARD_ENTER || usage == KEYPAD_ENTER) return (KeyEvent){KEY_ENTER, 0};
    if (usage == KEYBOARD_BACKSPACE) return (KeyEvent){KEY_BACKSPACE, 0};
    if (usage == KEYBOARD_ESCAPE) return (KeyEvent){KEY_HOME, 0};
    return none;
}

static int has_key(const uint8_t* keys, unsigned int count, uint8_t usage) {
    for (unsigned int i = 0; i < count; i++) {
        if (keys[i] == usage) return 1;
    }
    return 0;
}

static int repeatable(KeyEvent event) {
    return event.type == KEY_TEXT || event.type == KEY_BACKSPACE;
}

void usb_hid_keyboard_reset(UsbHidKeyboard* keyboard) {
    *keyboard = (UsbHidKeyboard){0};
}

int usb_hid_keyboard_report(UsbHidKeyboard* keyboard, const uint8_t* report, size_t bytes,
                            uint32_t now_ms, KeyEvent events[USB_HID_KEY_EVENTS_MAX]) {
    if (!report || bytes != USB_HID_BOOT_KEYBOARD_BYTES) {
        keyboard->uncertain = 1;
        return HW_PROTOCOL;
    }

    /* All six slots are one snapshot. Error usages invalidate the entire set. */
    const uint8_t* keys = report + 2;
    for (unsigned int i = 0; i < USB_HID_KEY_EVENTS_MAX; i++) {
        if (keys[i] && keys[i] <= 3) {
            keyboard->uncertain = 1;
            return HW_PROTOCOL;
        }
    }

    uint8_t presses[USB_HID_KEY_EVENTS_MAX];
    unsigned int pressed = 0;
    uint8_t leds = keyboard->leds;
    for (unsigned int i = 0; i < USB_HID_KEY_EVENTS_MAX; i++) {
        uint8_t usage = keys[i];
        if (!usage || has_key(keyboard->keys, USB_HID_KEY_EVENTS_MAX, usage) ||
            has_key(presses, pressed, usage)) continue;
        presses[pressed++] = usage;
        if (usage == KEYBOARD_CAPS_LOCK) leds ^= USB_HID_LED_CAPS_LOCK;
        if (usage == KEYPAD_NUM_LOCK) leds ^= USB_HID_LED_NUM_LOCK;
        if (usage == KEYBOARD_SCROLL_LOCK) leds ^= USB_HID_LED_SCROLL_LOCK;
    }

    uint8_t modifiers = report[0];
    int restart = keyboard->uncertain || keyboard->modifiers != modifiers || keyboard->leds != leds;
    if (!has_key(keys, USB_HID_KEY_EVENTS_MAX, keyboard->repeat_usage)) keyboard->repeat_usage = 0;
    int count = 0;
    for (unsigned int i = 0; i < pressed; i++) {
        KeyEvent event = translate_key(presses[i], modifiers, leds);
        if (event.type != KEY_NONE) events[count++] = event;
        if (repeatable(event)) {
            keyboard->repeat_usage = presses[i];
            restart = 1;
        }
    }
    if (restart) {
        keyboard->repeat_started_ms = now_ms;
        keyboard->repeat_delay_ms = USB_HID_REPEAT_DELAY_MS;
    }
    memcpy(keyboard->keys, keys, sizeof(keyboard->keys));
    keyboard->modifiers = modifiers;
    keyboard->leds = leds;
    keyboard->uncertain = 0;
    return count;
}

int usb_hid_keyboard_repeat(UsbHidKeyboard* keyboard, uint32_t now_ms, KeyEvent* event) {
    if (keyboard->uncertain || !keyboard->repeat_usage ||
        now_ms - keyboard->repeat_started_ms < keyboard->repeat_delay_ms) return 0;
    KeyEvent next = translate_key(keyboard->repeat_usage, keyboard->modifiers, keyboard->leds);
    if (!repeatable(next)) return 0;
    *event = next;
    keyboard->repeat_started_ms = now_ms;
    keyboard->repeat_delay_ms = USB_HID_REPEAT_INTERVAL_MS;
    return 1;
}

static int clip_coordinate(int64_t position, int extent) {
    if (position < 0) return 0;
    if (position >= extent) return extent - 1;
    return (int)position;
}

void usb_hid_mouse_init(UsbHidMouse* mouse, int width, int height, int x, int y) {
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    *mouse = (UsbHidMouse){clip_coordinate(x, width), clip_coordinate(y, height), width, height, 0};
}

static int displacement(uint8_t byte) {
    return byte < 128 ? byte : (int)byte - 256;
}

int usb_hid_mouse_report(UsbHidMouse* mouse, const uint8_t* report, size_t bytes,
                         UsbHidMouseEvent* event) {
    if (!report || bytes < USB_HID_BOOT_MOUSE_MIN_BYTES || bytes > USB_HID_BOOT_MOUSE_MAX_BYTES)
        return HW_PROTOCOL;
    uint8_t buttons = report[0] & (USB_HID_MOUSE_LEFT | USB_HID_MOUSE_RIGHT | USB_HID_MOUSE_MIDDLE);
    uint8_t changed = buttons ^ mouse->buttons;
    int x = clip_coordinate((int64_t)mouse->x + displacement(report[1]), mouse->width);
    int y = clip_coordinate((int64_t)mouse->y + displacement(report[2]), mouse->height);
    uint8_t moved = x != mouse->x || y != mouse->y;
    if (!moved && !changed) return 0;
    mouse->x = x;
    mouse->y = y;
    mouse->buttons = buttons;
    *event = (UsbHidMouseEvent){x, y, buttons, changed, moved};
    return 1;
}

int usb_hid_mouse_disconnect(UsbHidMouse* mouse, UsbHidMouseEvent* event) {
    if (!mouse->buttons) return 0;
    *event = (UsbHidMouseEvent){mouse->x, mouse->y, 0, mouse->buttons, 0};
    mouse->buttons = 0;
    return 1;
}

int usb_hid_mouse_to_touch(const UsbHidMouseEvent* event, uint32_t id, TouchContact* contact) {
    TouchEventType type;
    int down = (event->buttons & USB_HID_MOUSE_LEFT) != 0;
    if (event->changed & USB_HID_MOUSE_LEFT) type = down ? TOUCH_EVENT_DOWN : TOUCH_EVENT_UP;
    else if (down && event->moved) type = TOUCH_EVENT_MOVE;
    else return 0;
    *contact = (TouchContact){{type, event->x, event->y}, id};
    return 1;
}
