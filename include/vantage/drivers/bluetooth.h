#pragma once
#include <vantage/abi/bluetooth.h>

typedef struct {
    void* context;
    /* Send one complete H4 packet. Return BT_OK or a negative error.
     * BT_BUSY means no bytes were written and the packet can be retried. */
    int (*send)(void*, const uint8_t*, size_t);
    /* Read one byte: 1 on success, 0 if empty, or a negative error. */
    int (*receive)(void*, uint8_t*);
    /* Monotonic microseconds modulo 2^32. */
    uint32_t (*time_us)(void*);
} BtH4Io;

/* Driver-owned state. Zero initialization represents BT_DISABLED. */
typedef struct {
    BtH4Io io;
    BtStatus status;
    BtDevice devices[BT_DEVICE_LIMIT];
    uint8_t name_types[BT_DEVICE_LIMIT];

    uint32_t step_started, frame_started;
    uint8_t step, command_sent, command_credits;
    uint16_t rx_remaining;
    uint8_t rx_type, rx_header_used, rx_header[4], rx_event[255];
} BtHci;

/* Calls on a controller must be serialized. Init copies the callbacks and
 * queues Reset. Poll advances the controller with bounded reads and at most
 * one send attempt. Init can restart BT_FAILED on a usable transport. */
int bt_hci_init(BtHci*, const BtH4Io*);
void bt_hci_poll(BtHci*);

/* Queues a five-second active scan, replacing the previous results. The
 * state stays BT_SCANNING through the acknowledgment of scan disable. */
int bt_hci_scan_start(BtHci*);
void bt_hci_status(const BtHci*, BtStatus*);
/* Returns BT_OK for a published result, 0 for an absent in-range index,
 * or a negative result for disabled, failed or invalid requests. */
int bt_hci_device(const BtHci*, uint32_t, BtDevice*);
