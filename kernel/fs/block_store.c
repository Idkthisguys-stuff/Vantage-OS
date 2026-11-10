#include <vantage/kernel/block_store.h>

static const unsigned char magic[8] = {'V', 'S', 'T', 'O', 'R', 'E', '3', 0};
static uint32_t get32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t get64(const unsigned char* p) { return get32(p) | ((uint64_t)get32(p + 4) << 32); }
static void put32(unsigned char* p, uint32_t v) {
    for (unsigned int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i));
}
static void put64(unsigned char* p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static uint32_t sectors(uint32_t size) { return (size + 511U) / 512U; }
static uint64_t base(const BlockStore* s, unsigned int bank) {
    return s->snapshot.region.first + bank * (s->snapshot.region.sectors / 2);
}
static int region_valid(const SectorDevice* d, const StoreRegion* r) {
    return d && d->read && d->write && d->sync && r && r->first &&
           r->sectors >= BLOCK_MIN_SECTORS && r->first < d->sectors && r->sectors <= d->sectors - r->first;
}
static int fault(BlockStore* s) { s->snapshot.state = -1; return FILE_IO_ERROR; }
static int zero(const unsigned char* p, unsigned int n) {
    while (n--) if (*p++) return 0;
    return 1;
}

static int entries_valid(const BlockEntry* entries) {
    for (unsigned int i = 0; i < FILE_LIMIT; i++) {
        const BlockEntry* f = &entries[i];
        if (f->used > FILE_DIRECTORY || f->size > FILE_CAPACITY) return 0;
        if (!f->used) {
            if (!zero((const unsigned char*)f->name, STORE_NAME_BYTES) || f->size || f->parent || f->sector) return 0;
            continue;
        }
        if (!store_name_valid(f->name) || (f->used == FILE_DIRECTORY && f->size)) return 0;
        unsigned int n = 0;
        while (n < STORE_NAME_BYTES && f->name[n]) n++;
        if (!zero((const unsigned char*)f->name + n, STORE_NAME_BYTES - n)) return 0;
        for (unsigned int j = 0; j < i; j++)
            if (entries[j].used && entries[j].parent == f->parent &&
                !memcmp(entries[j].name, f->name, STORE_NAME_BYTES)) return 0;
        unsigned int node = i + 1, length = 4, depth = 0;
        while (node) {
            if (node > FILE_LIMIT || ++depth > FILE_LIMIT) return 0;
            const BlockEntry* ancestor = &entries[node - 1];
            if (node != i + 1 && ancestor->used != FILE_DIRECTORY) return 0;
            n = 0;
            while (n < STORE_NAME_BYTES && ancestor->name[n]) n++;
            length += n + 1;
            if (length >= FILE_PATH_MAX) return 0;
            node = ancestor->parent;
        }
    }
    return 1;
}

static void metadata(const BlockEntry* entries, unsigned int index, uint32_t* next, unsigned char* out) {
    memset(out, 0, 512);
    for (unsigned int j = 0; j < 8; j++) {
        const BlockEntry* f = &entries[index * 8 + j];
        unsigned char* p = out + j * 64;
        if (!f->used) continue;
        memcpy(p, f->name, STORE_NAME_BYTES);
        put32(p + 32, f->used); put32(p + 36, f->size); put32(p + 40, f->parent);
        if (f->size) { put32(p + 44, *next); *next += sectors(f->size); }
    }
}

static int load(BlockStore* s, unsigned int bank, BlockEntry* entries, uint64_t* generation, uint32_t* payload) {
    SectorDevice* d = &s->snapshot.device;
    unsigned char buf[512];
    uint64_t first = base(s, bank);
    if (d->read(d->context, first, buf)) return FILE_IO_ERROR;
    uint32_t count = get32(buf + 28), expected = get32(buf + 24);
    if (memcmp(buf, magic, 8) || get32(buf + 8) != 3 || get32(buf + 12) != bank ||
        !get64(buf + 16) || count < BLOCK_META_SECTORS ||
        count > BLOCK_META_SECTORS + FILE_LIMIT * (FILE_CAPACITY / 512U) ||
        (uint64_t)count >= s->snapshot.region.sectors / 2 ||
        get64(buf + 32) != s->snapshot.region.first || get64(buf + 40) != s->snapshot.region.sectors ||
        !zero(buf + 48, 460) || get32(buf + 508) != store_crc32(0, buf, 508)) return FILE_CORRUPT;
    *generation = get64(buf + 16); *payload = count;
    memset(entries, 0, sizeof(*entries) * FILE_LIMIT);
    uint32_t crc = 0, next = BLOCK_META_SECTORS;
    for (unsigned int offset = 0; offset < count; offset++) {
        if (d->read(d->context, first + 1 + offset, buf)) return FILE_IO_ERROR;
        crc = store_crc32(crc, buf, sizeof(buf));
        if (offset < BLOCK_META_SECTORS) {
            for (unsigned int j = 0; j < 8; j++) {
                const unsigned char* p = buf + j * 64;
                BlockEntry* f = &entries[offset * 8 + j];
                memcpy(f->name, p, STORE_NAME_BYTES);
                f->used = get32(p + 32); f->size = get32(p + 36);
                f->parent = get32(p + 40); f->sector = get32(p + 44);
                if (!zero(p + 48, 16) || f->size > FILE_CAPACITY) return FILE_CORRUPT;
                if (f->size) {
                    if (f->sector != next || sectors(f->size) > count - next) return FILE_CORRUPT;
                    next += sectors(f->size);
                } else if (f->sector) return FILE_CORRUPT;
            }
        } else {
            /* Canonical zero padding after EOF in every final data sector. */
            for (unsigned int i = 0; i < FILE_LIMIT; i++) {
                const BlockEntry* f = &entries[i];
                if (f->size && (f->size & 511U) && offset == f->sector + sectors(f->size) - 1 &&
                    !zero(buf + (f->size & 511U), 512U - (f->size & 511U))) return FILE_CORRUPT;
            }
        }
    }
    return next == count && crc == expected && entries_valid(entries) ? 0 : FILE_CORRUPT;
}

int block_mount(BlockStore* s, const SectorDevice* d, const StoreRegion* r, BlockEntry entries[FILE_LIMIT]) {
    if (!s || !entries || !region_valid(d, r)) return FILE_BAD;
    *s = (BlockStore){0}; s->snapshot.device = *d; s->snapshot.region = *r;
    uint64_t gen[2] = {0, 0}; uint32_t count[2] = {0, 0}; int status[2];
    for (unsigned int i = 0; i < 2; i++) {
        status[i] = load(s, i, entries, &gen[i], &count[i]);
        if (status[i] == FILE_IO_ERROR) { memset(entries, 0, sizeof(*entries) * FILE_LIMIT); return fault(s); }
    }
    if ((status[0] && status[1]) || (!status[0] && !status[1] && gen[0] == gen[1])) {
        memset(entries, 0, sizeof(*entries) * FILE_LIMIT); return FILE_CORRUPT;
    }
    unsigned int active = status[0] ? 1U : status[1] || gen[0] > gen[1] ? 0U : 1U;
    int result = load(s, active, entries, &s->snapshot.generation, &s->payload);
    if (result) { memset(entries, 0, sizeof(*entries) * FILE_LIMIT); s->snapshot.state = -1; return result; }
    s->snapshot.active = active; s->snapshot.state = 1;
    return status[0] || status[1] ? STORE_MOUNT_RECOVERED : 0;
}

int block_read(BlockStore* s, const BlockEntry* f, uint32_t offset, void* data, uint32_t length) {
    if (!s || s->snapshot.state != 1) return FILE_UNAVAILABLE;
    if (!f || f->used != FILE_REGULAR || offset > f->size || length > f->size - offset || (length && !data)) return FILE_BAD;
    if (f->size && (f->size > FILE_CAPACITY || f->sector < BLOCK_META_SECTORS ||
        f->sector > s->payload || sectors(f->size) > s->payload - f->sector)) return FILE_BAD;
    unsigned char buf[512]; unsigned char* out = data;
    while (length) {
        uint32_t skip = offset & 511U, n = 512U - skip;
        if (n > length) n = length;
        if (s->snapshot.device.read(s->snapshot.device.context,
            base(s, s->snapshot.active) + 1 + f->sector + offset / 512U, buf)) return fault(s);
        memcpy(out, buf + skip, n); offset += n; out += n; length -= n;
    }
    return 0;
}

int block_commit(BlockStore* s, BlockEntry entries[FILE_LIMIT], const BlockPatch* patch) {
    if (!s || s->snapshot.state != 1) return FILE_UNAVAILABLE;
    if (!entries || !entries_valid(entries)) return FILE_BAD;
    if (patch && (patch->file >= FILE_LIMIT || !patch->data || !patch->length ||
        entries[patch->file].used != FILE_REGULAR || patch->offset > entries[patch->file].size ||
        patch->length > entries[patch->file].size - patch->offset)) return FILE_BAD;
    if (s->snapshot.generation == ~(uint64_t)0) return FILE_NO_SPACE;
    uint32_t count = BLOCK_META_SECTORS;
    for (unsigned int i = 0; i < FILE_LIMIT; i++) {
        const BlockEntry* f = &entries[i];
        count += sectors(f->size);
        uint32_t copied = f->size;
        if (patch && patch->file == i && patch->offset + patch->length == f->size) copied = patch->offset;
        if (copied && (f->sector < BLOCK_META_SECTORS || f->sector > s->payload ||
            sectors(copied) > s->payload - f->sector)) return FILE_BAD;
    }
    if ((uint64_t)count >= s->snapshot.region.sectors / 2) return FILE_NO_SPACE;
    SectorDevice* d = &s->snapshot.device;
    unsigned int bank = s->snapshot.active ^ 1U;
    uint64_t first = base(s, bank), source = base(s, s->snapshot.active);
    unsigned char buf[512] = {0};
    if (d->write(d->context, first, buf) || d->sync(d->context)) return fault(s);
    uint32_t crc = 0, next = BLOCK_META_SECTORS;
    for (unsigned int offset = 0; offset < BLOCK_META_SECTORS; offset++) {
        metadata(entries, offset, &next, buf);
        crc = store_crc32(crc, buf, sizeof(buf));
        if (d->write(d->context, first + 1 + offset, buf)) return fault(s);
    }
    next = BLOCK_META_SECTORS;
    for (unsigned int i = 0; i < FILE_LIMIT; i++) {
        const BlockEntry* f = &entries[i];
        for (uint32_t part = 0; part < sectors(f->size); part++) {
            uint32_t start = part * 512U;
            memset(buf, 0, sizeof(buf));
            /* New bytes are entirely supplied by patch; never read stale sectors
             * after growth across a sector boundary. */
            uint32_t old_size = f->size;
            if (patch && patch->file == i && patch->offset + patch->length == f->size) old_size = patch->offset;
            if (start < old_size && d->read(d->context, source + 1 + f->sector + part, buf)) return fault(s);
            if (patch && patch->file == i) {
                uint32_t lo = patch->offset > start ? patch->offset : start;
                uint32_t end = patch->offset + patch->length;
                uint32_t hi = end < start + 512U ? end : start + 512U;
                if (hi > lo) memcpy(buf + lo - start, (const unsigned char*)patch->data + lo - patch->offset, hi - lo);
            }
            if (f->size - start < 512U) memset(buf + f->size - start, 0, 512U - (f->size - start));
            crc = store_crc32(crc, buf, sizeof(buf));
            if (d->write(d->context, first + 1 + next++, buf)) return fault(s);
        }
    }
    if (d->sync(d->context)) return fault(s);
    uint32_t readback = 0;
    for (unsigned int offset = 0; offset < count; offset++) {
        if (d->read(d->context, first + 1 + offset, buf)) return fault(s);
        readback = store_crc32(readback, buf, sizeof(buf));
    }
    if (readback != crc) return fault(s);
    memset(buf, 0, sizeof(buf)); memcpy(buf, magic, 8);
    put32(buf + 8, 3); put32(buf + 12, bank); put64(buf + 16, s->snapshot.generation + 1);
    put32(buf + 24, crc); put32(buf + 28, count);
    put64(buf + 32, s->snapshot.region.first); put64(buf + 40, s->snapshot.region.sectors);
    put32(buf + 508, store_crc32(0, buf, 508));
    uint32_t header_crc = store_crc32(0, buf, sizeof(buf));
    if (d->write(d->context, first, buf) || d->sync(d->context) || d->read(d->context, first, buf) ||
        store_crc32(0, buf, sizeof(buf)) != header_crc) return fault(s);
    next = BLOCK_META_SECTORS;
    for (unsigned int i = 0; i < FILE_LIMIT; i++) {
        entries[i].sector = entries[i].size ? next : 0;
        next += sectors(entries[i].size);
    }
    s->snapshot.active = bank; s->snapshot.generation++; s->payload = count;
    return 0;
}

int block_format(BlockStore* s, const SectorDevice* d, const StoreRegion* r, uint64_t confirmation) {
    static BlockEntry empty[FILE_LIMIT];
    if (!s || confirmation != STORE_FORMAT_CONFIRM || !region_valid(d, r)) return FILE_BAD;
    *s = (BlockStore){0}; s->snapshot.device = *d; s->snapshot.region = *r;
    memset(empty, 0, sizeof(empty));
    unsigned char buf[512] = {0};
    for (unsigned int bank = 0; bank < 2; bank++)
        if (d->write(d->context, base(s, bank), buf)) return fault(s);
    if (d->sync(d->context)) return fault(s);
    s->snapshot.state = 1; s->snapshot.active = 1;
    int result = block_commit(s, empty, NULL);
    return result ? result : block_commit(s, empty, NULL);
}
