/* Standalone native test; link pi4_xhci.c, pi4_usb.c and drivers/hw/wait.c
 * with -DVANTAGE_SIM and -Iinclude. The DMA view changes only on cache calls. */
#include <vantage/arch/mmu.h>
#include <vantage/drivers/pi4_usb.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); exit(1); \
} } while (0)

#define HOST 0xfd500000ULL
#define PHYS 0x01000000ULL
#define SHIFTED 0x200000000ULL
#define MEMORY_BYTES 0x20000U
#define OP 0x20
#define PORTSC (OP + 0x400)
#define DOORBELL 0x700
#define IRQ 0x820
#define EVENT_COUNT 64
#define TYPE(trb) (((trb).control >> 10) & 63)
#define PACKET(context) ((context)[1] >> 16)

typedef struct {
    uint64_t parameter;
    uint32_t status, control;
} Trb;

typedef struct {
    uint64_t next;
    unsigned int cycle, wraps;
} Ring;

typedef struct {
    Ring ring[32];
    uint64_t pending[32], buffer[32];
    unsigned int packet[32];
    unsigned int port, speed;
    int enabled, configured, halted;
} Slot;

typedef struct {
    uint32_t pcie[0x9214 / 4], xhci[0x1000 / 4];
    uint64_t dma_base;
    uintptr_t cpu_base;
    uint8_t memory[MEMORY_BYTES];
    uint32_t time, status_at;
    uint64_t status_trb;
    unsigned int stride, event_index, event_cycle, event_wraps, event_consumer;
    unsigned int ep0_wraps, interrupt_wraps, firmware_calls, resets;
    unsigned int stall_recoveries, timeout_stops, disable_count, status_slot;
    int firmware, running, drop_control;
    Ring command;
    Slot slot[5];
} Model;

static Model model;

static uint32_t* reg(Model* m, unsigned int offset) {
    CHECK(offset < sizeof(m->xhci) && !(offset & 3));
    return &m->xhci[offset / 4];
}

static uint64_t reg64(Model* m, unsigned int offset) {
    return *reg(m, offset) | ((uint64_t)*reg(m, offset + 4) << 32);
}

static unsigned int port_offset(unsigned int port) { return PORTSC + (port - 1) * 16; }

static void* dma_ptr(Model* m, uint64_t pointer, size_t bytes) {
    CHECK(m->running && m->firmware);
    CHECK((m->pcie[4 / 4] & 4) && (m->pcie[0x8004 / 4] & 4));
    CHECK(pointer >= m->dma_base && pointer - m->dma_base <= MEMORY_BYTES);
    size_t offset = (size_t)(pointer - m->dma_base);
    CHECK(bytes <= MEMORY_BYTES - offset);
    return m->memory + offset;
}

static uint64_t read_dma64(Model* m, uint64_t pointer) {
    uint64_t value;
    memcpy(&value, dma_ptr(m, pointer, sizeof(value)), sizeof(value));
    return value;
}

static void complete(Model* m, unsigned int type, unsigned int code, unsigned int slot,
                     unsigned int dci, unsigned int residue, uint64_t pointer) {
    uint64_t segment = reg64(m, IRQ + 0x10);
    CHECK(*reg(m, IRQ + 8) == 1 && !(segment & 63));
    CHECK(read_dma64(m, segment + 8) == EVENT_COUNT);
    uint64_t events = read_dma64(m, segment);
    Trb event = {pointer, (code << 24) | residue,
                 (slot << 24) | (dci << 16) | (type << 10) | m->event_cycle};
    memcpy(dma_ptr(m, events + m->event_index * sizeof(Trb), sizeof(Trb)), &event, sizeof(event));
    if (++m->event_index == EVENT_COUNT) {
        m->event_index = 0;
        m->event_cycle ^= 1;
        m->event_wraps++;
    }
    CHECK(m->event_index != m->event_consumer);
    *reg(m, OP + 4) |= 1U << 3;
}

