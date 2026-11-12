#include <vantage/drivers/genet.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

#define REV 0x0000U
#define PORT_CTRL 0x0004U
#define OOB_CTRL 0x008cU
#define INTRL2_0_MASK 0x0210U
#define RBUF_CTRL 0x0300U
#define UMAC_CMD 0x0808U
#define UMAC_MAC0 0x080cU
#define UMAC_MAC1 0x0810U
#define MAX_FRAME 0x0814U
#define MDIO 0x0e14U
#define MDF_CTRL 0x0e50U
#define MDF_ADDR 0x0e54U
#define RX_DESC 0x2000U
#define RX_RING 0x3000U
#define RDMA 0x3040U
#define TX_DESC 0x4000U
#define TX_RING 0x5000U
#define TDMA 0x5040U
#define BUS 0x120000000ULL /* above 4 GiB: descriptors need their HI word */
#define SOP_EOP 0x6000U
#define RGMII_LINK 0x10U

static Genet genet;
typedef struct {
    uint32_t regs[0x10000 / 4];
    unsigned char shadow[sizeof(Genet)]; /* what the device sees of the context */
    unsigned char sent[32][GENET_BUFFER_BYTES];
    unsigned int sent_bytes[32], sent_count;
    uint16_t phy[32], partner, partner_gb, rx_prod, tx_cons, discards;
    uint32_t clock, rev, reads, writes, mdio_ops, properties;
    unsigned int rx_slot, tx_slot, reset_reads, busy_reads;
    int owner, cable, latched, restarted, mdio_stuck, first_fail, dma_stuck, mac_fail, sticky;
    int tx_hold, crc_forced, rx_handed, last_cache;
    uint8_t mac[6];
} Model;
static Model model;
#define REG(offset) model.regs[(offset) / 4]

static void phy_defaults(void) {
    memset(model.phy, 0, sizeof(model.phy));
    model.phy[0] = 0x1140; model.phy[2] = 0x600d; model.phy[3] = 0x84a2;
    model.phy[4] = 0x0de1; model.phy[9] = 0x0000;
    model.restarted = 0;
}

static void reset(void) {
    static const uint8_t mac[6] = {0xdc, 0xa6, 0x32, 0x12, 0x34, 0x56};
    memset(&model, 0, sizeof(model));
    memset(model.shadow, 0xa5, sizeof(model.shadow));
    memcpy(model.mac, mac, 6);
    model.owner = 1; model.rev = 0x06000000U; model.last_cache = -1;
    REG(RDMA + 8) = REG(TDMA + 8) = 1; /* DMA_STATUS: disabled */
    phy_defaults();
}

static unsigned char* device(uint64_t bus, unsigned int bytes) {
    CHECK(bus >= BUS && bus - BUS + bytes <= sizeof(genet));
    unsigned char* p = model.shadow + (bus - BUS);
    CHECK(p >= model.shadow + offsetof(Genet, rx) && p + bytes <= model.shadow + offsetof(Genet, tx) + sizeof(genet.tx));
    return p;
}

static uint16_t bmsr(void) {
    uint16_t status = 0x7949;
    int common = (model.partner & model.phy[4] & 0x03e0U) || (model.partner_gb & ((uint32_t)model.phy[9] << 2));
    if (model.cable && !model.latched && model.restarted && (model.phy[0] & 0x1000U) && common) status |= 0x0024;
    model.latched = 0;
    return status;
}

static void mdio(uint32_t command) {
    unsigned int address = (command >> 21) & 31U, reg = (command >> 16) & 31U, op = (command >> 26) & 3U;
    uint32_t data = command & 0xffffU, fail = 0;
    model.mdio_ops++;
    if (op == 2) {
        if (address != 1 || model.first_fail) { fail = 1U << 28; model.first_fail = 0; }
        else if (reg == 0 && model.reset_reads) { model.reset_reads--; data = 0x8000U | model.phy[0]; }
        else if (reg == 1) data = bmsr();
        else if (reg == 5) data = model.partner;
        else if (reg == 10) data = model.partner_gb;
        else data = model.phy[reg];
    } else {
        CHECK(op == 1 && address == 1);
        if (reg == 0 && (data & 0x8000U)) { phy_defaults(); if (model.reset_reads < 1000) model.reset_reads = 3; }
        else if (reg == 0) { model.phy[0] = (uint16_t)(data & ~0x0200U); if (data & 0x0200U) model.restarted = 1; }
        else model.phy[reg] = (uint16_t)data;
    }
    REG(MDIO) = (command & 0x1fff0000U) | fail | (op == 2 ? data : 0) | (1U << 29);
    model.busy_reads = 1;
}

