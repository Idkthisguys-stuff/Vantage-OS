#include <vantage/kernel/files.h>
#include <vantage/kernel/block_store.h>
#ifndef VANTAGE_SIM
#include <vantage/kernel/task.h>
#endif

static int services_owned(void) {
#ifdef VANTAGE_SIM
    return 1;
#else
    return task_services_allowed();
#endif
}

#define NAME_MAX 32U
#define HANDLE_BITS 5U
#define HANDLE_MASK 31U

typedef struct {
    uint64_t owner, id, generation;
    uint32_t file, offset, flags;
} OpenFile;

static BlockEntry stored_files[FILE_LIMIT];
static StoredFile legacy_files[STORE_LEGACY_LIMIT];
static BlockStore block_volume;
static int block_mode;
static Store volume;
static OpenFile open_files[FILE_HANDLE_LIMIT];

static int have_handles(void) {
    for (unsigned int i = 0; i < FILE_HANDLE_LIMIT; i++) if (open_files[i].owner) return 1;
    return 0;
}

int files_unmount(void) {
    if (!services_owned()) return FILE_UNAVAILABLE;
    if (have_handles()) return FILE_BUSY;
    volume.state = 0;
    memset(stored_files, 0, sizeof(stored_files));
    return 0;
}

int files_mount(const SectorDevice* device, unsigned int partition) {
    if (!services_owned()) return FILE_UNAVAILABLE;
    if (volume.state == 1 || have_handles()) return FILE_BUSY;
    StoreRegion region;
    int result = store_partition(device, partition, &region);
    if (result) return result;
    block_mode = 0;
    if (region.sectors >= BLOCK_MIN_SECTORS) {
        result = block_mount(&block_volume, device, &region, stored_files);
        if (result >= 0) {
            int legacy = store_mount(&volume, device, &region, legacy_files);
            if (legacy >= 0) {
                volume.state = 0; memset(stored_files, 0, sizeof(stored_files));
                return FILE_CORRUPT; /* Never choose between mixed valid formats. */
            }
            if (legacy == FILE_IO_ERROR) return legacy;
            block_mode = 1; volume = block_volume.snapshot; return result;
        }
        if (result == FILE_IO_ERROR) return result;
    }
    result = store_mount(&volume, device, &region, legacy_files);
    memset(stored_files, 0, sizeof(stored_files));
    if (result >= 0) for (unsigned int i = 0; i < STORE_LEGACY_LIMIT; i++) {
        memcpy(stored_files[i].name, legacy_files[i].name, NAME_MAX);
        stored_files[i].used = legacy_files[i].used;
        stored_files[i].size = legacy_files[i].size;
        stored_files[i].parent = legacy_files[i].parent;
    }
    return result;
}

static int commit(const BlockPatch* patch) {
    if (volume.generation == ~(uint64_t)0) return FILE_NO_SPACE;
    int result;
    if (block_mode) {
        result = block_commit(&block_volume, stored_files, patch);
        volume = block_volume.snapshot;
    } else {
        for (unsigned int i = 0; i < STORE_LEGACY_LIMIT; i++) {
            memcpy(legacy_files[i].name, stored_files[i].name, NAME_MAX);
            legacy_files[i].used = stored_files[i].used;
            legacy_files[i].size = stored_files[i].size;
            legacy_files[i].parent = stored_files[i].parent;
            memset(legacy_files[i].data + legacy_files[i].size, 0, STORE_LEGACY_CAPACITY - legacy_files[i].size);
        }
        if (patch) memcpy(legacy_files[patch->file].data + patch->offset, patch->data, patch->length);
        result = store_commit(&volume, legacy_files);
    }
    /* Even a rejected commit must not expose staged changes as durable files. */
    if (result && result != FILE_NO_SPACE && result != FILE_BAD) volume.state = -1;
    return result;
}

static int same_name(const char* a, const char* b) {
    for (unsigned int i = 0; i < NAME_MAX; i++) {
        if (a[i] != b[i]) return 0;
        if (!a[i]) return 1;
    }
    return 0;
}

static unsigned int child(unsigned int parent, const char* name) {
    for (unsigned int i = 0; i < FILE_LIMIT; i++)
        if (stored_files[i].used && stored_files[i].parent == parent &&
            same_name(stored_files[i].name, name)) return i + 1;
    return 0;
}

/* Walk existing ancestors before interpreting ..: missing/file parents cannot
 * be bypassed by lexical cancellation. Kernel relative paths start at /ram. */
