#include <vantage/arch/mmu.h>
#include "pi4_usb_internal.h"

/* Intel xHCI specification, sections 4 through 6. All MMIO is 32-bit.
 * The VL805 and its DMA objects use little-endian words. */
#define CAP_HCS1 0x04
#define CAP_HCS2 0x08
#define CAP_HCC1 0x10
#define CAP_DBOFF 0x14
#define CAP_RTSOFF 0x18
#define CMD 0x00
#define STATUS 0x04
#define PAGE_SIZE 0x08
#define CRCR 0x18
#define DCBAAP 0x30
#define CONFIG 0x38
#define PORTSC 0x400
#define PORT_STRIDE 0x10
#define IMAN 0x00
#define IMOD 0x04
#define ERSTSZ 0x08
#define ERSTBA 0x10
#define ERDP 0x18

#define CMD_RUN 1U
#define CMD_RESET 2U
#define STATUS_HALTED 1U
#define STATUS_HOST_ERROR ((1U << 2) | (1U << 12))
#define STATUS_EVENT (1U << 3)
#define STATUS_PORT_CHANGE (1U << 4)
#define STATUS_NOT_READY (1U << 11)
#define ERDP_BUSY (1U << 3)

#define PORT_CONNECTED 1U
#define PORT_ENABLED (1U << 1)
#define PORT_OVERCURRENT (1U << 3)
#define PORT_RESET (1U << 4)
#define PORT_POWER (1U << 9)
#define PORT_SPEED_SHIFT 10
#define PORT_CONNECT_CHANGE (1U << 17)
#define PORT_RESET_CHANGE (1U << 21)
#define PORT_CHANGES (0x7fU << 17)
/* Preserve ordinary writable fields. Omit PED, reset and link-state strobes. */
#define PORT_PRESERVE (PORT_POWER | (3U << 14) | (7U << 25))

#define TRB_TYPE(n) ((uint32_t)(n) << 10)
#define TRB_CYCLE 1U
#define TRB_TOGGLE (1U << 1)
#define TRB_ISP (1U << 2)
#define TRB_IOC (1U << 5)
#define TRB_IMMEDIATE (1U << 6)
#define TRB_IN (1U << 16)
#define TRB_NORMAL TRB_TYPE(1)
#define TRB_SETUP TRB_TYPE(2)
#define TRB_DATA TRB_TYPE(3)
#define TRB_STATUS TRB_TYPE(4)
#define TRB_LINK TRB_TYPE(6)
#define TRB_ENABLE_SLOT TRB_TYPE(9)
#define TRB_DISABLE_SLOT TRB_TYPE(10)
#define TRB_ADDRESS_DEVICE TRB_TYPE(11)
#define TRB_CONFIGURE_EP TRB_TYPE(12)
#define TRB_EVALUATE_CONTEXT TRB_TYPE(13)
#define TRB_RESET_EP TRB_TYPE(14)
#define TRB_STOP_EP TRB_TYPE(15)
#define TRB_SET_DEQUEUE TRB_TYPE(16)
#define TRB_NO_OP TRB_TYPE(23)
#define EVENT_TRANSFER TRB_TYPE(32)
#define EVENT_COMMAND TRB_TYPE(33)
#define EVENT_PORT TRB_TYPE(34)
#define EVENT_HOST TRB_TYPE(37)
#define EVENT_TYPE (63U << 10)
#define EVENT_DATA (1U << 2)
#define CC_SUCCESS 1
#define CC_TRANSACTION 4
#define CC_STALL 6
#define CC_SHORT 13

#define ROOT_PORTS 32U
#define DEVICE_SLOTS 4U
#define INTERRUPT_ENDPOINTS 2U
#define SCRATCHPADS 4U
#define REPORT_MAX PI4_USB_INTERRUPT_MAX
#define DMA_PAGE 4096U
#define DMA_PHYS_LIMIT 0x40000000ULL
#define RING_TRBS 16U
#define EVENT_TRBS 64U
#define COMMAND_US 100000U
#define CONTROL_US 250000U
#define RESET_US 1000000U
#define WAIT_POLLS 8000000U
#define FIRMWARE_XHCI_RESET 0x00030058U
#define VL805_PCI_ADDRESS (1U << 20)

enum UsbSpeed { USB_FULL = 1, USB_LOW, USB_HIGH };

typedef struct {
    uint64_t parameter;
    uint32_t status, control;
} Trb;

typedef struct {
    HW_ALIGN(256) Trb trb[RING_TRBS];
    /* The VL805 may fetch four TRBs past a Link. Keep that memory dedicated
     * to this ring so it cannot prefetch stale entries of another endpoint. */
    Trb reserved[RING_TRBS];
} TransferRing;

typedef struct {
    uint64_t address;
    uint32_t count, reserved;
} EventSegment;

typedef struct {
    union {
        HW_ALIGN(DMA_PAGE) uint8_t context_page[DMA_PAGE];
        struct {
            uint32_t output[32 * 16];
            TransferRing ep0, interrupt[INTERRUPT_ENDPOINTS];
        };
    };
    uint8_t control[PI4_USB_CONTROL_MAX];
    union {
        uint8_t report_page[DMA_PAGE];
        uint8_t report[INTERRUPT_ENDPOINTS][REPORT_MAX];
    };
} DeviceMemory;

