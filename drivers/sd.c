/*
 * Copyright (C) 2018 bzt (bztsrc@github)
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 */

// https://github.com/bztsrc/raspi3-tutorial/blob/master/0B_readsector/sd.c (this code is NOT completely mine)

#include "../includes/sd.h"
#include "../kernel/sched/delays/delay.h"
#include "../includes/io.h"

#define EMMC_BASE 0xFE340000
#define EMMC_ARG2 ((volatile unsigned int*)(EMMC_BASE + 0x00340000))
#define EMMC_BLKSIZECNT ((volatile unsigned int*)(EMMC_BASE + 0x00340004))
#define EMMC_ARG1 ((volatile unsigned int*)(EMMC_BASE + 0x00340008))
#define EMMC_CMDTM ((volatile unsigned int*)(EMMC_BASE + 0x0034000C))
#define EMMC_RESP0 ((volatile unsigned int*)(EMMC_BASE + 0x00340010))
#define EMMC_RESP1 ((volatile unsigned int*)(EMMC_BASE + 0x00340014))
#define EMMC_RESP2 ((volatile unsigned int*)(EMMC_BASE + 0x00340018))
#define EMMC_RESP3 ((volatile unsigned int*)(EMMC_BASE + 0x0034001C))
#define EMMC_DATA ((volatile unsigned int*)(EMMC_BASE + 0x00340020))
#define EMMC_STATUS ((volatile unsigned int*)(EMMC_BASE + 0x00340024))
#define EMMC_CTRL0 ((volatile unsigned int*)(EMMC_BASE + 0x00340028))
#define EMMC_CTRL1 ((volatile unsigned int*)(EMMC_BASE + 0x0034002C))
#define EMMC_INTRPT ((volatile unsigned int*)(EMMC_BASE + 0x00340030))
#define EMMC_INT_MASK ((volatile unsigned int*)(EMMC_BASE + 0x00340034))
#define EMMC_INT_EN ((volatile unsigned int*)(EMMC_BASE + 0x00340038))
#define EMMC_CTRL2 ((volatile unsigned int*)(EMMC_BASE + 0x0034003C))
#define EMMC_SLOTISR_VER ((volatile unsigned int*)(EMMC_BASE + 0x003400FC))

// command flags

#define CMD_NEED_APP 0x80000000
#define CMD_RSPNS_48 0x00020000
#define CMD_ERRORS_MASK 0xfff9c004
#define CMD_RCA_MASK 0xffff0000

// commands
#define CMD_GO_IDLE 0x00000000
#define CMD_ALL_SEND_CID 0x02010000
#define CMD_SEND_REL_ADDR 0x03020000
#define CMD_CARD_SELECT 0x07030000
#define CMD_SEND_IF_COND 0x08020000
#define CMD_STOP_TRANS 0x0C030000
#define CMD_READ_SINGLE 0x11220010
#define CMD_READ_MULTI 0x12220032
#define CMD_SET_BLOCKCNT 0x17020000
#define CMD_APP_CMD 0x37000000
#define CMD_SET_BUS_WIDTH (0x06020000|CMD_NEED_APP)
#define CMD_SEND_OP_COND (0x29020000|CMD_NEED_APP)
#define CMD_SEND_SCR (0x33220010|CMD_NEED_APP)

// status register settings
#define SR_READ_AVAILABLE 0x00000800
#define SR_DAT_INHIBIT 0x00000002
#define SR_CMD_INHIBIT 0x00000001
#define SR_APP_CMD 0x00000020

// interrupt register settings

#define INT_DATA_TIMEOUT 0x00100000
#define INT_CMD_TIMEOUT 0x00010000
#define INT_READ_RDY 0x00000020
#define INT_CMD_DONE 0x00000001
#define INT_ERR_MASK 0x017E8000

// control register settings
#define C0_SPI_MODE_EN 0x00100000
#define C0_HCTL_HS_EN 0x00000004
#define C0_HCTL_DWITDH 0x00000002
#define C1_SRST_DATA 0x04000000
#define C1_SRST_CMD 0x02000000
#define C1_SRST_HC 0x01000000
#define C1_TOUNIT_DIS 0x000f0000
#define C1_TOUNIT_MAX 0x000e0000
#define C1_CLK_GENSEL 0x00000020
#define C1_CLK_EN 0x00000004
#define C1_CLK_STABLE 0x00000002
#define C1_CLK_INTLEN 0x00000001