static Trb fetch(Model* m, Ring* ring, uint64_t* address) {
    Trb trb;
    *address = ring->next;
    memcpy(&trb, dma_ptr(m, *address, sizeof(trb)), sizeof(trb));
    CHECK((trb.control & 1) == ring->cycle);
    if (TYPE(trb) == 6) {
        CHECK((trb.control & 0x12) == 2 && !(trb.parameter & 63));
        const uint8_t zero[4 * 16] = {0};
        CHECK(!memcmp(dma_ptr(m, *address + sizeof(Trb), sizeof(zero)), zero, sizeof(zero)));
        ring->next = trb.parameter;
        ring->cycle ^= 1;
        ring->wraps++;
        *address = ring->next;
        memcpy(&trb, dma_ptr(m, *address, sizeof(trb)), sizeof(trb));
        CHECK((trb.control & 1) == ring->cycle && TYPE(trb) != 6);
    }
    ring->next += sizeof(Trb);
    return trb;
}

static void load_endpoint(Model* m, Slot* slot, unsigned int dci, const uint32_t* ep) {
    uint64_t pointer = ep[2] | ((uint64_t)ep[3] << 32);
    CHECK((ep[1] & 6) == 6 && (pointer & 63) == 1);
    CHECK(((ep[1] >> 3) & 7) == (dci == 1 ? 4U : 7U));
    slot->ring[dci] = (Ring){pointer & ~63ULL, 1, 0};
    slot->packet[dci] = PACKET(ep);
    if (dci != 1) CHECK(ep[4] == ((PACKET(ep) << 16) | PACKET(ep)));
    (void)dma_ptr(m, pointer & ~63ULL, 16);
}

static void command(Model* m) {
    uint64_t address;
    Trb trb = fetch(m, &m->command, &address);
    unsigned int id = trb.control >> 24, dci = (trb.control >> 16) & 31;
    CHECK(id < 5);
    Slot* slot = &m->slot[id];
    switch (TYPE(trb)) {
    case 23:
        CHECK(!id && !trb.parameter);
        break;
    case 9:
        CHECK(!id);
        for (id = 1; id < 5 && m->slot[id].enabled; id++) {}
        CHECK(id < 5);
        m->slot[id].enabled = 1;
        break;
    case 10:
        CHECK(id && slot->enabled);
        m->disable_count++;
        *slot = (Slot){0};
        break;
    case 11:
    case 12:
    case 13: {
        CHECK(id && slot->enabled && !(trb.parameter & 63));
        uint32_t input[33 * 16];
        size_t bytes = 33 * m->stride * sizeof(*input);
        memcpy(input, dma_ptr(m, trb.parameter, bytes), bytes);
        CHECK(!input[0]);
        const uint32_t* slot_ctx = input + m->stride;
        unsigned int port = slot_ctx[1] >> 16;
        if (TYPE(trb) == 11) {
            CHECK(input[1] == 3 && (slot_ctx[0] >> 27) == 1);
            CHECK(port == 1 || port == 2);
            slot->port = port;
            slot->speed = (*reg(m, port_offset(port)) >> 10) & 15;
            CHECK(((slot_ctx[0] >> 20) & 15) == slot->speed && !(slot_ctx[0] & 0xfffff));
            uint64_t output = read_dma64(m, reg64(m, OP + 0x30) + 8 * id);
            CHECK(output && !(output & 63));
            memset(dma_ptr(m, output, 32 * m->stride * 4), 0x3c, 32 * m->stride * 4);
            load_endpoint(m, slot, 1, input + 2 * m->stride);
            CHECK(slot->packet[1] == (slot->speed == 3 ? 64U : 8U));
        } else if (TYPE(trb) == 13) {
            CHECK(input[1] == 2 && slot->speed == 1);
            CHECK(input[2 * m->stride + 1] == 32U << 16);
            slot->packet[1] = 32;
        } else {
            CHECK((input[1] & 3) == 1 && port == slot->port && !slot->configured);
            for (unsigned int i = 3; i < 32; i += 2) {
                if (!(input[1] & (1U << i))) continue;
                CHECK((slot_ctx[0] >> 27) >= i);
                const uint32_t* ep = input + (i + 1) * m->stride;
                CHECK(ep[0] == ((slot->speed == 3 ? 3U : 6U) << 16));
                load_endpoint(m, slot, i, ep);
            }
        }
        break;
    }
    case 14:
        CHECK(id && dci == 1 && slot->halted);
        slot->halted = 0;
        m->stall_recoveries++;
        break;
    case 15:
        CHECK(id && dci == 1 && m->drop_control);
        m->timeout_stops++;
        break;
    case 16:
        CHECK(id && dci == 1 && !slot->halted);
        slot->ring[dci].next = trb.parameter & ~15ULL;
        slot->ring[dci].cycle = (unsigned int)trb.parameter & 1;
        break;
    default:
        CHECK(0);
    }
    complete(m, 33, 1, id, 0, 0, address);
}

