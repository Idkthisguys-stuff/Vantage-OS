#pragma once

#include <vantage/drivers/hardware.h>

#define USB_STORAGE_SECTOR_BYTES 512U
#define USB_STORAGE_MAX_READ_SECTORS 32U

/* A configured SCSI transparent, Bulk-Only USB interface. Transfers return
 * the actual byte count or a negative HwResult. HW_BUSY denotes endpoint
 * STALL on bulk transfers; the control operations return HW_OK on success.
 * The host owns timeouts, DMA staging, and endpoint lifetime. */
typedef struct {
    void* context;
    int (*bulk)(void* context, uint8_t endpoint, void* data, uint32_t bytes);
    int (*reset)(void* context, uint8_t interface_number);
    int (*clear_halt)(void* context, uint8_t endpoint);
    uint8_t interface_number, bulk_in, bulk_out;
} UsbStorageTransport;

typedef struct {
    UsbStorageTransport transport;
    uint32_t tag, sectors;
} UsbStorage;

/* LUN 0, 512-byte sectors, READ CAPACITY(10) and READ(10). */
int usb_storage_open(UsbStorage* storage, const UsbStorageTransport* transport);
int usb_storage_read(UsbStorage* storage, uint32_t lba, uint32_t count, void* data);