// slotisr_ver values
#define HOST_SPEC_NUM 0x00ff0000
#define HOST_SPEC_NUM_SHIFT 16
#define HOST_SPEC_V3 2
#define HOST_SPEC_V2 1
#define HOST_SPEC_V1 0

// scr flags
#define SCR_SD_BUS_WIDTH_4 0x00000400
#define SCR_SUPP_SET_BLKCNT 0x02000000
// for large SDs
#define SCR_SUPP_CCS 0x00000001

#define ACMD41_VOLTAGE 0x00ff8000
#define ACMD41_CMD_COMPLETE 0x80000000
#define ACMD41_CMD_CCS 0x40000000
#define ACMD41_ARG_HC 0x51ff8000

unsigned long sd_scr[2], sd_ocr, sd_rca, sd_err, sd_hv;

int sd_status(unsigned int mask) {
    int cnt = 50000; while ((*EMMC_STATUS & mask) && !(*EMMC_INTRPT & INT_ERR_MASK) && cnt--) wait_msec(1);

    return (cnt <= 0 || (*EMMC_INTRPT * INT_ERR_MASK)) ? SD_ERROR : SD_OK;
}

int sd_int(unsigned int mask) {
    unsigned int r, m=mask | INT_ERR_MASK;
    int cnt = 1000000; while(!(*EMMC_INTRPT & m) && cnt--) wait_msec(1);
    r = *EMMC_INTRPT;

    if (cnt<=0 || (r & INT_CMD_TIMEOUT) || (r & INT_DATA_TIMEOUT)) {
        *EMMC_INTRPT = r;
        return SD_TIMEOUT;
    }

    if (r & INT_ERR_MASK) {
        *EMMC_INTRPT = r;
        return SD_ERROR;
    }

    *EMMC_INTRPT = mask;
    return 0;
}

int sd_cmd(unsigned int code, unsigned int arg) {
    int r = 0;
    sd_err = SD_OK;

    if (code & CMD_NEED_APP) {
        r = sd_cmd(CMD_APP_CMD| (sd_rca?CMD_RSPNS_48:0), sd_rca);

        if (!sd_rca && !r) {
            uart_writeText("[ERR] App failed to send SD APP command\n");
            sd_err = SD_ERROR;
            return 0;
        }

        code &= ~CMD_NEED_APP;
    }

    if (sd_status(SR_CMD_INHIBIT)) {
        uart_writeText("[ERR] EMMC BUSY\n");
        sd_err = SD_TIMEOUT;
        
        return 0;
    }

    *EMMC_INTRPT = *EMMC_INTRPT;
    *EMMC_ARG1 = arg;
    *EMMC_CMDTM = code;

    if (code == CMD_SEND_OP_COND) {
        wait_msec(100);
    } else if (code == CMD_SEND_IF_COND || code == CMD_APP_CMD) {
        wait_msec(100);
    }

    if ((r = sd_int(INT_CMD_DONE))) {
        uart_writeText("[ERR] Failed to send EMMC command\n");
        sd_err = r;
        return 0;
    }

    r = *EMMC_RESP0;

    if (code == CMD_GO_IDLE || code == CMD_APP_CMD) {
        return 0;
    } else if (code == (CMD_APP_CMD | CMD_RSPNS_48)) {
        return r&SR_APP_CMD;
    } else if (code == CMD_SEND_OP_COND) {
        return r;
    } else if (code == CMD_SEND_IF_COND) {
        return r == arg ? SD_OK : SD_ERROR;
    } else if (code == CMD_ALL_SEND_CID) {
        r |= *EMMC_RESP3;
        r |= *EMMC_RESP2;
        r |= *EMMC_RESP1;
        return r;
    } else if (code == CMD_SEND_REL_ADDR) {
        sd_err = (((r&0x1fff))|((r&0x2000)<<6)|((r&0x4000)<<8)|((r&0x8000)<<8))&CMD_ERRORS_MASK; // "DEAR GOD" - Idkthisguy's last words typing this out ;-;
        return r&CMD_RCA_MASK;
    }

    return r&CMD_RCA_MASK;

    return 0;
}

int sd_readBlock(unsigned int lba, unsigned char *buf, unsigned int num) {
    return 0;
}