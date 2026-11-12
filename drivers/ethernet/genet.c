/* BCM2711 GENET v5 MAC with the BCM54213PE PHY on the UniMAC MDIO bus, polled.
 * Register layout and bring-up order follow U-Boot drivers/net/bcmgenet.c
 * (GPL-2.0+) and Circle lib/bcm54213.cpp (GPL-3.0); Linux v6.12
 * drivers/net/ethernet/broadcom/genet was consulted for behaviour only.
 * In the Pi 4 firmware device tree the scb bus places 0x7d580000 at
 * 0xfd580000 and its dma-ranges map RAM one to one, so descriptors take ARM
 * physical addresses (40 bits, split into LO/HI words). */
#include <vantage/drivers/genet.h>

#define SYS_REV_CTRL 0x0000U
#define SYS_PORT_CTRL 0x0004U
#define SYS_RBUF_FLUSH_CTRL 0x0008U
#define EXT_RGMII_OOB_CTRL 0x008cU
#define INTRL2_0 0x0200U
#define INTRL2_1 0x0240U
#define INTRL2_CLEAR 0x08U
#define INTRL2_MASK_SET 0x10U
#define RBUF_CTRL 0x0300U
#define RBUF_TBUF_SIZE_CTRL 0x03b4U
#define UMAC_CMD 0x0808U
#define UMAC_MAC0 0x080cU
#define UMAC_MAC1 0x0810U
#define UMAC_MAX_FRAME_LEN 0x0814U
#define UMAC_TX_FLUSH 0x0b34U
#define UMAC_MIB_CTRL 0x0d80U
#define MDIO_CMD 0x0e14U
#define UMAC_MDF_CTRL 0x0e50U
#define UMAC_MDF_ADDR 0x0e54U
#define RX_DESC 0x2000U
#define RX_RING16 0x3000U
#define RDMA 0x3040U
#define TX_DESC 0x4000U
#define TX_RING16 0x5000U
#define TDMA 0x5040U
#define HFB_CTRL 0xfc00U
#define HFB_FLT_ENABLE 0xfc04U

/* Ring 16 registers. The hardware-owned index is the RX producer and the TX
 * consumer; the read/write pointers are descriptor word offsets. */
#define RING_POINTER 0x00U
#define RING_HW_INDEX 0x08U
#define RING_SW_INDEX 0x0cU
#define RING_BUF_SIZE 0x10U
#define RING_START 0x14U
#define RING_END 0x1cU
#define RING_DONE_THRESH 0x24U
#define RING_FLOW 0x28U
#define RING_POINTER2 0x2cU
#define DMA_RING_CFG 0x00U
#define DMA_CTRL 0x04U
#define DMA_STATUS 0x08U
#define DMA_BURST 0x0cU
#define DMA_INDEX2RING 0x70U

#define DMA_EN 0x1U
#define DMA_RING16_EN (1U << 17)
#define DMA_DISABLED 0x1U
#define DESC_BYTES 12U
#define DESC_OWN 0x8000U
#define DESC_EOP 0x4000U
#define DESC_SOP 0x2000U
#define DESC_TX_QTAG (0x3fU << 7)
#define DESC_TX_APPEND_CRC 0x40U
#define DESC_RX_ERRORS 0x1fU /* overrun, CRC, RX_ER, odd nibble, too long */
#define RX_PAD 2U
#define FRAME_MIN 60U

#define CMD_TX_EN 0x1U
#define CMD_RX_EN 0x2U
#define CMD_SPEED_SHIFT 2
#define CMD_SPEED_MASK (3U << CMD_SPEED_SHIFT)
#define CMD_CRC_FWD 0x40U
#define CMD_RX_PAUSE_IGNORE 0x100U
#define CMD_HD_EN 0x400U
#define CMD_SW_RESET 0x2000U
#define CMD_LCL_LOOP_EN 0x8000U
#define CMD_TX_PAUSE_IGNORE 0x10000000U
#define RGMII_LINK 0x10U
#define OOB_DISABLE 0x20U
#define RGMII_MODE_EN 0x40U
#define ID_MODE_DIS 0x10000U
#define PORT_MODE_EXT_GPHY 3U
#define RBUF_64B_EN 0x1U
#define RBUF_ALIGN_2B 0x2U

