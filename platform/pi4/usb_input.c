#include <vantage/platform/pi4/usb_input.h>
#include <vantage/drivers/usb_hid.h>

#define ROOT_PORTS_MAX 255U
#define DEVICE_COUNT 2U
#define PORT_SCAN_US 100000U
#define CONTROLLER_RETRY_US 1000000U
#define KEY_QUEUE_SIZE 32U
#define TOUCH_QUEUE_SIZE 16U
#define CONFIG_HEADER_BYTES 9U

enum { GET_DESCRIPTOR = 6, SET_CONFIGURATION = 9, DESCRIPTOR_CONFIGURATION = 2 };

typedef struct {
    unsigned int port;
    UsbHidBootInterfaces boot;
} InputDevice;

/* Serialized with the controller, input staging and framebuffer on core 0. */
static const HwOps* input_io;
static InputDevice devices[DEVICE_COUNT];
static InputDevice *keyboard_device, *mouse_device;
static UsbHidKeyboard keyboard;
static UsbHidMouse mouse;
static uint8_t ignored[ROOT_PORTS_MAX + 1];
static unsigned int scan_port;
static uint32_t last_us, last_scan, last_restart;
static uint64_t elapsed_us;
static int recovering;

static uint8_t config[PI4_USB_CONTROL_MAX];
static uint8_t report[PI4_USB_INTERRUPT_MAX];
static KeyEvent keys[KEY_QUEUE_SIZE];
static TouchContact touches[TOUCH_QUEUE_SIZE];
static unsigned int key_head, key_count, touch_head, touch_count;
static int touch_active;

