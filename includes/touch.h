#pragma once

#include "stdint.h"

#define TOUCH_ADDR 0x38
#define PANEL_ADDR 0x45
#define TOUCH_WIDTH 800
#define TOUCH_HEIGHT 480
#define TOUCH_FLIP_X 1
#define TOUCH_FLIP_Y 1

uint32_t touch_init(void);
uint32_t touch_read(int* x, int* y);