static int resolve(const char* path, unsigned int* node, unsigned int* parent,
                   char name[NAME_MAX]) {
    if (!path) return FILE_BAD;
    unsigned int length = 0;
    while (length < FILE_PATH_MAX && path[length]) length++;
    if (!length || length == FILE_PATH_MAX) return FILE_BAD;
    unsigned int pos = 0, current = 0;
    if (path[0] == '/') {
        if (length == 1) { *node = *parent = 0; name[0] = 0; return 0; }
        if (length < 4 || memcmp(path, "/ram", 4) || (path[4] && path[4] != '/')) return FILE_BAD;
        pos = 4;
    }
    name[0] = 0; *parent = 0;
    while (pos < length) {
        while (pos < length && path[pos] == '/') pos++;
        if (pos == length) break;
        if (current && stored_files[current - 1].used != FILE_DIRECTORY) return FILE_BAD;
        unsigned int n = 0;
        memset(name, 0, NAME_MAX);
        while (pos < length && path[pos] != '/') {
            if (n == NAME_MAX - 1) return FILE_BAD;
            name[n++] = path[pos++];
        }
        *parent = current;
        if (n == 1 && name[0] == '.') continue;
        if (n == 2 && name[0] == '.' && name[1] == '.') {
            current = current ? stored_files[current - 1].parent : 0;
            continue;
        }
        if (!store_name_valid(name)) return FILE_BAD;
        current = child(current, name);
        if (!current) {
            if (pos != length) return FILE_NOT_FOUND;
            *node = FILE_LIMIT + 1;
            return 0;
        }
    }
    if (length && path[length - 1] == '/' && current &&
        stored_files[current - 1].used != FILE_DIRECTORY) return FILE_BAD;
    *node = current;
    return 0;
}

static void node_path(unsigned int node, char path[FILE_PATH_MAX]) {
    /* Mount and creation guarantee an acyclic, addressable tree. */
    unsigned int chain[FILE_LIMIT], count = 0, length = 4;
    while (node) { chain[count++] = node; node = stored_files[node - 1].parent; }
    memset(path, 0, FILE_PATH_MAX);
    memcpy(path, "/ram", 4);
    while (count) {
        const char* name = stored_files[chain[--count] - 1].name;
        path[length++] = '/';
        while (*name) path[length++] = *name++;
    }
}

static int create_node(unsigned int parent, const char* name, unsigned int type) {
    char path[FILE_PATH_MAX];
    node_path(parent, path);
    unsigned int length = 0, n = 0;
    while (path[length]) length++;
    while (name[n]) n++;
    if (length + 1 + n >= (block_mode ? FILE_PATH_MAX : FILE_LEGACY_PATH_MAX)) return FILE_BAD;
    for (unsigned int i = 0; i < (block_mode ? FILE_LIMIT : STORE_LEGACY_LIMIT); i++) {
        if (stored_files[i].used) continue;
        stored_files[i] = (BlockEntry){0};
        memcpy(stored_files[i].name, name, NAME_MAX);
        stored_files[i].used = type;
        stored_files[i].parent = parent;
        return (int)i;
    }
    return FILE_NO_SPACE;
}

static OpenFile* lookup(uint64_t owner, uint64_t handle) {
    unsigned int slot = (unsigned int)(handle & HANDLE_MASK);
    if (!owner || !slot || slot > FILE_HANDLE_LIMIT) return NULL;
    OpenFile* open = &open_files[slot - 1];
    return open->owner == owner && open->id == handle ? open : NULL;
}

int64_t files_open(uint64_t owner, const char* path, uint32_t flags) {
    char name[NAME_MAX] = {0};
    if (!services_owned() || !owner || volume.state != 1) return FILE_UNAVAILABLE;
    if (!(flags & (FILE_READ | FILE_WRITE)) ||
        (flags & ~(FILE_READ | FILE_WRITE | FILE_CREATE | FILE_TRUNCATE)) ||
        ((flags & (FILE_CREATE | FILE_TRUNCATE)) && !(flags & FILE_WRITE))) return FILE_BAD;
    unsigned int node, parent;
    int resolved = resolve(path, &node, &parent, name);
    if (resolved) return resolved;
    if (!node || (node <= FILE_LIMIT && stored_files[node - 1].used == FILE_DIRECTORY)) return FILE_BAD;
    OpenFile* open = NULL;
    for (unsigned int i = 0; i < FILE_HANDLE_LIMIT; i++) {
        if (!open_files[i].owner && open_files[i].generation < (0x7fffffffffffffffULL >> HANDLE_BITS)) {
            open = &open_files[i];
            break;
        }
    }
    /* Reserve capacity before creating/truncating anything. */
    if (!open) return FILE_NO_SPACE;
    unsigned int existing = node - 1;
    int changed = 0;
    if (existing == FILE_LIMIT) {
        if (!(flags & FILE_CREATE)) return FILE_NOT_FOUND;
        int slot = create_node(parent, name, FILE_REGULAR);
        if (slot < 0) return slot;
        existing = (unsigned int)slot;
        changed = 1;
    }
    BlockEntry* file = &stored_files[existing];
    uint32_t old_size = file->size;
    if (flags & FILE_TRUNCATE) {
        file->size = 0;
        changed = 1;
    }
    if (changed) {
        int result = commit(NULL);
        if (result) {
            if (node > FILE_LIMIT) *file = (BlockEntry){0}; else file->size = old_size;
            return result;
        }
    }
    open->generation++;
    open->id = (open->generation << HANDLE_BITS) | ((unsigned int)(open - open_files) + 1U);
    open->owner = owner;
    open->file = existing;
    open->offset = 0;
    open->flags = flags;
    return (int64_t)open->id;
}

