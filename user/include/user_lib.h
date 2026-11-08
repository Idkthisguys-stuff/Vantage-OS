#pragma once
#include "../../includes/std.h"

uint64_t sys_task_id(void);
uint32_t sys_task_yield(void);
/* Returns 0 when scheduling is unavailable; otherwise does not return. */
uint32_t sys_task_exit(int status);

#include "../../includes/video/graphics.h"
#include "../../includes/input/touch.h"
#include "../../includes/video/display.h"

struct circle_args {
    int x;
    int y;
    int r;
};

uint32_t sys_touch_read(TouchEvent* event);

// DISPLAY SYSCALLS
void sys_canvas_set_color(uint32_t color);
void sys_canvas_clear(void);
void sys_canvas_fill_circle(int x, int y, int r);
void sys_canvas_present(void);
