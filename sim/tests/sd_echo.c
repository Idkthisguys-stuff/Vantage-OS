#include "sd_mock.h"
#include <stdio.h>

int main(void) {
    unsigned int bad[] = { 0, 0x1ab, 0xaa, 0xdeadbeef };
    unsigned int i;
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        sd_mock_reset(bad[i]);
        if (sd_cmd(CMD_SEND_IF_COND, 0x1aa) != SD_ERROR ||
            sd_err != (unsigned long)SD_ERROR) {
            fprintf(stderr, "Bad CMD8 echo %x was not propagated\n", bad[i]);
            return 1;
        }
    }
    sd_mock_reset(0x1aa);
    sd_err = (unsigned long)SD_ERROR;
    if (sd_cmd(CMD_SEND_IF_COND, 0x1aa) != SD_OK || sd_err != SD_OK) return 1;
    puts("PASS: CMD8 mismatches reach sd_err and valid echoes clear stale errors");
    return 0;
}
