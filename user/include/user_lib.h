#pragma once
#include "../../includes/std.h"

#include "../../includes/video/graphics.h"
#include "../../includes/input/touch.h"
#include "../../includes/video/display.h"
#include "../../includes/console.h"

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

uint32_t sys_key_read(KeyEvent* event);
uint32_t sys_canvas_text_row(const TextRow* row);
uint32_t sys_canvas_button(const CanvasButton* button);
uint32_t sys_console_info(ConsoleInfo* info);
void sys_frame_wait(void);
uint32_t sys_time_us(void); /* Wrapping monotonic microseconds. */
void sys_console_write(const char* text, size_t length);
