#include <vantage/drivers/usb_storage.h>

/* USB-IF Bulk-Only Transport 1.0: CBW/CSW fields are little-endian.
 * The SCSI block commands and read-capacity response are big-endian. */
#define CBW_SIGNATURE 0x43425355U
#define CSW_SIGNATURE 0x53425355U

static void put_le32(uint8_t* field, uint32_t value) {
    for (uint32_t i = 0; i < 4; i++) field[i] = (uint8_t)(value >> (8 * i));
}

static uint32_t get_le32(const uint8_t* field) {
    uint32_t value = 0;
    for (uint32_t i = 0; i < 4; i++) value |= (uint32_t)field[i] << (8 * i);
    return value;
}

static uint32_t get_be32(const uint8_t* field) {
    uint32_t value = 0;
    for (uint32_t i = 0; i < 4; i++) value = (value << 8) | field[i];
    return value;
}

static int recover(UsbStorage* storage, int error) {
    UsbStorageTransport* transport = &storage->transport;
    if (error == HW_OFF ||
        transport->reset(transport->context, transport->interface_number) != HW_OK ||
        transport->clear_halt(transport->context, transport->bulk_in) != HW_OK ||
        transport->clear_halt(transport->context, transport->bulk_out) != HW_OK) {
        storage->sectors = 0;
        return HW_OFF;
    }
    return error;
}

static int scsi_read(UsbStorage* storage, const uint8_t* command, uint8_t command_length,
                     void* data, uint32_t length) {
    UsbStorageTransport* transport = &storage->transport;
    uint8_t cbw[31] = {0}, csw[13];
    uint32_t tag = ++storage->tag;
    put_le32(cbw, CBW_SIGNATURE);
    put_le32(cbw + 4, tag);
    put_le32(cbw + 8, length);
    cbw[12] = 0x80;
    cbw[14] = command_length;
    memcpy(cbw + 15, command, command_length);

    int sent = transport->bulk(transport->context, transport->bulk_out, cbw, sizeof(cbw));
    if (sent != (int)sizeof(cbw)) return recover(storage, sent < 0 ? sent : HW_PROTOCOL);

    int received = transport->bulk(transport->context, transport->bulk_in, data, length);
    if (received == HW_BUSY) {
        if (transport->clear_halt(transport->context, transport->bulk_in) != HW_OK)
            return recover(storage, HW_PROTOCOL);
        received = 0;
    } else if (received < 0 || (uint32_t)received > length) {
        return recover(storage, received < 0 ? received : HW_PROTOCOL);
    }

    int status = transport->bulk(transport->context, transport->bulk_in, csw, sizeof(csw));
    if (status == HW_BUSY && transport->clear_halt(transport->context, transport->bulk_in) == HW_OK)
        status = transport->bulk(transport->context, transport->bulk_in, csw, sizeof(csw));
    if (status != (int)sizeof(csw)) return recover(storage, status < 0 ? status : HW_PROTOCOL);
    uint32_t residue = get_le32(csw + 8);
    if (get_le32(csw) != CSW_SIGNATURE || get_le32(csw + 4) != tag || residue > length || csw[12] > 1)
        return recover(storage, HW_PROTOCOL);
    if (csw[12] == 1) return HW_IO;
    if ((uint32_t)received != length || residue != 0) return recover(storage, HW_PROTOCOL);
    return HW_OK;
}

int usb_storage_open(UsbStorage* storage, const UsbStorageTransport* transport) {
    if (!storage || !transport || !transport->bulk || !transport->reset || !transport->clear_halt ||
        (transport->bulk_in & 0xf0U) != 0x80U || !(transport->bulk_in & 15U) ||
        !transport->bulk_out || transport->bulk_out > 15U) return HW_INVALID;

    *storage = (UsbStorage){ .transport = *transport };
    const uint8_t capacity[10] = {0x25};
    uint8_t response[8];
    int result = scsi_read(storage, capacity, sizeof(capacity), response, sizeof(response));
    if (result == HW_IO) {
        /* Clear a device's contingent condition after power-up, then retry once. */
        const uint8_t sense[6] = {0x03, 0, 0, 0, 18};
        uint8_t details[18];
        result = scsi_read(storage, sense, sizeof(sense), details, sizeof(details));
        if (result == HW_OK)
            result = scsi_read(storage, capacity, sizeof(capacity), response, sizeof(response));
    }
    if (result != HW_OK) return result;

    uint32_t last = get_be32(response);
    if (last == 0xffffffffU || get_be32(response + 4) != USB_STORAGE_SECTOR_BYTES)
        return HW_UNSUPPORTED;
    storage->sectors = last + 1U;
    return HW_OK;
}

int usb_storage_read(UsbStorage* storage, uint32_t lba, uint32_t count, void* data) {
    if (!storage || !count || count > USB_STORAGE_MAX_READ_SECTORS || !data) return HW_INVALID;
    if (!storage->sectors) return HW_OFF;
    if (lba >= storage->sectors || count > storage->sectors - lba) return HW_INVALID;

    uint8_t command[10] = {0x28, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
                           (uint8_t)(lba >> 8), (uint8_t)lba, 0,
                           (uint8_t)(count >> 8), (uint8_t)count, 0};
    return scsi_read(storage, command, sizeof(command), data, count * USB_STORAGE_SECTOR_BYTES);
}
