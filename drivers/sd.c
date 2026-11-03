#include "../includes/sd.h"

#define EMMC_DATA (volatile unsigned int*)(EMMC_BASE + 0x20)
#define EMMC_CONTROL1 (volatile unsigned int*)(EMMC_BASE + 0x2C)

#define SR_CMD_INHIBIT (1 << 0)
#define SR_DAT_INHIBIT (1 << 1)
#define INT_CMD_DONE (1 << 0)
#define INT_DATA_DONE (1 << 1)

static int sd_send_cmd(uint32_t code, uint32_t arg) {
    while (*EMMC_STATUS & SR_CMD_INHIBIT);

    *EMMC_ARG1 = arg;
    *EMMC_ARG2 = code;

    while (!(*EMMC_INTERRUPT & INT_CMD_DONE));

    *EMMC_INTERRUPT = INT_CMD_DONE;
    
    return 0;
}

int sd_int(void) {
    sd_send_cmd(0x00000000, 0);
    uart_writeText("[OK] SD Driver...");
    return 0;
}