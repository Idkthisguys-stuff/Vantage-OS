#include <vantage/arch/mmu.h>
#include <vantage/drivers/pi4_usb.h>

/* BCM2711 PCIe register layout and reset timings from the bcm2711 device
 * tree and Broadcom's Linux pcie-brcmstb driver (docs/USB.md). */
#define PCIE_BASE 0xfd500000ULL
#define PCIE_SW_INIT 0x9210
#define PCIE_BRIDGE_RESET (1U << 1)
#define PCIE_PERST 1U
#define PCIE_HARD_DEBUG 0x4204
#define PCIE_SERDES_IDDQ (1U << 27)
#define PCIE_CONTROL 0x4008
#define PCIE_BURST_MASK (3U << 20)
#define PCIE_SCB_ACCESS (1U << 12)
#define PCIE_CFG_READ_UR (1U << 13)
#define PCIE_STATUS 0x4068
#define PCIE_ROOT_PORT (1U << 7)
#define PCIE_LINK_UP 0x30U
#define PCIE_INBOUND_BAR1 0x402c
#define PCIE_INBOUND_BAR2 0x4034
#define PCIE_INBOUND_BAR3 0x403c
#define PCIE_BAR_SIZE_MASK 0x1fU
#define PCIE_OUTBOUND_LO 0x400c
#define PCIE_OUTBOUND_HI 0x4010
#define PCIE_OUTBOUND_BASE_LIMIT 0x4070
#define PCIE_OUTBOUND_BASE_HI 0x4080
#define PCIE_OUTBOUND_LIMIT_HI 0x4084
#define PCIE_EXT_INDEX 0x9000
#define PCIE_EXT_DATA 0x8000
#define VL805_BDF (1U << 20)
#define VL805_ID 0x34831106U
#define VL805_FIRMWARE_TAG 0x00030058U
#define PCI_XHCI_CLASS 0x0c0330U
#define PCI_COMMAND 0x04
#define PCI_MEMORY 2U
#define PCI_MASTER 4U
#define PCI_INTX_DISABLE (1U << 10)
#define PCI_BAR0 0x10
#define PCI_BAR1 0x14
#define PCI_MEMORY_BASE 0xf8000000U

static uint32_t read_reg(const HwOps* io, unsigned int offset) {
    return io->read(io->context, PCIE_BASE + offset);
}

static void write_reg(const HwOps* io, unsigned int offset, uint32_t value) {
    io->write(io->context, PCIE_BASE + offset, value);
}

static void update_reg(const HwOps* io, unsigned int offset, uint32_t mask, uint32_t value) {
    write_reg(io, offset, (read_reg(io, offset) & ~mask) | value);
    (void)read_reg(io, offset);
}

static int start_link(const HwOps* io, Pi4UsbProbe* probe) {
    update_reg(io, PCIE_SW_INIT, PCIE_BRIDGE_RESET | PCIE_PERST,
               PCIE_BRIDGE_RESET | PCIE_PERST);
    if (hw_delay(io, 100) != HW_OK) return HW_TIMEOUT;
    update_reg(io, PCIE_SW_INIT, PCIE_BRIDGE_RESET, 0);
    update_reg(io, PCIE_HARD_DEBUG, PCIE_SERDES_IDDQ, 0);
    if (hw_delay(io, 100) != HW_OK) return HW_TIMEOUT;

    /* Select 128-byte bursts and disable the inbound windows for this probe. */
    update_reg(io, PCIE_CONTROL, PCIE_BURST_MASK, PCIE_SCB_ACCESS | PCIE_CFG_READ_UR);
    update_reg(io, PCIE_INBOUND_BAR1, PCIE_BAR_SIZE_MASK, 0);
    update_reg(io, PCIE_INBOUND_BAR2, PCIE_BAR_SIZE_MASK, 0);
    update_reg(io, PCIE_INBOUND_BAR3, PCIE_BAR_SIZE_MASK, 0);
    probe->detail = read_reg(io, PCIE_STATUS);
    if (!(probe->detail & PCIE_ROOT_PORT)) return HW_UNSUPPORTED;

    probe->stage = PI4_USB_LINK;
    update_reg(io, PCIE_SW_INIT, PCIE_PERST, 0);
    if (hw_delay(io, 100000) != HW_OK) return HW_TIMEOUT;
    int result = hw_wait(io, PCIE_BASE + PCIE_STATUS, PCIE_LINK_UP, PCIE_LINK_UP, 100000);
    probe->detail = read_reg(io, PCIE_STATUS);
    return result;
}

static void map_outbound_window(const HwOps* io) {
    uint32_t base_mb = (uint32_t)(MMU_PCIE_DEVICE_BASE >> 20);
    uint32_t limit_mb = (uint32_t)((MMU_PCIE_DEVICE_BASE + MMU_PCIE_DEVICE_BYTES - 1) >> 20);
    uint32_t pci_limit = PCI_MEMORY_BASE + (uint32_t)MMU_PCIE_DEVICE_BYTES - 1;
    write_reg(io, PCIE_OUTBOUND_LO, PCI_MEMORY_BASE);
    write_reg(io, PCIE_OUTBOUND_HI, 0);
    update_reg(io, PCIE_OUTBOUND_BASE_LIMIT, 0xfff0fff0U,
               ((limit_mb & 0xfff) << 20) | ((base_mb & 0xfff) << 4));
    update_reg(io, PCIE_OUTBOUND_BASE_HI, 0xff, base_mb >> 12);
    update_reg(io, PCIE_OUTBOUND_LIMIT_HI, 0xff, limit_mb >> 12);
    write_reg(io, 0x20, (pci_limit & 0xfff00000U) | (PCI_MEMORY_BASE >> 16));
}