/* Static DMA storage survives link loss, firmware timeouts and failed resets.
 * Page placement also keeps individual DMA objects within 64 KiB boundaries
 * and separates controller-written structures from CPU-written cache lines. */
static struct {
    union {
        HW_ALIGN(DMA_PAGE) uint8_t input_page[DMA_PAGE];
        uint32_t input[33 * 16];
    };
    union {
        HW_ALIGN(DMA_PAGE) uint8_t controller_page[DMA_PAGE];
        struct {
            Trb event[EVENT_TRBS];
            TransferRing command;
            /* Separate the pointer arrays and ERST on 64-byte boundaries. */
            uint64_t dcbaa[8], scratch_array[8];
            EventSegment segment;
        };
    };
    uint8_t scratch[SCRATCHPADS][DMA_PAGE];
    DeviceMemory device[DEVICE_SLOTS];
} dma;

_Static_assert(sizeof(Trb) == 16 && sizeof(TransferRing) == 512, "Unexpected TRB layout");
_Static_assert(sizeof(DeviceMemory) == DMA_PAGE * 3, "Unexpected device DMA layout");

typedef struct {
    unsigned int index, cycle;
} Producer;

typedef struct {
    Producer producer;
    uint64_t setup_trb, data_trb, last_trb;
    unsigned int requested, actual, code;
    int pending, done, short_data;
} Transfer;

typedef struct {
    Transfer transfer;
    unsigned int dci, max_packet, interval;
} InterruptEndpoint;

typedef struct {
    Transfer ep0;
    InterruptEndpoint interrupt[INTERRUPT_ENDPOINTS];
    unsigned int port, slot, speed_id, speed, entries;
    int detached, unusable, configured;
} Device;

typedef struct {
    uint8_t usb2, slot_type;
    uint8_t speed[16];
} RootPort;

static struct {
    const HwOps* io;
    Pi4UsbPcie pcie;
    uintptr_t op, db, intr;
    uint64_t dma_base, command_trb;
    unsigned int context_words, slots, ports, scratchpads, event_index, event_cycle;
    unsigned int command_code, command_slot;
    int running, armed, command_done, quarantined;
    Producer command;
    RootPort root[ROOT_PORTS + 1];
    Device device[DEVICE_SLOTS];
} hc;

static uint32_t read32(uintptr_t address) {
    return hc.io->read(hc.io->context, address);
}

static void write32(uintptr_t address, uint32_t value) {
    hc.io->write(hc.io->context, address, value);
}

static void write64(uintptr_t address, uint64_t value) {
    write32(address, (uint32_t)value);
    write32(address + 4, (uint32_t)(value >> 32));
}

static void cache(void* ptr, size_t bytes, enum HwCache op) {
    hc.io->cache(hc.io->context, ptr, bytes, op);
}

static uint64_t bus(const void* ptr) {
    return hc.dma_base + ((uintptr_t)ptr - (uintptr_t)&dma);
}

static int owned(void) {
    return hc.io && hc.io->owned(hc.io->context);
}

static uintptr_t port_reg(unsigned int port) {
    return hc.op + PORTSC + (port - 1) * PORT_STRIDE;
}

static Device* find_device(unsigned int port) {
    for (unsigned int i = 0; i < DEVICE_SLOTS; i++)
        if (hc.device[i].port == port) return &hc.device[i];
    return NULL;
}

static DeviceMemory* device_memory(const Device* device) {
    return &dma.device[device - hc.device];
}

static void write_port(unsigned int port, uint32_t current, uint32_t change) {
    write32(port_reg(port), (current & PORT_PRESERVE) | change);
}

static int bar_contains(unsigned int offset, unsigned int bytes) {
    return offset <= hc.pcie.bar_bytes && bytes <= hc.pcie.bar_bytes - offset;
}

static int usb2_protocol(unsigned int offset, uint32_t header) {
    if (!bar_contains(offset, 16) || read32(MMU_PCIE_DEVICE_BASE + offset + 4) != 0x20425355U)
        return HW_PROTOCOL;  /* Supported Protocol name string: USB followed by a space. */
    uint32_t ports = read32(MMU_PCIE_DEVICE_BASE + offset + 8);
    unsigned int first = ports & 0xff, count = (ports >> 8) & 0xff, psi = ports >> 28;
    unsigned int bytes = 16 + 4 * psi, next = ((header >> 8) & 0xff) * 4;
    if (!first || !count || first + count - 1 > hc.ports ||
        !bar_contains(offset, bytes) || (next && next < bytes)) return HW_PROTOCOL;
    RootPort protocol = {.usb2 = 1};
    protocol.slot_type = read32(MMU_PCIE_DEVICE_BASE + offset + 12) & 31;
    if (!psi) {
        protocol.speed[USB_FULL] = USB_FULL;
        protocol.speed[USB_LOW] = USB_LOW;
        protocol.speed[USB_HIGH] = USB_HIGH;
    }
    for (unsigned int i = 0; i < psi; i++) {
        uint32_t speed = read32(MMU_PCIE_DEVICE_BASE + offset + 16 + 4 * i);
        unsigned int id = speed & 15, exponent = (speed >> 4) & 3, rate = speed >> 16;
        /* USB 2 rates in the Supported Protocol speed table. */
        if (exponent == 1 && rate == 1500) protocol.speed[id] = USB_LOW;
        if (exponent == 2 && rate == 12) protocol.speed[id] = USB_FULL;
        if (exponent == 2 && rate == 480) protocol.speed[id] = USB_HIGH;
    }
    for (unsigned int port = first; port < first + count; port++) hc.root[port] = protocol;
    return HW_OK;
}

