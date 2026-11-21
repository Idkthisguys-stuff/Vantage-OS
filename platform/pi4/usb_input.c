#include <vantage/platform/pi4/usb_input.h>
#include <vantage/drivers/usb_hid.h>

#define ROOT_PORTS_MAX 255U
#define DEVICE_COUNT 2U
#define PORT_SCAN_US 100000U
#define CONTROLLER_RETRY_US 1000000U
#define KEY_QUEUE_SIZE 32U
#define TOUCH_QUEUE_SIZE 16U
#define CONFIG_HEADER_BYTES 9U
#define HUB_BITMAP_BYTES ((PI4_USB_HUB_MAX_PORTS + 1U + 7U) / 8U)
#define HUB_DESCRIPTOR_MAX (7U + 2U * HUB_BITMAP_BYTES)
#define HUB_DEBOUNCE_US 100000U
#define HUB_RESET_TIMEOUT_US 250000U
#define HUB_RESET_RECOVERY_US 10000U

enum {
    GET_STATUS = 0, CLEAR_FEATURE = 1, SET_FEATURE = 3, GET_DESCRIPTOR = 6, SET_CONFIGURATION = 9,
    DESCRIPTOR_DEVICE = 1, DESCRIPTOR_CONFIGURATION = 2, DESCRIPTOR_INTERFACE = 4,
    DESCRIPTOR_ENDPOINT = 5, DESCRIPTOR_HUB = 0x29, USB_CLASS_HUB = 9
};
enum {
    PORT_CONNECTION = 0, PORT_ENABLE = 1, PORT_OVERCURRENT = 3, PORT_RESET = 4,
    PORT_POWER = 8, PORT_LOW_SPEED = 9, PORT_HIGH_SPEED = 10, C_PORT_CONNECTION = 16
};
enum {
    HUB_IDLE, HUB_DEBOUNCE, HUB_RESETTING, HUB_RECOVERY, HUB_ACTIVE, HUB_IGNORED, HUB_RESET_FAILED
};

typedef struct {
    unsigned int port;
    UsbHidBootInterfaces boot;
} InputDevice;

typedef struct {
    uint16_t packet;
    uint8_t configuration, endpoint, interval;
} HubEndpoint;

typedef struct {
    uint8_t ports, think_time, power_delay, switched_power;
} HubDescriptor;

typedef struct {
    uint32_t since;
    uint8_t state;
} HubPort;

typedef struct {
    unsigned int root, ports, endpoint, reset_port, next_port;
    uint32_t powered_at, power_delay_us, pending;
    int switched_power;
    HubPort downstream[PI4_USB_HUB_MAX_PORTS + 1];
} InputHub;

/* Serialized with the controller, input staging and framebuffer on core 0. */
static const HwOps* input_io;
static InputDevice devices[DEVICE_COUNT];
static InputDevice *keyboard_device, *mouse_device;
static InputHub hub;
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

