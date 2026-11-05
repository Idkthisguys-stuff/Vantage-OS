#pragma once

#include "../../includes/sd.h"

/* Compile the real driver against register storage for command-response tests.
 * This mock does not emulate write-one-to-clear interrupt registers or timings.
 * Each test starts a fresh transaction with interrupts cleared.
 */
#define VANTAGE_SD_TEST
static volatile unsigned int sd_registers[17];
#define EMMC_ARG2 (&sd_registers[0])
#define EMMC_BLKSIZECNT (&sd_registers[1])
#define EMMC_ARG1 (&sd_registers[2])
#define EMMC_CMDTM (&sd_registers[3])
#define EMMC_RESP0 (&sd_registers[4])
#define EMMC_RESP1 (&sd_registers[5])
#define EMMC_RESP2 (&sd_registers[6])
#define EMMC_RESP3 (&sd_registers[7])
#define EMMC_DATA (&sd_registers[8])
#define EMMC_STATUS (&sd_registers[9])
#define EMMC_CTRL0 (&sd_registers[10])
#define EMMC_CTRL1 (&sd_registers[11])
#define EMMC_INTRPT (&sd_registers[12])
#define EMMC_INT_MASK (&sd_registers[13])
#define EMMC_INT_EN (&sd_registers[14])
#define EMMC_CTRL2 (&sd_registers[15])
#define EMMC_SLOTISR_VER (&sd_registers[16])

#include "../../drivers/sd.c"

void uart_writeText(const char* text) { (void)text; }
void wait_cycles(unsigned int n) { (void)n; }
void wait_msec(unsigned int n) {
    (void)n;
    *EMMC_INTRPT = INT_CMD_DONE | INT_READ_RDY;
}

static void sd_mock_reset(unsigned int response) {
    unsigned int i;
    for (i = 0; i < 17; i++) sd_registers[i] = 0;
    *EMMC_RESP0 = response;
    *EMMC_DATA = 0x12345678;
    sd_err = SD_OK;
    sd_rca = 0;
    sd_scr[0] = sd_scr[1] = 0;
}