static unsigned int ring_size(uint32_t ring) { return (REG(ring + 0x1c) + 1) / 3; }

static int tx_enabled(void) {
    return (REG(TDMA + 4) & 0x20001U) == 0x20001U && (REG(TDMA) & 0x10000U) && (REG(UMAC_CMD) & 1U) &&
           (REG(OOB_CTRL) & RGMII_LINK);
}

static void tx_run(void) {
    while (!model.tx_hold && tx_enabled() && model.tx_cons != (uint16_t)REG(TX_RING + 0x0c)) {
        CHECK(REG(TX_RING + 0x14) == 0 && model.tx_slot < ring_size(TX_RING));
        uint32_t desc = TX_DESC + model.tx_slot * 12;
        uint32_t status = REG(desc);
        unsigned int bytes = status >> 16;
        CHECK((status & 0xffffU) == (SOP_EOP | 0x40U | (0x3fU << 7)));
        CHECK(bytes >= 60 && bytes <= GENET_FRAME_MAX);
        unsigned char* data = device(((uint64_t)REG(desc + 8) << 32) | REG(desc + 4), bytes);
        unsigned int slot = model.sent_count++ % 32;
        memcpy(model.sent[slot], data, bytes);
        model.sent_bytes[slot] = bytes;
        model.tx_cons++;
        model.tx_slot = (model.tx_slot + 1) % ring_size(TX_RING);
    }
}

static uint32_t read32(void* context, uintptr_t address) {
    (void)context;
    CHECK(address >= GENET_BASE && address < GENET_BASE + 0x10000 && !(address & 3));
    uint32_t offset = (uint32_t)(address - GENET_BASE);
    model.reads++;
    if (offset == REV) return model.rev;
    if (offset == MDIO) {
        if (model.mdio_stuck) return REG(MDIO) | (1U << 29);
        if (model.busy_reads) { model.busy_reads--; return REG(MDIO); }
        return REG(MDIO) &= ~(1U << 29);
    }
    if (offset == RDMA + 8 || offset == TDMA + 8) return model.dma_stuck ? 0U : (uint32_t)!(REG(offset - 4) & 1U);
    if (offset == RX_RING + 8) return ((uint32_t)model.discards << 16) | model.rx_prod;
    if (offset == TX_RING + 8) return model.tx_cons;
    if (offset == UMAC_CMD && model.crc_forced) return REG(offset) | 0x40U;
    return REG(offset);
}

static void write32(void* context, uintptr_t address, uint32_t value) {
    (void)context;
    CHECK(address >= GENET_BASE && address < GENET_BASE + 0x10000 && !(address & 3));
    uint32_t offset = (uint32_t)(address - GENET_BASE);
    model.writes++;
    if (offset == MDIO) {
        if (model.mdio_stuck) { REG(MDIO) = value; return; }
        if (value & (1U << 29)) mdio(value);
        else REG(MDIO) = value;
        return;
    }
    if (offset == RX_RING + 8) { if (!model.sticky) model.rx_prod = (uint16_t)value; return; }
    if (offset == TX_RING + 8) { if (!model.sticky) model.tx_cons = (uint16_t)value; return; }
    REG(offset) = value;
    if (offset == RX_RING || offset == RX_RING + 0x2c) model.rx_slot = value / 3;
    if (offset == TX_RING || offset == TX_RING + 0x2c) model.tx_slot = value / 3;
    if (offset == RX_RING + 0x0c && (REG(RDMA + 4) & 1U))
        CHECK((uint16_t)(model.rx_prod - value) <= ring_size(RX_RING));
    if (offset == RDMA + 4 && (value & 1U)) {
        /* Buffers go to the device with no dirty line that could later overwrite DMA data. */
        CHECK(!memcmp(model.shadow + offsetof(Genet, rx), genet.rx, sizeof(genet.rx)));
        model.rx_handed = 1;
    }
    if (offset == TX_RING + 0x0c && (REG(TDMA + 4) & 1U)) { CHECK(model.last_cache == HW_CLEAN); tx_run(); }
}

