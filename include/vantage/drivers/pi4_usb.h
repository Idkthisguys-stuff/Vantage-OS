#pragma once
#include <vantage/drivers/hardware.h>

enum Pi4UsbStage {
    PI4_USB_BRIDGE = 1, PI4_USB_LINK, PI4_USB_ENDPOINT,
    PI4_USB_FIRMWARE, PI4_USB_BAR, PI4_USB_XHCI, PI4_USB_READY
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
