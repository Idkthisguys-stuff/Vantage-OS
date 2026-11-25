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
#define PI4_USB_BULK_MAX 16384U
#define PI4_USB_HUB_MAX_PORTS 15U
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

/* Scan 1-based xHCI logical root ports. For an opened child use the path
 * root | (downstream << 8), with a 1-based downstream port. This checks its
 * slot and the root link. Hub class code monitors downstream connection changes
 * and closes the affected slots. Count is zero when the controller is offline. */
unsigned int pi4_usb_port_count(void);
int pi4_usb_port_connected(unsigned int path);

/* Open resets and addresses a direct-root device and discovers EP0's packet
 * size. Close accepts root and child paths and retires any children before
 * their parent, including pending transfers. Close an old slot before reopening. */
int pi4_usb_open_port(unsigned int port);
int pi4_usb_close_port(unsigned int path);

/* Stage a root-attached high-speed hub's single-TT fields before its first
 * Configure Endpoint command. think_time is the raw 0..3 hub descriptor field.
 * Leave the hub in its default single-TT alternate setting. */
int pi4_usb_configure_hub(unsigned int root, unsigned int num_ports, unsigned int think_time);

/* Address a child after hub class code has debounced and reset its port and
 * waited for reset recovery. Speed values are 1 (full), 2 (low) and 3 (high).
 * The parent must have completed USB configuration and hub slot setup. */
int pi4_usb_open_child(unsigned int root, unsigned int downstream, unsigned int speed);

/* EP0 data has setup->length bytes of caller storage (or NULL for length 0).
 * Direction comes from request_type. HW_OK sets actual, including short IN.
 * The transport owns SET_ADDRESS. Send SET_CONFIGURATION once, after adding
 * the endpoints below. After a control timeout reopen the port. PI4_USB_STALL
 * means the request failed and EP0 is ready for the next request. */
int pi4_usb_control(unsigned int path, const Pi4UsbSetup* setup, void* data,
                    size_t* actual);

/* Before SET_CONFIGURATION pass USB descriptor values, including the IN bit in
 * endpoint_address. Up to two interrupt IN endpoints per device are supported. */
int pi4_usb_configure_interrupt_in(unsigned int path, uint8_t endpoint_address,
                                  uint16_t max_packet, uint8_t interval);

/* After SET_CONFIGURATION call on each input tick with capacity >= max_packet.
 * HW_BUSY means a request is pending; HW_OK copies one report and sets actual.
 * Caller storage is only borrowed during the call. Reopen after endpoint error. */
int pi4_usb_interrupt_in(unsigned int path, uint8_t endpoint_address, void* report,
                        size_t capacity, size_t* actual);

/* USB 2 bulk endpoint configured before SET_CONFIGURATION. A synchronous
 * transfer returns a byte count or HwResult. HW_BUSY indicates STALL. */
int pi4_usb_configure_bulk(unsigned int path, uint8_t endpoint_address, uint16_t max_packet);
int pi4_usb_bulk(unsigned int path, uint8_t endpoint_address, void* data, uint32_t bytes);
int pi4_usb_clear_bulk_halt(unsigned int path, uint8_t endpoint_address);