static int read_capabilities(Pi4UsbProbe* probe) {
    const uintptr_t base = MMU_PCIE_DEVICE_BASE;
    if (!bar_contains(0, 0x20)) return HW_PROTOCOL;
    uint32_t cap = read32(base), hcs1 = read32(base + CAP_HCS1);
    uint32_t hcs2 = read32(base + CAP_HCS2), hcc1 = read32(base + CAP_HCC1);
    uint32_t db = read32(base + CAP_DBOFF), rt = read32(base + CAP_RTSOFF);
    unsigned int op = cap & 0xff, ports = hcs1 >> 24, slots = hcs1 & 0xff;
    if (slots > DEVICE_SLOTS) slots = DEVICE_SLOTS;
    unsigned int operational_end = op + PORTSC + ports * PORT_STRIDE;
    if (op < 0x20 || (op & 3) || !ports || ports > ROOT_PORTS || !slots ||
        !(hcs1 & (0x7ffU << 8)) || !bar_contains(0, operational_end) ||
        (db & 3) || (rt & 31) || db < operational_end || rt < operational_end ||
        !bar_contains(db, (slots + 1) * 4) || !bar_contains(rt, 0x40) ||
        (db < rt + 0x40 && rt < db + (slots + 1) * 4)) return HW_PROTOCOL;

    hc.scratchpads = ((hcs2 >> 16) & 0x3e0) | (hcs2 >> 27);
    if (hc.scratchpads > SCRATCHPADS || (hc.pcie.dma_base && !(hcc1 & 1))) return HW_UNSUPPORTED;
    hc.ports = ports;
    hc.slots = slots;
    hc.context_words = hcc1 & (1U << 2) ? 16 : 8;
    hc.op = base + op;
    hc.db = base + db;
    hc.intr = base + rt + 0x20;

    unsigned int offset = (hcc1 >> 16) * 4;
    for (unsigned int i = 0; offset && i < 64; i++) {
        if (offset < 0x20 || !bar_contains(offset, 4)) return HW_PROTOCOL;
        uint32_t header = read32(base + offset);
        unsigned int id = header & 0xff, next = (header >> 8) & 0xff;
        if (header == 0xffffffffU || !id) return HW_PROTOCOL;
        if (id == 1) {
            if (!bar_contains(offset, 8) || (next && next < 2)) return HW_PROTOCOL;
            if (header & (1U << 16)) {
                write32(base + offset, header | (1U << 24));
                int result = hw_wait(hc.io, base + offset, 1U << 16, 0, COMMAND_US);
                if (result != HW_OK) return result;
            }
            write32(base + offset + 4, 0);  /* Disable legacy SMI enables. */
        }
        if (id == 2 && (header >> 24) == 2) {
            int result = usb2_protocol(offset, header);
            if (result != HW_OK) return result;
        }
        offset = next ? offset + 4 * next : 0;
    }
    probe->detail = offset;
    return offset ? HW_PROTOCOL : HW_OK;
}

static int reset_controller(void) {
    if (!pi4_usb_pcie_link(hc.io)) return HW_OFF;
    int result = hw_wait(hc.io, hc.op + STATUS, STATUS_NOT_READY, 0, RESET_US);
    if (result != HW_OK) return result;
    write32(hc.op + CMD, 0);
    result = hw_wait(hc.io, hc.op + STATUS, STATUS_HALTED, STATUS_HALTED, COMMAND_US);
    if (result != HW_OK) return result;
    if (read32(hc.op + STATUS) == 0xffffffffU || !pi4_usb_pcie_link(hc.io)) return HW_OFF;
    write32(hc.op + CMD, CMD_RESET);
    result = hw_wait(hc.io, hc.op + CMD, CMD_RESET, 0, RESET_US);
    if (result != HW_OK) return result;
    result = hw_wait(hc.io, hc.op + STATUS, STATUS_NOT_READY | STATUS_HALTED,
                     STATUS_HALTED, RESET_US);
    if (result != HW_OK) return result;
    return pi4_usb_pcie_link(hc.io) ? HW_OK : HW_OFF;
}

static int shutdown_controller(void) {
    hc.running = 0;
    int result = hc.armed ? reset_controller() : HW_OK;
    if (result != HW_OK) hc.quarantined = 1;
    pi4_usb_pcie_close(hc.io, !hc.quarantined);
    if (!hc.quarantined) hc.armed = 0;
    hc.io = NULL;
    return result;
}

static int fatal(int result) {
    shutdown_controller();
    return result;
}

/* A single outstanding command or transfer per ring. Write and clean every
 * later TRB (including a Link on wrap), then release the first TRB's cycle bit
 * before ringing the doorbell. Control stages are separate TDs, so Link CH=0. */
static void queue_trbs(TransferRing* ring, Producer* producer, const Trb* trbs,
                       unsigned int count, uint64_t* addresses) {
    Trb* first = &ring->trb[producer->index];
    for (unsigned int i = 0; i < count; i++) {
        Trb* target = &ring->trb[producer->index++];
        addresses[i] = bus(target);
        *target = trbs[i];
        target->control |= producer->cycle ^ (i == 0);
        cache(target, sizeof(*target), HW_CLEAN);
        if (producer->index == RING_TRBS - 1) {
            Trb* link = &ring->trb[producer->index];
            *link = (Trb){bus(ring), 0, TRB_LINK | TRB_TOGGLE | producer->cycle};
            cache(link, sizeof(*link), HW_CLEAN);
            producer->index = 0;
            producer->cycle ^= 1U;
        }
    }
    first->control ^= TRB_CYCLE;
    cache(first, sizeof(*first), HW_CLEAN);
}