static uint32_t clock_us(void* context) { (void)context; model.clock += 7; return model.clock; }
static int owned(void* context) { (void)context; return model.owner; }
static uint64_t physical(void* context, const void* p) {
    (void)context;
    const unsigned char* base = (const unsigned char*)&genet;
    CHECK((const unsigned char*)p >= base && (const unsigned char*)p < base + sizeof(genet));
    return BUS + (uint64_t)((const unsigned char*)p - base);
}
static void cache(void* context, void* p, size_t bytes, enum HwCache operation) {
    (void)context;
    size_t start = (size_t)((unsigned char*)p - (unsigned char*)&genet), end = start + bytes;
    CHECK(start >= offsetof(Genet, rx) && end <= offsetof(Genet, tx) + sizeof(genet.tx) && bytes);
    start &= ~(size_t)63; end = (end + 63) & ~(size_t)63;
    unsigned char* cpu = (unsigned char*)&genet;
    if (operation != HW_INVALIDATE) memcpy(model.shadow + start, cpu + start, end - start);
    if (operation != HW_CLEAN) memcpy(cpu + start, model.shadow + start, end - start);
    model.last_cache = operation;
}
static int property(void* context, uint32_t tag, uint32_t* values, unsigned int count) {
    (void)context;
    CHECK(tag == 0x00010003U && count == 2);
    model.properties++;
    if (model.mac_fail) return HW_TIMEOUT;
    const uint8_t* m = model.mac;
    values[0] = (uint32_t)m[0] | ((uint32_t)m[1] << 8) | ((uint32_t)m[2] << 16) | ((uint32_t)m[3] << 24);
    values[1] = (uint32_t)m[4] | ((uint32_t)m[5] << 8);
    return HW_OK;
}

static const HwOps io = {NULL, read32, write32, clock_us, owned, cache, physical, property};

/* The device writes a frame into the next RX buffer: two pad bytes first. */
static int deliver(const unsigned char* frame, unsigned int bytes, uint32_t flags) {
    if ((REG(RDMA + 4) & 0x20001U) != 0x20001U || !(REG(UMAC_CMD) & 2U)) return 0;
    CHECK(model.rx_handed && REG(RX_RING + 0x14) == 0);
    if ((uint16_t)(model.rx_prod - (uint16_t)REG(RX_RING + 0x0c)) >= ring_size(RX_RING)) { model.discards++; return 0; }
    uint32_t desc = RX_DESC + model.rx_slot * 12;
    CHECK(bytes + 2 <= GENET_BUFFER_BYTES);
    unsigned char* data = device(((uint64_t)REG(desc + 8) << 32) | REG(desc + 4), bytes + 2);
    data[0] = data[1] = 0xee;
    memcpy(data + 2, frame, bytes);
    REG(desc) = ((bytes + 2) << 16) | flags;
    model.rx_prod++;
    model.rx_slot = (model.rx_slot + 1) % ring_size(RX_RING);
    return 1;
}

static void pattern(unsigned char* frame, unsigned int bytes, unsigned int seed) {
    for (unsigned int i = 0; i < bytes; i++) frame[i] = (unsigned char)(seed * 31U + i * 7U);
}

static int poll_later(void) { model.clock += GENET_LINK_POLL_US; return genet_poll(&genet); }

static void link_up(uint16_t partner, uint16_t partner_gb) {
    model.partner = partner; model.partner_gb = partner_gb; model.cable = 1;
    CHECK(poll_later() == 1);
}

