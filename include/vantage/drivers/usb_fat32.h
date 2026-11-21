#pragma once

#include <vantage/drivers/usb_storage.h>

typedef struct {
    void* context;
    uint32_t sectors;
    int (*read)(void* context, uint32_t lba, void* sector);
} UsbFat32Source;

typedef struct {
    UsbFat32Source source;
    uint32_t fat, data, clusters, sectors_per_cluster, root;
} UsbFat32;

typedef struct {
    uint32_t cluster, entry, visited, scanned;
    uint32_t cached_sector, cached;
    uint8_t sector[USB_STORAGE_SECTOR_BYTES];
} UsbFat32Directory;

typedef struct {
    char name[13];
    uint32_t cluster, bytes;
} UsbFat32File;

typedef struct {
    uint32_t cluster, offset;
} UsbFat32Reader;

/* Read-only FAT32, either directly on the device or in a primary MBR FAT32
 * partition. The root iterator returns regular files with DOS short names. */
UsbFat32Source usb_fat32_storage_source(UsbStorage* disk);
int usb_fat32_mount(UsbFat32* volume, const UsbFat32Source* source);
int usb_fat32_next(const UsbFat32* volume, UsbFat32Directory* cursor, UsbFat32File* file);
/* Inspect at most one directory sector, or advance the FAT chain. */
#define USB_FAT32_MORE 2
int usb_fat32_step(const UsbFat32* volume, UsbFat32Directory* cursor, UsbFat32File* file);
/* Output must hold one sector. Returns bytes read, zero at EOF, or HwResult. */
int usb_fat32_read(const UsbFat32* volume, const UsbFat32File* file, UsbFat32Reader* reader, void* sector);
