#pragma once

#include <vantage/abi/key.h>
#include <vantage/abi/touch.h>
#include <vantage/drivers/hardware.h>

#define USB_HID_SETUP_BYTES 8U
#define USB_HID_BOOT_KEYBOARD_BYTES 8U
#define USB_HID_BOOT_MOUSE_MIN_BYTES 3U
#define USB_HID_BOOT_MOUSE_MAX_BYTES 8U
#define USB_HID_KEY_EVENTS_MAX 6U
#define USB_HID_REPEAT_DELAY_MS 500U
#define USB_HID_REPEAT_INTERVAL_MS 50U

typedef struct {
    uint8_t interface_number, endpoint, interval, transactions;
    uint16_t max_packet_size;
} UsbHidBootInterface;

typedef struct {
    uint8_t configuration_value;
    UsbHidBootInterface keyboard, mouse;
} UsbHidBootInterfaces;

/* Scan one complete configuration, selecting the first boot interface of each
 * kind in alternate setting zero. An absent kind has endpoint zero. Interval
 * is the raw bInterval and max_packet_size is the payload per transaction. The
 * host interprets these fields at the negotiated bus speed.
 * Returns HW_OK with at least one match, HW_UNSUPPORTED with none, HW_PROTOCOL
 * for malformed descriptors, or HW_INVALID for a null argument. The output is
 * cleared on failure. */
int usb_hid_find_boot_interfaces(const uint8_t* config, size_t bytes,
                                UsbHidBootInterfaces* interfaces);

/* Send SET_PROTOCOL after SET_CONFIGURATION and before polling. Both requests
 * have no data stage. SET_IDLE duration is in units of 4 ms, with zero selecting
 * change-only reports. SET_IDLE is optional for a boot mouse. */
void usb_hid_set_boot_protocol(uint8_t interface_number, uint8_t setup[USB_HID_SETUP_BYTES]);
void usb_hid_set_idle(uint8_t interface_number, uint8_t duration, uint8_t setup[USB_HID_SETUP_BYTES]);

enum {
    USB_HID_LED_NUM_LOCK = 1, USB_HID_LED_CAPS_LOCK = 2, USB_HID_LED_SCROLL_LOCK = 4
};

/* Use the returned setup bytes with the one-byte UsbHidKeyboard.leds payload. */
void usb_hid_set_keyboard_leds(uint8_t interface_number, uint8_t setup[USB_HID_SETUP_BYTES]);

typedef struct {
    uint8_t leds;
    uint8_t keys[USB_HID_KEY_EVENTS_MAX], modifiers, repeat_usage, uncertain;
    uint16_t repeat_delay_ms;
    uint32_t repeat_started_ms;
} UsbHidKeyboard;

/* Initialize or discard held keys, repeat and lock state on disconnect. */
void usb_hid_keyboard_reset(UsbHidKeyboard* keyboard);

/* Return the number of new presses translated into KeyEvents. Releases update
 * held state only. Use the actual transfer length. HW_PROTOCOL indicates a bad
 * length or an error usage. The last valid held set is retained and repeat is
 * suspended until a valid report arrives. */
int usb_hid_keyboard_report(UsbHidKeyboard* keyboard, const uint8_t* report, size_t bytes,
                            uint32_t now_ms, KeyEvent events[USB_HID_KEY_EVENTS_MAX]);

/* Call after processing any available reports. The last newly pressed printable
 * key or Backspace repeats while held. Timing uses a wrapping monotonic clock.
 * Repeat stops on release and restarts its delay on a modifier or lock change.
 * Each call emits at most one event, even if polling fell behind. */
int usb_hid_keyboard_repeat(UsbHidKeyboard* keyboard, uint32_t now_ms, KeyEvent* event);

enum {
    USB_HID_MOUSE_LEFT = 1, USB_HID_MOUSE_RIGHT = 2, USB_HID_MOUSE_MIDDLE = 4
};

typedef struct {
    int x, y, width, height;
    uint8_t buttons;
} UsbHidMouse;

typedef struct {
    int x, y;
    uint8_t buttons, changed, moved;
} UsbHidMouseEvent;

/* Clip the initial position and subsequent motion to the supplied extents.
 * An extent smaller than one is treated as one pixel. */
void usb_hid_mouse_init(UsbHidMouse* mouse, int width, int height, int x, int y);

/* Return 1 for movement or a button change, 0 for no change, or HW_PROTOCOL
 * for a bad length. Bytes after the three-byte boot prefix are ignored. */
int usb_hid_mouse_report(UsbHidMouse* mouse, const uint8_t* report, size_t bytes,
                         UsbHidMouseEvent* event);

/* Release all held buttons at the last position. Return 1 if any were held. */
int usb_hid_mouse_disconnect(UsbHidMouse* mouse, UsbHidMouseEvent* event);

/* Left-button transitions take precedence over motion. Motion generates MOVE
 * while left is held. Use the same caller-assigned ID from DOWN through UP. */
int usb_hid_mouse_to_touch(const UsbHidMouseEvent* event, uint32_t id, TouchContact* contact);