static void start(void) {
    CHECK(genet_init(&genet, &io) == HW_OK);
    link_up(0x05e1, 0x0c00);
}

static void absent_and_refused(void) {
    reset(); model.owner = 0;
    CHECK(genet_init(&genet, &io) == HW_INVALID && model.reads == 0);
    uint32_t revisions[] = {0, 0x05000000U, 0xffffffffU};
    for (unsigned int i = 0; i < 3; i++) {
        reset(); model.rev = revisions[i];
        CHECK(genet_init(&genet, &io) == HW_OFF);
        CHECK(model.reads == 1 && model.writes == 0 && model.properties == 0);
        CHECK(genet.state == GENET_OFF && genet_poll(&genet) == HW_OFF);
        unsigned char frame[64] = {0};
        uint32_t mbps = 7;
        CHECK(genet_send(&genet, frame, 64) == HW_OFF && genet_receive(&genet, frame, 64) == HW_OFF);
        CHECK(genet_carrier(&genet, &mbps) == 0 && mbps == 0);
    }
}

static void bring_up_failures(void) {
    reset(); model.mac_fail = 1;
    CHECK(genet_init(&genet, &io) == HW_TIMEOUT && genet.state == GENET_FAILED);
    CHECK(REG(RDMA + 4) == 0 && genet_poll(&genet) == HW_TIMEOUT);
    reset(); model.mac[0] = 0x01;
    CHECK(genet_init(&genet, &io) == HW_PROTOCOL);
    reset(); memset(model.mac, 0, 6);
    CHECK(genet_init(&genet, &io) == HW_PROTOCOL);
    reset(); model.dma_stuck = 1;
    CHECK(genet_init(&genet, &io) == HW_TIMEOUT && model.mdio_ops == 0);
    reset(); model.mdio_stuck = 1;
    CHECK(genet_init(&genet, &io) == HW_TIMEOUT && model.clock < 50000);
    CHECK(REG(RDMA + 4) == 0 && REG(TDMA + 4) == 0 && !(REG(UMAC_CMD) & 3U));
    reset(); model.phy[3] = 0x5c90;
    model.phy[2] = 0x0143;
    CHECK(genet_init(&genet, &io) == HW_UNSUPPORTED && REG(RDMA + 4) == 0);
    reset(); model.reset_reads = 0xffffffffU;
    CHECK(genet_init(&genet, &io) == HW_TIMEOUT);
    CHECK(model.clock >= 500000 && model.clock < 2000000);
}

static void configuration(void) {
    reset(); model.first_fail = 1; model.sticky = 1; model.rx_prod = 0xfff0; model.tx_cons = 0xfffa;
    CHECK(genet_init(&genet, &io) == HW_OK && genet.state == GENET_READY);
    CHECK(!memcmp(genet.mac, model.mac, 6) && genet.phy_id == 0x600d84a2U);
    CHECK(REG(UMAC_MAC0) == 0xdca63212U && REG(UMAC_MAC1) == 0x3456U);
    CHECK(REG(MDF_ADDR) == 0xffffU && REG(MDF_ADDR + 4) == 0xffffffffU);
    CHECK(REG(MDF_ADDR + 8) == 0xdca6U && REG(MDF_ADDR + 12) == 0x32123456U && REG(MDF_CTRL) == 0x18000U);
    CHECK(REG(PORT_CTRL) == 3 && (REG(OOB_CTRL) & 0x10070U) == 0x10040U);
    CHECK(REG(UMAC_CMD) == 0x10000103U && REG(MAX_FRAME) == 1536 && (REG(RBUF_CTRL) & 3U) == 2U);
    CHECK(REG(INTRL2_0_MASK) == 0xffffffffU);
    CHECK(model.phy[4] == 0x01e1 && model.phy[9] == 0x0300 && (model.phy[0] & 0x1000U) && model.restarted);
    CHECK(REG(RDMA + 4) == 0x20001U && REG(TDMA + 4) == 0x20001U && REG(RDMA) == 0x10000U && REG(TDMA) == 0x10000U);
    CHECK(REG(RX_RING + 0x10) == ((GENET_RX_RING << 16) | 2048U) && REG(RX_RING + 0x1c) == GENET_RX_RING * 3 - 1);
    CHECK(REG(TX_RING + 0x10) == ((GENET_TX_RING << 16) | 2048U) && REG(TX_RING + 0x1c) == GENET_TX_RING * 3 - 1);
    CHECK(REG(RX_DESC + 8) == 1 && REG(RX_DESC + 4) == (uint32_t)(offsetof(Genet, rx) + 0x20000000U));
    CHECK(REG(RX_DESC + (GENET_RX_RING - 1) * 12) == ((2048U << 16) | 0x8000U));
    CHECK(REG(RX_RING + 0x0c) == 0xfff0U && REG(TX_RING + 0x0c) == 0xfffaU);
    uint32_t mbps = 1;
    unsigned char frame[64] = {0};
    CHECK(genet_carrier(&genet, &mbps) == 0 && mbps == 0);
    CHECK(genet_send(&genet, frame, 64) == HW_OFF && genet_receive(&genet, frame, 64) == 0);
    CHECK(genet_poll(&genet) == 0);
}

