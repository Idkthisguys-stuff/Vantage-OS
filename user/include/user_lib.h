#pragma once
#include "../../includes/stdint.h"

#include "../../includes/video/graphics.h"
#include "../../includes/touch.h"
#include "../../includes/video/display.h"

struct circle_args {
    int x;
    int y;
    int r;
};

uint32_t sys_touch_read(int* buf);

// DISPLAY SYSCALLS
void sys_canvas_set_color(uint32_t color);
void sys_canvas_clear(void);
void sys_canvas_fill_circle(int x, int y, int r);
void sys_canvas_present(void);