static void control(Model* m, unsigned int id, Slot* slot) {
    uint64_t address;
    CHECK(slot->enabled && !slot->halted);
    Trb setup = fetch(m, &slot->ring[1], &address);
    CHECK(TYPE(setup) == 2 && (setup.control & 0x60) == 0x40 && setup.status == 8);
    unsigned int type = (uint8_t)setup.parameter, request = (uint8_t)(setup.parameter >> 8);
    unsigned int length = (unsigned int)(setup.parameter >> 48);
    int in = !!(type & 0x80);
    CHECK(((setup.control >> 16) & 3) == (length ? (in ? 3U : 2U) : 0));
    if (m->drop_control) return;
    if (type == 0x21 && request == 10) {
        slot->halted = 1;
        complete(m, 32, 6, id, 1, 0, address);
        return;
    }
    int short_in = 0;
    if (length) {
        Trb data = fetch(m, &slot->ring[1], &address);
        CHECK(TYPE(data) == 3 && data.status == length && !!(data.control & 0x10000) == in);
        CHECK((data.parameter & 0xffff) + length <= 0x10000);
        uint8_t* buffer = dma_ptr(m, data.parameter, length);
        if (in) {
            CHECK(type == 0x80 && request == 6 && ((setup.parameter >> 16) & 0xffff) == 0x100);
            uint8_t descriptor[18] = {18, 1, 0x00, 0x02, 0, 0, 0, 32, 0x34, 0x12};
            if (slot->speed == 2) descriptor[7] = 8;
            if (slot->speed == 3) descriptor[7] = 64;
            unsigned int count = length < sizeof(descriptor) ? length : sizeof(descriptor);
            memcpy(buffer, descriptor, count);
            if (count < length) {
                CHECK(data.control & 4);
                complete(m, 32, 13, id, 1, length - count, address);
                short_in = 1;
            }
        } else {
            CHECK(type == 0x21 && request == 9 && length == 1 && buffer[0] == 0x05);
        }
    }
    if (!type && request == 9) {
        CHECK(slot->packet[3] && !slot->configured);
        slot->configured = 1;
    }
    Trb status = fetch(m, &slot->ring[1], &address);
    CHECK(TYPE(status) == 4 && !status.status && (status.control & 32));
    CHECK(!!(status.control & 0x10000) == (!length || !in));
    if (short_in) {
        CHECK(!m->status_trb);
        m->status_trb = address;
        m->status_slot = id;
        m->status_at = m->time + 2000;
    } else {
        complete(m, 32, 1, id, 1, 0, address);
    }
    if (slot->ring[1].wraps) m->ep0_wraps = slot->ring[1].wraps;
}

static void interrupt(Model* m, Slot* slot, unsigned int dci) {
    CHECK(slot->configured && slot->packet[dci] && !slot->pending[dci]);
    Trb trb = fetch(m, &slot->ring[dci], &slot->pending[dci]);
    CHECK(TYPE(trb) == 1 && (trb.control & 32) && trb.status == slot->packet[dci]);
    slot->buffer[dci] = trb.parameter;
    if (slot->ring[dci].wraps) m->interrupt_wraps = slot->ring[dci].wraps;
}

static void deliver(Model* m, unsigned int port, unsigned int dci, const uint8_t* report,
                    unsigned int length) {
    for (unsigned int id = 1; id < 5; id++) {
        Slot* slot = &m->slot[id];
        if (slot->port != port) continue;
        CHECK(slot->pending[dci] && length <= slot->packet[dci]);
        memcpy(dma_ptr(m, slot->buffer[dci], slot->packet[dci]), report, length);
        unsigned int residue = slot->packet[dci] - length;
        complete(m, 32, residue ? 13 : 1, id, dci, residue, slot->pending[dci]);
        slot->pending[dci] = 0;
        return;
    }
    CHECK(0);
}

