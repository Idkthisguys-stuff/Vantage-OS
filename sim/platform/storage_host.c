#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#endif
#include "storage_host.h"
#include <vantage/kernel/files.h>
#include <vantage/kernel/usb_media.h>
#include <stdio.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#define seek_file _fseeki64
#define tell_file _ftelli64
#else
#include <unistd.h>
#define seek_file fseeko
#define tell_file ftello
#endif

static FILE* image;
static FILE* usb_image;

static int position(FILE* f, uint64_t lba) {
    if (lba > 0x7fffffffffffffffULL / STORE_SECTOR_BYTES) return -1;
    return seek_file(f, (int64_t)(lba * STORE_SECTOR_BYTES), SEEK_SET);
}
static int read_sector(void* context, uint64_t lba, void* data) {
    FILE* f = context;
    return position(f, lba) || fread(data, 1, STORE_SECTOR_BYTES, f) != STORE_SECTOR_BYTES ? -1 : 0;
}

static int read_usb_sector(void* context, uint32_t lba, void* data) {
    return read_sector(context, lba, data) ? HW_IO : HW_OK;
}

static int regular_file(FILE* f) {
#ifdef _WIN32
    struct _stat64 info;
    return !_fstat64(_fileno(f), &info) && (info.st_mode & _S_IFMT) == _S_IFREG;
#else
    struct stat info;
    return !fstat(fileno(f), &info) && S_ISREG(info.st_mode);
#endif
}

int sim_usb_open(const char* path) {
    if (!path || usb_image) return FILE_BAD;
    FILE* f = fopen(path, "rb");
    if (!f) return FILE_IO_ERROR;
    if (!regular_file(f) || seek_file(f, 0, SEEK_END)) { fclose(f); return FILE_BAD; }
    int64_t bytes = tell_file(f);
    if (bytes <= 0 || bytes % USB_FILE_CHUNK || (uint64_t)bytes / USB_FILE_CHUNK > 0xffffffffU) {
        fclose(f);
        return FILE_BAD;
    }
    UsbFat32Source source = {f, (uint32_t)(bytes / USB_FILE_CHUNK), read_usb_sector};
    int result = usb_media_attach(&source);
    if (result != HW_OK) { fclose(f); return FILE_CORRUPT; }
    usb_image = f;
    return 0;
}
static int write_sector(void* context, uint64_t lba, const void* data) {
    FILE* f = context;
    return position(f, lba) || fwrite(data, 1, STORE_SECTOR_BYTES, f) != STORE_SECTOR_BYTES ? -1 : 0;
}
static int sync_image(void* context) {
    FILE* f = context;
    if (fflush(f)) return -1;
#ifdef _WIN32
    return _commit(_fileno(f));
#else
    return fsync(fileno(f));
#endif
}

int sim_storage_open(const char* path, unsigned int partition) {
    if (!path) return FILE_BAD;
    if (image) return FILE_BUSY;
    FILE* f = fopen(path, "r+b");
    if (!f) return FILE_IO_ERROR;
    if (!regular_file(f) || seek_file(f, 0, SEEK_END)) { fclose(f); return FILE_BAD; }
    int64_t bytes = tell_file(f);
    if (bytes <= 0 || bytes % STORE_SECTOR_BYTES) { fclose(f); return FILE_BAD; }
    SectorDevice device = {f, (uint64_t)bytes / STORE_SECTOR_BYTES, read_sector, write_sector, sync_image};
    int result = files_mount(&device, partition);
    if (result < 0) { fclose(f); return result; }
    image = f;
    return result;
}

int sim_storage_close(void) {
    if (!image) return FILE_UNAVAILABLE;
    int result = files_unmount();
    if (result) return result;
    result = fclose(image) ? FILE_IO_ERROR : 0;
    image = NULL;
    return result;
}