static int read_configuration(unsigned int port, size_t* bytes) {
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
    if (actual != setup.length || config[0] != CONFIG_HEADER_BYTES ||
        config[1] != DESCRIPTOR_CONFIGURATION || le16(config + 2) != setup.length) return HW_UNSUPPORTED;
    *bytes = actual;
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

static int configure_boot(unsigned int port, size_t bytes, InputDevice* device) {
    UsbHidBootInterfaces boot;
    if (usb_hid_find_boot_interfaces(config, bytes, &boot) != HW_OK) return HW_UNSUPPORTED;
    if (!usable(&boot.keyboard, USB_HID_BOOT_KEYBOARD_BYTES)) boot.keyboard.endpoint = 0;
    if (!usable(&boot.mouse, USB_HID_BOOT_MOUSE_MIN_BYTES)) boot.mouse.endpoint = 0;
    if ((keyboard_device || !boot.keyboard.endpoint) && (mouse_device || !boot.mouse.endpoint))
        return HW_UNSUPPORTED;

    Pi4UsbSetup setup = {0, SET_CONFIGURATION, boot.configuration_value, 0, 0};
    int result = configure_interrupt(port, &boot.keyboard);
    if (result == HW_OK) result = configure_interrupt(port, &boot.mouse);
    if (result == HW_OK) result = control(port, setup, NULL, NULL);
    if (result == HW_OK) result = setup_boot(port, &boot.keyboard, 1);
    if (result == HW_OK) result = setup_boot(port, &boot.mouse, 0);
    if (result == HW_OK) *device = (InputDevice){port, boot};
    return result;
}

/* USB 2.0 §11.23: high-speed hubs use the single TT at alternate setting zero. */
static HubEndpoint find_hub_endpoint(size_t bytes) {
    HubEndpoint endpoint = {0};
    int hub_interface = 0, default_interface_seen = 0;
    if (config[4] != 1 || !config[5]) return endpoint;
    for (size_t offset = CONFIG_HEADER_BYTES; offset < bytes;) {
        const uint8_t* descriptor = config + offset;
        unsigned int length = descriptor[0];
        if (length < 2 || length > bytes - offset) return (HubEndpoint){0};
        if (descriptor[1] == DESCRIPTOR_INTERFACE) {
            if (length < 9) return (HubEndpoint){0};
            if (!descriptor[3] && default_interface_seen++) return (HubEndpoint){0};
            hub_interface = !descriptor[3] && descriptor[4] == 1 && descriptor[5] == USB_CLASS_HUB &&
                            !descriptor[6] && descriptor[7] <= 1;
        } else if (hub_interface && descriptor[1] == DESCRIPTOR_ENDPOINT) {
            if (length < 7 || endpoint.endpoint) return (HubEndpoint){0};
            uint8_t address = descriptor[2];
            uint16_t packet = le16(descriptor + 4);
            if ((address & 0xf0U) != 0x80U || !(address & 15U) || descriptor[3] != 3 ||
                !packet || packet > sizeof(report) || !descriptor[6] || descriptor[6] > 16)
                return (HubEndpoint){0};
            endpoint = (HubEndpoint){packet, config[5], address, descriptor[6]};
        }
        offset += length;
    }
    return endpoint;
}

static int read_hub_descriptor(unsigned int root, HubEndpoint endpoint, HubDescriptor* hub_desc) {
    uint8_t data[HUB_DESCRIPTOR_MAX];
    size_t actual;
    Pi4UsbSetup setup = {0xa0, GET_DESCRIPTOR, DESCRIPTOR_HUB << 8, 0, sizeof(data)};
    int result = control(root, setup, data, &actual);
    if (result != HW_OK) return result;
    if (actual < 7 || actual != data[0] || data[1] != DESCRIPTOR_HUB ||
        !data[2] || data[2] > PI4_USB_HUB_MAX_PORTS) return HW_UNSUPPORTED;
    unsigned int bitmap = (data[2] + 8U) / 8U;
    if (actual != 7U + 2U * bitmap || endpoint.packet < bitmap) return HW_UNSUPPORTED;
    uint16_t characteristics = le16(data + 3);
    *hub_desc = (HubDescriptor){data[2], (uint8_t)((characteristics >> 5) & 3U),
                              data[5], (characteristics & 3U) < 2U};
    return HW_OK;
}

static int configure_hub_endpoint(unsigned int root, HubEndpoint endpoint) {
    int result = pi4_usb_configure_interrupt_in(root, endpoint.endpoint, endpoint.packet, endpoint.interval);
    if (result != HW_OK) return result;
    Pi4UsbSetup setup = {0, SET_CONFIGURATION, endpoint.configuration, 0, 0};
    return control(root, setup, NULL, NULL);
}

static int hub_feature(unsigned int root, unsigned int port, uint8_t request, unsigned int feature) {
    Pi4UsbSetup setup = {port ? 0x23 : 0x20, request, (uint16_t)feature, (uint16_t)port, 0};
    return control(root, setup, NULL, NULL);
}

static int hub_status(unsigned int root, unsigned int port, unsigned int* status, unsigned int* changes) {
    uint8_t data[4];
    size_t actual;
    Pi4UsbSetup setup = {port ? 0xa3 : 0xa0, GET_STATUS, 0, (uint16_t)port, sizeof(data)};
    int result = control(root, setup, data, &actual);
    if (result != HW_OK) return result;
    if (actual != sizeof(data)) return HW_PROTOCOL;
    *status = le16(data);
    *changes = le16(data + 2);
    for (unsigned int bit = 0; bit < (port ? 5U : 2U); bit++) {
        if (!(*changes & (1U << bit))) continue;
        result = hub_feature(root, port, CLEAR_FEATURE, port ? C_PORT_CONNECTION + bit : bit);
        if (result != HW_OK) return result;
    }
    return HW_OK;
}

static int check_hub_status(unsigned int root) {
    unsigned int status, changes;
    int result = hub_status(root, 0, &status, &changes);
    if (result == HW_OK && (status & 3U)) return HW_IO;
    return result;
}

static int open_hub(unsigned int root, HubEndpoint endpoint) {
    uint8_t device[18];
    size_t actual;
    Pi4UsbSetup setup = {0x80, GET_DESCRIPTOR, DESCRIPTOR_DEVICE << 8, 0, sizeof(device)};
    int result = control(root, setup, device, &actual);
    if (result != HW_OK) return result;
    if (actual != sizeof(device) || device[0] != sizeof(device) || device[1] != DESCRIPTOR_DEVICE ||
        device[4] != USB_CLASS_HUB || device[5] || (device[6] != 1 && device[6] != 2) ||
        device[7] != 64 || !device[17]) return HW_UNSUPPORTED;

    HubDescriptor descriptor;
    result = read_hub_descriptor(root, endpoint, &descriptor);
    if (result == PI4_USB_STALL || result == HW_UNSUPPORTED) {
        /* USB 2.0 leaves GET_DESCRIPTOR(HUB) undefined in Addressed state.
         * Get the descriptor from a configured hub, then reopen the slot so
         * its first Configure Endpoint can install the Hub and TT fields. */
        result = configure_hub_endpoint(root, endpoint);
        if (result == HW_OK) result = read_hub_descriptor(root, endpoint, &descriptor);
        if (result == HW_OK) result = pi4_usb_close_port(root);
        if (result == HW_OK) result = pi4_usb_open_port(root);
    }
    if (result == HW_OK) result = pi4_usb_configure_hub(root, descriptor.ports, descriptor.think_time);
    if (result == HW_OK) result = configure_hub_endpoint(root, endpoint);
    if (result == HW_OK) result = check_hub_status(root);
    if (result != HW_OK) return result;

    if (descriptor.switched_power) {
        for (unsigned int port = 1; port <= descriptor.ports; port++) {
            result = hub_feature(root, port, SET_FEATURE, PORT_POWER);
            if (result != HW_OK) return result;
        }
    }
    hub = (InputHub){
        .root = root, .ports = descriptor.ports, .endpoint = endpoint.endpoint,
        .powered_at = input_io->time_us(input_io->context),
        .power_delay_us = descriptor.power_delay * 2000U, .switched_power = descriptor.switched_power,
        .pending = (1U << (descriptor.ports + 1)) - 2U
    };
    return HW_OK;
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

static InputDevice* device_on_port(unsigned int port) {
    for (unsigned int i = 0; i < DEVICE_COUNT; i++)
        if (devices[i].port == port) return &devices[i];
    return NULL;
}

static void set_hub_port_state(unsigned int port, uint8_t state) {
    hub.downstream[port] = (HubPort){input_io->time_us(input_io->context), state};
}

static void close_device(InputDevice* device) {
    unsigned int port = device->port;
    forget_device(device);
    /* Previously skipped devices can now supply a missing input. */
    memset(ignored, 0, sizeof(ignored));
    for (unsigned int child = 1; child <= hub.ports; child++)
        if (hub.downstream[child].state == HUB_IGNORED) set_hub_port_state(child, HUB_DEBOUNCE);
    if (port >> 8) set_hub_port_state(port >> 8, HUB_DEBOUNCE);
    (void)pi4_usb_close_port(port);
}

static void close_hub(void) {
    for (unsigned int i = 0; i < DEVICE_COUNT; i++)
        if (devices[i].port >> 8) close_device(&devices[i]);
    (void)pi4_usb_close_port(hub.root);
    hub = (InputHub){0};
}

static int disable_hub_port(unsigned int port, uint8_t state) {
    int result = hub_feature(hub.root, port, CLEAR_FEATURE, PORT_ENABLE);
    hub.reset_port = 0;
    set_hub_port_state(port, state);
    return result;
}

static int attach_child(unsigned int port, unsigned int status, InputDevice* vacant) {
    unsigned int path = hub.root | (port << 8);
    unsigned int speed = (status & (1U << PORT_HIGH_SPEED)) ? 3
                       : (status & (1U << PORT_LOW_SPEED)) ? 2 : 1;
    int result = vacant ? pi4_usb_open_child(hub.root, port, speed) : HW_UNSUPPORTED;
    size_t bytes;
    if (result == HW_OK) result = read_configuration(path, &bytes);
    if (result == HW_OK) result = configure_boot(path, bytes, vacant);
    if (result == HW_OK) {
        hub.reset_port = 0;
        set_hub_port_state(port, HUB_ACTIVE);
        return HW_OK;
    }

    if (vacant) (void)pi4_usb_close_port(path);
    /* Failed enumeration may leave a device at the default address. Disable
     * this downstream link before allowing another port reset. */
    return disable_hub_port(port, result == HW_UNSUPPORTED ? HUB_IGNORED : HUB_DEBOUNCE);
}

static int poll_hub_port(unsigned int port, InputDevice* vacant) {
    unsigned int status, changes;
    int result = hub_status(hub.root, port, &status, &changes);
    if (result != HW_OK) return result;
    HubPort* child = &hub.downstream[port];
    int connected = (status & (1U << PORT_CONNECTION)) && !(status & (1U << PORT_OVERCURRENT)) &&
                    (!hub.switched_power || (status & (1U << PORT_POWER)));
    int enabled = (status & (1U << PORT_ENABLE)) != 0;
    if (!connected || (changes & (1U << PORT_CONNECTION)) ||
        (child->state == HUB_ACTIVE && (!enabled || (changes & (1U << PORT_RESET))))) {
        InputDevice* device = device_on_port(hub.root | (port << 8));
        if (device) close_device(device);
        if (hub.reset_port == port) hub.reset_port = 0;
        set_hub_port_state(port, connected ? HUB_DEBOUNCE : HUB_IDLE);
        return HW_OK;
    }

    uint32_t waited = input_io->time_us(input_io->context) - child->since;
    switch (child->state) {
        case HUB_IDLE:
            set_hub_port_state(port, HUB_DEBOUNCE);
            break;
        case HUB_DEBOUNCE:
            if (!vacant || hub.reset_port || waited < HUB_DEBOUNCE_US) break;
            result = hub_feature(hub.root, port, SET_FEATURE, PORT_RESET);
            if (result != HW_OK) return result;
            hub.reset_port = port;
            set_hub_port_state(port, HUB_RESETTING);
            break;
        case HUB_RESETTING:
            if (enabled && !(status & (1U << PORT_RESET)) && (changes & (1U << PORT_RESET)))
                set_hub_port_state(port, HUB_RECOVERY);
            else if (waited >= HUB_RESET_TIMEOUT_US) return disable_hub_port(port, HUB_RESET_FAILED);
            break;
        case HUB_RECOVERY:
            if (waited < HUB_RESET_RECOVERY_US) break;
            if (!enabled || (status & (1U << PORT_RESET))) return HW_IO;
            if ((status & (3U << PORT_LOW_SPEED)) == (3U << PORT_LOW_SPEED)) return HW_PROTOCOL;
            return attach_child(port, status, vacant);
    }
    return HW_OK;
}

static int poll_hub(uint32_t now) {
    size_t actual;
    int result = pi4_usb_interrupt_in(hub.root, (uint8_t)hub.endpoint, report, sizeof(report), &actual);
    if (result != HW_OK && result != HW_BUSY) return result;
    if (result == HW_OK) {
        unsigned int bytes = (hub.ports + 8U) / 8U;
        if (actual < bytes) return HW_PROTOCOL;
        for (unsigned int i = 0; i < bytes; i++) hub.pending |= (uint32_t)report[i] << (i * 8);
    }
    if (hub.pending & 1U) {
        hub.pending &= ~1U;
        result = check_hub_status(hub.root);
        if (result != HW_OK) return result;
    }
    if (now - hub.powered_at < hub.power_delay_us) return HW_OK;
    InputDevice* vacant = (!keyboard_device || !mouse_device) ? device_on_port(0) : NULL;
    for (unsigned int i = 0; i < hub.ports; i++) {
        hub.next_port = hub.next_port == hub.ports ? 1 : hub.next_port + 1;
        unsigned int port = hub.next_port;
        const HubPort* child = &hub.downstream[port];
        uint32_t waited = now - child->since;
        if (!(hub.pending & (1U << port)) && child->state != HUB_RESETTING &&
            !(child->state == HUB_RECOVERY && waited >= HUB_RESET_RECOVERY_US) &&
            !(child->state == HUB_DEBOUNCE && vacant && !hub.reset_port && waited >= HUB_DEBOUNCE_US)) continue;
        hub.pending &= ~(1U << port);
        return poll_hub_port(port, vacant);
    }
    return HW_OK;
}

static void scan_devices(unsigned int ports) {
    InputDevice* vacant = device_on_port(0);
    if (!vacant) return;

    /* Attempt at most one enumeration per scan. Advance past a failed
     * device so repeated timeouts cannot starve other ports. */
    for (unsigned int i = 0; i < ports; i++) {
        scan_port = scan_port >= ports ? 1 : scan_port + 1;
        if (hub.root == scan_port || device_on_port(scan_port)) continue;
        int result = pi4_usb_port_connected(scan_port);
        if (result == HW_OFF) ignored[scan_port] = 0;
        if (result != HW_OK || ignored[scan_port]) continue;
        result = pi4_usb_open_port(scan_port);
        size_t bytes;
        if (result == HW_OK) result = read_configuration(scan_port, &bytes);
        if (result == HW_OK) {
            HubEndpoint endpoint = find_hub_endpoint(bytes);
            if (!endpoint.endpoint) result = configure_boot(scan_port, bytes, vacant);
            else if (hub.root) result = HW_UNSUPPORTED;
            else result = open_hub(scan_port, endpoint);
        }
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
        hub = (InputHub){0};
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
    if (hub.root && (pi4_usb_port_connected(hub.root) != HW_OK || poll_hub(now) != HW_OK))
        close_hub();
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
    if (!hub.reset_port && (!keyboard_device || !mouse_device) && now - last_scan >= PORT_SCAN_US) {
        last_scan = now;
        scan_devices(ports);
        bind_devices();
    }
}
