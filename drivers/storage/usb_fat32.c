#include <vantage/drivers/usb_fat32.h>

#define SECTOR USB_STORAGE_SECTOR_BYTES
#define FAT32_LAST 0x0fffffefU
#define FAT32_EOF 0x0ffffff8U
#define MAX_ROOT_ENTRIES 8192U

static uint32_t le16(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 8; }

static uint32_t le32(const uint8_t* p) { return le16(p) | le16(p + 2) << 16; }

static int valid_cluster(const UsbFat32* volume, uint32_t cluster) {
    return cluster >= 2 && cluster - 2 < volume->clusters;
}

static int geometry(UsbFat32* volume, uint32_t start, uint32_t limit, const uint8_t* sector) {
    if (le16(sector + 510) != 0xaa55 || le16(sector + 11) != SECTOR ||
        le16(sector + 17) || le16(sector + 22) || le16(sector + 42)) return HW_UNSUPPORTED;

    uint32_t group = sector[13], reserved = le16(sector + 14), fats = sector[16];
    uint32_t total = le32(sector + 32), length = le32(sector + 36);
    uint32_t flags = le16(sector + 40), root = le32(sector + 44);
    uint32_t active = (flags & 0x80U) ? flags & 15U : 0;
    if (!group || group > 128 || (group & (group - 1)) || !reserved || !fats || fats > 2 ||
        active >= fats || total > limit || total <= reserved || !length ||
        length > (total - reserved) / fats) return HW_UNSUPPORTED;

    uint32_t overhead = reserved + fats * length;
    uint32_t clusters = (total - overhead) / group;
    if (clusters < 65525 || clusters > FAT32_LAST - 1 ||
        (clusters + 2U + 127U) / 128U > length || root < 2 || root - 2 >= clusters)
        return HW_UNSUPPORTED;

    volume->fat = start + reserved + active * length;
    volume->data = start + overhead;
    volume->sectors_per_cluster = group;
    volume->clusters = clusters;
    volume->root = root;
    return HW_OK;
}

static int storage_read(void* context, uint32_t lba, void* sector) {
    return usb_storage_read(context, lba, 1, sector);
}

UsbFat32Source usb_fat32_storage_source(UsbStorage* disk) {
    return (UsbFat32Source){ disk, disk ? disk->sectors : 0, storage_read };
}

int usb_fat32_mount(UsbFat32* volume, const UsbFat32Source* source) {
    if (!volume || !source || !source->sectors || !source->read) return HW_INVALID;
    *volume = (UsbFat32){ .source = *source };
    uint8_t sector[SECTOR], boot[SECTOR];
    int result = source->read(source->context, 0, sector);
    if (result != HW_OK) return result;
    if (geometry(volume, 0, source->sectors, sector) == HW_OK) return HW_OK;
    if (le16(sector + 510) != 0xaa55) return HW_UNSUPPORTED;
    for (uint32_t i = 0; i < 4; i++) {
        const uint8_t* partition = sector + 446 + i * 16;
        uint32_t start = le32(partition + 8), limit = le32(partition + 12);
        if ((partition[4] != 0x0b && partition[4] != 0x0c) || !start ||
            start >= source->sectors || !limit || limit > source->sectors - start) continue;
        result = source->read(source->context, start, boot);
        if (result != HW_OK) return result;
        if (geometry(volume, start, limit, boot) == HW_OK) return HW_OK;
    }
    return HW_UNSUPPORTED;
}

static int next_cluster(const UsbFat32* volume, uint32_t cluster, uint32_t* next) {
    uint8_t sector[SECTOR];
    int result = volume->source.read(volume->source.context, volume->fat + cluster / 128U, sector);
    if (result != HW_OK) return result;
    *next = le32(sector + cluster % 128U * 4) & 0x0fffffffU;
    if (*next >= FAT32_EOF) return 0;
    if (*next == cluster || !valid_cluster(volume, *next)) return HW_PROTOCOL;
    return HW_OK;
}

static int read_sector(const UsbFat32* volume, uint32_t cluster, uint32_t index, void* data) {
    return volume->source.read(volume->source.context,
                               volume->data + (cluster - 2) * volume->sectors_per_cluster + index, data);
}

