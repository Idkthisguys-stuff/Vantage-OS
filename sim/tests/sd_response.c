#include "sd_mock.h"
#include <stdio.h>

static int check_read_error(unsigned int response, unsigned long flags) {
    unsigned int buffer[128];
    unsigned int i;
    sd_mock_reset(response);
    sd_scr[0] = flags;
    for (i = 0; i < 128; i++) buffer[i] = 0xcccccccc;
    if (sd_readblock(0, (unsigned char*)buffer, 1) != 0 || sd_err == SD_OK) return 0;
    for (i = 0; i < 128; i++) if (buffer[i] != 0xcccccccc) return 0;
    return 1;
}

int main(void) {
    unsigned int bit;
    unsigned int buffer[128];
    for (bit = 0; bit < 32; bit++) {
        unsigned int error = 1u << bit;
        if ((error & CMD_ERRORS_MASK) &&
            (!check_read_error(error, SCR_SUPP_CCS) || !check_read_error(error, 0))) {
            fprintf(stderr, "Card error bit %u did not stop the read\n", bit);
            return 1;
        }
    }
    sd_mock_reset(0x900); /* Ready for data and transfer state: not an error. */
    sd_scr[0] = SCR_SUPP_CCS;
    if (sd_readblock(0, (unsigned char*)buffer, 1) != 512 || sd_err) return 1;
    for (bit = 0; bit < 128; bit++) if (buffer[bit] != 0x12345678) return 1;

    sd_mock_reset(ACMD41_CMD_COMPLETE | ACMD41_CMD_CCS | ACMD41_VOLTAGE);
    if ((unsigned int)sd_cmd(CMD_SEND_OP_COND & ~CMD_NEED_APP, ACMD41_ARG_HC) !=
        (ACMD41_CMD_COMPLETE | ACMD41_CMD_CCS | ACMD41_VOLTAGE) || sd_err) return 1;

    sd_mock_reset(1u << 22);
    sd_rca = 0x10000;
    sd_cmd(CMD_SET_BUS_WIDTH, sd_rca | 2);
    if (sd_err == SD_OK) return 1;

    {
        unsigned int commands[] = { CMD_CARD_SELECT, CMD_STOP_TRANS, CMD_SET_BLOCKCNT };
        for (bit = 0; bit < sizeof(commands) / sizeof(commands[0]); bit++) {
            sd_mock_reset(1u << 22);
            if (sd_cmd(commands[bit], 0) != SD_ERROR || sd_err == SD_OK) return 1;
            sd_mock_reset(0x900);
            if (sd_cmd(commands[bit], 0) != SD_OK || sd_err != SD_OK) return 1;
        }
    }
    puts("PASS: R1 errors stop reads before writes; valid R1 and OCR responses succeed");
    return 0;
}