static void init_ring(TransferRing* ring, Producer* producer) {
    memset(ring, 0, sizeof(*ring));
    cache(ring, sizeof(*ring), HW_CLEAN);
    *producer = (Producer){.cycle = 1};
}

static int initialize_dma(void) {
    uint32_t pages = read32(hc.op + PAGE_SIZE);
    if (pages == 0xffffffffU || !(pages & 1)) return HW_UNSUPPORTED;
    uint64_t physical = hc.io->physical(hc.io->context, &dma);
    const uint8_t* end = (const uint8_t*)&dma + sizeof(dma) - 1;
    if (!physical || (physical & (DMA_PAGE - 1)) || physical > DMA_PHYS_LIMIT - sizeof(dma) ||
        hc.io->physical(hc.io->context, end) != physical + sizeof(dma) - 1) return HW_INVALID;
    hc.dma_base = hc.pcie.dma_base + physical;
    memset(&dma, 0, sizeof(dma));
    hc.command.cycle = 1;
    hc.event_cycle = 1;
    dma.segment = (EventSegment){bus(dma.event), EVENT_TRBS, 0};
    if (hc.scratchpads) {
        dma.dcbaa[0] = bus(dma.scratch_array);
        for (unsigned int i = 0; i < hc.scratchpads; i++) dma.scratch_array[i] = bus(dma.scratch[i]);
    }
    cache(&dma, sizeof(dma), HW_CLEAN_INVALIDATE);

    hc.armed = 1;
    write32(hc.op + CONFIG, hc.slots);
    write64(hc.op + DCBAAP, bus(dma.dcbaa));
    write64(hc.op + CRCR, bus(&dma.command) | TRB_CYCLE);
    write32(hc.intr + IMAN, 1);  /* Clear IP and keep IE disabled for polling. */
    write32(hc.intr + IMOD, 0);
    write32(hc.intr + ERSTSZ, 1);
    write64(hc.intr + ERDP, bus(dma.event));
    write64(hc.intr + ERSTBA, bus(&dma.segment));
    write32(hc.op + CMD, CMD_RUN);
    return hw_wait(hc.io, hc.op + STATUS, STATUS_HALTED, 0, COMMAND_US);
}

static int port_status(unsigned int port, uint32_t* status) {
    uint32_t value = read32(port_reg(port));
    if (value == 0xffffffffU) return HW_IO;
    Device* device = find_device(port);
    if (device && (!(value & PORT_CONNECTED) || (value & PORT_CONNECT_CHANGE)))
        device->detached = 1;
    if (value & PORT_CHANGES) write_port(port, value, value & PORT_CHANGES);
    *status = value;
    return HW_OK;
}

static int transfer_event(const Trb* event, unsigned int slot, unsigned int code) {
    unsigned int dci = (event->control >> 16) & 31;
    Device* device = NULL;
    for (unsigned int i = 0; i < DEVICE_SLOTS; i++) {
        if (hc.device[i].slot == slot) device = &hc.device[i];
    }
    if (!device || !slot || !dci || device->detached) return HW_OK;

    Transfer* transfer = dci == 1 ? &device->ep0 : NULL;
    for (unsigned int i = 0; i < INTERRUPT_ENDPOINTS; i++) {
        if (device->interrupt[i].dci == dci) transfer = &device->interrupt[i].transfer;
    }
    if (!transfer || !transfer->pending || transfer->done) return HW_OK;

    uint64_t address = event->parameter;
    int data = transfer->data_trb && address == transfer->data_trb;
    int last = address == transfer->last_trb;
    if ((event->control & EVENT_DATA) ||
        (!data && !last && address != transfer->setup_trb) || !address)
        return HW_PROTOCOL;

    unsigned int residue = event->status & 0xffffff;
    if (code == CC_SHORT && data) {
        if (!residue || residue > transfer->requested || transfer->short_data) return HW_PROTOCOL;
        transfer->actual = transfer->requested - residue;
        transfer->short_data = 1;
        return HW_OK; /* EP0 still owes the status stage completion. */
    }
    if (code == CC_SUCCESS || code == CC_SHORT) {
        if (!last || residue > transfer->requested) return HW_PROTOCOL;
        if (dci == 1) {
            if (!transfer->short_data) {
                if (code != CC_SUCCESS || residue) return HW_PROTOCOL;
                transfer->actual = transfer->requested;
            }
        } else {
            if ((code == CC_SUCCESS && residue) || (code == CC_SHORT && !residue)) return HW_PROTOCOL;
            transfer->actual = transfer->requested - residue;
        }
    }
    transfer->code = code;
    transfer->done = 1;
    return HW_OK;
}