static void expect_link(uint32_t mbps, int full) {
    uint32_t reported = 0;
    CHECK(genet_carrier(&genet, &reported) == 1 && reported == mbps && genet.full_duplex == full);
    uint32_t speed = mbps == 1000 ? 2U : mbps == 100 ? 1U : 0U;
    CHECK(((REG(UMAC_CMD) >> 2) & 3U) == speed && ((REG(UMAC_CMD) & 0x400U) != 0) == !full);
    CHECK((REG(UMAC_CMD) & 3U) == 3U && (REG(OOB_CTRL) & RGMII_LINK));
}

static void cable_drop(void) {
    model.cable = 0; model.latched = 1;
    CHECK(poll_later() == 1);
    uint32_t mbps = 5;
    CHECK(genet_carrier(&genet, &mbps) == 0 && mbps == 0 && !(REG(OOB_CTRL) & RGMII_LINK));
}

static void link_changes(void) {
    reset();
    CHECK(genet_init(&genet, &io) == HW_OK);
    CHECK(poll_later() == 0);
    link_up(0x05e1, 0x0c00);
    expect_link(1000, 1);
    uint32_t ops = model.mdio_ops;
    CHECK(genet_poll(&genet) == 0 && genet_poll(&genet) == 0 && model.mdio_ops == ops);
    CHECK(poll_later() == 0 && model.mdio_ops == ops + 1);
    cable_drop();
    unsigned char frame[64] = {0};
    CHECK(genet_send(&genet, frame, 64) == HW_OFF);
    link_up(0x00a1, 0x0400);
    expect_link(1000, 0);
    cable_drop();
    link_up(0x00a1, 0);
    expect_link(100, 0);
    cable_drop();
    /* While down, a stale latched-low status must not hide a returned link. */
    model.latched = 1;
    link_up(0x0061, 0);
    expect_link(10, 1);
    /* A link drop shorter than one poll is still seen through the latch. */
    model.latched = 1;
    CHECK(poll_later() == 1 && !genet.link);
    CHECK(poll_later() == 1);
    expect_link(10, 1);
    model.partner_gb = 0x8c00;
    model.partner = 0x05e1;
    model.latched = 1;
    CHECK(poll_later() == 1 && !genet.link);
    CHECK(poll_later() == 0 && !genet.link);
}

