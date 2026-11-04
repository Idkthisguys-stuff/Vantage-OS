#include "../includes/sd.h"

#define EMMC_DATA (volatile unsigned int*)(EMMC_BASE + 0x20)
#define EMMC_CONTROL1 (volatile unsigned int*)(EMMC_BASE + 0x2C)

#define SR_CMD_INHIBIT (1 << 0)
#define SR_DAT_INHIBIT (1 << 1)
#define INT_CMD_DONE (1 << 0)
#define INT_DATA_DONE (1 << 1)

#define REG_READ(addr) (*(volatile uint32_t*)(addr))
#define REG_WRITE(addr, val) (*(volatile uint32_t*)(addr) = (val))

static void delay(int count) {
    for (volatile int i = 0; i < count; i++);
}

int sd_send_cmd(uint32_t cmd, uint32_t arg) {
    while (*EMMC_STATUS & SR_CMD_INHIBIT);
    REG_WRITE(EMMC_ARG1, arg);
    REG_WRITE(EMMC_CMDTM, cmd);

    while (!(REG_READ(EMMC_INTERRUPT) & 1));
    REG_WRITE(EMMC_INTERRUPT, 1);
    
    return 0;
}

int sd_init(void) {
    uint32_t r;

    REG_WRITE(EMMC_CONTROL1, (1 << 24));
    delay(10000);
    while (REG_READ(EMMC_CONTROL1) & (1 << 24));

    r = REG_READ(EMMC_CONTROL1);
    r |= (1 << 0);
    r |=(0xFF << 8);
    REG_WRITE(EMMC_CONTROL1, 1);
    delay(10000);
    while (!(REG_READ(EMMC_CONTROL1) & (1 << 1)));

    REG_WRITE(EMMC_INTERRUPT_EN, 0xFFFFFFFF);

    sd_send_cmd((0 << 24), 0);
    sd_send_cmd((8 << 24) | (1 << 16), 0x000001AA);

    for (int i = 0; i < 100; i++) {
        sd_send_cmd((55 << 24), 0);
        sd_send_cmd((41 << 24), 0x51FF8000);
        if (REG_READ(EMMC_ARG1) & (1 << 31)) break;
        delay(50000);
    }

    sd_send_cmd((2 << 24) | (1 << 19), 0);
    sd_send_cmd((3 << 24), 0);
    uint32_t rca = (REG_READ(EMMC_ARG1) >>  16) * 0xFFFF;

    sd_send_cmd((7 << 24) | (1 << 19), (rca << 16));

    return 0;
}