#include "keyboard.h"
#include "terminal.h"
#include <vantage/abi/graphics.h>
#include <vantage/abi/key.h>
#include <vantage/user.h>

static Terminal terminal;
static uint32_t editor_contact;
static uint32_t header_rows, header_contact, closed, opened;

static const CanvasButton home_button = {
    .x = 12, .y = 4, .width = 82, .height = 24, .radius = 6,
    .foreground = RGB(233, 245, 244), .background = RGB(33, 76, 74),
    .label = "Home"
};

static uint32_t in_header(const TouchEvent* touch) {
    return touch->x >= (int)home_button.x &&
           touch->x < (int)(home_button.x + home_button.width) &&
           touch->y >= (int)home_button.y &&
           touch->y < (int)(home_button.y + home_button.height);
}

static uint32_t info(void* context, ConsoleInfo* out) {
    (void)context;
    return sys_console_info(out);
}

static void output(void* context, char character) {
    (void)context;
    sys_console_write(&character, 1);
}

static void app_init(uint32_t reserved_rows) {
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

    editor_contact = header_contact = TOUCH_CONTACTS_MAX;
    header_rows = rows > reserved_rows + 4 ? reserved_rows : 0;
    rows = keyboard_init(columns, rows) - header_rows;

    if (!terminal_init(&terminal, columns, rows, info, output, NULL)) {
        const char *msg = "[ERR] Failed to init terminal\n";
        sys_console_write(msg, 29);
        return;
    }

}

void terminal_app_init(void) {
    app_init(0);
    terminal_net_startup();
}

void terminal_app_open(void) {
    closed = 0;
    editor_contact = header_contact = TOUCH_CONTACTS_MAX;
    if (!opened) {
        opened = 1;
        app_init(2);
        return;
    }
    sys_canvas_set_color(RGB(16, 20, 28));
    sys_canvas_clear();
    for (uint32_t row = 0; row < terminal.rows; row++) terminal.dirty[row] = 1;
    keyboard_redraw();
}

uint32_t terminal_app_closed(void) { return closed; }

static void close_app(void) {
    terminal.edit_touch = ~0U;
    keyboard_cancel_touches();
    closed = 1;
}

void terminal_app_step(void) {
    KeyEvent key;
    TouchContact contact;
    uint32_t count, row, col, changed = 0;
    for (count = 0; count < 64 && sys_key_read(&key); count++) {
        if (header_rows && key.type == KEY_HOME) { close_app(); return; }
        terminal_key(&terminal, &key);
    }
    for (count = 0; count < 64 && sys_touch_contact_read(&contact); count++) {
        if (contact.id >= TOUCH_CONTACTS_MAX) continue;
        const TouchEvent* touch = &contact.event;
        if (header_rows) {
            if (touch->type == TOUCH_EVENT_DOWN && in_header(touch)) header_contact = contact.id;
            if (contact.id == header_contact && touch->type != TOUCH_EVENT_DOWN) {
                if (touch->type == TOUCH_EVENT_UP && in_header(touch)) {
                    close_app();
                    return;
                }
                if (touch->type == TOUCH_EVENT_UP || !in_header(touch)) header_contact = TOUCH_CONTACTS_MAX;
            }
        }
        TouchEvent body_touch = *touch;
        body_touch.y -= (int)(header_rows * TEXT_CELL_HEIGHT);
        if (touch->type == TOUCH_EVENT_DOWN && editor_contact == TOUCH_CONTACTS_MAX &&
            (uint32_t)body_touch.y < terminal.rows * TEXT_CELL_HEIGHT) {
            editor_contact = contact.id;
        }
        if (contact.id == editor_contact) {
            terminal_touch(&terminal, &body_touch);
            if (touch->type == TOUCH_EVENT_UP) editor_contact = TOUCH_CONTACTS_MAX;
        }
        if (keyboard_touch(&contact, &key)) terminal_key(&terminal, &key);
    }
    terminal_job_step(&terminal);
    if (keyboard_repeat(&key)) terminal_key(&terminal, &key);
    for (row = 0; row < terminal.rows; row++) {
        TextRow text;
        if (!terminal.dirty[row]) continue;
        text.row = row + header_rows;
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
    if (keyboard_draw()) changed = 1;
    if (changed) {
        if (header_rows) sys_canvas_button(&home_button);
        sys_canvas_present();
    }
}
