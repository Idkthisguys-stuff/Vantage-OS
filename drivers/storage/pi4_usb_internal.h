#pragma once
#include <vantage/drivers/pi4_usb.h>

typedef struct {
    uint32_t bar_bytes;
    uint64_t dma_base;
} Pi4UsbPcie;

/* Only the VL805 transport owns the inbound aperture. */
int pi4_usb_pcie_open(const HwOps* io, Pi4UsbProbe* probe, Pi4UsbPcie* pcie);
int pi4_usb_pcie_enable(const HwOps* io);
int pi4_usb_pcie_link(const HwOps* io);
int pi4_usb_pcie_caps(const HwOps* io, Pi4UsbProbe* probe);
void pi4_usb_pcie_close(const HwOps* io, int release_claim);
