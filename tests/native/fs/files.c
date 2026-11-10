#include <vantage/kernel/files.h>
#include "storage_fixture.h"
/* This suite intentionally exercises the frozen v1/v2 compatibility backend. */
#undef FILE_LIMIT
#undef FILE_CAPACITY
#define FILE_LIMIT STORE_LEGACY_LIMIT
#define FILE_CAPACITY STORE_LEGACY_CAPACITY

static union { uint64_t aligned; unsigned char bytes[1024]; } user;

uint32_t user_buffer_valid(uintptr_t address, size_t size) {
    uintptr_t start = (uintptr_t)user.bytes, end = start + sizeof(user.bytes);
    return address >= start && address < end && size <= end - address;
}

static void clean(void) {
    files_release(1); files_release(2);
    FileEntry entry = {0};
    while (files_list(1, &entry) == 1) CHECK(files_remove(1, entry.path) == 0);
}

static void paths_and_modes(void) {
    const char* bad[] = {"", "/ram/", "/sd/a", "/ram/.", "/ram/..",
                         "/ram/a b",
                         "/ram/12345678901234567890123456789012"};
    for (unsigned int i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(files_open(1, bad[i], FILE_WRITE | FILE_CREATE) == FILE_BAD);
    CHECK(files_open(1, NULL, FILE_READ) == FILE_BAD);
    CHECK(files_open(0, "/ram/a", FILE_READ) == FILE_UNAVAILABLE);
    CHECK(files_open(1, "/ram/a", FILE_READ) == FILE_NOT_FOUND);
    CHECK(files_open(1, "/ram/a", 0) == FILE_BAD);
    CHECK(files_open(1, "/ram/a", 0x100) == FILE_BAD);
    CHECK(files_open(1, "/ram/a", FILE_READ | FILE_CREATE) == FILE_BAD);
    CHECK(files_open(1, "/ram/a", FILE_READ | FILE_TRUNCATE) == FILE_BAD);
    int64_t h = files_open(1, "/ram/a", FILE_WRITE | FILE_CREATE);
    CHECK(h > 0);
    char out[8] = {0};
    CHECK(files_read(1, (uint64_t)h, out, 1) == FILE_ACCESS);
    CHECK(files_write(1, (uint64_t)h, "hello", 5) == 5);
    CHECK(files_close(1, (uint64_t)h) == 0);
    h = files_open(1, "/ram/a", FILE_READ);
    CHECK(h > 0);
    CHECK(files_write(1, (uint64_t)h, "x", 1) == FILE_ACCESS);
    CHECK(files_read(1, (uint64_t)h, out, 8) == 5 && !memcmp(out, "hello", 5));
    CHECK(files_read(1, (uint64_t)h, out, 8) == 0);
    CHECK(files_seek(1, (uint64_t)h, 6) == FILE_BAD);
    CHECK(files_seek(1, (uint64_t)h, 0) == 0);
    CHECK(files_read(1, (uint64_t)h, NULL, 0) == 0);
    CHECK(files_read(1, (uint64_t)h, NULL, 1) == FILE_BAD);
    clean();
}

static int directory(unsigned int op, const char* path, FileDirectory* out) {
    FileDirectory request = {0};
    request.operation = op;
    snprintf(request.path, sizeof(request.path), "%s", path);
    int result = files_directory(1, &request);
    if (out) *out = request;
    return result;
}

static void directories(void) {
    FileDirectory entry;
    CHECK(directory(DIRECTORY_CREATE, "/ram/empty", NULL) == 0);
    CHECK(directory(DIRECTORY_CREATE, "/ram/docs", NULL) == 0);
    CHECK(directory(DIRECTORY_CREATE, "/ram/docs/sub", NULL) == 0);
    CHECK(directory(DIRECTORY_CREATE, "/ram/docs", NULL) == FILE_BAD);
    CHECK(files_open(1, "/ram/docs", FILE_WRITE | FILE_CREATE) == FILE_BAD);
    CHECK(files_remove(1, "/ram/docs") == FILE_BAD);
    CHECK(directory(DIRECTORY_REMOVE, "/ram", NULL) == FILE_BAD);
    CHECK(directory(DIRECTORY_REMOVE, "/ram/docs", NULL) == FILE_BUSY);
    CHECK(directory(DIRECTORY_CREATE, "/ram/missing/sub", NULL) == FILE_NOT_FOUND);
    CHECK(files_open(1, "/ram/missing/../bad", FILE_WRITE | FILE_CREATE) == FILE_NOT_FOUND);
    CHECK(directory(DIRECTORY_STAT, "/ram/../../.", &entry) == 0);
    CHECK(entry.type == FILE_DIRECTORY && !strcmp(entry.entry, "/ram"));
    int64_t h = files_open(1, "docs/sub/../note", FILE_READ | FILE_WRITE | FILE_CREATE);
    CHECK(h > 0 && files_write(1, (uint64_t)h, "nested", 6) == 6);
    CHECK(files_close(1, (uint64_t)h) == 0);
    CHECK(directory(DIRECTORY_CREATE, "/ram/docs/note", NULL) == FILE_BAD);
    CHECK(files_open(1, "/ram/docs/note/../bad", FILE_WRITE | FILE_CREATE) == FILE_BAD);
    CHECK(directory(DIRECTORY_STAT, "/ram/docs/./sub/..", &entry) == 0);
    CHECK(!strcmp(entry.entry, "/ram/docs"));
    CHECK(directory(DIRECTORY_LIST, "/ram/docs", &entry) == 1);
    CHECK(entry.type == FILE_DIRECTORY && !strcmp(entry.entry, "/ram/docs/sub"));
    CHECK(files_directory(1, &entry) == 1 && entry.type == FILE_REGULAR && entry.size == 6);
    CHECK(files_directory(1, &entry) == 0);
    CHECK(files_unmount() == 0 && files_mount(&device, 0) == 0);
    CHECK(directory(DIRECTORY_STAT, "/ram/empty", &entry) == 0 && entry.type == FILE_DIRECTORY);
    h = files_open(1, "/ram/docs/note", FILE_READ);
    char data[8];
    CHECK(h > 0 && files_read(1, (uint64_t)h, data, 8) == 6 && !memcmp(data, "nested", 6));
    CHECK(files_close(1, (uint64_t)h) == 0);
    CHECK(files_remove(1, "/ram/docs/note") == 0);
    CHECK(directory(DIRECTORY_REMOVE, "/ram/docs/sub", NULL) == 0);
    CHECK(directory(DIRECTORY_REMOVE, "/ram/docs", NULL) == 0);
    CHECK(directory(DIRECTORY_REMOVE, "/ram/empty", NULL) == 0);
    /* Shared entry capacity includes empty directories. */
    for (unsigned int i = 0; i < FILE_LIMIT; i++) {
        char path[48]; snprintf(path, sizeof(path), "/ram/d%u", i);
        CHECK(directory(DIRECTORY_CREATE, path, NULL) == 0);
    }
    CHECK(directory(DIRECTORY_CREATE, "/ram/full", NULL) == FILE_NO_SPACE);
    CHECK(files_open(1, "/ram/full", FILE_WRITE | FILE_CREATE) == FILE_NO_SPACE);
    for (unsigned int i = 0; i < FILE_LIMIT; i++) {
        char path[48]; snprintf(path, sizeof(path), "/ram/d%u", i);
        CHECK(directory(DIRECTORY_REMOVE, path, NULL) == 0);
    }
    CHECK(directory(DIRECTORY_CREATE, "/ram/1234567890123456789012345678901", NULL) == 0);
    CHECK(directory(DIRECTORY_CREATE, "/ram/1234567890123456789012345678901/1234567890", NULL) == 0);
    CHECK(directory(DIRECTORY_CREATE, "/ram/1234567890123456789012345678901/12345678901", NULL) == FILE_BAD);
    CHECK(directory(DIRECTORY_REMOVE, "/ram/1234567890123456789012345678901/1234567890", NULL) == 0);
    CHECK(directory(DIRECTORY_REMOVE, "/ram/1234567890123456789012345678901", NULL) == 0);
    FileDirectory* request = (FileDirectory*)user.bytes;
    memset(request, 0, sizeof(*request));
    memcpy(request->path, "/ram/sysdir", 12); request->operation = DIRECTORY_CREATE;
    CHECK(handle_file_call(SYS_FILE_DIRECTORY, (uintptr_t)request, 0) == FILE_UNAVAILABLE);
    CHECK(handle_file_call(SYS_FILE_DIRECTORY, (uintptr_t)request + 1, 1) == FILE_BAD);
    CHECK(handle_file_call(SYS_FILE_DIRECTORY, (uintptr_t)request, 1) == 0);
    request->operation = DIRECTORY_STAT;
    CHECK(handle_file_call(SYS_FILE_DIRECTORY, (uintptr_t)request, 1) == 0 && request->type == FILE_DIRECTORY);
    request->operation = DIRECTORY_REMOVE;
    CHECK(handle_file_call(SYS_FILE_DIRECTORY, (uintptr_t)request, 1) == 0);
    request->operation = 99;
    CHECK(handle_file_call(SYS_FILE_DIRECTORY, (uintptr_t)request, 1) == FILE_BAD);
    request->operation = DIRECTORY_STAT;
    CHECK(handle_file_call(SYS_FILE_DIRECTORY, (uintptr_t)user.bytes + sizeof(user.bytes) - 48, 1) == FILE_BAD);
    memset(request->path, 'x', sizeof(request->path));
    CHECK(handle_file_call(SYS_FILE_DIRECTORY, (uintptr_t)request, 1) == FILE_BAD);
}

static void capacity_and_atomic_open(void) {
    uint64_t handles[FILE_HANDLE_LIMIT];
    for (unsigned int i = 0; i < FILE_HANDLE_LIMIT; i++) {
        int64_t h = files_open(1, "/ram/keep", FILE_READ | FILE_WRITE | FILE_CREATE);
        CHECK(h > 0); handles[i] = (uint64_t)h;
    }
    CHECK(files_write(1, handles[0], "saved", 5) == 5);
    CHECK(files_open(1, "/ram/keep", FILE_WRITE | FILE_TRUNCATE) == FILE_NO_SPACE);
    CHECK(files_open(1, "/ram/absent", FILE_WRITE | FILE_CREATE) == FILE_NO_SPACE);
    CHECK(files_close(1, handles[1]) == 0);
    CHECK(files_open(1, "/ram/absent", FILE_READ) == FILE_NOT_FOUND);
    char out[8] = {0};
    CHECK(files_read(1, handles[2], out, 8) == 5 && !memcmp(out, "saved", 5));
    CHECK(files_seek(1, handles[0], 0) == 0);
    unsigned char full[FILE_CAPACITY]; memset(full, 0xa5, sizeof(full));
    CHECK(files_write(1, handles[0], full, sizeof(full)) == FILE_CAPACITY);
    CHECK(files_write(1, handles[0], full, 1) == FILE_NO_SPACE);
    CHECK(files_write(1, handles[0], full, 0xffffffffU) == FILE_NO_SPACE);
    CHECK(files_seek(1, handles[0], 0) == 0);
    CHECK(files_read(1, handles[0], full, sizeof(full)) == FILE_CAPACITY);
    for (unsigned int i = 0; i < sizeof(full); i++) CHECK(full[i] == 0xa5);
    int64_t trunc = files_open(2, "/ram/keep", FILE_WRITE | FILE_TRUNCATE);
    CHECK(trunc > 0);
    CHECK(files_read(1, handles[0], full, 1) == 0);
    CHECK(files_write(1, handles[0], full, 1) == FILE_BAD);
    clean();
    for (unsigned int i = 0; i < FILE_LIMIT; i++) {
        char path[FILE_PATH_MAX]; snprintf(path, sizeof(path), "/ram/f%u", i);
        int64_t h = files_open(1, path, FILE_WRITE | FILE_CREATE);
        CHECK(h > 0); CHECK(files_close(1, (uint64_t)h) == 0);
    }
    CHECK(files_open(1, "/ram/overflow", FILE_WRITE | FILE_CREATE) == FILE_NO_SPACE);
    clean();
}

static void ownership_and_reuse(void) {
    int64_t h = files_open(1, "/ram/shared", FILE_READ | FILE_WRITE | FILE_CREATE);
    CHECK(h > 0);
    CHECK(files_write(2, (uint64_t)h, "x", 1) == FILE_BAD_HANDLE);
    CHECK(files_close(2, (uint64_t)h) == FILE_BAD_HANDLE);
    CHECK(files_remove(2, "/ram/shared") == FILE_BUSY);
    CHECK(files_write(1, (uint64_t)h, "x", 1) == 1);
    files_release(1);
    CHECK(files_close(1, (uint64_t)h) == FILE_BAD_HANDLE);
    int64_t fresh = files_open(2, "/ram/shared", FILE_READ);
    CHECK(fresh > 0 && fresh != h);
    char c = 0;
    CHECK(files_read(2, (uint64_t)h, &c, 1) == FILE_BAD_HANDLE);
    CHECK(files_read(2, (uint64_t)fresh, &c, 1) == 1 && c == 'x');
    CHECK(files_close(2, (uint64_t)fresh) == 0);
    CHECK(files_remove(1, "/ram/shared") == 0);
    CHECK(files_remove(1, "/ram/shared") == FILE_NOT_FOUND);
    CHECK(files_open(1, "/ram/shared", FILE_READ) == FILE_NOT_FOUND);
    FileEntry entry = {FILE_LIMIT + 1, 0, {0}};
    CHECK(files_list(1, &entry) == FILE_BAD);
    entry.cursor = 0;
    CHECK(files_list(1, &entry) == 0 && entry.cursor == FILE_LIMIT && !entry.size && !entry.path[0]);
}

static void syscall_bounds(void) {
    FileOpen* open = (FileOpen*)user.bytes;
    memset(open, 0, sizeof(*open));
    memcpy(open->path, "/ram/io", sizeof("/ram/io")); open->flags = FILE_READ | FILE_WRITE | FILE_CREATE;
    uintptr_t p = (uintptr_t)user.bytes;
    CHECK(handle_file_call(SYS_FILE_OPEN, p, 0) == FILE_UNAVAILABLE);
    CHECK(handle_file_call(SYS_FILE_OPEN, 0, 1) == FILE_BAD);
    CHECK(handle_file_call(SYS_FILE_OPEN, p + 1, 1) == FILE_BAD);
    CHECK(handle_file_call(SYS_FILE_OPEN, p + sizeof(user.bytes) - 8, 1) == FILE_BAD);
    open->reserved = 1; CHECK(handle_file_call(SYS_FILE_OPEN, p, 1) == FILE_BAD);
    open->reserved = 0;
    int64_t h = handle_file_call(SYS_FILE_OPEN, p, 1); CHECK(h > 0);
    FileIo* io = (FileIo*)user.bytes; memset(io, 0, sizeof(*io));
    io->handle = (uint64_t)h; io->length = 3; memcpy(io->data, "abc", 3);
    CHECK(handle_file_call(SYS_FILE_WRITE, p, 1) == 3);
    CHECK(files_seek(1, (uint64_t)h, 0) == 0);
    CHECK(handle_file_call(SYS_FILE_READ, p + 1, 1) == FILE_BAD);
    io->length = FILE_IO_MAX + 1;
    CHECK(handle_file_call(SYS_FILE_READ, p, 1) == FILE_BAD);
    io->length = FILE_IO_MAX; io->reserved = 1;
    CHECK(handle_file_call(SYS_FILE_READ, p, 1) == FILE_BAD);
    io->reserved = 0; memset(io->data, 0xcc, sizeof(io->data));
    CHECK(handle_file_call(SYS_FILE_READ, p, 1) == 3 && io->length == 3);
    CHECK(!memcmp(io->data, "abc", 3));
    for (unsigned int i = 3; i < FILE_IO_MAX; i++) CHECK(!io->data[i]);
    CHECK(handle_file_call(SYS_FILE_CLOSE, (uintptr_t)h, 1) == 0);
    CHECK(handle_file_call(SYS_FILE_CLOSE, (uintptr_t)h, 1) == FILE_BAD_HANDLE);
    clean();
}

static void mixed_io(void) {
    unsigned char expected[FILE_CAPACITY] = {0}, buffer[80];
    uint32_t seed = 0x5ef192abU, size = 0, offset = 0;
    int64_t h = files_open(1, "/ram/model", FILE_READ | FILE_WRITE | FILE_CREATE);
    CHECK(h > 0);
    for (unsigned int i = 0; i < 5000; i++) {
        seed = seed * 1664525U + 1013904223U;
        uint32_t length = (seed >> 16) % (uint32_t)sizeof(buffer);
        if ((seed & 3U) == 0) {
            uint32_t next = (seed >> 8) % (FILE_CAPACITY + 16);
            int result = files_seek(1, (uint64_t)h, next);
            if (next <= size) { CHECK(result == 0); offset = next; }
            else CHECK(result == FILE_BAD);
        } else if ((seed & 3U) == 1) {
            memset(buffer, (int)(seed >> 24), sizeof(buffer));
            int result = files_write(1, (uint64_t)h, buffer, length);
            if (length <= FILE_CAPACITY - offset) {
                CHECK(result == (int)length);
                memcpy(expected + offset, buffer, length); offset += length;
                if (offset > size) size = offset;
            } else CHECK(result == FILE_NO_SPACE);
        } else {
            uint32_t count = length < size - offset ? length : size - offset;
            CHECK(files_read(1, (uint64_t)h, buffer, length) == (int)count);
            CHECK(!memcmp(buffer, expected + offset, count)); offset += count;
        }
        FileEntry entry = {0};
        CHECK(files_list(1, &entry) == 1 && entry.size == size);
    }
    clean();
}

int main(void) {
    Store store;
    fixture_reset();
    CHECK(files_open(1, "/ram/absent", FILE_READ) == FILE_UNAVAILABLE);
    FileDirectory unmounted = {0};
    CHECK(files_directory(1, &unmounted) == FILE_UNAVAILABLE);
    CHECK(store_format(&store, &device, &region, STORE_FORMAT_CONFIRM) == 0);
    CHECK(files_mount(&device, 0) == 0);
    paths_and_modes(); directories(); capacity_and_atomic_open(); ownership_and_reuse(); syscall_bounds(); mixed_io();
    int64_t h = files_open(1, "/ram/reboot", FILE_READ | FILE_WRITE | FILE_CREATE);
    CHECK(h > 0 && files_write(1, (uint64_t)h, "persistent", 10) == 10);
    CHECK(files_unmount() == FILE_BUSY);
    CHECK(files_close(1, (uint64_t)h) == 0 && files_unmount() == 0);
    CHECK(files_mount(&device, 0) == 0);
    int64_t fresh = files_open(1, "/ram/reboot", FILE_READ);
    char data[16];
    CHECK(fresh > 0 && fresh != h && files_read(1, (uint64_t)fresh, data, 16) == 10);
    CHECK(!memcmp(data, "persistent", 10));
    CHECK(files_close(1, (uint64_t)fresh) == 0);
    disk.fail_at = disk.calls + 1;
    CHECK(files_open(1, "/ram/reboot", FILE_WRITE | FILE_TRUNCATE) == FILE_IO_ERROR);
    CHECK(files_open(1, "/ram/reboot", FILE_READ) == FILE_UNAVAILABLE);
    disk.fail_at = 0;
    CHECK(files_unmount() == 0 && files_mount(&device, 0) == 0);
    fresh = files_open(1, "/ram/reboot", FILE_READ);
    CHECK(files_read(1, (uint64_t)fresh, data, 16) == 10 && !memcmp(data, "persistent", 10));
    CHECK(files_close(1, (uint64_t)fresh) == 0);
    fresh = files_open(1, "/ram/reboot", FILE_READ | FILE_WRITE);
    disk.fail_at = disk.calls + 10; disk.tear = 255;
    CHECK(files_write(1, (uint64_t)fresh, "uncertain", 9) == FILE_IO_ERROR);
    CHECK(files_read(1, (uint64_t)fresh, data, 16) == FILE_UNAVAILABLE);
    FileEntry entry = {0}; CHECK(files_list(1, &entry) == FILE_UNAVAILABLE);
    CHECK(files_close(1, (uint64_t)fresh) == 0 && files_unmount() == 0);
    disk.fail_at = 0;
    CHECK(files_mount(&device, 0) == STORE_MOUNT_RECOVERED);
    fresh = files_open(1, "/ram/reboot", FILE_READ | FILE_WRITE);
    CHECK(files_read(1, (uint64_t)fresh, data, 16) == 10 && !memcmp(data, "persistent", 10));
    CHECK(files_close(1, (uint64_t)fresh) == 0);
    fresh = files_open(1, "/ram/reboot", FILE_WRITE | FILE_TRUNCATE);
    CHECK(fresh > 0 && files_close(1, (uint64_t)fresh) == 0 && files_unmount() == 0);
    CHECK(files_mount(&device, 0) == 0);
    fresh = files_open(1, "/ram/reboot", FILE_READ);
    CHECK(files_read(1, (uint64_t)fresh, data, 16) == 0 && files_close(1, (uint64_t)fresh) == 0);
    CHECK(files_remove(1, "/ram/reboot") == 0 && files_unmount() == 0 && files_mount(&device, 0) == 0);
    CHECK(files_open(1, "/ram/reboot", FILE_READ) == FILE_NOT_FOUND);
    puts("PASS: persistent files, remount, bounds, ownership, atomic failures and syscall snapshots");
    return 0;
}