static uint32_t read32(void* context, uintptr_t address) {
    Model* m = context;
    if (address >= MMU_PCIE_DEVICE_BASE) {
        CHECK(m->firmware && (m->pcie[0x8004 / 4] & 2));
        return *reg(m, (unsigned int)(address - MMU_PCIE_DEVICE_BASE));
    }
    unsigned int offset = (unsigned int)(address - HOST);
    CHECK(address >= HOST && offset < sizeof(m->pcie));
    if (offset == 0x4068) return m->pcie[0x9210 / 4] & 1 ? 0x80 : 0xb0;
    if (offset == 0x8010 && m->pcie[offset / 4] == 0xffffffffU) return 0xfffff004;
    return m->pcie[offset / 4] | (offset == 0x8010 ? 4U : 0);
}

static void write32(void* context, uintptr_t address, uint32_t value) {
    Model* m = context;
    if (address < MMU_PCIE_DEVICE_BASE) {
        unsigned int offset = (unsigned int)(address - HOST);
        CHECK(address >= HOST && offset < sizeof(m->pcie));
        if ((offset == 4 || offset == 0x8004) && (value & 4)) {
            unsigned int size = m->dma_base > SHIFTED ? 18U : 17U;
            CHECK(m->pcie[0x4034 / 4] == size && (m->pcie[0x4008 / 4] >> 27) == size);
            CHECK(m->pcie[0x4038 / 4] == (m->dma_base > SHIFTED ? 2U : 0U));
        }
        m->pcie[offset / 4] = value;
        return;
    }
    unsigned int offset = (unsigned int)(address - MMU_PCIE_DEVICE_BASE);
    CHECK(m->firmware && (m->pcie[0x8004 / 4] & 6) == 6);
    uint32_t* place = reg(m, offset);
    if (offset == OP) {
        CHECK(!(value & ~3U));
        m->running = value == 1;
        *reg(m, OP + 4) = !m->running;
        if (value == 2) {
            memset(m->slot, 0, sizeof(m->slot));
            m->resets++;
            m->event_index = m->event_consumer = 0;
            m->event_cycle = 1;
            value = 0;
        }
    } else if (offset == OP + 4 || offset == IRQ) {
        CHECK(!(value & ~(offset == IRQ ? 1U : 0x18U)));
        *place &= ~value;
        return;
    } else if (offset >= PORTSC && offset <= port_offset(3) && !(offset & 15)) {
        unsigned int port = 1 + (offset - PORTSC) / 16;
        CHECK(!(value & ~((0x7fU << 17) | (1U << 9) | (1U << 4))));
        *place &= ~(value & (0x7fU << 17));
        if (value & (1U << 4)) {
            CHECK((*place & 1) && (value & (1U << 9)));
            *place |= 2 | (1U << 21);
            complete(m, 34, 1, 0, 0, 0, (uint64_t)port << 24);
        }
        *place |= value & (1U << 9);
        return;
    }
    *place = value;
    if (offset == OP + 0x1c) {
        uint64_t crcr = reg64(m, OP + 0x18);
        CHECK((crcr & 63) == 1);
        m->command = (Ring){crcr & ~63ULL, 1, 0};
    }
    if (offset == IRQ + 0x1c && m->running) {
        uint64_t events = read_dma64(m, reg64(m, IRQ + 0x10));
        unsigned int next = (m->event_consumer + 1) % EVENT_COUNT;
        CHECK(reg64(m, IRQ + 0x18) == ((events + 16 * next) | 8));
        m->event_consumer = next;
    }
    if (offset == DOORBELL) {
        CHECK(m->running && !value);
        command(m);
    } else if (offset > DOORBELL && offset <= DOORBELL + 16) {
        unsigned int id = (offset - DOORBELL) / 4;
        CHECK(m->running && value && value < 32);
        if (value == 1) control(m, id, &m->slot[id]);
        else interrupt(m, &m->slot[id], value);
    }
}

static uint32_t time_us(void* context) {
    Model* m = context;
    m->time += 500;
    if (m->status_trb && m->time >= m->status_at) {
        complete(m, 32, 1, m->status_slot, 1, 0, m->status_trb);
        m->status_trb = 0;
    }
    return m->time;
}
static int owned(void* context) { return context == &model; }

