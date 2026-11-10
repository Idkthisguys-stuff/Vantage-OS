#include <vantage/kernel/files.h>
#include <vantage/kernel/block_store.h>
#define FIXTURE_VOLUME_SECTORS (BLOCK_MIN_SECTORS + 65U)
#include "storage_fixture.h"

static union { uint64_t aligned; unsigned char bytes[1024]; } user;
static unsigned char content[FILE_CAPACITY], output[FILE_CAPACITY];
static unsigned char baseline[DISK_SECTORS][512];
static BlockEntry entries[FILE_LIMIT];

uint32_t user_buffer_valid(uintptr_t address, size_t size) {
    uintptr_t start = (uintptr_t)user.bytes, end = start + sizeof(user.bytes);
    return address >= start && address < end && size <= end - address;
}
static int directory(unsigned int operation, const char* path) {
    FileDirectoryLong request = {0};
    request.operation = operation;
    snprintf(request.path, sizeof(request.path), "%s", path);
    return files_directory_long(1, &request);
}
static void fresh(void) {
    BlockStore store;
    files_release(1); CHECK(files_unmount() == 0);
    fixture_reset();
    CHECK(block_format(&store, &device, &region, STORE_FORMAT_CONFIRM) == 0);
    unsigned int writes = disk.writes;
    CHECK(files_mount(&device, 0) == 0 && disk.writes == writes);
}
static void capacities(void) {
    fresh();
    /* Every slot may hold a real empty directory, not a fixed 1 MiB buffer. */
    for (unsigned int i = 0; i < FILE_LIMIT; i++) {
        char path[48]; snprintf(path, sizeof(path), "/ram/d%u", i);
        CHECK(directory(DIRECTORY_CREATE, path) == 0);
    }
    CHECK(directory(DIRECTORY_CREATE, "/ram/full") == FILE_NO_SPACE);
    CHECK(files_unmount() == 0 && files_mount(&device, 0) == 0);
    FileDirectoryLong request = {0}; request.operation = DIRECTORY_LIST;
    memcpy(request.path, "/ram", 5);
    unsigned int count = 0;
    while (files_directory_long(1, &request) == 1) count++;
    CHECK(count == FILE_LIMIT);
    fresh();
    int64_t h = files_open(1, "/ram/big", FILE_READ | FILE_WRITE | FILE_CREATE);
    CHECK(h > 0);
    for (unsigned int i = 0; i < FILE_CAPACITY; i++) content[i] = (unsigned char)(i * 17U + i / 511U);
    CHECK(files_write(1, (uint64_t)h, content, FILE_CAPACITY) == FILE_CAPACITY);
    CHECK(files_write(1, (uint64_t)h, content, 1) == FILE_NO_SPACE);
    CHECK(files_seek(1, (uint64_t)h, 510) == 0);
    CHECK(files_write(1, (uint64_t)h, "cross-sector", 12) == 12);
    memcpy(content + 510, "cross-sector", 12);
    CHECK(files_seek(1, (uint64_t)h, FILE_CAPACITY - 1) == 0);
    CHECK(files_write(1, (uint64_t)h, "z", 1) == 1); content[FILE_CAPACITY - 1] = 'z';
    CHECK(files_close(1, (uint64_t)h) == 0 && files_unmount() == 0 && files_mount(&device, 0) == 0);
    h = files_open(1, "/ram/big", FILE_READ);
    CHECK(h > 0 && files_read(1, (uint64_t)h, output, FILE_CAPACITY) == FILE_CAPACITY);
    CHECK(!memcmp(content, output, FILE_CAPACITY));
    CHECK(files_close(1, (uint64_t)h) == 0);
    /* Shared capacity exhaustion is rejected before any I/O or namespace change. */
    h = files_open(1, "/ram/other", FILE_READ | FILE_WRITE | FILE_CREATE);
    unsigned int writes = disk.writes;
    CHECK(files_write(1, (uint64_t)h, content, FILE_CAPACITY) == FILE_NO_SPACE && disk.writes == writes);
    CHECK(files_read(1, (uint64_t)h, output, 1) == 0);
    CHECK(files_write(1, (uint64_t)h, content, 1) == 1);
    CHECK(files_close(1, (uint64_t)h) == 0);
    CHECK(files_remove(1, "/ram/big") == 0);
    h = files_open(1, "/ram/other", FILE_READ | FILE_WRITE);
    CHECK(files_write(1, (uint64_t)h, content, FILE_CAPACITY) == FILE_CAPACITY);
    int64_t trunc = files_open(1, "/ram/other", FILE_WRITE | FILE_TRUNCATE);
    CHECK(trunc > 0 && files_close(1, (uint64_t)trunc) == 0);
    CHECK(files_read(1, (uint64_t)h, output, 1) == 0);
    CHECK(files_write(1, (uint64_t)h, content, 1) == FILE_BAD);
    CHECK(files_seek(1, (uint64_t)h, 0) == 0 && files_write(1, (uint64_t)h, "z", 1) == 1);
    CHECK(files_close(1, (uint64_t)h) == 0);
}
static void long_paths_and_abi(void) {
    fresh();
    char path[FILE_PATH_MAX] = "/ram";
    /* Seven 31-byte directory components plus a 26-byte file: exactly 255 bytes. */
    unsigned int length = 4;
    for (unsigned int depth = 0; depth < 7; depth++) {
        path[length++] = '/'; memset(path + length, (int)('a' + depth), 31); length += 31; path[length] = 0;
        CHECK(directory(DIRECTORY_CREATE, path) == 0);
    }
    path[length++] = '/'; memset(path + length, 'z', 26); length += 26; path[length] = 0;
    CHECK(length == 255);
    FileOpenLong* open = (FileOpenLong*)user.bytes;
    memset(open, 0, sizeof(*open)); memcpy(open->path, path, sizeof(path)); open->flags = FILE_READ | FILE_WRITE | FILE_CREATE;
    int64_t h = handle_file_call(SYS_FILE_OPEN_LONG, (uintptr_t)user.bytes, 1);
    CHECK(h > 0 && files_write(1, (uint64_t)h, "long", 4) == 4 && files_close(1, (uint64_t)h) == 0);
    FileDirectoryLong* request = (FileDirectoryLong*)user.bytes;
    memset(request, 0, sizeof(*request)); memcpy(request->path, path, sizeof(path));
    CHECK(handle_file_call(SYS_FILE_DIRECTORY_LONG, (uintptr_t)user.bytes, 1) == 0);
    CHECK(request->size == 4 && !strcmp(request->entry, path));
    CHECK(files_unmount() == 0 && files_mount(&device, 0) == 0);
    CHECK(directory(DIRECTORY_STAT, path) == 0);
    char too_long[257]; memcpy(too_long, path, 255); too_long[255] = 'z'; too_long[256] = 0;
    CHECK(files_open(1, too_long, FILE_READ) == FILE_BAD);
    memset(open, 'x', sizeof(*open)); open->flags = FILE_READ; open->reserved = 0;
    CHECK(handle_file_call(SYS_FILE_OPEN_LONG, (uintptr_t)user.bytes, 1) == FILE_BAD);
    CHECK(handle_file_call(SYS_FILE_DIRECTORY_LONG, (uintptr_t)user.bytes + 1, 1) == FILE_BAD);
    CHECK(handle_file_call(SYS_FILE_DIRECTORY_LONG, (uintptr_t)user.bytes + 800, 1) == FILE_BAD);
    FileOpen* old = (FileOpen*)user.bytes;
    memset(old, 'x', sizeof(*old)); old->flags = FILE_READ; old->reserved = 0;
    CHECK(handle_file_call(SYS_FILE_OPEN, (uintptr_t)user.bytes, 1) == FILE_BAD);
    FilePathLong* remove = (FilePathLong*)user.bytes;
    memcpy(remove->path, path, sizeof(path));
    CHECK(handle_file_call(SYS_FILE_REMOVE_LONG, (uintptr_t)user.bytes, 1) == 0);
}
static void mixed_io(void) {
    fresh();
    int64_t h = files_open(1, "/ram/model", FILE_READ | FILE_WRITE | FILE_CREATE);
    CHECK(h > 0);
    memset(content, 0, sizeof(content));
    uint32_t seed = 0x5ef192abU, size = 0, offset = 0;
    unsigned char buffer[700];
    for (unsigned int i = 0; i < 1500; i++) {
        seed = seed * 1664525U + 1013904223U;
        uint32_t length = (seed >> 16) % (uint32_t)sizeof(buffer);
        if ((seed & 3U) == 0) {
            uint32_t next = (seed >> 8) % (size + 16U);
            int result = files_seek(1, (uint64_t)h, next);
            if (next <= size) { CHECK(result == 0); offset = next; }
            else CHECK(result == FILE_BAD);
        } else if ((seed & 3U) == 1) {
            memset(buffer, (int)(seed >> 24), sizeof(buffer));
            CHECK(files_write(1, (uint64_t)h, buffer, length) == (int)length);
            memcpy(content + offset, buffer, length); offset += length;
            if (offset > size) size = offset;
        } else {
            uint32_t count = length < size - offset ? length : size - offset;
            CHECK(files_read(1, (uint64_t)h, buffer, length) == (int)count);
            CHECK(!memcmp(buffer, content + offset, count)); offset += count;
        }
    }
    CHECK(files_close(1, (uint64_t)h) == 0 && files_unmount() == 0 && files_mount(&device, 0) == 0);
    h = files_open(1, "/ram/model", FILE_READ);
    CHECK(h > 0 && files_read(1, (uint64_t)h, output, size) == (int)size && !memcmp(content, output, size));
    CHECK(files_seek(1, (uint64_t)h, 0) == 0);
    disk.fail_at = disk.calls + 1;
    CHECK(files_read(1, (uint64_t)h, output, 1) == FILE_IO_ERROR);
    CHECK(files_read(1, (uint64_t)h, output, 1) == FILE_UNAVAILABLE);
    disk.fail_at = 0; CHECK(files_close(1, (uint64_t)h) == 0);
}
static uint32_t word(const unsigned char* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void corruption(void) {
    for (unsigned int kind = 0; kind < 11; kind++) {
        fresh(); CHECK(files_unmount() == 0);
        BlockStore store;
        CHECK(block_mount(&store, &device, &region, entries) == 0);
        entries[0].used = FILE_DIRECTORY; memcpy(entries[0].name, "dir", 4);
        entries[1].used = FILE_REGULAR; entries[1].size = 3; entries[1].parent = 1;
        memcpy(entries[1].name, "note", 5);
        BlockPatch patch = {1, 0, 3, "old"};
        CHECK(block_commit(&store, entries, &patch) == 0);
        unsigned char* header = disk.bytes[region.first + store.snapshot.active * (region.sectors / 2)];
        unsigned char* a = header + 512;
        unsigned char* b = a + 64;
        if (kind == 0) fixture_put32(a + 40, FILE_LIMIT + 1);
        if (kind == 1) fixture_put32(a + 40, 1);
        if (kind == 2) fixture_put32(a + 32, FILE_REGULAR);
        if (kind == 3) memcpy(a + 128, a, 64);
        if (kind == 4) fixture_put32(b + 44, BLOCK_META_SECTORS + 1);
        if (kind == 5) fixture_put32(b + 36, FILE_CAPACITY + 1);
        if (kind == 6) a[48] = 1;
        if (kind == 7) a[10] = 'x';
        if (kind == 8) fixture_put32(a + 128 + 44, 1);
        if (kind == 9) header[(1 + BLOCK_META_SECTORS) * 512 + 511] = 1;
        if (kind == 10) fixture_put32(a + 36, 1);
        fixture_put32(header + 24, store_crc32(0, header + 512, word(header + 28) * 512));
        fixture_put32(header + 508, store_crc32(0, header, 508));
        unsigned int writes = disk.writes;
        CHECK(block_mount(&store, &device, &region, entries) == STORE_MOUNT_RECOVERED && disk.writes == writes);
        CHECK(!entries[0].used);
    }
    fresh(); CHECK(files_unmount() == 0);
    BlockStore store;
    CHECK(block_mount(&store, &device, &region, entries) == 0);
    store.snapshot.generation = ~(uint64_t)0;
    unsigned int writes = disk.writes;
    CHECK(block_commit(&store, entries, NULL) == FILE_NO_SPACE && disk.writes == writes);
    disk.fail_at = disk.calls + 1;
    CHECK(block_mount(&store, &device, &region, entries) == FILE_IO_ERROR && store.snapshot.state == -1);
    disk.fail_at = 0;
    fresh(); CHECK(files_unmount() == 0);
    /* Explicit provisioning of a second geometry must not make mixed valid
     * formats mountable; regular tooling invalidates both layouts first. */
    Store legacy;
    CHECK(store_format(&legacy, &device, &region, STORE_FORMAT_CONFIRM) == 0);
    writes = disk.writes;
    CHECK(files_mount(&device, 0) == FILE_CORRUPT && disk.writes == writes);
    fresh(); CHECK(files_unmount() == 0);
    unsigned char* header = disk.bytes[region.first];
    fixture_put32(header + 16, 2);
    fixture_put32(header + 508, store_crc32(0, header, 508));
    CHECK(block_mount(&store, &device, &region, entries) == FILE_CORRUPT);
    fresh(); CHECK(files_unmount() == 0);
    disk.bytes[region.first][0] ^= 1;
    disk.bytes[region.first + region.sectors / 2][0] ^= 1;
    writes = disk.writes;
    CHECK(files_mount(&device, 0) == FILE_CORRUPT && disk.writes == writes);
}
static void recovery(void) {
    fresh(); CHECK(files_unmount() == 0);
    BlockStore store;
    CHECK(block_mount(&store, &device, &region, entries) == 0);
    entries[0].used = FILE_REGULAR; entries[0].size = 3; memcpy(entries[0].name, "note", 5);
    BlockPatch patch = {0, 0, 3, "old"};
    CHECK(block_commit(&store, entries, &patch) == 0);
    entries[1].used = FILE_REGULAR; entries[1].size = 3; memcpy(entries[1].name, "other", 6);
    BlockPatch seed = {1, 0, 3, "yes"};
    CHECK(block_commit(&store, entries, &seed) == 0);
    memcpy(baseline, disk.bytes, sizeof(baseline));
    entries[2].used = FILE_DIRECTORY; memcpy(entries[2].name, "empty", 6);
    memset(content, 'n', 600);
    patch = (BlockPatch){0, 1, 600, content}; entries[0].size = 601;
    disk.calls = 0;
    CHECK(block_commit(&store, entries, &patch) == 0);
    unsigned int boundaries = disk.calls;
    for (unsigned int buffered = 0; buffered <= 1; buffered++) {
        for (unsigned int boundary = 1; boundary <= boundaries; boundary++) {
            const unsigned int tears[] = {0, 1, 255, 511, 512};
            for (unsigned int t = 0; t < sizeof(tears) / sizeof(tears[0]); t++) {
                memcpy(disk.bytes, baseline, sizeof(baseline)); disk.fail_at = 0; disk.buffered = 0;
                CHECK(block_mount(&store, &device, &region, entries) == 0);
                entries[0].size = 601;
                entries[2].used = FILE_DIRECTORY; memcpy(entries[2].name, "empty", 6);
                memcpy(disk.pending, disk.bytes, sizeof(disk.bytes));
                disk.calls = 0; disk.fail_at = boundary; disk.tear = tears[t]; disk.buffered = buffered;
                CHECK(block_commit(&store, entries, &patch) == FILE_IO_ERROR && store.snapshot.state == -1);
                disk.fail_at = 0; disk.buffered = 0;
                int result = block_mount(&store, &device, &region, entries);
                CHECK(result == 0 || result == STORE_MOUNT_RECOVERED);
                CHECK(entries[0].size == 3 || entries[0].size == 601);
                CHECK(block_read(&store, &entries[0], 0, output, entries[0].size) == 0);
                if (entries[0].size == 3) CHECK(!memcmp(output, "old", 3));
                else CHECK(output[0] == 'o' && !memcmp(output + 1, content, 600));
                CHECK(entries[2].used == (entries[0].size == 601 ? FILE_DIRECTORY : 0));
                CHECK(entries[1].used == FILE_REGULAR && entries[1].size == 3);
                CHECK(block_read(&store, &entries[1], 0, output, 3) == 0 && !memcmp(output, "yes", 3));
                CHECK(!memcmp(disk.bytes[0], baseline[0], 8 * 512));
                CHECK(!memcmp(disk.bytes[region.first + region.sectors], baseline[region.first + region.sectors], 8 * 512));
                CHECK(!memcmp(disk.bytes[region.first + region.sectors - 1], baseline[region.first + region.sectors - 1], 512));
            }
        }
    }
    printf("PASS: V3 %u cut points, five tears, direct/buffered devices\n", boundaries);
}
int main(void) {
    capacities(); long_paths_and_abi(); mixed_io(); corruption(); recovery();
    puts("PASS: 256 entries, 255-byte paths, 1 MiB files, shared capacity and additive ABI");
    return 0;
}
