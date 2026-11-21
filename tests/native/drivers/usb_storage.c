#include <vantage/drivers/usb_fat32.h>
#include <stdio.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "USB storage check failed at line %d\n", __LINE__); return 1; \
} } while (0)

typedef struct {
    uint8_t command, tag[4];
    uint32_t phase, calls, resets, clears, read_lba;
    int fat_disk, broken_chain;
    int bad_tag, stall_data, stall_status, fail_reset, attention, off, invalid_order;
} Device;

static void put32(uint8_t* out, uint32_t value) {
    for (unsigned int i = 0; i < 4; i++) out[i] = (uint8_t)(value >> (i * 8));
}

static int fat_sector(const Device* device, uint8_t* packet) {
    memset(packet, 0, 512);
    switch (device->read_lba) {
    case 0:
        packet[450] = 0x0c;
        put32(packet + 454, 32);
        put32(packet + 458, 66100);
        packet[510] = 0x55;
        packet[511] = 0xaa;
        break;
    case 32:
        packet[12] = 2;
        packet[13] = 1;
        packet[14] = 32;
        packet[16] = 1;
        put32(packet + 32, 66100);
        put32(packet + 36, 513);
        put32(packet + 44, 2);
        packet[510] = 0x55;
        packet[511] = 0xaa;
        break;
    case 64:
        put32(packet + 8, 0x0fffffff);
        put32(packet + 12, 5);
        put32(packet + 20, device->broken_chain ? 3 : 0x0fffffff);
        break;
    case 577:
        memcpy(packet, "NOTES   BIN", 11);
        packet[26] = 3;
        put32(packet + 28, 600);
        break;
    case 578:
        memset(packet, 0x5a, 512);
        break;
    case 580:
        memset(packet, 0x6b, 512);
        break;
    default:
        return HW_IO;
    }
    return 512;
}

static int bulk(void* context, uint8_t endpoint, void* data, uint32_t bytes) {
    Device* device = context;
    uint8_t* packet = data;
    device->calls++;
    if (device->off) return HW_OFF;

    if (!device->phase && endpoint == 2 && bytes == 31) {
        if (memcmp(packet, "USBC", 4) || packet[12] != 0x80 || packet[13] ||
            (packet[14] != 10 && packet[14] != 6)) return HW_PROTOCOL;
        memcpy(device->tag, packet + 4, 4);
        device->command = packet[15];
        device->read_lba = ((uint32_t)packet[17] << 24) | ((uint32_t)packet[18] << 16) |
                           ((uint32_t)packet[19] << 8) | packet[20];
        if (device->command == 0x28 && !device->fat_disk &&
            (packet[8] != 0 || packet[9] != 4 || packet[22] != 0 || packet[23] != 2))
            return HW_PROTOCOL;
        device->phase = 1;
        return 31;
    }
    if (device->phase == 1 && endpoint == 0x81) {
        device->phase = 2;
        if (device->stall_data) return HW_BUSY;
        if (device->command == 0x25 && bytes == 8) {
            const uint8_t capacity[] = {0, 0, 1, 255, 0, 0, 2, 0};
            memcpy(packet, capacity, bytes);
            if (device->fat_disk) {
                packet[1] = 1;
                packet[2] = 0x11;
                packet[3] = 0x6f;
            }
            return 8;
        }
        if (device->command == 3 && bytes == 18) {
            device->attention = 0;
            memset(packet, 0, bytes);
            return 18;
        }
        if (device->command == 0x28 && device->fat_disk && bytes == 512)
            return fat_sector(device, packet);
        if (device->command == 0x28 && bytes == 1024) {
            memset(packet, 0x5a, bytes);
            return 1024;
        }
    }
    if (device->phase == 2 && endpoint == 0x81 && bytes == 13) {
        if (device->stall_status) { device->stall_status--; return HW_BUSY; }
        device->phase = 0;
        memset(packet, 0, bytes);
        memcpy(packet, "USBS", 4);
        memcpy(packet + 4, device->tag, 4);
        if (device->bad_tag) packet[4] ^= 1;
        if (device->attention || device->stall_data) packet[12] = 1;
        return 13;
    }
    device->invalid_order = 1;
    return HW_PROTOCOL;
}

static int reset(void* context, uint8_t number) {
    Device* device = context;
    device->resets++;
    device->phase = 0;
    if (number != 3) return HW_PROTOCOL;
    return device->fail_reset ? HW_TIMEOUT : HW_OK;
}

static int clear_halt(void* context, uint8_t endpoint) {
    Device* device = context;
    device->clears++;
    if (endpoint != 0x81 && endpoint != 2) return HW_PROTOCOL;
    return HW_OK;
}

int main(void) {
    Device device = { .attention = 1 };
    const UsbStorageTransport transport = { &device, bulk, reset, clear_halt, 3, 0x81, 2 };
    UsbStorage disk = {0};
    uint8_t data[1024];

    CHECK(usb_storage_open(&disk, &transport) == HW_OK && disk.sectors == 512 && device.calls == 9);
    CHECK(usb_storage_read(&disk, 510, 2, data) == HW_OK);
    CHECK(data[0] == 0x5a && data[1023] == 0x5a && device.read_lba == 510);
    CHECK(usb_storage_read(&disk, 511, 2, data) == HW_INVALID);
    CHECK(usb_storage_read(&disk, 0, 33, data) == HW_INVALID && device.calls == 12);

    device.stall_status = 1;
    CHECK(usb_storage_read(&disk, 1, 2, data) == HW_OK && device.clears == 1 && device.resets == 0);
    device.stall_data = 1;
    CHECK(usb_storage_read(&disk, 1, 2, data) == HW_IO && device.clears == 2 && device.resets == 0);
    device.stall_data = 0;
    device.bad_tag = 1;
    CHECK(usb_storage_read(&disk, 1, 2, data) == HW_PROTOCOL && device.clears == 4 && device.resets == 1);
    device.bad_tag = 0;
    CHECK(usb_storage_read(&disk, 1, 2, data) == HW_OK);
    device.bad_tag = device.fail_reset = 1;
    CHECK(usb_storage_read(&disk, 1, 2, data) == HW_OFF && disk.sectors == 0 && device.resets == 2);
    CHECK(usb_storage_read(&disk, 1, 2, data) == HW_OFF && !device.invalid_order);
    device = (Device){ .fat_disk = 1 };
    CHECK(usb_storage_open(&disk, &transport) == HW_OK && disk.sectors == 70000);
    UsbFat32 volume;
    UsbFat32Directory cursor = {0};
    UsbFat32File file;
    UsbFat32Reader reader = {0};
    UsbFat32Source source = usb_fat32_storage_source(&disk);
    CHECK(usb_fat32_mount(&volume, &source) == HW_OK);
    CHECK(usb_fat32_next(&volume, &cursor, &file) == HW_OK);
    CHECK(!strcmp(file.name, "NOTES.BIN") && file.bytes == 600);
    CHECK(usb_fat32_next(&volume, &cursor, &file) == 0);
    CHECK(usb_fat32_read(&volume, &file, &reader, data) == 512 && data[511] == 0x5a);
    CHECK(usb_fat32_read(&volume, &file, &reader, data) == 88 && data[87] == 0x6b);
    CHECK(usb_fat32_read(&volume, &file, &reader, data) == 0);

    reader = (UsbFat32Reader){0};
    device.broken_chain = 1;
    CHECK(usb_fat32_read(&volume, &file, &reader, data) == 512);
    CHECK(usb_fat32_read(&volume, &file, &reader, data) == HW_PROTOCOL);
    return 0;
}