int files_read(uint64_t owner, uint64_t handle, void* data, uint32_t length) {
    if (!services_owned() || volume.state != 1) return FILE_UNAVAILABLE;
    OpenFile* open = lookup(owner, handle);
    if (!open) return FILE_BAD_HANDLE;
    if (!(open->flags & FILE_READ)) return FILE_ACCESS;
    if (length && !data) return FILE_BAD;
    BlockEntry* file = &stored_files[open->file];
    uint32_t available = open->offset < file->size ? file->size - open->offset : 0;
    if (length > available) length = available;
    if (length) {
        if (block_mode) {
            int result = block_read(&block_volume, file, open->offset, data, length);
            volume = block_volume.snapshot;
            if (result) return result;
        } else memcpy(data, legacy_files[open->file].data + open->offset, length);
    }
    open->offset += length;
    return (int)length;
}

int files_write(uint64_t owner, uint64_t handle, const void* data, uint32_t length) {
    if (!services_owned() || volume.state != 1) return FILE_UNAVAILABLE;
    OpenFile* open = lookup(owner, handle);
    if (!open) return FILE_BAD_HANDLE;
    if (!(open->flags & FILE_WRITE)) return FILE_ACCESS;
    if (length && !data) return FILE_BAD;
    BlockEntry* file = &stored_files[open->file];
    if (open->offset > file->size) return FILE_BAD;
    if (length > (block_mode ? FILE_CAPACITY : STORE_LEGACY_CAPACITY) - open->offset) return FILE_NO_SPACE;
    if (!length) return 0;
    uint32_t old_size = file->size;
    uint32_t end = open->offset + length;
    if (end > file->size) file->size = end;
    BlockPatch patch = {open->file, open->offset, length, data};
    int result = commit(&patch);
    if (result) { file->size = old_size; return result; }
    open->offset = end;
    return (int)length;
}

int files_seek(uint64_t owner, uint64_t handle, uint32_t offset) {
    if (!services_owned() || volume.state != 1) return FILE_UNAVAILABLE;
    OpenFile* open = lookup(owner, handle);
    if (!open) return FILE_BAD_HANDLE;
    if (offset > stored_files[open->file].size) return FILE_BAD;
    open->offset = offset;
    return 0;
}

int files_close(uint64_t owner, uint64_t handle) {
    if (!services_owned()) return FILE_UNAVAILABLE;
    OpenFile* open = lookup(owner, handle);
    if (!open) return FILE_BAD_HANDLE;
    open->owner = 0;
    return 0;
}

void files_release(uint64_t owner) {
    if (!services_owned() || !owner) return;
    for (unsigned int i = 0; i < FILE_HANDLE_LIMIT; i++)
        if (open_files[i].owner == owner) open_files[i].owner = 0;
}

int files_list(uint64_t owner, FileEntry* entry) {
    if (!services_owned() || !owner || volume.state != 1) return FILE_UNAVAILABLE;
    unsigned int limit = block_mode ? FILE_LIMIT : STORE_LEGACY_LIMIT;
    if (!entry || entry->cursor > limit) return FILE_BAD;
    unsigned int start = entry->cursor;
    *entry = (FileEntry){0};
    for (unsigned int i = start; i < limit; i++) {
        if (stored_files[i].used != FILE_REGULAR || stored_files[i].parent) continue;
        entry->cursor = i + 1;
        entry->size = stored_files[i].size;
        char path[FILE_PATH_MAX];
        node_path(i + 1, path);
        memcpy(entry->path, path, FILE_LEGACY_PATH_MAX);
        return 1;
    }
    entry->cursor = limit;
    return 0;
}