#define MDIO_START_BUSY (1U << 29)
#define MDIO_READ_FAIL (1U << 28)
#define MDIO_RD (2U << 26)
#define MDIO_WR (1U << 26)
#define MDIO_TIMEOUT_US 10000U
#define MDIO_FAILURES 3U
#define PHY_ADDRESS 1U
#define PHY_RESET_US 500000U
#define PHY_ID 0x600d84a0U
#define PHY_ID_MASK 0xfffffff0U

#define MII_BMCR 0U
#define MII_BMSR 1U
#define MII_PHYSID1 2U
#define MII_PHYSID2 3U
#define MII_ADVERTISE 4U
#define MII_LPA 5U
#define MII_CTRL1000 9U
#define MII_STAT1000 10U
#define BMCR_RESET 0x8000U
#define BMCR_ANENABLE 0x1000U
#define BMCR_ANRESTART 0x0200U
#define BMSR_LINK 0x0004U
#define BMSR_ANEGCOMPLETE 0x0020U
#define ADVERTISE_10_100 0x01e1U
#define ADVERTISE_1000 0x0300U
#define STAT1000_FAULT 0x8000U

#define TAG_BOARD_MAC 0x00010003U

static uint32_t rd(const Genet* g, uint32_t offset) {
    return g->io->read(g->io->context, (uintptr_t)(GENET_BASE + offset));
}

static void wr(const Genet* g, uint32_t offset, uint32_t value) {
    g->io->write(g->io->context, (uintptr_t)(GENET_BASE + offset), value);
}

static void modify(const Genet* g, uint32_t offset, uint32_t clear, uint32_t set) {
    wr(g, offset, (rd(g, offset) & ~clear) | set);
}

static uint32_t now(const Genet* g) { return g->io->time_us(g->io->context); }

static int mdio(Genet* g, uint32_t command, uint16_t* value) {
    wr(g, MDIO_CMD, command | (g->phy << 21));
    modify(g, MDIO_CMD, 0, MDIO_START_BUSY);
    int result = hw_wait(g->io, (uintptr_t)(GENET_BASE + MDIO_CMD), MDIO_START_BUSY, 0, MDIO_TIMEOUT_US);
    if (result != HW_OK || !value) return result;
    uint32_t status = rd(g, MDIO_CMD);
    if (status & MDIO_READ_FAIL) return HW_IO;
    *value = (uint16_t)status;
    return HW_OK;
}

static int phy_read(Genet* g, unsigned int reg, uint16_t* value) {
    return mdio(g, MDIO_RD | ((uint32_t)reg << 16), value);
}

static int phy_write(Genet* g, unsigned int reg, uint16_t value) {
    return mdio(g, MDIO_WR | ((uint32_t)reg << 16) | value, NULL);
}

static int phy_start(Genet* g) {
    uint16_t id1 = 0, id2 = 0, control = BMCR_RESET;
    g->phy = PHY_ADDRESS;
    /* A first transaction may fail on Broadcom PHYs; only a timeout counts. */
    int result = phy_read(g, MII_BMSR, &id1);
    if (result == HW_TIMEOUT) return result;
    if ((result = phy_read(g, MII_PHYSID1, &id1)) != HW_OK ||
        (result = phy_read(g, MII_PHYSID2, &id2)) != HW_OK) return result;
    g->phy_id = ((uint32_t)id1 << 16) | id2;
    if ((g->phy_id & PHY_ID_MASK) != PHY_ID) return HW_UNSUPPORTED;
    if ((result = phy_write(g, MII_BMCR, BMCR_RESET)) != HW_OK) return result;
    uint32_t start = now(g);
    for (unsigned int i = 0; i < 100000U && (control & BMCR_RESET); i++) {
        if ((result = phy_read(g, MII_BMCR, &control)) != HW_OK) return result;
        if ((control & BMCR_RESET) && now(g) - start >= PHY_RESET_US) return HW_TIMEOUT;
    }
    if (control & BMCR_RESET) return HW_TIMEOUT;
    if ((result = phy_write(g, MII_ADVERTISE, ADVERTISE_10_100)) != HW_OK ||
        (result = phy_write(g, MII_CTRL1000, ADVERTISE_1000)) != HW_OK) return result;
    return phy_write(g, MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART);
}

