#pragma once
#include <vantage/base.h>
#include <vantage/kernel/canvas.h>
#include <vantage/abi/key.h>
#include <vantage/drivers/touch.h>
#include "../terminal/terminal.h"
#include <vantage/user.h>

#define KEYBOARD_ROWS 17U
#define KEYBOARD_KEYS 43U
#define NO_KEY (~0U)

#define MAX_TOUCH_POINTS 10 // 10, even though my display is 5 points :/

#define KEYBOARD_BUF_SIZE 32 // Buffer for fast typers and for the keyboard to keep up

typedef struct Terminal Terminal;

typedef struct {
    KeyEvent events[KEYBOARD_BUF_SIZE];
    uint32_t head;
    uint32_t tail;
} KeyboardBuf;

extern KeyboardBuf keyboard_buf_queue;
extern uint32_t keyboard_columns;
extern uint32_t keyboard_top;
extern uint32_t keyboard_shift;
extern uint32_t keyboard_full_dirty;
extern uint32_t keyboard_dirty_a;
extern uint32_t keyboard_dirty_b;
extern uint32_t keyboard_pressed[KEYBOARD_KEYS];
extern uint32_t keyboard_repeat_time;
extern uint32_t keyboard_repeat_delay;
extern uint32_t point_key[MAX_TOUCH_POINTS];

uint32_t keyboard_touch(const TouchEvent* touch, KeyEvent* event);
uint32_t keyboard_draw(void);
uint32_t keyboard_buf_pop(KeyEvent* event);
void keyboard_buf_push(const KeyEvent* event);