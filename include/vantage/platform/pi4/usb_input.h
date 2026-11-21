#pragma once

#include <vantage/abi/key.h>
#include <vantage/abi/touch.h>
#include <vantage/drivers/pi4_usb.h>

/* USB=1, core 0 with DAIF masked. Coordinates use the logical canvas. */
int pi4_usb_input_start(const HwOps* io, Pi4UsbProbe* probe, int width, int height);
void pi4_usb_input_poll(void);

/* Pop one queued event, or return 0 without writing the output. */
uint32_t pi4_usb_key_read(KeyEvent* event);
uint32_t pi4_usb_touch_read(TouchContact* contact);
/* Input readiness without staging an event from a detachable device. */
uint32_t pi4_usb_key_pending(void);
uint32_t pi4_usb_touch_pending(void);
/* Return 1 with the current cursor position while a mouse is bound. */
uint32_t pi4_usb_cursor(int* x, int* y);