/* Resolves the negotiated mode the way IEEE 802.3 clause 28 priorities do. */
static int phy_link(Genet* g, int* up, uint32_t* mbps, int* full) {
    uint16_t status = 0, gstatus = 0, gcontrol = 0, partner = 0, advert = 0;
    int result = phy_read(g, MII_BMSR, &status);
    /* Link status latches low; re-read for the current state while down. */
    if (result == HW_OK && !g->link) result = phy_read(g, MII_BMSR, &status);
    if (result != HW_OK) return result;
    *up = 0;
    if ((status & (BMSR_LINK | BMSR_ANEGCOMPLETE)) != (BMSR_LINK | BMSR_ANEGCOMPLETE)) return HW_OK;
    /* Renegotiation drops the link first, so an unbroken link keeps its mode. */
    if (g->link) { *up = 1; *mbps = g->mbps; *full = g->full_duplex; return HW_OK; }
    if ((result = phy_read(g, MII_STAT1000, &gstatus)) != HW_OK ||
        (result = phy_read(g, MII_CTRL1000, &gcontrol)) != HW_OK ||
        (result = phy_read(g, MII_LPA, &partner)) != HW_OK ||
        (result = phy_read(g, MII_ADVERTISE, &advert)) != HW_OK) return result;
    if (gstatus & STAT1000_FAULT) return HW_OK;
    uint32_t gigabit = gstatus & ((uint32_t)gcontrol << 2), common = (uint32_t)partner & advert;
    if (gigabit & 0x0c00U) { *mbps = 1000; *full = (gigabit & 0x0800U) != 0; }
    else if (common & 0x0180U) { *mbps = 100; *full = (common & 0x0100U) != 0; }
    else if (common & 0x0060U) { *mbps = 10; *full = (common & 0x0040U) != 0; }
    else return HW_OK;
    *up = 1;
    return HW_OK;
}

/* The UMAC speed also selects the RGMII transmit clock (25 or 125 MHz). */
static void mac_link(const Genet* g) {
    if (!g->link) { modify(g, EXT_RGMII_OOB_CTRL, RGMII_LINK, 0); return; }
    uint32_t speed = g->mbps == 1000 ? 2U : g->mbps == 100 ? 1U : 0U;
    modify(g, EXT_RGMII_OOB_CTRL, OOB_DISABLE, RGMII_LINK);
    modify(g, UMAC_CMD, CMD_SPEED_MASK | CMD_HD_EN,
           (speed << CMD_SPEED_SHIFT) | (g->full_duplex ? 0U : CMD_HD_EN));
}

static int fail(Genet* g, int error) {
    modify(g, UMAC_CMD, CMD_TX_EN | CMD_RX_EN, 0);
    wr(g, RDMA + DMA_CTRL, 0);
    wr(g, TDMA + DMA_CTRL, 0);
    modify(g, EXT_RGMII_OOB_CTRL, RGMII_LINK, 0);
    g->state = GENET_FAILED;
    g->error = error;
    g->link = 0;
    g->mbps = 0;
    return error;
}

/* Stops DMA a previous owner (a network-booting bootloader) may have left
 * running before its rings and buffers are replaced. */
static int dma_stop(const Genet* g) {
    wr(g, TDMA + DMA_CTRL, 0);
    wr(g, RDMA + DMA_CTRL, 0);
    int result = hw_wait(g->io, (uintptr_t)(GENET_BASE + TDMA + DMA_STATUS), DMA_DISABLED, DMA_DISABLED, 10000);
    if (result == HW_OK)
        result = hw_wait(g->io, (uintptr_t)(GENET_BASE + RDMA + DMA_STATUS), DMA_DISABLED, DMA_DISABLED, 10000);
    wr(g, TDMA + DMA_RING_CFG, 0);
    wr(g, RDMA + DMA_RING_CFG, 0);
    wr(g, UMAC_TX_FLUSH, 1);
    (void)hw_delay(g->io, 10);
    wr(g, UMAC_TX_FLUSH, 0);
    return result;
}

static void umac_reset(const Genet* g) {
    wr(g, SYS_RBUF_FLUSH_CTRL, 0);
    (void)hw_delay(g->io, 10);
    wr(g, UMAC_CMD, 0);
    /* Local loopback keeps the receive clock stable through the reset. */
    wr(g, UMAC_CMD, CMD_SW_RESET | CMD_LCL_LOOP_EN);
    (void)hw_delay(g->io, 2);
    wr(g, UMAC_CMD, 0);
    modify(g, SYS_RBUF_FLUSH_CTRL, 0, 2U);
    (void)hw_delay(g->io, 10);
    modify(g, SYS_RBUF_FLUSH_CTRL, 2U, 0);
    (void)hw_delay(g->io, 10);
    wr(g, UMAC_MIB_CTRL, 7U);
    wr(g, UMAC_MIB_CTRL, 0);
    wr(g, UMAC_MAX_FRAME_LEN, 1536U);
    /* No 64-byte status block; two pad bytes precede each received frame. */
    modify(g, RBUF_CTRL, RBUF_64B_EN, RBUF_ALIGN_2B);
    wr(g, RBUF_TBUF_SIZE_CTRL, 1);
    for (unsigned int i = 0; i < 2; i++) {
        uint32_t block = i ? INTRL2_1 : INTRL2_0;
        wr(g, block + INTRL2_MASK_SET, 0xffffffffU);
        wr(g, block + INTRL2_CLEAR, 0xffffffffU);
    }
}

