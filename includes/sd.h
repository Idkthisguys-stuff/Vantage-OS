#pragma once
#include "std.h"

#define SD_BLOCK_SIZE 512U
#define SD_READ_MAX_BLOCKS 128U

enum {
    SD_OK = 0,
    SD_TIMEOUT = -1,
    SD_ERROR = -2,
    SD_INVALID = -3,
    SD_UNSUPPORTED = -4,
    SD_NOT_READY = -5
};

/* Offsets are relative to the host controller. Callbacks must return promptly.
 * now_us is a monotonic wrapping 64-bit microsecond counter; delay_us waits at
 * least the requested interval. The caller exclusively owns the controller. */
typedef struct {
    void* context;
    uint32_t (*read32)(void* context, uint32_t offset);
    void (*write32)(void* context, uint32_t offset, uint32_t value);
    uint64_t (*now_us)(void* context);
    void (*delay_us)(void* context, uint32_t microseconds);
} SdIo;

typedef struct {
    SdIo io;
    uint64_t sectors;
    uint32_t base_clock_hz, clock_hz;
    uint32_t rca, block_addressed, ready;
    uint32_t last_interrupt, last_response;
} SdCard;

/* SD v2 SDSC/SDHC/SDXC, 1-bit bus, default speed, programmed-I/O reads.
 * Use a zero-initialized or previously initialized card and a separate SdIo.
 * Firmware/board code must establish power, 3.3V signaling and clock routing.
 * Any hardware/protocol failure invalidates the card until reinitialized. */
int sd_card_init(SdCard* card, const SdIo* io, uint32_t base_clock_hz);
/* Returns SD_OK only after all sectors complete. On error the destination can
 * contain partial data and must be discarded. Invalid arguments cause no I/O. */
int sd_card_read(SdCard* card, uint64_t lba, uint32_t count,
                 void* buffer, size_t buffer_bytes);

/* Pi 4 EMMC2 adapter: core 0, EL1, masked exceptions, exclusive ownership.
 * The input clock must be obtained from firmware/board configuration and remain
 * fixed. Normal boot deliberately does not call this unvalidated hardware path. */
int sd_init(uint32_t base_clock_hz);
int sd_read_blocks(uint64_t lba, uint32_t count, void* buffer, size_t buffer_bytes);
/* Legacy byte-count API: caller supplies room for count * 512 bytes. */
int sd_readBlock(unsigned int lba, unsigned char* buffer, unsigned int count);