static void transmit(void) {
    reset(); model.sticky = 1; model.tx_cons = 0xfffa;
    start();
    unsigned char frame[GENET_FRAME_MAX + 1];
    CHECK(genet_send(&genet, frame, 13) == HW_INVALID && genet_send(&genet, frame, GENET_FRAME_MAX + 1) == HW_INVALID);
    CHECK(genet_send(&genet, NULL, 60) == HW_INVALID);
    for (unsigned int i = 0; i < 200; i++) {
        unsigned int bytes = i == 0 ? 14 : i == 1 ? GENET_FRAME_MAX : 14 + (i * 37U) % (GENET_FRAME_MAX - 13);
        pattern(frame, bytes, i);
        CHECK(genet_send(&genet, frame, bytes) == HW_OK);
        unsigned int slot = (model.sent_count - 1) % 32, wire = bytes < 60 ? 60 : bytes;
        CHECK(model.sent_count == i + 1 && model.sent_bytes[slot] == wire);
        CHECK(!memcmp(model.sent[slot], frame, bytes));
        for (unsigned int k = bytes; k < wire; k++) CHECK(model.sent[slot][k] == 0);
        if (i % 7 == 0) CHECK(genet_poll(&genet) == 1);
    }
    CHECK(genet.tx_frames == 200 && (uint16_t)(model.tx_cons - 0xfffaU) == 200);
    /* Completion is reclaimed before a full ring is reported. */
    model.tx_hold = 1;
    unsigned int before = model.sent_count;
    for (unsigned int i = 0; i < GENET_TX_RING; i++) {
        pattern(frame, 100, 1000 + i);
        CHECK(genet_send(&genet, frame, 100) == HW_OK);
    }
    CHECK(genet_send(&genet, frame, 100) == HW_BUSY && genet_poll(&genet) == 0);
    model.tx_hold = 0; tx_run();
    CHECK(model.sent_count == before + GENET_TX_RING);
    for (unsigned int i = 0; i < GENET_TX_RING; i++) {
        pattern(frame, 100, 1000 + i);
        CHECK(!memcmp(model.sent[(before + i) % 32], frame, 100));
    }
    CHECK(genet_poll(&genet) == 1 && genet_send(&genet, frame, 100) == HW_OK);
}

static void receive(void) {
    reset(); model.sticky = 1; model.rx_prod = 0xfff0;
    start();
    unsigned char frame[GENET_BUFFER_BYTES], out[GENET_FRAME_MAX];
    pattern(frame, 60, 1);
    CHECK(deliver(frame, 60, SOP_EOP | 0x40U) && genet_poll(&genet) == 1);
    CHECK(genet_receive(&genet, out, sizeof(out)) == 60 && !memcmp(out, frame, 60));
    CHECK(genet_receive(&genet, out, sizeof(out)) == 0 && genet_poll(&genet) == 0);
    /* Errors, fragments and impossible lengths are dropped; good frames around them survive. */
    static const uint32_t bad[] = {SOP_EOP | 0x02U, SOP_EOP | 0x01U, SOP_EOP | 0x10U, 0x2000U, 0x4000U};
    for (unsigned int i = 0; i < 5; i++) CHECK(deliver(frame, 60, bad[i]));
    CHECK(deliver(frame, 10, SOP_EOP));
    CHECK(deliver(frame, 1600, SOP_EOP));
    pattern(frame, GENET_FRAME_MAX, 2);
    CHECK(deliver(frame, GENET_FRAME_MAX, SOP_EOP));
    CHECK(genet_receive(&genet, out, sizeof(out)) == (int)GENET_FRAME_MAX && !memcmp(out, frame, GENET_FRAME_MAX));
    CHECK(genet.rx_dropped == 7 && genet_receive(&genet, out, sizeof(out)) == 0);
    /* The ring wraps many times, and the 16-bit indices wrap from 0xfff0. */
    for (unsigned int batch = 0; batch < 20; batch++) {
        for (unsigned int i = 0; i < 20; i++) {
            pattern(frame, 64 + i, batch * 100 + i);
            CHECK(deliver(frame, 64 + i, SOP_EOP));
        }
        for (unsigned int i = 0; i < 20; i++) {
            pattern(frame, 64 + i, batch * 100 + i);
            CHECK(genet_receive(&genet, out, sizeof(out)) == (int)(64 + i) && !memcmp(out, frame, 64 + i));
        }
    }
    CHECK(genet.rx_frames == 402);
    /* A full ring makes the device discard; the queued frames stay intact. */
    for (unsigned int i = 0; i < GENET_RX_RING + 8; i++) {
        pattern(frame, 80, 5000 + i);
        CHECK(deliver(frame, 80, SOP_EOP) == (i < GENET_RX_RING));
    }
    CHECK(model.discards == 8 && genet_poll(&genet) == 1);
    for (unsigned int i = 0; i < GENET_RX_RING; i++) {
        pattern(frame, 80, 5000 + i);
        CHECK(genet_receive(&genet, out, sizeof(out)) == 80 && !memcmp(out, frame, 80));
    }
    CHECK(genet_receive(&genet, out, sizeof(out)) == 0);
    CHECK(deliver(frame, 200, SOP_EOP) && genet_receive(&genet, out, 100) == HW_NO_SPACE);
    CHECK(genet_receive(&genet, out, sizeof(out)) == 0);
    /* Hardware that forwards the FCS reports it in the length. */
    model.crc_forced = 1;
    CHECK(genet_init(&genet, &io) == HW_OK && genet.crc_forward);
    link_up(0x05e1, 0x0c00);
    pattern(frame, 64, 9);
    CHECK(deliver(frame, 68, SOP_EOP) && genet_receive(&genet, out, sizeof(out)) == 64);
    CHECK(!memcmp(out, frame, 64));
}