/* Destination filter entries: broadcast and the station address only. */
static void mac_address(const Genet* g) {
    static const uint8_t broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const uint8_t* m = g->mac;
    wr(g, UMAC_MAC0, ((uint32_t)m[0] << 24) | ((uint32_t)m[1] << 16) | ((uint32_t)m[2] << 8) | m[3]);
    wr(g, UMAC_MAC1, ((uint32_t)m[4] << 8) | m[5]);
    wr(g, UMAC_MDF_CTRL, 0);
    for (unsigned int i = 0; i < 2; i++) {
        const uint8_t* a = i ? g->mac : broadcast;
        wr(g, UMAC_MDF_ADDR + i * 8, ((uint32_t)a[0] << 8) | a[1]);
        wr(g, UMAC_MDF_ADDR + i * 8 + 4, ((uint32_t)a[2] << 24) | ((uint32_t)a[3] << 16) |
                                         ((uint32_t)a[4] << 8) | a[5]);
    }
    wr(g, UMAC_MDF_CTRL, (1U << 16) | (1U << 15));
    wr(g, HFB_CTRL, 0);
    wr(g, HFB_FLT_ENABLE, 0);
    wr(g, HFB_FLT_ENABLE + 4, 0);
    for (unsigned int i = 0; i < 8; i++) wr(g, RDMA + DMA_INDEX2RING + i * 4, 0);
}

static void ring_init(Genet* g) {
    wr(g, RDMA + DMA_BURST, 8);
    wr(g, RX_RING16 + RING_HW_INDEX, 0);
    wr(g, RX_RING16 + RING_SW_INDEX, 0);
    wr(g, RX_RING16 + RING_BUF_SIZE, (GENET_RX_RING << 16) | GENET_BUFFER_BYTES);
    wr(g, RX_RING16 + RING_FLOW, (5U << 16) | 16U);
    wr(g, RX_RING16 + RING_START, 0);
    wr(g, RX_RING16 + RING_POINTER, 0);
    wr(g, RX_RING16 + RING_POINTER2, 0);
    wr(g, RX_RING16 + RING_END, GENET_RX_RING * 3 - 1);
    for (unsigned int i = 0; i < GENET_RX_RING; i++) {
        uint64_t bus = g->io->physical(g->io->context, g->rx[i]);
        wr(g, RX_DESC + i * DESC_BYTES + 4, (uint32_t)bus);
        wr(g, RX_DESC + i * DESC_BYTES + 8, (uint32_t)(bus >> 32));
        wr(g, RX_DESC + i * DESC_BYTES, (GENET_BUFFER_BYTES << 16) | DESC_OWN);
    }
    /* The hardware-owned indices may not accept a reset; follow them. */
    g->rx_consumer = (uint16_t)rd(g, RX_RING16 + RING_HW_INDEX);
    wr(g, RX_RING16 + RING_SW_INDEX, g->rx_consumer);
    wr(g, RDMA + DMA_RING_CFG, 1U << 16);

    wr(g, TDMA + DMA_BURST, 8);
    wr(g, TX_RING16 + RING_SW_INDEX, 0);
    wr(g, TX_RING16 + RING_HW_INDEX, 0);
    wr(g, TX_RING16 + RING_DONE_THRESH, 1);
    wr(g, TX_RING16 + RING_FLOW, 0);
    wr(g, TX_RING16 + RING_BUF_SIZE, (GENET_TX_RING << 16) | GENET_BUFFER_BYTES);
    wr(g, TX_RING16 + RING_START, 0);
    wr(g, TX_RING16 + RING_POINTER, 0);
    wr(g, TX_RING16 + RING_POINTER2, 0);
    wr(g, TX_RING16 + RING_END, GENET_TX_RING * 3 - 1);
    g->tx_consumer = g->tx_producer = (uint16_t)rd(g, TX_RING16 + RING_HW_INDEX);
    wr(g, TX_RING16 + RING_SW_INDEX, g->tx_producer);
    wr(g, TDMA + DMA_RING_CFG, 1U << 16);
}

