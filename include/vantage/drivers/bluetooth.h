#pragma once
#include <vantage/abi/bluetooth.h>

typedef struct {
    void* context;
    /* Queue an H4 packet. On BT_BUSY the transport may have accepted part
     * of it. The same packet is supplied on the next attempt. */
    int (*send)(void*, const uint8_t*, size_t);
    /* Read one byte: 1 on success, 0 if empty, or a negative error. */
    int (*receive)(void*, uint8_t*);
    /* Monotonic microseconds modulo 2^32. */
    uint32_t (*time_us)(void*);
    /* Controller-specific guard time after acknowledging HCI Reset. */
    uint32_t reset_settle_us;
} BtH4Io;

/* Optional immutable BCM4345C0 RAM-only HCD records, without H4 type bytes. */
typedef struct {
    const uint8_t* data;
    size_t size;
} BtPatch;

/* Driver-owned state. Zero initialization represents BT_DISABLED. */
typedef struct {
    BtH4Io io;
    BtStatus status;
    BtDevice devices[BT_DEVICE_LIMIT];
    uint8_t name_types[BT_DEVICE_LIMIT];

    uint32_t step_started, frame_started;
    BtPatch patch;
    size_t patch_offset;
    uint32_t patch_started;
    uint16_t manufacturer, rom_subversion;
    uint8_t step, polled, command_sent, command_credits;
    uint16_t rx_remaining;
    uint8_t rx_type, rx_header_used, rx_header[4], rx_event[255];
} BtHci;

/* Calls on a controller must be serialized. Init copies the callbacks and
 * queues Reset. Poll advances the controller with bounded reads and at most
 * one send attempt. Init can restart BT_FAILED on a usable transport. */
int bt_hci_init(BtHci*, const BtH4Io*, const BtPatch*);
void bt_hci_poll(BtHci*);
uint16_t bt_hci_pending_opcode(const BtHci*);

/* Queues a five-second active scan, replacing the previous results. The
 * state stays BT_SCANNING through the acknowledgment of scan disable. */
int bt_hci_scan_start(BtHci*);
void bt_hci_status(const BtHci*, BtStatus*);
/* Returns BT_OK for a published result, 0 for an absent in-range index,
 * or a negative result for disabled, failed or invalid requests. */
int bt_hci_device(const BtHci*, uint32_t, BtDevice*);
