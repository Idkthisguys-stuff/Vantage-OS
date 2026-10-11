#include "../../includes/mmu.h"

#define MAIR_VALUE 0x44FF00ULL
#define TCR_VALUE (32ULL | (1ULL << 8) | (1ULL << 10) | (3ULL << 12) | (2ULL << 16) | (1ULL << 23) | (1ULL << 31))
#define ATTR_DEVICE 0
#define ATTR_NORMAL 1
#define ATTR_NONCACHED 2
#define PERIPHERAL_START 0xFC000000ULL
#define BLOCK_SIZE 0x200000ULL

static uint64_t l1[4] __attribute__((aligned(4096)));
static uint64_t l2[4][512] __attribute__((aligned(4096)));

void mmu_init(uintptr_t fbStart, uint32_t fbSize) {
    uint64_t el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    if (((el >> 2) & 3) != 2) {
        return;
    }

    uint64_t fbEnd = (uint64_t)fbStart + fbSize;

    for (uint64_t g = 0; g < 4; g++) {
        for (uint64_t i = 0; i < 512; i++) {
            uint64_t addr = (g << 30) | (i << 21);
            uint64_t desc;

            if (addr >= PERIPHERAL_START) {
                desc = addr | (ATTR_DEVICE << 2) | (2ULL << 8) | (1ULL << 10) | (1ULL << 54) | 1;
            } else if (addr + BLOCK_SIZE > fbStart && addr < fbEnd) {
                desc = addr | (ATTR_NONCACHED << 2) | (2ULL << 8) | (1ULL << 10) | 1;
            } else {
                desc = addr | (ATTR_NORMAL << 2) | (3ULL << 8) | (1ULL << 10) | 1;
            }

            l2[g][i] = desc;
        }
        l1[g] = (uint64_t)(uintptr_t)l2[g] | 3;
    }

    uint64_t mair = MAIR_VALUE;
    uint64_t tcr = TCR_VALUE;
    uint64_t ttbr = (uint64_t)(uintptr_t)l1;

    __asm__ volatile("msr mair_el2, %0" :: "r"(mair) : "memory");
    __asm__ volatile("msr tcr_el2, %0" :: "r"(tcr) : "memory");
    __asm__ volatile("msr ttbr0_el2, %0" :: "r"(ttbr) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
    __asm__ volatile("tlbi alle2" ::: "memory");
    __asm__ volatile("ic iallu" ::: "memory");
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("isb" ::: "memory");

    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el2" : "=r"(sctlr));
    sctlr |= (1ULL << 0) | (1ULL << 2) | (1ULL << 12);
    __asm__ volatile("msr sctlr_el2, %0" :: "r"(sctlr) : "memory");
    __asm__ volatile("isb" ::: "memory");
}

// Man I'm dead T-T