static int dispatch_event(const Trb* event) {
    unsigned int code = event->status >> 24, slot = event->control >> 24;
    switch (event->control & EVENT_TYPE) {
    case EVENT_COMMAND:
        if (!hc.command_trb || hc.command_done || event->parameter != hc.command_trb)
            return HW_PROTOCOL;
        hc.command_code = code;
        hc.command_slot = slot;
        hc.command_done = 1;
        return HW_OK;
    case EVENT_TRANSFER:
        return transfer_event(event, slot, code);
    case EVENT_PORT: {
        unsigned int port = (event->parameter >> 24) & 0xff;
        uint32_t status;
        if (!port || port > hc.ports || code != CC_SUCCESS) return HW_PROTOCOL;
        return port_status(port, &status);
    }
    case EVENT_HOST:
        return HW_IO;
    default:
        return HW_OK;
    }
}

static int poll_events(void) {
    if (!pi4_usb_pcie_link(hc.io)) return fatal(HW_OFF);
    uint32_t status = read32(hc.op + STATUS);
    if (status & (STATUS_HOST_ERROR | STATUS_HALTED | STATUS_NOT_READY)) return fatal(HW_IO);
    for (unsigned int i = 0; i < EVENT_TRBS; i++) {
        Trb* memory = &dma.event[hc.event_index];
        cache(memory, sizeof(*memory), HW_INVALIDATE);
        const volatile Trb* visible = memory;
        uint32_t control = visible->control;
        if ((control & TRB_CYCLE) != hc.event_cycle) break;
        Trb event = {visible->parameter, visible->status, control};
        int result = dispatch_event(&event);
        if (result != HW_OK) return fatal(result);
        if (++hc.event_index == EVENT_TRBS) {
            hc.event_index = 0;
            hc.event_cycle ^= 1U;
        }
        write64(hc.intr + ERDP, bus(&dma.event[hc.event_index]) | ERDP_BUSY);
    }
    if (status & (STATUS_EVENT | STATUS_PORT_CHANGE)) {
        write32(hc.op + STATUS, status & (STATUS_EVENT | STATUS_PORT_CHANGE));
        write32(hc.intr + IMAN, 1);
    }
    return HW_OK;
}

static int command(uint32_t control, uint64_t parameter, unsigned int slot) {
    int result = poll_events();
    if (result != HW_OK) return result;
    Trb trb = {parameter, 0, control | (slot << 24)};
    hc.command_done = 0;
    hc.command_code = 0;
    queue_trbs(&dma.command, &hc.command, &trb, 1, &hc.command_trb);
    write32(hc.db, 0);
    uint32_t start = hc.io->time_us(hc.io->context);
    for (unsigned int i = 0; i < WAIT_POLLS; i++) {
        result = poll_events();
        if (result != HW_OK) return result;
        if (hc.command_done) {
            hc.command_trb = 0;
            if ((slot && hc.command_slot != slot) ||
                (control == TRB_NO_OP && hc.command_slot)) return fatal(HW_PROTOCOL);
            return hc.command_code == CC_SUCCESS ? HW_OK : HW_IO;
        }
        if (hc.io->time_us(hc.io->context) - start >= COMMAND_US) break;
    }
    return fatal(HW_TIMEOUT);
}

int pi4_usb_start(const HwOps* io, Pi4UsbProbe* probe) {
    if (!probe) return HW_INVALID;
    *probe = (Pi4UsbProbe){.stage = PI4_USB_BRIDGE};
    if (!hw_ops_valid(io) || !io->owned(io->context) || !io->cache || !io->physical || !io->property)
        return HW_INVALID;
    if (hc.running) return HW_BUSY;
    if (hc.quarantined) return HW_OFF;
    memset(&hc, 0, sizeof(hc));
    int result = pi4_usb_pcie_open(io, probe, &hc.pcie);
    if (result != HW_OK) return result;
    hc.io = io;

    probe->stage = PI4_USB_DMA;
    result = pi4_usb_pcie_enable(io);
    if (result != HW_OK) return fatal(result);
    probe->stage = PI4_USB_FIRMWARE;
    uint32_t address = VL805_PCI_ADDRESS;
    result = io->property(io->context, FIRMWARE_XHCI_RESET, &address, 1);
    if (result == HW_TIMEOUT) hc.quarantined = 1;
    if (result == HW_OK) result = hw_delay(io, 2000);
    if (result == HW_OK) result = pi4_usb_pcie_enable(io);
    if (result != HW_OK) return fatal(result);

    result = pi4_usb_pcie_caps(io, probe);
    if (result == HW_OK) result = read_capabilities(probe);
    if (result != HW_OK) return fatal(result);
    probe->stage = PI4_USB_RESET;
    result = reset_controller();
    probe->detail = pi4_usb_pcie_link(io) ? read32(hc.op + STATUS) : 0xffffffffU;
    if (result != HW_OK) return fatal(result);

    probe->stage = PI4_USB_DMA;
    result = initialize_dma();
    if (result != HW_OK) return fatal(result);
    probe->stage = PI4_USB_NO_OP;
    result = command(TRB_NO_OP, 0, 0);
    probe->detail = hc.command_code;
    if (result != HW_OK) return hc.io ? fatal(result) : result;

    for (unsigned int port = 1; port <= hc.ports; port++) {
        if (!hc.root[port].usb2) continue;
        uint32_t status = read32(port_reg(port));
        if (status == 0xffffffffU) return fatal(HW_IO);
        if (!(status & PORT_POWER)) write_port(port, status, PORT_POWER);
    }
    hc.running = 1;
    probe->stage = PI4_USB_RUNNING;
    return HW_OK;
}

