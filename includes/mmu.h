#pragma once

#include "std.h"

void mmu_init(uintptr_t fbStart, uint32_t fbSize);

void mmu_populate_tables(uint64_t level1[4], uint64_t level2[4][512],
                         uintptr_t fbStart, uint32_t fbSize,
                         uintptr_t userStart, uintptr_t userEnd);

static inline uint32_t user_region_contains(uintptr_t start, uintptr_t end,
                                           uintptr_t buffer, size_t size) {
    return start < end && buffer >= start && buffer < end &&
           size <= end - buffer;
}
