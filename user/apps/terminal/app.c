#include "../keyboard/keyboard.h"
#include "terminal.h"
#include "vantage/abi/graphics.h"
#include "vantage/abi/key.h"
#include <vantage/user.h>

static Terminal terminal;

static uint32_t info(void* context, ConsoleInfo* out) {
    (void)context;
    return sys_console_info(out);
}

static void output(void* context, char character) {
    (void)context;
    sys_console_write(&character, 1);
}

void terminal_app_init(void) {
    ConsoleInfo settings;
    uint32_t columns, rows;
    sys_canvas_set_color(RGB(16, 20, 28));
    sys_canvas_clear();

    const char *boot_msg = "[USER] terminal_app_init reached\n";
    sys_console_write(boot_msg, 33);    
    
    if (!sys_console_info(&settings)) {
        columns = 60;
        rows = 50;
    } else {
        columns = settings.columns < TEXT_MAX_COLUMNS ? settings.columns : TEXT_MAX_COLUMNS;
        rows = settings.rows < TERMINAL_MAX_ROWS ? settings.rows : TERMINAL_MAX_ROWS;
    }

    keyboard_columns = 0;
    keyboard_shift = keyboard_full_dirty = 0;
    keyboard_dirty_a = keyboard_dirty_b = NO_KEY;
    
    for (uint32_t i = 0; i < MAX_TOUCH_POINTS; i++) {
        point_key[i] = NO_KEY;
    }
    
    for (uint32_t i = 0; i < KEYBOARD_KEYS; i++) {
        keyboard_pressed[i] = 0;
    }
    
    if (columns >= 50 && rows >= KEYBOARD_ROWS + 4) {
        keyboard_columns = columns;
        keyboard_top = rows - KEYBOARD_ROWS;
        keyboard_full_dirty = 1;
        rows = keyboard_top;
    }

    if (!terminal_init(&terminal, columns, rows, info, output, NULL)) {
        const char *msg = "[ERR] Failed to init terminal\n";
        sys_console_write(msg, 29);
        return;
    }
    
    terminal_net_startup();
}

void terminal_app_step(void) {
    KeyEvent key;
    TouchEvent touch;
    uint32_t count, row, col, changed = 0;
    for (count = 0; count < 64 && sys_key_read(&key); count++) terminal_key(&terminal, &key);
    for (count = 0; count < 64 && sys_touch_read(&touch); count++) {
        terminal_touch(&terminal, &touch);
        keyboard_touch(&touch, NULL);
    }
    terminal_job_step(&terminal);
    KeyEvent buffered_key;
    while (keyboard_buf_pop(&buffered_key)) {
        terminal_key(&terminal, &buffered_key);
    }
    if (keyboard_pressed[37]) {
        uint32_t now = sys_time_us();
        if (now - keyboard_repeat_time >= keyboard_repeat_delay) {
            const KeyEvent backspace = {KEY_BACKSPACE, 0};
            terminal_key(&terminal, &backspace);
            keyboard_repeat_time = now;
            keyboard_repeat_delay = 70000;
        }
    }
    if (keyboard_pressed[40]) {
        uint32_t now = sys_time_us();
        if (now - keyboard_repeat_time >= keyboard_repeat_delay) {
            const KeyEvent space = {KEY_TEXT, ' '};
            terminal_key(&terminal, &space);
            keyboard_repeat_time = now;
            keyboard_repeat_delay = 70000;
        }
    }
    for (row = 0; row < terminal.rows; row++) {
        TextRow text;
        if (!terminal.dirty[row]) continue;
        text.row = row;
        text.length = terminal.columns;
        text.foreground = RGB(220, 230, 238);
        text.background = RGB(16, 20, 28);
        if (terminal.editing) {
            if (terminal_editor_selected(&terminal, row)) text.background = RGB(35, 63, 91);
            else if (row >= terminal.rows - 6) text.background = RGB(49, 61, 80);
        }
        text.cursor = !terminal.editing && row == terminal.row && terminal.column < terminal.columns ? terminal.column : TEXT_NO_CURSOR;
        for (col = 0; col < TEXT_MAX_COLUMNS; col++) text.text[col] = col < terminal.columns ? terminal.cells[row][col] : ' ';
        if (sys_canvas_text_row(&text)) {
            terminal.dirty[row] = 0;
            changed = 1;
        }
    }
    if ((keyboard_full_dirty || keyboard_dirty_a != NO_KEY || keyboard_dirty_b != NO_KEY) && keyboard_draw()) {
        keyboard_full_dirty = 0;
        keyboard_dirty_a = keyboard_dirty_b = NO_KEY;
        changed = 1;
    }
    if (changed) sys_canvas_present();
}

void user_task(void) {
    sys_console_write("[USER] Entered user_task\n", 25);
    terminal_app_init();
    for (;;) {
        terminal_app_step();
        sys_frame_wait();
    }
}
