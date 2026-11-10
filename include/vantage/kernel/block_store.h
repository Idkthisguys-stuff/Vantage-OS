#pragma once
#include <vantage/kernel/storage.h>

/* V3: two independently checksummed snapshots, packed sector allocations.
 * File data stays on the device; RAM holds only the bounded directory table.
 * Mutations stream live sectors into the inactive bank before publication.
 * All calls require exclusive, non-yielding core-0 ownership. */
#define BLOCK_META_SECTORS (FILE_LIMIT / 8U)
#define BLOCK_MIN_SECTORS (2U * (1U + BLOCK_META_SECTORS + FILE_CAPACITY / 512U))
typedef struct {
    char name[STORE_NAME_BYTES];
    uint32_t used, size, parent, sector;
} BlockEntry;
typedef struct {
    Store snapshot;
    uint32_t payload;
} BlockStore;
typedef struct {
    unsigned int file;
    uint32_t offset, length;
    const void* data;
} BlockPatch;

int block_mount(BlockStore* store, const SectorDevice* device, const StoreRegion* region,
                BlockEntry entries[FILE_LIMIT]);
int block_commit(BlockStore* store, BlockEntry entries[FILE_LIMIT], const BlockPatch* patch);
int block_read(BlockStore* store, const BlockEntry* entry, uint32_t offset, void* data, uint32_t length);
int block_format(BlockStore* store, const SectorDevice* device, const StoreRegion* region,
                 uint64_t confirmation);