static uint64_t physical(void* context, const void* address) {
    Model* m = context;
    if (!m->cpu_base) m->cpu_base = (uintptr_t)address;
    return PHYS + ((uintptr_t)address - m->cpu_base);
}

static void cache(void* context, void* address, size_t bytes, enum HwCache operation) {
    Model* m = context;
    uintptr_t offset = (uintptr_t)address - m->cpu_base;
    CHECK(offset <= MEMORY_BYTES && bytes <= MEMORY_BYTES - offset);
    if (operation == HW_INVALIDATE) memcpy(address, m->memory + offset, bytes);
    else memcpy(m->memory + offset, address, bytes);
}

static int property(void* context, uint32_t tag, uint32_t* words, unsigned int count) {
    Model* m = context;
    CHECK(tag == 0x00030058 && count == 1 && words[0] == 0x100000);
    CHECK(m->pcie[0x8004 / 4] == 0x406);
    m->firmware_calls++;
    m->firmware = 1;
    return HW_OK;
}

static void setup_model(Model* m, unsigned int stride, uint64_t shift,
                        unsigned int speed1, unsigned int speed2) {
    memset(m, 0, sizeof(*m));
    m->dma_base = PHYS + shift;
    m->stride = stride;
    m->pcie[0] = 0x271114e4;
    m->pcie[0x8000 / 4] = 0x34831106;
    m->pcie[0x8008 / 4] = 0x0c033001;
    m->pcie[0x4038 / 4] = (uint32_t)(shift >> 32);
    *reg(m, 0) = 0x01000020;
    *reg(m, 4) = 0x03000120;
    *reg(m, 0x10) = ((0x500U / 4) << 16) | 1U | (stride == 16 ? 4U : 0U);
    *reg(m, 0x14) = DOORBELL;
    *reg(m, 0x18) = IRQ - 0x20;
    *reg(m, OP + 4) = 1;
    *reg(m, OP + 8) = 1;
    *reg(m, 0x500) = 0x02000402;
    *reg(m, 0x504) = 0x20425355;
    *reg(m, 0x508) = 0x0201;
    *reg(m, 0x510) = 0x03000002;
    *reg(m, 0x514) = 0x20425355;
    *reg(m, 0x518) = 0x0103;
    *reg(m, port_offset(1)) = (speed1 << 10) | 1 | (1U << 17);
    *reg(m, port_offset(2)) = (speed2 << 10) | 1 | (1U << 17);
}

static const HwOps io = {&model, read32, write32, time_us, owned, cache, physical, property};

static void configure(unsigned int port, unsigned int endpoint, unsigned int packet, unsigned int interval) {
    size_t actual;
    const Pi4UsbSetup set_config = {0, 9, 1, 0, 0};
    CHECK(pi4_usb_configure_interrupt_in(port, (uint8_t)endpoint, (uint16_t)packet,
                                        (uint8_t)interval) == HW_OK);
    CHECK(pi4_usb_control(port, &set_config, NULL, &actual) == HW_OK && !actual);
}

