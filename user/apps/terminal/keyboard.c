#include "keyboard.h"
#include <vantage/user.h>

#define KEYBOARD_ROWS 17U
#define KEYBOARD_KEYS 43U
#define NO_KEY KEYBOARD_KEYS
#define ALL_KEYS ((1ULL << KEYBOARD_KEYS) - 1)

enum { SHIFT = 29, DELETE = 37, CANCEL = 38, SPACE = 40, ENTER = 42 };

static uint32_t keyboard_columns, keyboard_top, keyboard_shift, panel_dirty;
static uint64_t dirty_keys;
static uint32_t point_key[TOUCH_CONTACTS_MAX];
static uint32_t key_holders[KEYBOARD_KEYS];
static uint32_t repeat_time[KEYBOARD_KEYS], repeat_delay[KEYBOARD_KEYS];

uint32_t keyboard_init(uint32_t columns, uint32_t rows) {
    keyboard_columns = keyboard_shift = 0;
    dirty_keys = panel_dirty = 0;
    for (uint32_t id = 0; id < TOUCH_CONTACTS_MAX; id++) point_key[id] = NO_KEY;
    for (uint32_t key = 0; key < KEYBOARD_KEYS; key++) key_holders[key] = 0;
    if (columns < 50 || rows < KEYBOARD_ROWS + 4) return rows;

    keyboard_columns = columns;
    keyboard_top = rows - KEYBOARD_ROWS;
    panel_dirty = 1;
    dirty_keys = ALL_KEYS;
    return keyboard_top;
}

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
        if (key == 38) label = "Cancel";
        else if (key == 40) label = "Space";
        else if (key == 42) label = "Enter";
    }
    b.x = 8 + left * (width - 16) / 1000 + 3;
    b.width = (right * (width - 16) / 1000) - (left * (width - 16) / 1000) - 6;
    b.y = keyboard_top * TEXT_CELL_HEIGHT + 12 + row * 50;
    b.height = 42; b.radius = 7;
    b.text_scale = label ? 1 : 2;
    b.foreground = RGB(232, 238, 248);
    b.background = key == 42 ? RGB(40, 105, 205) : key == 29 || key >= 37 ? RGB(49, 61, 80) : RGB(63, 76, 96);
    if (key == 29 && keyboard_shift) b.background = RGB(40, 105, 205);
    if (key_holders[key]) b.background = RGB(83, 148, 236);
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

static uint32_t repeatable(uint32_t key) { return key == DELETE || key == SPACE; }

static void key_event(uint32_t key, KeyEvent* event) {
    event->type = key == DELETE ? KEY_BACKSPACE :
                  key == ENTER ? KEY_ENTER : key == CANCEL ? KEY_CANCEL : KEY_TEXT;
    event->character = event->type == KEY_TEXT ? (uint32_t)keyboard_character(key) : 0;
    if (event->type == KEY_TEXT && keyboard_shift) {
        keyboard_shift = 0;
        dirty_keys = ALL_KEYS;
    }
}

static void release(uint32_t id) {
    uint32_t key = point_key[id];
    if (key == NO_KEY) return;
    point_key[id] = NO_KEY;
    if (--key_holders[key] == 0) dirty_keys |= 1ULL << key;
}

uint32_t keyboard_touch(const TouchContact* contact, KeyEvent* event) {
    uint32_t id = contact->id;
    if (!keyboard_columns || id >= TOUCH_CONTACTS_MAX) return 0;

    const TouchEvent* touch = &contact->event;
    uint32_t key = keyboard_hit(touch->x, touch->y);
    uint32_t pressed = point_key[id];
    if (touch->type == TOUCH_EVENT_DOWN) {
        release(id);
        if (key == NO_KEY) return 0;
        point_key[id] = key;
        if (key_holders[key]++ != 0) return 0;
        dirty_keys |= 1ULL << key;
        if (!repeatable(key)) return 0;
        repeat_time[key] = sys_time_us();
        repeat_delay[key] = 400000;
        key_event(key, event);
        return 1;
    }
    if (pressed == NO_KEY) return 0;
    if (touch->type == TOUCH_EVENT_MOVE) {
        if (key != pressed) release(id);
        return 0;
    }
    if (touch->type != TOUCH_EVENT_UP) return 0;

    release(id);
    if (pressed != key || key_holders[key] || repeatable(key)) return 0;
    if (key == SHIFT) {
        keyboard_shift = !keyboard_shift;
        dirty_keys = ALL_KEYS;
        return 0;
    }
    key_event(key, event);
    return 1;
}

uint32_t keyboard_repeat(KeyEvent* event) {
    static const uint32_t keys[] = {DELETE, SPACE};
    uint32_t now = sys_time_us();
    for (uint32_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        uint32_t key = keys[i];
        if (!key_holders[key] || now - repeat_time[key] < repeat_delay[key]) continue;
        key_event(key, event);
        /* Drop overdue repeats instead of emitting a burst after a stall. */
        repeat_time[key] = now;
        repeat_delay[key] = 70000;
        return 1;
    }
    return 0;
}

static uint32_t keyboard_draw_key(uint32_t key) {
    CanvasButton button = keyboard_button(key), shadow = button;
    shadow.y += 3;
    shadow.background = RGB(10, 15, 23);
    shadow.label[0] = 0;
    return sys_canvas_button(&shadow) && sys_canvas_button(&button);
}

uint32_t keyboard_draw(void) {
    if (!dirty_keys) return 0;
    if (panel_dirty) {
        CanvasButton panel = {0};
        panel.y = keyboard_top * TEXT_CELL_HEIGHT;
        panel.width = keyboard_columns * TEXT_CELL_WIDTH;
        panel.height = KEYBOARD_ROWS * TEXT_CELL_HEIGHT;
        panel.background = RGB(24, 31, 43);
        if (!sys_canvas_button(&panel)) return 0;
        panel_dirty = 0;
    }
    uint32_t changed = 0;
    for (uint32_t key = 0; key < KEYBOARD_KEYS; key++) {
        uint64_t bit = 1ULL << key;
        if ((dirty_keys & bit) && keyboard_draw_key(key)) {
            dirty_keys &= ~bit;
            changed = 1;
        }
    }
    return changed;
}