static void device_lost(void) {
    unsigned char frame[64] = {0};
    reset();
    start();
    model.rx_prod = (uint16_t)(model.rx_prod + GENET_RX_RING + 1);
    CHECK(genet_poll(&genet) == HW_PROTOCOL && genet.state == GENET_FAILED);
    CHECK(REG(RDMA + 4) == 0 && REG(TDMA + 4) == 0 && !(REG(UMAC_CMD) & 3U) && !(REG(OOB_CTRL) & RGMII_LINK));
    CHECK(genet_poll(&genet) == HW_PROTOCOL && genet_send(&genet, frame, 64) == HW_PROTOCOL);
    CHECK(genet_receive(&genet, frame, 64) == HW_PROTOCOL && genet_carrier(&genet, NULL) == 0);

    reset();
    start();
    model.tx_cons = (uint16_t)(model.tx_cons + 1);
    CHECK(genet_poll(&genet) == HW_PROTOCOL);

    reset();
    start();
    model.mdio_stuck = 1;
    uint32_t begin = model.clock;
    CHECK(poll_later() >= 0 && poll_later() >= 0 && poll_later() == HW_TIMEOUT);
    CHECK(model.clock - begin < 3 * GENET_LINK_POLL_US + 100000U && genet.state == GENET_FAILED);

    /* MDIO recovers before the third consecutive failure: the link stays. */
    reset();
    start();
    model.mdio_stuck = 1;
    CHECK(poll_later() == 0 && poll_later() == 0);
    model.mdio_stuck = 0;
    CHECK(poll_later() == 0 && genet.link && genet.state == GENET_READY);

    reset();
    start();
    model.tx_hold = 1;
    CHECK(genet_send(&genet, frame, 64) == HW_OK && genet_poll(&genet) == 0);
    model.clock += GENET_TX_STALL_US / 2;
    CHECK(genet_poll(&genet) >= 0);
    model.clock += GENET_TX_STALL_US / 2;
    CHECK(genet_poll(&genet) == HW_TIMEOUT);

    /* No stall is counted while the link is down. */
    reset();
    start();
    model.tx_hold = 1;
    CHECK(genet_send(&genet, frame, 64) == HW_OK);
    cable_drop();
    model.clock += 3 * GENET_TX_STALL_US;
    CHECK(genet_poll(&genet) == 0);
    model.tx_hold = 0;
    link_up(0x05e1, 0x0c00);
    tx_run();
    CHECK(genet_poll(&genet) == 1 && genet.state == GENET_READY);
}

int main(void) {
    absent_and_refused();
    bring_up_failures();
    configuration();
    link_changes();
    transmit();
    receive();
    device_lost();
    puts("PASS: GENET probe, MAC and filter setup, PHY negotiation and link changes, TX/RX rings, "
         "wrap, error drops, cache hand-over, MDIO and DMA failures");
    return 0;
}
