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

#ifndef VANTAGE_SD_TEST
#define EMMC_BASE 0xFE340000
#define EMMC_ARG2 ((volatile unsigned int*)(EMMC_BASE + 0x00))
#define EMMC_BLKSIZECNT ((volatile unsigned int*)(EMMC_BASE + 0x04))
#define EMMC_ARG1 ((volatile unsigned int*)(EMMC_BASE + 0x08))
#define EMMC_CMDTM ((volatile unsigned int*)(EMMC_BASE + 0x0C))
#define EMMC_RESP0 ((volatile unsigned int*)(EMMC_BASE + 0x10))
#define EMMC_RESP1 ((volatile unsigned int*)(EMMC_BASE + 0x14))
#define EMMC_RESP2 ((volatile unsigned int*)(EMMC_BASE + 0x18))
#define EMMC_RESP3 ((volatile unsigned int*)(EMMC_BASE + 0x1C))
#define EMMC_DATA ((volatile unsigned int*)(EMMC_BASE + 0x20))
#define EMMC_STATUS ((volatile unsigned int*)(EMMC_BASE + 0x24))
#define EMMC_CTRL0 ((volatile unsigned int*)(EMMC_BASE + 0x28))
#define EMMC_CTRL1 ((volatile unsigned int*)(EMMC_BASE + 0x2C))
#define EMMC_INTRPT ((volatile unsigned int*)(EMMC_BASE + 0x30))
#define EMMC_INT_MASK ((volatile unsigned int*)(EMMC_BASE + 0x34))
#define EMMC_INT_EN ((volatile unsigned int*)(EMMC_BASE + 0x38))
#define EMMC_CTRL2 ((volatile unsigned int*)(EMMC_BASE + 0x3C))
#define EMMC_SLOTISR_VER ((volatile unsigned int*)(EMMC_BASE + 0xFC))
#endif

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

    return (cnt <= 0 || (*EMMC_INTRPT & INT_ERR_MASK)) ? SD_ERROR : SD_OK;
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
        r = sd_cmd(CMD_APP_CMD | CMD_RSPNS_48, sd_rca);

        if (sd_err || !r) {
            uart_writeText("[ERR] App failed to send SD APP command\n");
            if (!sd_err) sd_err = SD_ERROR;
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
        uart_writeText("[ERR] Failed to send EMMC command, code=");
        uart_writeHex(code);
        uart_writeText(" INTRPT=");
        uart_writeHex(*EMMC_INTRPT);
        uart_writeText(" STATUS=");
        uart_writeHex(*EMMC_STATUS);
        uart_writeText(" CTRL0=");
        uart_writeHex(*EMMC_CTRL0);
        uart_writeText(" CTRL1=");
        uart_writeHex(*EMMC_CTRL1);
        uart_writeText("\n");
        sd_err = r;
        return 0;
    }

    r = *EMMC_RESP0;

    if (code == CMD_GO_IDLE || code == CMD_APP_CMD) {
        return 0;
    } else if (code == (CMD_APP_CMD | CMD_RSPNS_48)) {
        if (r & CMD_ERRORS_MASK) {
            sd_err = SD_ERROR;
            return 0;
        }
        return r&SR_APP_CMD;
    } else if (code == (CMD_SEND_OP_COND & ~CMD_NEED_APP)) {
        return r;
    } else if (code == CMD_SEND_IF_COND) {
        sd_err = r == arg ? SD_OK : SD_ERROR;
        return sd_err;
    } else if (code == CMD_ALL_SEND_CID) {
        r |= *EMMC_RESP3;
        r |= *EMMC_RESP2;
        r |= *EMMC_RESP1;
        return r;
    } else if (code == CMD_SEND_REL_ADDR) {
        sd_err = (((r&0x1fff))|((r&0x2000)<<6)|((r&0x4000)<<8)|((r&0x8000)<<8))&CMD_ERRORS_MASK; // "DEAR GOD" - Idkthisguy's last words typing this out ;-;
        return r&CMD_RCA_MASK;
    }
    
    sd_err = (r & CMD_ERRORS_MASK) ? SD_ERROR : SD_OK;
    return sd_err;
}