static void transfer_test(unsigned int stride, uint64_t shift, unsigned int speed2) {
    setup_model(&model, stride, shift, 1, speed2);
    Pi4UsbProbe probe;
    CHECK(pi4_usb_start(&io, &probe) == HW_OK && probe.stage == PI4_USB_RUNNING);
    CHECK(model.firmware_calls == 1 && model.resets == 1);
    CHECK(pi4_usb_port_count() == 3 && pi4_usb_port_connected(3) == HW_UNSUPPORTED);
    CHECK(pi4_usb_open_port(1) == HW_OK && pi4_usb_open_port(2) == HW_OK);
    CHECK(model.slot[1].packet[1] == 32 && model.slot[2].packet[1] == (speed2 == 3 ? 64U : 8U));

    /* Two slots, including two interrupt endpoints on the first (composite) device. */
    CHECK(pi4_usb_configure_interrupt_in(1, 0x82, 4, 10) == HW_OK);
    configure(1, 0x81, 8, 10);
    configure(2, 0x81, 8, speed2 == 3 ? 4U : 10U);
    size_t actual;
    uint8_t reply[64], report[8], second[8];
    const Pi4UsbSetup set_idle = {0x21, 10, 0, 0, 0};
    const Pi4UsbSetup set_leds = {0x21, 9, 0x200, 0, 1};
    const Pi4UsbSetup get_device = {0x80, 6, 0x100, 0, sizeof(reply)};
    uint8_t leds = 5;
    CHECK(pi4_usb_control(1, &set_idle, NULL, &actual) == PI4_USB_STALL && !actual);
    CHECK(model.stall_recoveries == 1);
    CHECK(pi4_usb_control(1, &set_leds, &leds, &actual) == HW_OK && actual == 1);
    CHECK(pi4_usb_interrupt_in(1, 0x81, report, sizeof(report), &actual) == HW_BUSY);
    CHECK(pi4_usb_interrupt_in(1, 0x82, report, sizeof(report), &actual) == HW_BUSY);
    CHECK(pi4_usb_interrupt_in(2, 0x81, second, sizeof(second), &actual) == HW_BUSY);
    for (unsigned int i = 0; i < 34; i++) {
        uint8_t mouse[3] = {(uint8_t)i, 0xfe, 0x04};
        uint8_t keyboard[8] = {2, 0, (uint8_t)i};
        deliver(&model, 2, 3, mouse, sizeof(mouse));
        deliver(&model, 1, 3, keyboard, sizeof(keyboard));
        deliver(&model, 1, 5, mouse, sizeof(mouse));
        memset(reply, 0xee, sizeof(reply));
        CHECK(pi4_usb_control(1, &get_device, reply, &actual) == HW_OK);
        CHECK(!model.status_trb && actual == 18 && reply[7] == 32 && reply[18] == 0xee);
        CHECK(pi4_usb_interrupt_in(1, 0x81, report, sizeof(report), &actual) == HW_OK);
        CHECK(actual == sizeof(keyboard) && !memcmp(report, keyboard, actual));
        CHECK(pi4_usb_interrupt_in(1, 0x82, report, sizeof(report), &actual) == HW_OK);
        CHECK(actual == sizeof(mouse) && !memcmp(report, mouse, actual));
        CHECK(pi4_usb_interrupt_in(2, 0x81, second, sizeof(second), &actual) == HW_OK);
        CHECK(actual == sizeof(mouse) && !memcmp(second, mouse, actual));
        CHECK(pi4_usb_interrupt_in(2, 0x81, second, sizeof(second), &actual) == HW_BUSY && !actual);
    }
    CHECK(model.ep0_wraps >= 2 && model.interrupt_wraps >= 2 && model.event_wraps >= 2);

    /* A reconnect before the next polling tick still invalidates the old slot. */
    *reg(&model, port_offset(2)) = (speed2 << 10) | 1 | (1U << 9) | (1U << 17);
    complete(&model, 34, 1, 0, 0, 0, 2U << 24);
    CHECK(pi4_usb_interrupt_in(2, 0x81, second, sizeof(second), &actual) == HW_OFF);
    CHECK(pi4_usb_close_port(2) == HW_OK && pi4_usb_close_port(2) == HW_OK);
    for (unsigned int i = 0; i < 5; i++) {
        CHECK(pi4_usb_open_port(2) == HW_OK);
        CHECK(pi4_usb_close_port(2) == HW_OK);
    }
    CHECK(model.command.wraps >= 1);
    model.drop_control = 1;
    CHECK(pi4_usb_control(1, &get_device, reply, &actual) == HW_TIMEOUT && !actual);
    CHECK(model.timeout_stops == 1 && model.disable_count == 7 && pi4_usb_port_count() == 3);
    model.drop_control = 0;
    CHECK(pi4_usb_open_port(1) == HW_OK);
    CHECK(pi4_usb_stop() == HW_OK && !pi4_usb_port_count() && model.resets == 2);
    CHECK((model.pcie[0x9210 / 4] & 1) && !(model.pcie[0x4034 / 4] & 31));
}

int main(void) {
    transfer_test(8, 0, 2);
    transfer_test(16, SHIFTED, 3);
    puts("xHCI direct USB 2 transfers passed (32-/64-byte contexts, legacy/shifted DMA)");
    return 0;
}