int pi4_usb_stop(void) {
    if (!hc.running) return hc.quarantined ? HW_OFF : HW_OK;
    if (!owned()) return HW_INVALID;
    return shutdown_controller();
}

unsigned int pi4_usb_port_count(void) {
    return hc.running && owned() ? hc.ports : 0;
}

int pi4_usb_port_connected(unsigned int port) {
    if (!hc.running) return HW_OFF;
    if (!owned() || !port || port > hc.ports) return HW_INVALID;
    int result = poll_events();
    if (result != HW_OK) return result;
    if (!hc.root[port].usb2) return HW_UNSUPPORTED;
    uint32_t status;
    result = port_status(port, &status);
    if (result != HW_OK) return fatal(result);
    Device* device = find_device(port);
    if ((device && device->detached) || !(status & PORT_CONNECTED)) return HW_OFF;
    return status & PORT_OVERCURRENT ? HW_IO : HW_OK;
}

static int reset_port(unsigned int port, unsigned int* speed_id) {
    /* Require a stable connection for 100 ms before USB 2 reset. */
    int result = hw_delay(hc.io, 100000);
    if (result != HW_OK) return result;
    if (!pi4_usb_pcie_link(hc.io)) return fatal(HW_OFF);
    uint32_t status;
    result = port_status(port, &status);
    if (result != HW_OK) return fatal(result);
    if (!(status & PORT_CONNECTED) || (status & PORT_CONNECT_CHANGE)) return HW_OFF;
    if (status & (PORT_RESET | PORT_OVERCURRENT)) return HW_IO;

    write_port(port, status, PORT_RESET);
    result = hw_wait(hc.io, port_reg(port), PORT_RESET, 0, COMMAND_US);
    if (!pi4_usb_pcie_link(hc.io)) return fatal(HW_OFF);
    if (result != HW_OK) return result;
    /* USB reset recovery before issuing the first transfer. */
    result = hw_delay(hc.io, 10000);
    if (result != HW_OK) return result;
    if (!pi4_usb_pcie_link(hc.io)) return fatal(HW_OFF);
    result = port_status(port, &status);
    if (result != HW_OK) return fatal(result);
    if (!(status & PORT_CONNECTED) || (status & PORT_CONNECT_CHANGE)) return HW_OFF;
    if (!(status & PORT_ENABLED) || !(status & PORT_RESET_CHANGE) || (status & PORT_OVERCURRENT))
        return HW_IO;
    *speed_id = (status >> PORT_SPEED_SHIFT) & 15;
    return hc.root[port].speed[*speed_id] ? HW_OK : HW_UNSUPPORTED;
}

static void prepare_input(uint32_t add) {
    memset(dma.input, 0, sizeof(dma.input));
    dma.input[1] = add;
}

static void slot_context(const Device* device, unsigned int entries) {
    uint32_t* slot = dma.input + hc.context_words;
    slot[0] = (entries << 27) | (device->speed_id << 20);
    slot[1] = device->port << 16;
}

static uint32_t* endpoint_context(unsigned int dci) {
    return dma.input + (dci + 1) * hc.context_words;
}

static void prepare_endpoint(uint32_t* entry, unsigned int type, unsigned int packet,
                             unsigned int interval, const TransferRing* ring) {
    uint64_t dequeue = bus(ring) | TRB_CYCLE;
    entry[0] = interval << 16;
    entry[1] = (packet << 16) | (type << 3) | (3U << 1);
    entry[2] = (uint32_t)dequeue;
    entry[3] = (uint32_t)(dequeue >> 32);
    entry[4] = type == 4 ? 8 : (packet << 16) | packet;
}

static int context_command(uint32_t type, unsigned int slot) {
    cache(dma.input, sizeof(dma.input), HW_CLEAN);
    return command(type, bus(dma.input), slot);
}

static int release_device(Device* device) {
    int result = command(TRB_DISABLE_SLOT, 0, device->slot);
    if (result != HW_OK) return hc.io ? fatal(result) : result;
    dma.dcbaa[device->slot] = 0;
    cache(&dma.dcbaa[device->slot], sizeof(uint64_t), HW_CLEAN);
    *device = (Device){0};
    return HW_OK;
}

int pi4_usb_close_port(unsigned int port) {
    if (!hc.running) return HW_OFF;
    if (!owned() || !port || port > hc.ports) return HW_INVALID;
    Device* device = find_device(port);
    return device ? release_device(device) : HW_OK;
}

static unsigned int endpoint_target(unsigned int dci) {
    return dci << 16;
}

static int recover_control(Device* device) {
    int result = command(TRB_RESET_EP | endpoint_target(1), 0, device->slot);
    if (result != HW_OK) return result;
    const Producer* producer = &device->ep0.producer;
    uint64_t dequeue = bus(&device_memory(device)->ep0.trb[producer->index]) | producer->cycle;
    return command(TRB_SET_DEQUEUE | endpoint_target(1), dequeue, device->slot);
}

static int wait_control(Device* device) {
    uint32_t start = hc.io->time_us(hc.io->context);
    for (unsigned int i = 0; i < WAIT_POLLS; i++) {
        int result = pi4_usb_port_connected(device->port);
        if (result != HW_OK) return result;
        if (device->ep0.done) return HW_OK;
        if (hc.io->time_us(hc.io->context) - start >= CONTROL_US) break;
    }
    return HW_TIMEOUT;
}