int sd_readBlock(unsigned int lba, unsigned char *buffer, unsigned int num) {
    int r,c=0,d;

    if (num<1) num=1;

    if(sd_status(SR_DAT_INHIBIT)) {
        sd_err=SD_TIMEOUT; 
        return 0;
    }

    unsigned int *buf=(unsigned int *)buffer;

    if(sd_scr[0] & SCR_SUPP_CCS) {
        if(num > 1 && (sd_scr[0] & SCR_SUPP_SET_BLKCNT)) {
            sd_cmd(CMD_SET_BLOCKCNT,num);
            if(sd_err) return 0;
        }

        *EMMC_BLKSIZECNT = (num << 16) | 512;
        sd_cmd(num == 1 ? CMD_READ_SINGLE : CMD_READ_MULTI,lba);
        
        if(sd_err) return 0;
    } else {
        *EMMC_BLKSIZECNT = (1 << 16) | 512;
    }
    while( c < num ) {
        if(!(sd_scr[0] & SCR_SUPP_CCS)) {
            sd_cmd(CMD_READ_SINGLE,(lba+c)*512);
            if(sd_err) return 0;
        }

        if((r=sd_int(INT_READ_RDY))) {
            uart_writeText("\rERROR: Timeout waiting for ready to read\n");
            sd_err=r;
            return 0;
        }

        for(d=0;d<128;d++) buf[d] = *EMMC_DATA;
        c++; buf+=128;
    }
    if( num > 1 && !(sd_scr[0] & SCR_SUPP_SET_BLKCNT) && (sd_scr[0] & SCR_SUPP_CCS)) sd_cmd(CMD_STOP_TRANS,0);

    return sd_err!=SD_OK || c!=num? 0 : num*512;
}

int sd_clk(unsigned int f) {
    unsigned int d,c=41666666/f,x,s=32,h=0;
    int cnt = 100000;
    while((*EMMC_STATUS & (SR_CMD_INHIBIT|SR_DAT_INHIBIT)) && cnt--) wait_msec(1);
    if(cnt<=0) {
        uart_writeText("[ERR] SD timeout waiting for inhibit flag\n");
        return SD_ERROR;
    }

    *EMMC_CTRL1 &= ~C1_CLK_EN; wait_msec(10);

    x=c-1; 
    
    if(!x) s=0; else {
        if(!(x & 0xffff0000u)) {
            x <<= 16; s -= 16; 
        }
        if(!(x & 0xff000000u)) {
            x <<= 8;  s -= 8;
        }
        if(!(x & 0xf0000000u)) {
            x <<= 4;  s -= 4;
        }
        if(!(x & 0xc0000000u)) {
            x <<= 2;  s -= 2;
        }
        if(!(x & 0x80000000u)) {
            x <<= 1;  s -= 1;
        }

        if(s>0) s--;
        if(s>7) s=7;
    }

    if(sd_hv>HOST_SPEC_V2) d=c; else d=(1<<s);

    if(d<=2) {
        d=2;
        s=0;
    }
    
    if(sd_hv>HOST_SPEC_V2) h=(d&0x300)>>2;

    d=(((d&0x0ff)<<8)|h);

    *EMMC_CTRL1=(*EMMC_CTRL1&0xffff003f)|d; wait_msec(10);
    *EMMC_CTRL1 |= C1_CLK_EN; wait_msec(10);
    cnt=10000; while(!(*EMMC_CTRL1 & C1_CLK_STABLE) && cnt--) wait_msec(10);

    if(cnt<=0) {
        uart_writeText("[ERR] failed to get stable clock\n");
        return SD_ERROR;
    }

    return SD_OK;
}