static int short_name(char output[13], const uint8_t* entry) {
    uint32_t used = 0;
    for (uint32_t i = 0; i < 11; i++) {
        if (entry[i] == ' ') continue;
        if (entry[i] < 0x21 || entry[i] > 0x7e || entry[i] == '/' || entry[i] == 92) return 0;
        if (i == 8 && used) output[used++] = '.';
        output[used++] = (char)entry[i];
    }
    output[used] = 0;
    return used != 0;
}

int usb_fat32_step(const UsbFat32* volume, UsbFat32Directory* cursor, UsbFat32File* file) {
    if (!volume || !volume->root || !cursor || !file) return HW_PROTOCOL;
    if (!cursor->cluster) cursor->cluster = volume->root;
    uint32_t entries = volume->sectors_per_cluster * (SECTOR / 32);
    uint32_t loaded = 0;
    for (;;) {
        if (cursor->cluster == FAT32_EOF) return 0;
        if (!valid_cluster(volume, cursor->cluster) || cursor->entry > entries) return HW_PROTOCOL;
        if (cursor->entry == entries) {
            if (loaded) return USB_FAT32_MORE;
            if (++cursor->visited >= volume->clusters) return HW_PROTOCOL;
            uint32_t next;
            int result = next_cluster(volume, cursor->cluster, &next);
            if (result < 0) return result;
            cursor->cluster = result ? next : FAT32_EOF;
            cursor->entry = 0;
            cursor->cached = 0;
            return result ? USB_FAT32_MORE : 0;
        }
        uint32_t index = cursor->entry / (SECTOR / 32);
        if (!cursor->cached || index != cursor->cached_sector) {
            if (loaded) return USB_FAT32_MORE;
            int result = read_sector(volume, cursor->cluster, index, cursor->sector);
            if (result != HW_OK) return result;
            cursor->cached_sector = index;
            cursor->cached = loaded = 1;
        }
        if (cursor->scanned++ >= MAX_ROOT_ENTRIES) return HW_UNSUPPORTED;
        const uint8_t* entry = cursor->sector + (cursor->entry++ % (SECTOR / 32)) * 32;
        if (!entry[0]) { cursor->cluster = FAT32_EOF; return 0; }
        if (entry[0] == 0xe5 || (entry[11] & 0x18U) || !short_name(file->name, entry)) continue;
        file->cluster = ((le16(entry + 20) << 16) | le16(entry + 26)) & 0x0fffffffU;
        file->bytes = le32(entry + 28);
        if (file->bytes && !valid_cluster(volume, file->cluster)) return HW_PROTOCOL;
        return HW_OK;
    }
}

int usb_fat32_next(const UsbFat32* volume, UsbFat32Directory* cursor, UsbFat32File* file) {
    int result;
    do { result = usb_fat32_step(volume, cursor, file); } while (result == USB_FAT32_MORE);
    return result;
}

int usb_fat32_read(const UsbFat32* volume, const UsbFat32File* file, UsbFat32Reader* reader, void* sector) {
    if (!volume || !volume->root || !file || !reader || !sector || reader->offset > file->bytes)
        return HW_PROTOCOL;
    if (reader->offset == file->bytes) return 0;
    uint32_t count = file->bytes - reader->offset;
    if (count > SECTOR) count = SECTOR;
    uint32_t cluster = reader->offset ? reader->cluster : file->cluster;
    if (!valid_cluster(volume, cluster) || reader->offset % SECTOR ||
        (uint64_t)file->bytes > (uint64_t)volume->clusters * volume->sectors_per_cluster * SECTOR)
        return HW_PROTOCOL;

    uint32_t index = (reader->offset / SECTOR) % volume->sectors_per_cluster;
    if (reader->offset && !index) {
        int result = next_cluster(volume, cluster, &cluster);
        if (result != HW_OK) return result ? result : HW_PROTOCOL;
    }
    if (count == file->bytes - reader->offset) {
        uint32_t next;
        int result = next_cluster(volume, cluster, &next);
        if (result != 0) return result < 0 ? result : HW_PROTOCOL;
    }
    int result = read_sector(volume, cluster, index, sector);
    if (result != HW_OK) return result;
    reader->cluster = cluster;
    reader->offset += count;
    return (int)count;
}
