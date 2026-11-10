#include "storage_fixture.h"

static StoredFile image[FILE_LIMIT], recovered[FILE_LIMIT];
static unsigned char baseline[DISK_SECTORS][512];

static void formatted(Store* store) {
    fixture_reset();
    CHECK(store_format(store, &device, &region, STORE_FORMAT_CONFIRM) == 0);
    CHECK(store_mount(store, &device, &region, image) == 0);
}
static void headers_rechecksum(unsigned int bank) {
    unsigned char* header = disk.bytes[region.first + bank * STORE_BANK_SECTORS];
    uint32_t crc = store_crc32(0, header + 512, STORE_PAYLOAD_SECTORS * 512);
    fixture_put32(header + 24, crc);
    fixture_put32(header + 508, store_crc32(0, header, 508));
}
static void partition_checks(void) {
    StoreRegion out = {0, 0};
    fixture_reset();
    CHECK(store_partition(&device, 0, &out) == 0 && out.first == 8 && out.sectors == STORE_MIN_SECTORS);
    CHECK(store_partition(&device, 4, &out) == FILE_BAD);
    CHECK(store_partition(&device, 1, &out) == FILE_BAD);
    disk.bytes[0][510] = 0;
    CHECK(store_partition(&device, 0, &out) == FILE_CORRUPT);
    fixture_reset(); disk.bytes[0][450] = 0x0c;
    CHECK(store_partition(&device, 0, &out) == FILE_BAD);
    fixture_reset(); fixture_put32(disk.bytes[0] + 454, 0);
    CHECK(store_partition(&device, 0, &out) == FILE_CORRUPT);
    fixture_reset(); fixture_put32(disk.bytes[0] + 458, 0xffffffffU);
    CHECK(store_partition(&device, 0, &out) == FILE_CORRUPT);
    fixture_reset(); memcpy(disk.bytes[0] + 462, disk.bytes[0] + 446, 16);
    CHECK(store_partition(&device, 0, &out) == FILE_CORRUPT);
    fixture_reset(); disk.bytes[0][466] = 0xee;
    CHECK(store_partition(&device, 0, &out) == FILE_CORRUPT);
    CHECK(!disk.writes);
}
static void corruption_checks(void) {
    Store store;
    fixture_reset();
    CHECK(store_mount(&store, &device, &region, image) == FILE_CORRUPT && !disk.writes);
    CHECK(store_format(&store, &device, &region, 0) == FILE_BAD && !disk.writes);
    formatted(&store);
    image[0].used = 1; image[0].size = 5; memcpy(image[0].name, "hello", 6); memcpy(image[0].data, "world", 5);
    CHECK(store_commit(&store, image) == 0);
    unsigned int newest = store.active;
    uint64_t first = region.first + newest * STORE_BANK_SECTORS;
    disk.bytes[first + 2][0] ^= 1;
    CHECK(store_mount(&store, &device, &region, recovered) == STORE_MOUNT_RECOVERED && !recovered[0].used);
    disk.bytes[region.first + (newest ^ 1U) * STORE_BANK_SECTORS][0] ^= 1;
    CHECK(store_mount(&store, &device, &region, recovered) == FILE_CORRUPT && !store.state);
    for (unsigned int kind = 0; kind < 5; kind++) {
        formatted(&store);
        unsigned int bank = store.active;
        unsigned char* metadata = disk.bytes[region.first + bank * STORE_BANK_SECTORS + 1];
        memcpy(metadata, "bad", 4); fixture_put32(metadata + 32, 1);
        if (kind == 0) fixture_put32(metadata + 36, STORE_LEGACY_CAPACITY + 1);
        if (kind == 1) metadata[0] = '/';
        if (kind == 2) fixture_put32(metadata + 32, 3);
        if (kind == 3) metadata[44] = 1;
        if (kind == 4) memcpy(metadata + STORE_FILE_SECTORS * 512, metadata, 512);
        headers_rechecksum(bank);
        CHECK(store_mount(&store, &device, &region, recovered) == STORE_MOUNT_RECOVERED);
    }
    formatted(&store); store.generation = ~(uint64_t)0;
    unsigned int writes = disk.writes;
    CHECK(store_commit(&store, image) == FILE_NO_SPACE && disk.writes == writes);
    formatted(&store);
    fixture_put32(disk.bytes[region.first] + 16, 2);
    headers_rechecksum(0);
    CHECK(store_mount(&store, &device, &region, image) == FILE_CORRUPT);
    formatted(&store);
    disk.fail_at = disk.calls + 1;
    CHECK(store_mount(&store, &device, &region, image) == FILE_IO_ERROR && store.state == -1);
    disk.fail_at = 0;
}
static void power_cuts(unsigned int legacy) {
    Store store;
    formatted(&store);
    image[0].used = 1; image[0].size = 3;
    memcpy(image[0].name, "note", 5); memcpy(image[0].data, "old", 3);
    if (!legacy) {
        image[1].used = FILE_DIRECTORY; memcpy(image[1].name, "docs", 5);
        image[0].parent = 2;
    }
    CHECK(store_commit(&store, image) == 0);
    if (legacy) {
        for (unsigned int bank = 0; bank < 2; bank++) {
            fixture_put32(disk.bytes[region.first + bank * STORE_BANK_SECTORS] + 8, 1);
            headers_rechecksum(bank);
        }
    }
    memcpy(baseline, disk.bytes, sizeof(baseline));
    memcpy(image[0].data, "new", 3);
    image[2].used = FILE_DIRECTORY; memcpy(image[2].name, "empty", 6);
    disk.calls = 0;
    CHECK(store_commit(&store, image) == 0);
    unsigned int boundaries = disk.calls;
    for (unsigned int buffered = 0; buffered <= 1; buffered++) {
        for (unsigned int boundary = 1; boundary <= boundaries; boundary++) {
            const unsigned int tears[] = {0, 1, 255, 511, 512};
            for (unsigned int t = 0; t < sizeof(tears) / sizeof(tears[0]); t++) {
                memcpy(disk.bytes, baseline, sizeof(baseline));
                disk.fail_at = 0; disk.buffered = 0;
                CHECK(store_mount(&store, &device, &region, recovered) == 0);
                memcpy(disk.pending, disk.bytes, sizeof(disk.bytes));
                disk.calls = 0; disk.fail_at = boundary; disk.tear = tears[t]; disk.buffered = buffered;
                CHECK(store_commit(&store, image) == FILE_IO_ERROR && store.state == -1);
                CHECK(store_commit(&store, image) == FILE_UNAVAILABLE);
                /* Reboot loses volatile pending writes. Every recoverable state
                 * is exactly the old or new acknowledged-file snapshot. */
                disk.fail_at = 0; disk.buffered = 0;
                int result = store_mount(&store, &device, &region, recovered);
                CHECK(result == 0 || result == STORE_MOUNT_RECOVERED);
                CHECK(recovered[0].used == 1 && recovered[0].size == 3);
                CHECK(!memcmp(recovered[0].data, "old", 3) || !memcmp(recovered[0].data, "new", 3));
                CHECK(recovered[0].parent == (legacy ? 0U : 2U));
                CHECK(recovered[1].used == (legacy ? 0U : FILE_DIRECTORY));
                CHECK(recovered[2].used == (!memcmp(recovered[0].data, "new", 3) ? FILE_DIRECTORY : 0));
                CHECK(!memcmp(disk.bytes[0], baseline[0], 8 * 512));
                CHECK(!memcmp(disk.bytes[region.first + region.sectors], baseline[region.first + region.sectors], 8 * 512));
            }
        }
    }
    printf("PASS: %u I/O cut points, five torn-sector lengths, direct and sync-buffered devices (v%u baseline)\n",
           boundaries, legacy ? 1U : 2U);
}
static void versions_and_trees(void) {
    Store store;
    formatted(&store);
    image[0].used = FILE_REGULAR; image[0].size = 3;
    memcpy(image[0].name, "legacy", 7); memcpy(image[0].data, "old", 3);
    CHECK(store_commit(&store, image) == 0);
    for (unsigned int bank = 0; bank < 2; bank++) {
        fixture_put32(disk.bytes[region.first + bank * STORE_BANK_SECTORS] + 8, 1);
        headers_rechecksum(bank);
    }
    unsigned int writes = disk.writes;
    CHECK(store_mount(&store, &device, &region, recovered) == 0 && disk.writes == writes);
    CHECK(recovered[0].used == 1 && !recovered[0].parent && !memcmp(recovered[0].data, "old", 3));
    recovered[1].used = FILE_DIRECTORY; memcpy(recovered[1].name, "new", 4);
    CHECK(store_commit(&store, recovered) == 0);
    CHECK(disk.bytes[region.first + store.active * STORE_BANK_SECTORS][8] == 2);
    CHECK(store_mount(&store, &device, &region, image) == 0 && image[1].used == FILE_DIRECTORY);
    for (unsigned int kind = 0; kind < 10; kind++) {
        formatted(&store);
        unsigned int bank = store.active;
        unsigned char* a = disk.bytes[region.first + bank * STORE_BANK_SECTORS + 1];
        unsigned char* b = a + STORE_FILE_SECTORS * 512;
        memcpy(a, "dir", 4); fixture_put32(a + 32, FILE_DIRECTORY);
        memcpy(b, "child", 6); fixture_put32(b + 32, FILE_REGULAR); fixture_put32(b + 40, 1);
        if (kind == 0) fixture_put32(b + 40, FILE_LIMIT + 1);
        if (kind == 1) fixture_put32(a + 40, 1); /* Self cycle. */
        if (kind == 2) fixture_put32(a + 32, FILE_REGULAR); /* File parent. */
        if (kind == 3) { fixture_put32(a + 40, 2); fixture_put32(b + 32, FILE_DIRECTORY); }
        if (kind == 4) { memcpy(b, a, 512); } /* Duplicate sibling. */
        if (kind == 5) fixture_put32(a + 36, 1); /* Directory data. */
        if (kind == 6) { memset(a, 'a', 31); memset(b, 'b', 31); } /* Unaddressable tree. */
        if (kind == 7) fixture_put32(b + 40, 3); /* Unused parent. */
        if (kind == 8) { memset(b, 0, 512); fixture_put32(b + 40, 1); }
        if (kind == 9) a[10] = 'x'; /* Noncanonical name tail. */
        headers_rechecksum(bank);
        CHECK(store_mount(&store, &device, &region, recovered) == STORE_MOUNT_RECOVERED);
    }
}
int main(void) {
    CHECK(store_crc32(0, "123456789", 9) == 0xcbf43926U);
    CHECK(store_crc32(store_crc32(0, "1234", 4), "56789", 5) == 0xcbf43926U);
    partition_checks(); corruption_checks(); versions_and_trees(); power_cuts(0); power_cuts(1);
    return 0;
}