int pi4_usb_control(unsigned int port, const Pi4UsbSetup* setup, void* data,
                    size_t* actual) {
    if (!actual) return HW_INVALID;
    *actual = 0;
    if (!setup || setup->length > PI4_USB_CONTROL_MAX || (setup->length && !data)) return HW_INVALID;
    int result = pi4_usb_port_connected(port);
    if (result != HW_OK) return result;
    Device* device = find_device(port);
    if (!device || device->unusable) return HW_OFF;
    if ((!setup->request_type && setup->request == 5) ||
        (setup->request_type == 1 && setup->request == 11)) return HW_UNSUPPORTED;
    int configuring = !setup->request_type && setup->request == 9;
    if (configuring && (device->configured || device->entries == 1 ||
                        !setup->value || setup->value > 255 || setup->index || setup->length))
        return HW_INVALID;

    unsigned int length = setup->length;
    int in = !!(setup->request_type & 0x80);
    DeviceMemory* memory = device_memory(device);
    if (length) {
        if (!in) memcpy(memory->control, data, length);
        cache(memory->control, length, in ? HW_CLEAN_INVALIDATE : HW_CLEAN);
    }
    uint64_t packet = setup->request_type | ((uint64_t)setup->request << 8) |
                      ((uint64_t)setup->value << 16) | ((uint64_t)setup->index << 32) |
                      ((uint64_t)length << 48);
    uint32_t transfer_type = length ? (in ? 3U : 2U) << 16 : 0;
    Trb trbs[3] = {{packet, 8, TRB_SETUP | TRB_IMMEDIATE | transfer_type}};
    unsigned int count = 1;
    if (length) {
        trbs[count++] = (Trb){bus(memory->control), length, TRB_DATA | (in ? TRB_IN | TRB_ISP : 0)};
    }
    trbs[count++] = (Trb){0, 0, TRB_STATUS | TRB_IOC | ((!length || !in) ? TRB_IN : 0)};

    Transfer* transfer = &device->ep0;
    uint64_t addresses[3];
    queue_trbs(&memory->ep0, &transfer->producer, trbs, count, addresses);
    *transfer = (Transfer){
        .producer = transfer->producer, .setup_trb = addresses[0],
        .data_trb = length ? addresses[1] : 0, .last_trb = addresses[count - 1],
        .requested = length, .pending = 1
    };
    write32(hc.db + 4 * device->slot, 1);
    result = wait_control(device);
    if (result != HW_OK) {
        if (result == HW_TIMEOUT) {
            /* Stop the TD before retiring the slot. A raced completion can
             * make Stop fail harmlessly. Disable Slot still proves retirement. */
            (void)command(TRB_STOP_EP | endpoint_target(1), 0, device->slot);
            if (hc.running) (void)release_device(device);
        } else {
            device->unusable = 1;
        }
        return result;
    }
    transfer->pending = 0;
    unsigned int code = transfer->code;
    if (code == CC_STALL || code == CC_TRANSACTION) {
        result = recover_control(device);
        if (result != HW_OK) device->unusable = 1;
        if (result == HW_OK && code == CC_STALL) return PI4_USB_STALL;
        return result == HW_OK ? HW_IO : result;
    }
    if (code != CC_SUCCESS && code != CC_SHORT) {
        device->unusable = 1;
        return HW_IO;
    }
    if (in && length) {
        cache(memory->control, length, HW_INVALIDATE);
        memcpy(data, memory->control, transfer->actual);
    }
    *actual = transfer->actual;
    if (configuring) device->configured = 1;
    return HW_OK;
}

int pi4_usb_open_port(unsigned int port) {
    int result = pi4_usb_port_connected(port);
    if (result != HW_OK) return result;
    if (find_device(port)) return HW_BUSY;
    Device* device = find_device(0);
    if (!device) return HW_NO_SPACE;
    unsigned int speed_id;
    result = reset_port(port, &speed_id);
    if (result != HW_OK) return result;
    *device = (Device){.port = port, .speed_id = speed_id, .speed = hc.root[port].speed[speed_id],
                       .entries = 1};
    result = command(TRB_ENABLE_SLOT | (hc.root[port].slot_type << 16), 0, 0);
    if (result != HW_OK) {
        *device = (Device){0};
        return result;
    }
    unsigned int slot = hc.command_slot;
    if (!slot || slot > hc.slots || dma.dcbaa[slot]) return fatal(HW_PROTOCOL);
    device->slot = slot;
    DeviceMemory* memory = device_memory(device);
    memset(memory->output, 0, sizeof(memory->output));
    cache(memory->output, sizeof(memory->output), HW_CLEAN_INVALIDATE);
    init_ring(&memory->ep0, &device->ep0.producer);
    dma.dcbaa[slot] = bus(memory->output);
    cache(&dma.dcbaa[slot], sizeof(uint64_t), HW_CLEAN);
    unsigned int packet = device->speed == USB_HIGH ? 64 : 8;
    prepare_input(3);
    slot_context(device, 1);
    prepare_endpoint(endpoint_context(1), 4, packet, 0, &memory->ep0);
    result = device->detached ? HW_OFF : context_command(TRB_ADDRESS_DEVICE, slot);
    if (result == HW_OK) {
        const Pi4UsbSetup get_device = {0x80, 6, 0x0100, 0, 8};
        uint8_t descriptor[8];
        size_t actual;
        result = pi4_usb_control(port, &get_device, descriptor, &actual);
        if (result == HW_OK) {
            if (actual != sizeof(descriptor) || descriptor[0] != 18 || descriptor[1] != 1)
                result = HW_PROTOCOL;
            else if (descriptor[4] == 9)
                result = HW_UNSUPPORTED;  /* Direct root devices only. */
            else if (descriptor[7] != packet) {
                unsigned int reported = descriptor[7];
                if (device->speed != USB_FULL || (reported != 16 && reported != 32 && reported != 64))
                    result = HW_PROTOCOL;
                else {
                    prepare_input(2);
                    endpoint_context(1)[1] = reported << 16;
                    result = context_command(TRB_EVALUATE_CONTEXT, slot);
                }
            }
        }
    }
    if (result == PI4_USB_STALL) result = HW_IO;
    if (result == HW_OK) result = pi4_usb_port_connected(port);
    if (result != HW_OK && hc.running && device->slot) {
        int released = release_device(device);
        if (released != HW_OK) return released;
    }
    return result;
}