int files_remove(uint64_t owner, const char* path) {
    char name[NAME_MAX] = {0};
    if (!services_owned() || !owner || volume.state != 1) return FILE_UNAVAILABLE;
    unsigned int node, parent;
    int result = resolve(path, &node, &parent, name);
    if (result) return result;
    if (node > FILE_LIMIT) return FILE_NOT_FOUND;
    if (!node || stored_files[node - 1].used != FILE_REGULAR) return FILE_BAD;
    unsigned int i = node - 1;
    for (unsigned int j = 0; j < FILE_HANDLE_LIMIT; j++)
        if (open_files[j].owner && open_files[j].file == i) return FILE_BUSY;
    BlockEntry old = stored_files[i];
    stored_files[i] = (BlockEntry){0};
    result = commit(NULL);
    if (result) stored_files[i] = old;
    return result;
}

int files_directory_long(uint64_t owner, FileDirectoryLong* request) {
    if (!services_owned() || !owner || volume.state != 1) return FILE_UNAVAILABLE;
    if (!request || request->operation > DIRECTORY_REMOVE || request->cursor > FILE_LIMIT) return FILE_BAD;
    char name[NAME_MAX] = {0};
    unsigned int node, parent;
    int result = resolve(request->path, &node, &parent, name);
    if (result) return result;
    if (request->operation == DIRECTORY_CREATE) {
        if (node <= FILE_LIMIT) return FILE_BAD;
        result = create_node(parent, name, FILE_DIRECTORY);
        if (result < 0) return result;
        int committed = commit(NULL);
        if (committed) stored_files[result] = (BlockEntry){0};
        return committed;
    }
    if (node > FILE_LIMIT) return FILE_NOT_FOUND;
    unsigned int type = node ? stored_files[node - 1].used : FILE_DIRECTORY;
    if (request->operation == DIRECTORY_REMOVE) {
        if (!node || type != FILE_DIRECTORY) return FILE_BAD;
        for (unsigned int i = 0; i < FILE_LIMIT; i++)
            if (stored_files[i].used && stored_files[i].parent == node) return FILE_BUSY;
        BlockEntry old = stored_files[node - 1];
        stored_files[node - 1] = (BlockEntry){0};
        result = commit(NULL);
        if (result) stored_files[node - 1] = old;
        return result;
    }
    if (request->operation == DIRECTORY_LIST) {
        if (type != FILE_DIRECTORY) return FILE_BAD;
        for (unsigned int i = request->cursor; i < FILE_LIMIT; i++) {
            if (!stored_files[i].used || stored_files[i].parent != node) continue;
            request->cursor = i + 1;
            request->type = stored_files[i].used;
            request->size = stored_files[i].size;
            node_path(i + 1, request->entry);
            return 1;
        }
        request->cursor = FILE_LIMIT;
        request->type = request->size = 0;
        memset(request->entry, 0, sizeof(request->entry));
        return 0;
    }
    request->type = type;
    request->size = node ? stored_files[node - 1].size : 0;
    node_path(node, request->entry);
    return 0;
}

int files_directory(uint64_t owner, FileDirectory* request) {
    if (!services_owned() || !owner || volume.state != 1) return FILE_UNAVAILABLE;
    if (!request) return FILE_BAD;
    FileDirectoryLong wide = {0};
    wide.operation = request->operation; wide.cursor = request->cursor;
    unsigned int n = 0;
    while (n < FILE_LEGACY_PATH_MAX && request->path[n]) n++;
    if (n == FILE_LEGACY_PATH_MAX) return FILE_BAD;
    memcpy(wide.path, request->path, n + 1);
    /* Old callers cannot create paths they could not subsequently address. */
    if (wide.operation == DIRECTORY_CREATE) {
        char name[NAME_MAX] = {0}; unsigned int node, parent;
        if (services_owned() && owner && volume.state == 1 && !resolve(wide.path, &node, &parent, name)) {
            char path[FILE_PATH_MAX]; node_path(parent, path);
            unsigned int size = 0, component = 0;
            while (path[size]) size++;
            while (name[component]) component++;
            if (size + component + 1 >= FILE_LEGACY_PATH_MAX) return FILE_BAD;
        }
    }
    int result = files_directory_long(owner, &wide);
    if (result < 0) return result;
    n = 0;
    while (n < FILE_PATH_MAX && wide.entry[n]) n++;
    if (n >= FILE_LEGACY_PATH_MAX) return FILE_BAD;
    request->cursor = wide.cursor; request->type = wide.type; request->size = wide.size;
    memset(request->entry, 0, sizeof(request->entry)); memcpy(request->entry, wide.entry, n);
    return result;
}