static int dma_reachable(const Genet* g, const void* buffer) {
    uint64_t bus = g->io->physical(g->io->context, buffer);
    return bus && !(bus & 63U) && bus < (1ULL << 40) - GENET_BUFFER_BYTES;
}

int genet_init(Genet* genet, const HwOps* io) {
    if (!genet || !hw_ops_valid(io) || !io->cache || !io->physical || !io->property ||
        !io->owned(io->context)) return HW_INVALID;
    genet->io = io;
    genet->state = GENET_OFF;
    /* GENET v5 reports major revision 6 (v4 reports 5). */
    if (((rd(genet, SYS_REV_CTRL) >> 24) & 0xfU) != 6U) return HW_OFF;
    memset(genet, 0, sizeof(*genet));
    genet->io = io;
    for (unsigned int i = 0; i < GENET_RX_RING + GENET_TX_RING; i++)
        if (!dma_reachable(genet, i < GENET_RX_RING ? genet->rx[i] : genet->tx[i - GENET_RX_RING]))
            return HW_INVALID;
    uint32_t words[2] = {0, 0};
    int result = io->property(io->context, TAG_BOARD_MAC, words, 2);
    if (result != HW_OK) return fail(genet, result);
    for (unsigned int i = 0; i < 6; i++) genet->mac[i] = (uint8_t)(words[i / 4] >> (8 * (i % 4)));
    if ((genet->mac[0] & 1U) || !(genet->mac[0] | genet->mac[1] | genet->mac[2] |
                                  genet->mac[3] | genet->mac[4] | genet->mac[5]))
        return fail(genet, HW_PROTOCOL);
    wr(genet, SYS_PORT_CTRL, PORT_MODE_EXT_GPHY);
    if ((result = dma_stop(genet)) != HW_OK) return fail(genet, result);
    umac_reset(genet);
    mac_address(genet);
    /* The PHY adds the RX clock delay (rgmii-rxid) and the MAC adds none. */
    modify(genet, EXT_RGMII_OOB_CTRL, OOB_DISABLE | RGMII_LINK, RGMII_MODE_EN | ID_MODE_DIS);
    if ((result = phy_start(genet)) != HW_OK) return fail(genet, result);
    ring_init(genet);
    io->cache(io->context, genet->rx, sizeof(genet->rx), HW_CLEAN_INVALIDATE);
    wr(genet, RDMA + DMA_CTRL, DMA_RING16_EN | DMA_EN);
    wr(genet, TDMA + DMA_CTRL, DMA_RING16_EN | DMA_EN);
    wr(genet, UMAC_CMD, CMD_RX_PAUSE_IGNORE | CMD_TX_PAUSE_IGNORE | CMD_TX_EN | CMD_RX_EN);
    genet->crc_forward = (rd(genet, UMAC_CMD) & CMD_CRC_FWD) != 0;
    genet->link_checked = now(genet) - GENET_LINK_POLL_US;
    genet->state = GENET_READY;
    return HW_OK;
}

static int ready(const Genet* genet) {
    if (!genet || !genet->io) return HW_INVALID;
    if (genet->state != GENET_READY) return genet->state == GENET_FAILED ? genet->error : HW_OFF;
    return genet->io->owned(genet->io->context) ? HW_OK : HW_INVALID;
}

static int reclaim(Genet* genet) {
    uint16_t done = (uint16_t)rd(genet, TX_RING16 + RING_HW_INDEX);
    uint16_t finished = (uint16_t)(done - genet->tx_consumer);
    if (finished > (uint16_t)(genet->tx_producer - genet->tx_consumer)) return fail(genet, HW_PROTOCOL);
    if (!finished) return 0;
    genet->tx_consumer = done;
    genet->tx_progress = now(genet);
    return 1;
}

static int rx_pending(Genet* genet) {
    uint16_t pending = (uint16_t)((uint16_t)rd(genet, RX_RING16 + RING_HW_INDEX) - genet->rx_consumer);
    return pending > GENET_RX_RING ? fail(genet, HW_PROTOCOL) : pending;
}

