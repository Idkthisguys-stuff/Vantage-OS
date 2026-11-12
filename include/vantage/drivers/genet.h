#pragma once
#include <vantage/drivers/hardware.h>

/* BCM2711 GENET v5 at its low-peripheral-mode address (scb 0x7d580000). */
#define GENET_BASE 0xfd580000ULL
#define GENET_FRAME_MAX 1514U
#define GENET_BUFFER_BYTES 2048U
#define GENET_RX_RING 32U
#define GENET_TX_RING 16U
/* The PHY is polled at most this often; carrier reports the last result. */
#define GENET_LINK_POLL_US 100000U
/* Transmission that makes no progress this long with the link up stops the device. */
#define GENET_TX_STALL_US 1000000U

enum { GENET_OFF = 0, GENET_READY = 1, GENET_FAILED = -1 };

typedef struct {
    /* DMA buffers: whole cache lines, never shared with other data. */
    HW_ALIGN(64) unsigned char rx[GENET_RX_RING][GENET_BUFFER_BYTES];
    HW_ALIGN(64) unsigned char tx[GENET_TX_RING][GENET_BUFFER_BYTES];
    const HwOps* io;
    int state, error, link, full_duplex, crc_forward;
    uint32_t mbps, phy_id, link_checked, tx_progress;
    unsigned int phy, mdio_failures, rx_slot, tx_slot;
    uint16_t rx_consumer, tx_producer, tx_consumer;
    uint32_t rx_dropped, tx_frames, rx_frames;
    uint8_t mac[6];
} Genet;

/* Polled, core 0, masked EL1. Brings up the MAC with the firmware's board
 * MAC address, resets the PHY and starts auto-negotiation without waiting
 * for a link. HW_OFF when no GENET v5 answers at GENET_BASE. The context is
 * zeroed here and must stay allocated for kernel lifetime: its buffers
 * remain programmed into the DMA rings. */
int genet_init(Genet* genet, const HwOps* io);
/* Reclaims finished transmissions and re-reads the PHY link (rate-limited).
 * 1 = progress or frames waiting, 0 = idle, negative HwResult once the
 * device is lost; it then stays stopped. */
int genet_poll(Genet* genet);
/* Ethernet II frame without FCS, 14..GENET_FRAME_MAX bytes. HW_OFF without a
 * link, HW_BUSY while every transmit descriptor is in flight. */
int genet_send(Genet* genet, const void* frame, unsigned int bytes);
/* Next good frame (without FCS): its size, 0 when none is queued, or a
 * negative HwResult. Frames with receive errors are dropped. */
int genet_receive(Genet* genet, void* frame, unsigned int capacity);
/* 1 with *mbps while the link is up, 0 otherwise. No hardware access. */
int genet_carrier(const Genet* genet, uint32_t* mbps);
