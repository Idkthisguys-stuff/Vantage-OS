#pragma once

#include "std.h"
#include "io.h"

#define MMIO_BASE 0xFE000000
#define EMMC_BASE (MMIO_BASE + 0x300000)

#define EMMC_ARG2 (volatile unsigned int*)(EMMC_BASE + 0x00)
#define EMMC_BLKSIZECNT (volatile unsigned int*)(EMMC_BASE + 0x04)
#define EMMC_ARG1 (volatile unsigned int*)(EMMC_BASE + 0x08)
#define EMMC_CMDTM (volatile unsigned int*)(EMMC_BASE + 0x0C)
#define EMMC_RESP0 (volatile unsigned int*)(EMMC_BASE + 0x10)
#define EMMC_STATUS (volatile unsigned int*)(EMMC_BASE + 0x24)
#define EMMC_INTERRUPT (volatile unsigned int*)(EMMC_BASE + 0x30)
#define EMMC_INTERRUPT_EN (EMMC_BASE + 0x38)

int sd_init(void);
int sd_read(uint8_t* buf, uint32_t lba, uint32_t num_block);
int sd_write(const uint8_t* buf, uint32_t lba, uint32_t num_blocks);