#pragma once
#include <vantage/drivers/hardware.h>

enum Pi4UsbStage {
    PI4_USB_BRIDGE = 1, PI4_USB_LINK, PI4_USB_ENDPOINT,
    PI4_USB_BAR, PI4_USB_XHCI, PI4_USB_READY,
    PI4_USB_DMA, PI4_USB_FIRMWARE, PI4_USB_RESET, PI4_USB_NO_OP, PI4_USB_RUNNING
};

typedef struct {
    enum Pi4UsbStage stage;
    uint32_t detail;
    uint32_t version;
    uint32_t ports;
} Pi4UsbProbe;

/* Core 0 at EL1 during SD boot. Resets the Pi 4 PCIe link and probes the
 * VL805 capability registers with bus mastering and interrupts disabled. */
int pi4_usb_probe(const HwOps* io, Pi4UsbProbe* probe);

#define PI4_USB_CONTROL_MAX 4096U
#define PI4_USB_INTERRUPT_MAX 1024U
#define PI4_USB_STALL (-8) /* EP0 stalled and was recovered. */

typedef struct {
    uint8_t request_type, request;
    uint16_t value, index, length;
} Pi4UsbSetup;

/* Single controller. All calls, including the probe, require the same serialized
 * core-0 owner with DAIF masked. Keep io alive through stop. Start runs the PCIe
 * probe, initializes DMA and verifies a No Op completion before returning OK. */
int pi4_usb_start(const HwOps* io, Pi4UsbProbe* probe);
int pi4_usb_stop(void);

/* Scan 1-based xHCI logical ports. connected returns HW_OK for an attached
 * USB 2 device, HW_OFF for an empty/detached port, and HW_UNSUPPORTED for a
 * SuperSpeed port. Count is zero when the controller is unavailable. */
unsigned int pi4_usb_port_count(void);
int pi4_usb_port_connected(unsigned int port);

/* Open resets and addresses a direct-root device and discovers EP0's packet
 * size. Close releases its slot, including any pending interrupt transfers.
 * On detach close the old slot before attempting to open the port again. */
int pi4_usb_open_port(unsigned int port);
int pi4_usb_close_port(unsigned int port);

/* EP0 data has setup->length bytes of caller storage (or NULL for length 0).
 * Direction comes from request_type. HW_OK sets actual, including short IN.
 * The transport owns SET_ADDRESS. Send SET_CONFIGURATION once, after adding
 * the endpoints below. After a control timeout reopen the port. PI4_USB_STALL
 * means the request failed and EP0 is ready for the next request. */
int pi4_usb_control(unsigned int port, const Pi4UsbSetup* setup, void* data,
                    size_t* actual);

/* Before SET_CONFIGURATION pass USB descriptor values, including the IN bit in
 * endpoint_address. Up to two interrupt IN endpoints per device are supported. */
int pi4_usb_configure_interrupt_in(unsigned int port, uint8_t endpoint_address,
                                  uint16_t max_packet, uint8_t interval);

/* After SET_CONFIGURATION call on each input tick with capacity >= max_packet.
 * HW_BUSY means a request is pending; HW_OK copies one report and sets actual.
 * Caller storage is only borrowed during the call. Reopen after endpoint error. */
int pi4_usb_interrupt_in(unsigned int port, uint8_t endpoint_address, void* report,
                        size_t capacity, size_t* actual);
