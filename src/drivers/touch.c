#include "../../includes/touch.h"
#include "../../includes/i2c.h"

#define TOUCH_POINTS 10
#define TOUCH_DOWN 0
#define TOUCH_CONTACT 2

static uint32_t lastDown;
static int lastX;
static int lastY;

uint32_t touch_init(void) {
    i2c_init();
    return i2c_probe(TOUCH_ADDR);
}

uint32_t touch_read(int* x, int* y) {
    uint8_t buf[9];

    if (i2c_read_reg(TOUCH_ADDR, 0, buf, sizeof(buf))) {
        uint8_t count = buf[2] & 0xF;
        uint8_t event = (buf[3] >> 6) & 3;

        if (count == 0 || count > TOUCH_POINTS) {
            lastDown = 0;
        } else if (event == TOUCH_DOWN || event == TOUCH_CONTACT) {
            lastDown = 1;
            int rawX = ((buf[3] & 0xF) << 8) | buf[4];
            int rawY = ((buf[5] & 0xF) << 8) | buf[6];

            lastX = TOUCH_FLIP_X ? TOUCH_WIDTH - 1 - rawX : rawX;
            lastY = TOUCH_FLIP_Y ? TOUCH_HEIGHT - 1 - rawY : rawY;
        } else {
            lastDown = 0;
        }
    }

    *x = lastX;
    *y = lastY;
    return lastDown;
}