static int probe_bar(const HwOps* io, Pi4UsbProbe* probe) {
    const unsigned int endpoint = PCIE_EXT_DATA;
    uint32_t bar = read_reg(io, endpoint + PCI_BAR0);
    probe->detail = bar;
    if ((bar & 15) != 4) return HW_UNSUPPORTED;  /* 64-bit non-prefetchable memory. */
    /* Size BAR0 with memory decoding off, then restore it before checking. */
    uint32_t upper = read_reg(io, endpoint + PCI_BAR1);
    write_reg(io, endpoint + PCI_BAR0, 0xffffffffU);
    write_reg(io, endpoint + PCI_BAR1, 0xffffffffU);
    uint64_t mask = read_reg(io, endpoint + PCI_BAR0) & 0xfffffff0U;
    mask |= (uint64_t)read_reg(io, endpoint + PCI_BAR1) << 32;
    write_reg(io, endpoint + PCI_BAR0, bar);
    write_reg(io, endpoint + PCI_BAR1, upper);
    uint64_t bytes = ~mask + 1;
    if (bytes < 32 || bytes > MMU_PCIE_DEVICE_BYTES || (bytes & (bytes - 1))) return HW_PROTOCOL;

    map_outbound_window(io);
    write_reg(io, endpoint + PCI_BAR0, PCI_MEMORY_BASE);
    write_reg(io, endpoint + PCI_BAR1, 0);
    write_reg(io, PCI_COMMAND, PCI_MEMORY | PCI_INTX_DISABLE);
    write_reg(io, endpoint + PCI_COMMAND, PCI_MEMORY | PCI_INTX_DISABLE);
    uint32_t pci_decode = PCI_MEMORY | PCI_MASTER;
    if (read_reg(io, endpoint + PCI_BAR0) != (PCI_MEMORY_BASE | 4U) ||
        read_reg(io, endpoint + PCI_BAR1) != 0 ||
        (read_reg(io, PCI_COMMAND) & pci_decode) != PCI_MEMORY ||
        (read_reg(io, endpoint + PCI_COMMAND) & pci_decode) != PCI_MEMORY) return HW_IO;
    return HW_OK;
}

static int probe_controller(const HwOps* io, Pi4UsbProbe* probe) {
    const unsigned int endpoint = PCIE_EXT_DATA;
    probe->stage = PI4_USB_ENDPOINT;
    write_reg(io, PCI_COMMAND, PCI_INTX_DISABLE);
    update_reg(io, 0x18, 0x00ffffffU, 0x00010100U);  /* Primary 0, secondary/subordinate 1. */
    write_reg(io, PCIE_EXT_INDEX, VL805_BDF);
    probe->detail = read_reg(io, endpoint);
    if (probe->detail != VL805_ID ||
        (read_reg(io, endpoint + 0x08) >> 8) != PCI_XHCI_CLASS) return HW_UNSUPPORTED;
    write_reg(io, endpoint + PCI_COMMAND, PCI_INTX_DISABLE);

    /* VideoCore reloads the VL805 firmware on boards without a dedicated
     * EEPROM. Issue the notification before touching the xHCI registers. */
    probe->stage = PI4_USB_FIRMWARE;
    uint32_t address = VL805_BDF;
    int result = io->property(io->context, VL805_FIRMWARE_TAG, &address, 1);
    if (result != HW_OK) return result;
    if (hw_delay(io, 1000) != HW_OK) return HW_TIMEOUT;
    write_reg(io, PCIE_EXT_INDEX, VL805_BDF);
    write_reg(io, endpoint + PCI_COMMAND, PCI_INTX_DISABLE);
    if (read_reg(io, endpoint + PCI_COMMAND) & (PCI_MASTER | PCI_MEMORY)) return HW_IO;

    probe->stage = PI4_USB_BAR;
    result = probe_bar(io, probe);
    if (result != HW_OK) return result;
    probe->stage = PI4_USB_XHCI;
    probe->detail = io->read(io->context, MMU_PCIE_DEVICE_BASE);
    uint32_t version = probe->detail >> 16;
    if (version < 0x0090 || version >= 0x0200 || (probe->detail & 0xff) < 0x20) return HW_PROTOCOL;
    uint32_t params = io->read(io->context, MMU_PCIE_DEVICE_BASE + 4);
    uint32_t ports = params >> 24;
    if (params == 0xffffffffU || !ports || !(params & 0xffU)) return HW_PROTOCOL;
    probe->version = version;
    probe->ports = ports;
    probe->stage = PI4_USB_READY;
    return HW_OK;
}

int pi4_usb_probe(const HwOps* io, Pi4UsbProbe* probe) {
    if (!probe) return HW_INVALID;
    *probe = (Pi4UsbProbe){ .stage = PI4_USB_BRIDGE };
    if (!hw_ops_valid(io) || !io->property || !io->owned(io->context)) return HW_INVALID;
    probe->detail = read_reg(io, 0);
    if ((probe->detail & 0xffff) != 0x14e4) return HW_UNSUPPORTED;

    int result = start_link(io, probe);
    if (result == HW_OK) result = probe_controller(io, probe);
    if (result != HW_OK) update_reg(io, PCIE_SW_INIT, PCIE_PERST, PCIE_PERST);
    return result;
}