static unsigned int interrupt_dci(uint8_t address) {
    if ((address & 0xf0) != 0x80 || !(address & 15)) return 0;
    return 2 * (address & 15) + 1;
}

static InterruptEndpoint* find_interrupt(Device* device, unsigned int dci) {
    for (unsigned int i = 0; i < INTERRUPT_ENDPOINTS; i++) {
        if (device->interrupt[i].dci == dci) return &device->interrupt[i];
    }
    return NULL;
}

int pi4_usb_configure_interrupt_in(unsigned int port, uint8_t endpoint_address,
                                  uint16_t max_packet, uint8_t interval) {
    unsigned int dci = interrupt_dci(endpoint_address);
    if (!dci || !max_packet || max_packet > REPORT_MAX || !interval) return HW_INVALID;
    int result = pi4_usb_port_connected(port);
    if (result != HW_OK) return result;
    Device* device = find_device(port);
    if (!device || device->unusable) return HW_OFF;
    if (device->configured) return HW_INVALID;
    if ((device->speed == USB_LOW && max_packet > 8) ||
        (device->speed == USB_FULL && max_packet > 64) ||
        (device->speed == USB_HIGH && interval > 16)) return HW_INVALID;

    unsigned int esit = 0;
    if (device->speed == USB_HIGH) esit = interval - 1;
    else {
        /* USB 2 LS/FS descriptors use frames. Round down to a power of two
         * microframes so service is at least as frequent as requested. */
        for (unsigned int microframes = interval * 8U; microframes > 1; microframes >>= 1) esit++;
    }
    InterruptEndpoint* endpoint = find_interrupt(device, dci);
    if (endpoint)
        return endpoint->max_packet == max_packet && endpoint->interval == esit ? HW_OK : HW_BUSY;
    endpoint = find_interrupt(device, 0);
    if (!endpoint) return HW_NO_SPACE;
    unsigned int index = (unsigned int)(endpoint - device->interrupt);
    TransferRing* ring = &device_memory(device)->interrupt[index];
    unsigned int entries = dci > device->entries ? dci : device->entries;
    init_ring(ring, &endpoint->transfer.producer);
    prepare_input(1U | (1U << dci));
    slot_context(device, entries);
    prepare_endpoint(endpoint_context(dci), 7, max_packet, esit, ring);
    result = context_command(TRB_CONFIGURE_EP, device->slot);
    if (result == HW_OK) {
        endpoint->dci = dci;
        endpoint->max_packet = max_packet;
        endpoint->interval = esit;
        device->entries = entries;
    }
    return result;
}

int pi4_usb_interrupt_in(unsigned int port, uint8_t endpoint_address, void* report,
                        size_t capacity, size_t* actual) {
    unsigned int dci = interrupt_dci(endpoint_address);
    if (!dci || !report || !actual) return HW_INVALID;
    *actual = 0;
    int result = pi4_usb_port_connected(port);
    if (result != HW_OK) return result;
    Device* device = find_device(port);
    if (!device || device->unusable || !device->configured) return HW_OFF;
    InterruptEndpoint* endpoint = find_interrupt(device, dci);
    if (!endpoint || capacity < endpoint->max_packet) return HW_INVALID;
    Transfer* transfer = &endpoint->transfer;
    if (transfer->pending && !transfer->done) return HW_BUSY;
    unsigned int index = (unsigned int)(endpoint - device->interrupt);
    DeviceMemory* memory = device_memory(device);
    uint8_t* buffer = memory->report[index];
    result = HW_BUSY;
    if (transfer->done) {
        if (transfer->code != CC_SUCCESS && transfer->code != CC_SHORT) return HW_IO;
        cache(buffer, endpoint->max_packet, HW_INVALIDATE);
        memcpy(report, buffer, transfer->actual);
        *actual = transfer->actual;
        result = HW_OK;
    }
    cache(buffer, endpoint->max_packet, HW_CLEAN_INVALIDATE);
    Trb trb = {bus(buffer), endpoint->max_packet, TRB_NORMAL | TRB_IOC};
    uint64_t address;
    queue_trbs(&memory->interrupt[index], &transfer->producer, &trb, 1, &address);
    *transfer = (Transfer){
        .producer = transfer->producer, .last_trb = address,
        .requested = endpoint->max_packet, .pending = 1
    };
    write32(hc.db + 4 * device->slot, dci);
    return result;
}