int sd_init() {
    long r,cnt,ccs=0;

    r=*GPFSEL4; r&=~(7<<(7*3)); 
    *GPFSEL4=r;
    *GPPUD=2; wait_cycles(150); *GPPUDCLK1=(1<<15); wait_cycles(150); *GPPUD=0; *GPPUDCLK1=0;
    r=*GPHEN1; r|=1<<15; *GPHEN1=r;

    r=*GPFSEL4; r|=(7<<(8*3))|(7<<(9*3)); *GPFSEL4=r;
    *GPPUD=2; wait_cycles(150); *GPPUDCLK1=(1<<16);
    
    r = *GPFSEL5; r|=(7<<(0*3)) | (7<<(1*3)) | (7<<(2*3)) | (7<<(3*3)); *GPFSEL5=r;
    *GPPUD=2; wait_cycles(150);
    *GPPUDCLK1=(1<<18) | (1<<19) | (1<<20) | (1<<21);
    wait_cycles(150); *GPPUD=0; *GPPUDCLK1=0;

    sd_hv = (*EMMC_SLOTISR_VER & HOST_SPEC_NUM) >> HOST_SPEC_NUM_SHIFT;
    uart_writeText("[OK] EMMC GPIO set up\n");

    *EMMC_CTRL1 |= C1_SRST_HC;

    cnt=10000; do{
        wait_msec(10);
    } while( (*EMMC_CTRL1 & C1_SRST_HC) && cnt-- );

    if(cnt<=0) {
        uart_writeText("[ERR] EMMC failed to reset EMMC\n");
        return SD_ERROR;
    }

    *EMMC_CTRL0 |= 0x0F00;
    wait_msec(10);

    *EMMC_CTRL1 |= C1_CLK_INTLEN | C1_TOUNIT_MAX;
    wait_msec(10);

    if((r=sd_clk(400000))) return r;

    wait_msec(50);

    *EMMC_INT_EN   = 0xffffffff;
    *EMMC_INT_MASK = 0xffffffff;
    sd_scr[0]=sd_scr[1]=sd_rca=sd_err=0;
    sd_cmd(CMD_GO_IDLE,0);

    if(sd_err) return sd_err;

    sd_cmd(CMD_SEND_IF_COND,0x000001AA);
    if(sd_err) return sd_err;
    cnt=100; r=0; while(!(r&ACMD41_CMD_COMPLETE) && cnt--) {
        wait_cycles(400);
        r=sd_cmd(CMD_SEND_OP_COND,ACMD41_ARG_HC);
        uart_writeText("[OK] EMMC CMD_SEND_OP_COND returned ");
        uart_writeHex(r);
        uart_writeText("\n");

        if(sd_err!=SD_TIMEOUT && sd_err!=SD_OK ) {
            uart_writeText("[ERR] EMMC ACMD41 returned error\n");
            return sd_err;
        }
    }
    if(!(r&ACMD41_CMD_COMPLETE) || !cnt ) return SD_TIMEOUT;
    if(!(r&ACMD41_VOLTAGE)) return SD_ERROR;
    if(r&ACMD41_CMD_CCS) ccs=SCR_SUPP_CCS;

    sd_cmd(CMD_ALL_SEND_CID,0);

    sd_rca = sd_cmd(CMD_SEND_REL_ADDR,0);
    if(sd_err) return sd_err;

    if((r=sd_clk(25000000))) return r;

    sd_cmd(CMD_CARD_SELECT,sd_rca);
    if(sd_err) return sd_err;

    if(sd_status(SR_DAT_INHIBIT)) return SD_TIMEOUT;
    *EMMC_BLKSIZECNT = (1<<16) | 8;
    sd_cmd(CMD_SEND_SCR,0);
    if(sd_err) return sd_err;
    if(sd_int(INT_READ_RDY)) return SD_TIMEOUT;

    r=0; cnt=100000; while(r<2 && cnt) {
        if( *EMMC_STATUS & SR_READ_AVAILABLE )
            sd_scr[r++] = *EMMC_DATA;
        else
            wait_msec(1);
    }
    if(r!=2) return SD_TIMEOUT;
    if(sd_scr[0] & SCR_SD_BUS_WIDTH_4) {
        sd_cmd(CMD_SET_BUS_WIDTH,sd_rca|2);
        if(sd_err) return sd_err;
        
        *EMMC_CTRL0 |= C0_HCTL_DWITDH;
    }
    uart_writeText("[EMMC] EMMC supports ");
    if(sd_scr[0] & SCR_SUPP_SET_BLKCNT)
        uart_writeText("SET_BLKCNT ");
    if(ccs)
        uart_writeText("CCS ");
    uart_writeText("\n");

    sd_scr[0]&=~SCR_SUPP_CCS;
    sd_scr[0]|=ccs;

    uart_writeText("[OK] SD INITIALIZED!\n");
    return SD_OK;
}
