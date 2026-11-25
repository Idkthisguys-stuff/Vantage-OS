#include <vantage/platform/pi4/usb_disk.h>
#include <vantage/kernel/usb_media.h>

/* USB-IF Mass Storage Class, SCSI transparent Bulk-Only protocol. */
static unsigned int disk_path;
static UsbStorage disk;

static int bulk(void* context, uint8_t endpoint, void* data, uint32_t bytes) {
    return pi4_usb_bulk(*(const unsigned int*)context, endpoint, data, bytes);
}

static int reset(void* context, uint8_t interface_number) {
    const Pi4UsbSetup setup = {0x21, 0xff, 0, interface_number, 0};
    size_t actual;
    return pi4_usb_control(*(const unsigned int*)context, &setup, NULL, &actual);
}

static int clear_halt(void* context, uint8_t endpoint) {
    return pi4_usb_clear_bulk_halt(*(const unsigned int*)context, endpoint);
}

static int endpoints(const uint8_t* config, size_t bytes, UsbStorageTransport* transport,
                     uint16_t* packet_in, uint16_t* packet_out) {
    int selected = 0;
    for (size_t offset = 9; offset < bytes;) {
        if (bytes - offset < 2) return HW_PROTOCOL;
        const uint8_t* d = config + offset;
        if (d[0] < 2 || d[0] > bytes - offset) return HW_PROTOCOL;
        if (d[1] == 4) {
            if (d[0] < 9) return HW_PROTOCOL;
            if (selected && transport->bulk_in && transport->bulk_out) return HW_OK;
            transport->bulk_in = transport->bulk_out = 0;
            selected = d[3] == 0 && d[4] == 2 && d[5] == 8 && d[6] == 6 && d[7] == 0x50;
            if (selected) transport->interface_number = d[2];
        } else if (selected && d[1] == 5) {
            if (d[0] < 7 || d[3] != 2 || (d[2] & 0x70) || !(d[2] & 15)) return HW_PROTOCOL;
            uint16_t packet = (uint16_t)(d[4] | (uint16_t)d[5] << 8);
            if (d[2] & 0x80) {
                if (transport->bulk_in) return HW_PROTOCOL;
                transport->bulk_in = d[2];
                *packet_in = packet;
            } else {
                if (transport->bulk_out) return HW_PROTOCOL;
                transport->bulk_out = d[2];
                *packet_out = packet;
            }
        }
        offset += d[0];
    }
    return selected && transport->bulk_in && transport->bulk_out ? HW_OK : HW_UNSUPPORTED;
}

int pi4_usb_disk_configure(unsigned int path, const uint8_t* config, size_t bytes) {
    if (disk_path || !path || !config || bytes < 9 || !config[5]) return HW_UNSUPPORTED;
    UsbStorageTransport transport = { .context = &disk_path, .bulk = bulk,
                                      .reset = reset, .clear_halt = clear_halt };
    uint16_t packet_in = 0, packet_out = 0;
    int result = endpoints(config, bytes, &transport, &packet_in, &packet_out);
    if (result != HW_OK) return result;
    result = pi4_usb_configure_bulk(path, transport.bulk_in, packet_in);
    if (result == HW_OK) result = pi4_usb_configure_bulk(path, transport.bulk_out, packet_out);
    if (result == HW_OK) {
        const Pi4UsbSetup setup = {0, 9, config[5], 0, 0};
        size_t actual;
        result = pi4_usb_control(path, &setup, NULL, &actual);
    }
    if (result != HW_OK) return result;
    disk_path = path;
    result = usb_storage_open(&disk, &transport);
    if (result != HW_OK) { disk_path = 0; return result; }
    UsbFat32Source source = usb_fat32_storage_source(&disk);
    /* Keep an attached non-FAT volume visible as unsupported until unplug. */
    (void)usb_media_attach(&source);
    return HW_OK;
}

void pi4_usb_disk_detach(void) {
    usb_media_detach();
    disk = (UsbStorage){0};
    disk_path = 0;
}