int genet_poll(Genet* genet) {
    int result = ready(genet);
    if (result != HW_OK) return result;
    int progress = reclaim(genet);
    if (progress < 0) return progress;
    int pending = rx_pending(genet);
    if (pending < 0) return pending;
    if (pending) progress = 1;
    uint32_t time = now(genet);
    if (time - genet->link_checked >= GENET_LINK_POLL_US) {
        int up = 0, full = 0;
        uint32_t mbps = 0;
        genet->link_checked = time;
        result = phy_link(genet, &up, &mbps, &full);
        if (result != HW_OK) {
            if (++genet->mdio_failures >= MDIO_FAILURES) return fail(genet, result);
        } else {
            genet->mdio_failures = 0;
            if (up != genet->link || (up && (mbps != genet->mbps || full != genet->full_duplex))) {
                genet->link = up;
                genet->mbps = up ? mbps : 0;
                genet->full_duplex = up && full;
                genet->tx_progress = time;
                mac_link(genet);
                progress = 1;
            }
        }
    }
    if (genet->link && genet->tx_producer != genet->tx_consumer &&
        time - genet->tx_progress >= GENET_TX_STALL_US) return fail(genet, HW_TIMEOUT);
    return progress;
}

int genet_send(Genet* genet, const void* frame, unsigned int bytes) {
    int result = ready(genet);
    if (result != HW_OK) return result;
    if (!frame || bytes < 14 || bytes > GENET_FRAME_MAX) return HW_INVALID;
    if (!genet->link) return HW_OFF;
    if (reclaim(genet) < 0) return genet->error;
    if ((uint16_t)(genet->tx_producer - genet->tx_consumer) >= GENET_TX_RING) return HW_BUSY;
    if (genet->tx_producer == genet->tx_consumer) genet->tx_progress = now(genet);
    unsigned int slot = genet->tx_slot, length = bytes < FRAME_MIN ? FRAME_MIN : bytes;
    unsigned char* buffer = genet->tx[slot];
    memcpy(buffer, frame, bytes);
    if (length > bytes) memset(buffer + bytes, 0, length - bytes);
    genet->io->cache(genet->io->context, buffer, length, HW_CLEAN);
    uint64_t bus = genet->io->physical(genet->io->context, buffer);
    wr(genet, TX_DESC + slot * DESC_BYTES + 4, (uint32_t)bus);
    wr(genet, TX_DESC + slot * DESC_BYTES + 8, (uint32_t)(bus >> 32));
    wr(genet, TX_DESC + slot * DESC_BYTES,
       ((uint32_t)length << 16) | DESC_TX_QTAG | DESC_TX_APPEND_CRC | DESC_SOP | DESC_EOP);
    genet->tx_slot = (slot + 1) % GENET_TX_RING;
    genet->tx_producer++;
    wr(genet, TX_RING16 + RING_SW_INDEX, genet->tx_producer);
    genet->tx_frames++;
    return HW_OK;
}

int genet_receive(Genet* genet, void* frame, unsigned int capacity) {
    int result = ready(genet);
    if (result != HW_OK) return result;
    if (!frame) return HW_INVALID;
    for (unsigned int i = 0; i < GENET_RX_RING; i++) {
        int pending = rx_pending(genet);
        if (pending <= 0) return pending;
        unsigned int slot = genet->rx_slot;
        uint32_t status = rd(genet, RX_DESC + slot * DESC_BYTES);
        unsigned int length = (status >> 16) & 0xfffU, trailer = RX_PAD + (genet->crc_forward ? 4U : 0U);
        unsigned int bytes = length > trailer ? length - trailer : 0;
        result = 0;
        if ((status & (DESC_SOP | DESC_EOP)) != (DESC_SOP | DESC_EOP) || (status & DESC_RX_ERRORS) ||
            bytes < 14 || bytes > GENET_FRAME_MAX) genet->rx_dropped++;
        else if (bytes > capacity) result = HW_NO_SPACE;
        else {
            genet->io->cache(genet->io->context, genet->rx[slot], RX_PAD + bytes, HW_INVALIDATE);
            memcpy(frame, genet->rx[slot] + RX_PAD, bytes);
            genet->rx_frames++;
            result = (int)bytes;
        }
        genet->rx_slot = (slot + 1) % GENET_RX_RING;
        genet->rx_consumer++;
        wr(genet, RX_RING16 + RING_SW_INDEX, genet->rx_consumer);
        if (result) return result;
    }
    return 0;
}

int genet_carrier(const Genet* genet, uint32_t* mbps) {
    int up = genet && genet->state == GENET_READY && genet->link;
    if (mbps) *mbps = up ? genet->mbps : 0;
    return up;
}
