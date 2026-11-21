#pragma once

#include <vantage/abi/key.h>
#include <vantage/abi/touch.h>

/* Returns the number of rows available above the keyboard. */
uint32_t keyboard_init(uint32_t columns, uint32_t rows);
uint32_t keyboard_touch(const TouchContact* contact, KeyEvent* event);
uint32_t keyboard_repeat(KeyEvent* event);
void keyboard_cancel_touches(void);
void keyboard_redraw(void);
uint32_t keyboard_draw(void);