static uint16_t le16(const uint8_t* bytes) {
    return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

static void push_key(const KeyEvent* event) {
    if (key_count == KEY_QUEUE_SIZE) return;
    keys[(key_head + key_count) % KEY_QUEUE_SIZE] = *event;
    key_count++;
}

static TouchContact* touch_tail(void) {
    return &touches[(touch_head + touch_count - 1) % TOUCH_QUEUE_SIZE];
}

static void push_touch(const UsbHidMouseEvent* event) {
    TouchContact contact;
    if (!usb_hid_mouse_to_touch(event, TOUCH_CONTACT_MOUSE, &contact)) return;

    /* Reserve room for UP before admitting DOWN. A rejected DOWN drops
     * the whole gesture, even if the queue drains before the next report. */
    if (contact.event.type == TOUCH_EVENT_DOWN) {
        if (touch_count >= TOUCH_QUEUE_SIZE - 1) return;
        touch_active = 1;
    } else {
        if (!touch_active) return;
        if (contact.event.type == TOUCH_EVENT_UP) touch_active = 0;
        else {
            if (touch_count && touch_tail()->event.type == TOUCH_EVENT_MOVE) {
                *touch_tail() = contact;
                return;
            }
            if (touch_count >= TOUCH_QUEUE_SIZE - 1) return;
        }
    }
    touches[(touch_head + touch_count) % TOUCH_QUEUE_SIZE] = contact;
    touch_count++;
}

uint32_t pi4_usb_key_read(KeyEvent* event) {
    if (!key_count) return 0;
    *event = keys[key_head];
    key_head = (key_head + 1) % KEY_QUEUE_SIZE;
    key_count--;
    return 1;
}

uint32_t pi4_usb_touch_read(TouchContact* contact) {
    if (!touch_count) return 0;
    *contact = touches[touch_head];
    touch_head = (touch_head + 1) % TOUCH_QUEUE_SIZE;
    touch_count--;
    return 1;
}

uint32_t pi4_usb_key_pending(void) { return key_count != 0; }
uint32_t pi4_usb_touch_pending(void) { return touch_count != 0; }

uint32_t pi4_usb_cursor(int* x, int* y) {
    if (!mouse_device) return 0;
    *x = mouse.x;
    *y = mouse.y;
    return 1;
}

static int control(unsigned int port, Pi4UsbSetup setup, void* data, size_t* actual) {
    size_t unused;
    return pi4_usb_control(port, &setup, data, actual ? actual : &unused);
}

static int hid_control(unsigned int port, const uint8_t bytes[USB_HID_SETUP_BYTES], void* data) {
    Pi4UsbSetup setup = {bytes[0], bytes[1], le16(bytes + 2), le16(bytes + 4), le16(bytes + 6)};
    return control(port, setup, data, NULL);
}

static int read_configuration(unsigned int port, UsbHidBootInterfaces* boot) {
    Pi4UsbSetup setup = {0x80, GET_DESCRIPTOR, DESCRIPTOR_CONFIGURATION << 8, 0, CONFIG_HEADER_BYTES};
    size_t actual;
    int result = control(port, setup, config, &actual);
    if (result != HW_OK) return result;
    if (actual != CONFIG_HEADER_BYTES || config[0] != CONFIG_HEADER_BYTES ||
        config[1] != DESCRIPTOR_CONFIGURATION) return HW_UNSUPPORTED;
    setup.length = le16(config + 2);
    if (setup.length < CONFIG_HEADER_BYTES || setup.length > sizeof(config)) return HW_UNSUPPORTED;

    result = control(port, setup, config, &actual);
    if (result != HW_OK) return result;
    if (actual != setup.length || le16(config + 2) != setup.length ||
        usb_hid_find_boot_interfaces(config, actual, boot) != HW_OK) return HW_UNSUPPORTED;
    return HW_OK;
}

static int usable(const UsbHidBootInterface* interface, unsigned int report_min) {
    return interface->endpoint && interface->transactions == 1 &&
           interface->max_packet_size >= report_min &&
           interface->max_packet_size <= sizeof(report);
}

static int configure_interrupt(unsigned int port, const UsbHidBootInterface* interface) {
    if (!interface->endpoint) return HW_OK;
    return pi4_usb_configure_interrupt_in(port, interface->endpoint,
                                          interface->max_packet_size, interface->interval);
}

static int setup_boot(unsigned int port, const UsbHidBootInterface* interface, int keyboard_idle) {
    if (!interface->endpoint) return HW_OK;
    uint8_t bytes[USB_HID_SETUP_BYTES];
    usb_hid_set_boot_protocol(interface->interface_number, bytes);
    int result = hid_control(port, bytes, NULL);
    if (result == PI4_USB_STALL) return HW_UNSUPPORTED;
    if (result != HW_OK || !keyboard_idle) return result;
    usb_hid_set_idle(interface->interface_number, 0, bytes);
    result = hid_control(port, bytes, NULL);
    return result == PI4_USB_STALL ? HW_OK : result;
}

static int open_device(unsigned int port, InputDevice* device) {
    int result = pi4_usb_open_port(port);
    if (result != HW_OK) return result;
    UsbHidBootInterfaces boot;
    result = read_configuration(port, &boot);
    if (result != HW_OK) return result;
    if (!usable(&boot.keyboard, USB_HID_BOOT_KEYBOARD_BYTES)) boot.keyboard.endpoint = 0;
    if (!usable(&boot.mouse, USB_HID_BOOT_MOUSE_MIN_BYTES)) boot.mouse.endpoint = 0;
    if ((keyboard_device || !boot.keyboard.endpoint) && (mouse_device || !boot.mouse.endpoint))
        return HW_UNSUPPORTED;

    Pi4UsbSetup setup = {0, SET_CONFIGURATION, boot.configuration_value, 0, 0};
    result = configure_interrupt(port, &boot.keyboard);
    if (result == HW_OK) result = configure_interrupt(port, &boot.mouse);
    if (result == HW_OK) result = control(port, setup, NULL, NULL);
    if (result == HW_OK) result = setup_boot(port, &boot.keyboard, 1);
    if (result == HW_OK) result = setup_boot(port, &boot.mouse, 0);
    if (result == HW_OK) *device = (InputDevice){port, boot};
    return result;
}

static void bind_devices(void) {
    for (unsigned int i = 0; i < DEVICE_COUNT; i++) {
        InputDevice* device = &devices[i];
        if (!device->port) continue;
        if (!keyboard_device && device->boot.keyboard.endpoint) keyboard_device = device;
        if (!mouse_device && device->boot.mouse.endpoint) mouse_device = device;
    }
}

static void disconnect_mouse(void) {
    UsbHidMouseEvent event;
    if (!usb_hid_mouse_disconnect(&mouse, &event) || !touch_active) return;
    /* Discard the unfinished gesture if its DOWN is still in our queue.
     * Otherwise release the contact outside every control's hit area. */
    if (touch_count && touch_tail()->event.type == TOUCH_EVENT_MOVE) touch_count--;
    if (touch_count && touch_tail()->event.type == TOUCH_EVENT_DOWN) {
        touch_count--;
        touch_active = 0;
        return;
    }
    event.x = event.y = -1;
    push_touch(&event);
}

static void forget_device(InputDevice* device) {
    if (device == keyboard_device) {
        keyboard_device = NULL;
        usb_hid_keyboard_reset(&keyboard);
        key_head = key_count = 0;
    }
    if (device == mouse_device) {
        disconnect_mouse();
        mouse_device = NULL;
    }
    device->port = 0;
}

static void close_device(InputDevice* device) {
    unsigned int port = device->port;
    forget_device(device);
    /* Previously skipped devices can now supply a missing input. */
    memset(ignored, 0, sizeof(ignored));
    (void)pi4_usb_close_port(port);
}

static void scan_devices(unsigned int ports) {
    InputDevice* vacant = NULL;
    for (unsigned int i = 0; i < DEVICE_COUNT; i++) {
        if (!devices[i].port) {
            vacant = &devices[i];
            break;
        }
    }
    if (!vacant) return;

    /* Attempt at most one enumeration per scan. Advance past a failed
     * device so repeated timeouts cannot starve other ports. */
    for (unsigned int i = 0; i < ports; i++) {
        scan_port = scan_port >= ports ? 1 : scan_port + 1;
        if ((devices[0].port == scan_port) || (devices[1].port == scan_port)) continue;
        int result = pi4_usb_port_connected(scan_port);
        if (result == HW_OFF) ignored[scan_port] = 0;
        if (result != HW_OK || ignored[scan_port]) continue;
        result = open_device(scan_port, vacant);
        if (result != HW_OK) {
            (void)pi4_usb_close_port(scan_port);
            if (result == HW_UNSUPPORTED) ignored[scan_port] = 1;
        }
        break;
    }
}

static int poll_keyboard(uint32_t now_ms) {
    size_t actual;
    int result = pi4_usb_interrupt_in(keyboard_device->port, keyboard_device->boot.keyboard.endpoint,
                                      report, sizeof(report), &actual);
    if (result != HW_OK && result != HW_BUSY) return result;
    if (result == HW_OK) {
        KeyEvent events[USB_HID_KEY_EVENTS_MAX];
        uint8_t leds = keyboard.leds;
        int count = usb_hid_keyboard_report(&keyboard, report, actual, now_ms, events);
        for (int i = 0; i < count; i++) push_key(&events[i]);
        if (keyboard.leds != leds) {
            uint8_t bytes[USB_HID_SETUP_BYTES];
            usb_hid_set_keyboard_leds(keyboard_device->boot.keyboard.interface_number, bytes);
            leds = keyboard.leds;
            result = hid_control(keyboard_device->port, bytes, &leds);
            if (result != HW_OK && result != PI4_USB_STALL) return result;
        }
    }
    KeyEvent repeat;
    if (usb_hid_keyboard_repeat(&keyboard, now_ms, &repeat)) push_key(&repeat);
    return HW_OK;
}

static int poll_mouse(void) {
    size_t actual;
    int result = pi4_usb_interrupt_in(mouse_device->port, mouse_device->boot.mouse.endpoint,
                                      report, sizeof(report), &actual);
    if (result != HW_OK && result != HW_BUSY) return result;
    UsbHidMouseEvent event;
    if (result == HW_OK && usb_hid_mouse_report(&mouse, report, actual, &event) == 1)
        push_touch(&event);
    return HW_OK;
}

static void recover_controller(uint32_t now) {
    if (!recovering) {
        for (unsigned int i = 0; i < DEVICE_COUNT; i++) forget_device(&devices[i]);
        memset(ignored, 0, sizeof(ignored));
        last_restart = now;
        recovering = 1;
    }
    if (now - last_restart < CONTROLLER_RETRY_US) return;
    last_restart = now;
    /* The transport rejects stopping a quarantined controller. */
    if (pi4_usb_stop() != HW_OK) {
        input_io = NULL;
        return;
    }
    Pi4UsbProbe probe;
    if (pi4_usb_start(input_io, &probe) == HW_OK) recovering = 0;
}

int pi4_usb_input_start(const HwOps* io, Pi4UsbProbe* probe, int width, int height) {
    int result = pi4_usb_start(io, probe);
    if (result != HW_OK) return result;
    input_io = io;
    last_us = io->time_us(io->context);
    last_scan = last_us - PORT_SCAN_US;
    usb_hid_mouse_init(&mouse, width, height, width / 2, height / 2);
    return HW_OK;
}

void pi4_usb_input_poll(void) {
    if (!input_io) return;
    uint32_t now = input_io->time_us(input_io->context);
    elapsed_us += now - last_us;
    last_us = now;
    unsigned int ports = pi4_usb_port_count();
    if (!ports) {
        recover_controller(now);
        return;
    }
    for (unsigned int i = 0; i < DEVICE_COUNT; i++) {
        InputDevice* device = &devices[i];
        if (device->port && pi4_usb_port_connected(device->port) != HW_OK) close_device(device);
    }
    if (keyboard_device && poll_keyboard((uint32_t)(elapsed_us / 1000U)) != HW_OK)
        close_device(keyboard_device);
    if (mouse_device && poll_mouse() != HW_OK) close_device(mouse_device);

    ports = pi4_usb_port_count();
    if (!ports) {
        recover_controller(now);
        return;
    }
    bind_devices();
    if ((!keyboard_device || !mouse_device) && now - last_scan >= PORT_SCAN_US) {
        last_scan = now;
        scan_devices(ports);
        bind_devices();
    }
}
