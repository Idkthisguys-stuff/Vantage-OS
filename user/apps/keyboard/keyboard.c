#include "keyboard.h"
#include "vantage/abi/key.h"

Terminal terminal;

uint32_t keyboard_columns = 0;
uint32_t keyboard_top = 0;
uint32_t keyboard_shift = 0;
uint32_t keyboard_full_dirty = 0;
uint32_t keyboard_dirty_a = NO_KEY;
uint32_t keyboard_dirty_b = NO_KEY;
uint32_t keyboard_pressed[KEYBOARD_KEYS] = {0};
uint32_t keyboard_repeat_time = 0;
uint32_t keyboard_repeat_delay = 0;
uint32_t point_key[MAX_TOUCH_POINTS] = {0};

static char keyboard_character(uint32_t key) {
    char c;
    if (key < 10) return (keyboard_shift ? "!@#$%^&*()" : "1234567890")[key];
    if (key < 20) c = "qwertyuiop"[key - 10];
    else if (key < 29) c = "asdfghjkl"[key - 20];
    else if (key >= 30 && key <= 36) c = "zxcvbnm"[key - 30];
    else return key == 39 ? (keyboard_shift ? '/' : ',') : key == 41 ? '.' : ' ';
    if (keyboard_shift && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    return c;
}

static void keyboard_mark_key(uint32_t key) {
    if (key == NO_KEY) return;
    if (keyboard_dirty_a == NO_KEY || keyboard_dirty_a == key) keyboard_dirty_a = key;
    else keyboard_dirty_b = key;
}

/* Layout units keep rendering and hit testing identical at any supported width. */
static CanvasButton keyboard_button(uint32_t key) {
    CanvasButton b = {0};
    uint32_t row, left, right, width = keyboard_columns * TEXT_CELL_WIDTH;
    const char* label = NULL;
    if (key < 20) { row = key / 10; left = (key % 10) * 100; right = left + 100; }
    else if (key < 29) { row = 2; left = 50 + (key - 20) * 100; right = left + 100; }
    else if (key < 38) {
        row = 3;
        if (key == 29) { left = 0; right = 140; label = "Shift"; }
        else if (key == 37) { left = 840; right = 1000; label = "Del"; }
        else { left = 140 + (key - 30) * 100; right = left + 100; }
    } else {
        const uint32_t edges[] = {0, 150, 250, 700, 800, 1000};
        row = 4; left = edges[key - 38]; right = edges[key - 37];
        if (key == 38) label = "<-";
        else if (key == 40) label = " ";
        else if (key == 42) label = "->";
    }
    b.x = 8 + left * (width - 16) / 1000 + 3;
    b.width = (right * (width - 16) / 1000) - (left * (width - 16) / 1000) - 6;
    b.y = keyboard_top * TEXT_CELL_HEIGHT + 12 + row * 50;
    b.height = 42; b.radius = 7;
    b.text_scale = label ? 1 : 2;
    b.foreground = RGB(232, 238, 248);
    b.background = key == 42 ? RGB(40, 105, 205) : key == 29 || key >= 37 ? RGB(49, 61, 80) : RGB(63, 76, 96);
    if (key == 29 && keyboard_shift) b.background = RGB(40, 105, 205);
    if (keyboard_pressed[key]) b.background = RGB(83, 148, 236);
    if (label) { uint32_t i = 0; while (label[i] && i < sizeof(b.label) - 1) { b.label[i] = label[i]; i++; } }
    else b.label[0] = keyboard_character(key);
    if (key == 37 || key == 40 || (key == 42 && b.width >= 80)) b.text_scale = 2;
    return b;
}

static uint32_t keyboard_hit(int x, int y) {
    uint32_t key;
    if (x < 0 || y < 0) return NO_KEY;
    for (key = 0; key < KEYBOARD_KEYS; key++) {
        CanvasButton b = keyboard_button(key);
        if ((uint32_t)x >= b.x && (uint32_t)x - b.x < b.width &&
            (uint32_t)y >= b.y && (uint32_t)y - b.y < b.height) return key;
    }
    return NO_KEY;
}

uint32_t keyboard_touch(const TouchEvent* touch, KeyEvent* event) {
    uint32_t key;
    if (!keyboard_columns || !event) return 0;
    event->type = KEY_NONE;
    event->character = 0;
    
    key = keyboard_hit(touch->x, touch->y);
    
    if (touch->type == TOUCH_EVENT_DOWN) {
        if (key != NO_KEY) {
            keyboard_pressed[key] = 1;
            keyboard_mark_key(key);
            if (key == 37) {
                event->type = KEY_BACKSPACE;
                keyboard_repeat_time = sys_time_us();
                keyboard_repeat_delay = 400000;
                return 1;
            } else if (key == 29) {
                keyboard_shift = !keyboard_shift;
                keyboard_full_dirty = 1;
                return 0;
            } else if (key == 42) {
                event->type = KEY_ENTER;
                return 1;
            } else if (key == 38) {
                event->type = KEY_CANCEL;
                return 1;
            } else {
                event->type = KEY_TEXT;
                event->character = (uint32_t)keyboard_character(key);
                return 1;
            }
        }
        return 0;
    }
    
    if (touch->type == TOUCH_EVENT_UP) {
        uint32_t key = keyboard_hit(touch->x, touch->y);
        if (key != NO_KEY && keyboard_pressed[key]) {
            keyboard_pressed[key] = 0;
            keyboard_mark_key(key);
        }
        return 0;
    }
    return 0;
}

static uint32_t keyboard_draw_key(uint32_t key) {
    CanvasButton b = keyboard_button(key), shadow = b;
    shadow.y += 3;
    shadow.background = RGB(10, 15, 23);
    shadow.label[0] = 0;
    return sys_canvas_button(&shadow) && sys_canvas_button(&b);
}

uint32_t keyboard_draw(void) {
    uint32_t key, success = 1;
    if (keyboard_full_dirty) {
        CanvasButton panel = {0};
        panel.y = keyboard_top * TEXT_CELL_HEIGHT;
        panel.width = keyboard_columns * TEXT_CELL_WIDTH;
        panel.height = KEYBOARD_ROWS * TEXT_CELL_HEIGHT;
        panel.background = RGB(24, 31, 43);
        if (!sys_canvas_button(&panel)) return 0;
        for (key = 0; key < KEYBOARD_KEYS; key++) {
            if (!keyboard_draw_key(key)) success = 0;
        }
        return success;
    }
    if (keyboard_dirty_a != NO_KEY && !keyboard_draw_key(keyboard_dirty_a)) success = 0;
    if (keyboard_dirty_b != NO_KEY && !keyboard_draw_key(keyboard_dirty_b)) success = 0;
    return success;
}