#include "../include/terminal.h"

static void mirror(Terminal* t, char c) {
    if (t->output) t->output(t->context, c);
}

static void blank(Terminal* t) {
    uint32_t row, col;
    for (row = 0; row < t->rows; row++) {
        for (col = 0; col < t->columns; col++) t->cells[row][col] = ' ';
        t->dirty[row] = 1;
    }
    t->row = t->column = 0;
}

static void newline(Terminal* t) {
    uint32_t row, col;
    t->dirty[t->row] = 1;
    t->column = 0;
    if (++t->row == t->rows) {
        for (row = 1; row < t->rows; row++) {
            for (col = 0; col < t->columns; col++) t->cells[row - 1][col] = t->cells[row][col];
        }
        t->row--;
        for (col = 0; col < t->columns; col++) t->cells[t->row][col] = ' ';
        for (row = 0; row < t->rows; row++) t->dirty[row] = 1;
    }
    t->dirty[t->row] = 1;
}

static void put(Terminal* t, char c) {
    mirror(t, c);
    if (c == '\n') {
        newline(t);
        return;
    }
    if (t->column == t->columns) newline(t);
    t->cells[t->row][t->column++] = c;
    t->dirty[t->row] = 1;
}

static void write(Terminal* t, const char* text) {
    while (*text) put(t, *text++);
}

static void number(Terminal* t, uint64_t value) {
    char digits[20];
    uint32_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (count) put(t, digits[--count]);
}

static uint32_t command_is(const char* command, const char* word) {
    while (*word && *command == *word) { command++; word++; }
    return !*word && (!*command || *command == ' ');
}

static void memory(Terminal* t) {
    ConsoleInfo info = {0};
    if (!t->info || !t->info(t->context, &info)) {
        write(t, "Memory statistics unavailable.\n");
    } else if (info.memory_kind == CONSOLE_MEMORY_HOST) {
        write(t, "Simulator: host-managed memory.\nBack buffer: ");
        number(t, info.host_backbuffer_bytes);
        write(t, " bytes\nKernel MM is tested separately.\n");
    } else if (info.memory_kind == CONSOLE_MEMORY_KERNEL) {
        write(t, "RAM: "); number(t, info.memory.ram_bytes / (1024 * 1024));
        write(t, " MiB\nFree pages: "); number(t, info.memory.free_pages);
        put(t, '/'); number(t, info.memory.allocatable_pages);
        write(t, "\nHeap used: "); number(t, info.memory.heap_used_bytes);
        put(t, '/'); number(t, info.memory.heap_bytes);
        write(t, " bytes\n");
    } else write(t, "Memory statistics unavailable.\n");
}

static void execute(Terminal* t) {
    const char* command = t->command;
    while (*command == ' ') command++;
    if (!*command) return;
    if (command_is(command, "help")) {
        write(t, "help   Show commands\nclear  Clear the terminal\nmem    Memory statistics\necho   Print text\nabout  About this shell\n");
    } else if (command_is(command, "clear")) {
        blank(t);
        mirror(t, '\x1b');
        mirror(t, '['); mirror(t, '2'); mirror(t, 'J');
        mirror(t, '\x1b'); mirror(t, '['); mirror(t, 'H');
    } else if (command_is(command, "mem")) memory(t);
    else if (command_is(command, "echo")) {
        command += 4;
        while (*command == ' ') command++;
        write(t, command);
        put(t, '\n');
    } else if (command_is(command, "about")) {
        write(t, "Vantage-OS built-in shell.\nASCII input; no executable or file commands yet.\n");
    } else write(t, "Unknown command. Type 'help'.\n");
}

uint32_t terminal_init(Terminal* t, uint32_t columns, uint32_t rows,
                       TerminalInfoFn info, TerminalOutputFn output, void* context) {
    if (!t || columns < 16 || columns > TEXT_MAX_COLUMNS || rows < 4 || rows > TERMINAL_MAX_ROWS) return 0;
    t->columns = columns;
    t->rows = rows;
    t->length = t->overflow = 0;
    t->command[0] = 0;
    t->input_limit = columns * (rows - 1) - 2;
    if (t->input_limit >= TERMINAL_COMMAND_SIZE) t->input_limit = TERMINAL_COMMAND_SIZE - 1;
    t->info = info;
    t->output = output;
    t->context = context;
    blank(t);
    write(t, "Vantage terminal\nType 'help' for commands.\n\n> ");
    return 1;
}

void terminal_key(Terminal* t, const KeyEvent* event) {
    if (!t || !event || !t->columns || !t->rows) return;
    t->dirty[t->row] = 1;
    if (event->type == KEY_TEXT && event->character >= 32 && event->character <= 126) {
        if (t->length < t->input_limit && !t->overflow) {
            char c = (char)event->character;
            t->command[t->length++] = c;
            t->command[t->length] = 0;
            put(t, c);
        } else t->overflow = 1;
    } else if (event->type == KEY_BACKSPACE && t->length && !t->overflow) {
        t->command[--t->length] = 0;
        if (!t->column && t->row) {
            t->row--;
            t->column = t->columns;
        }
        if (t->column) t->cells[t->row][--t->column] = ' ';
        mirror(t, '\b'); mirror(t, ' '); mirror(t, '\b');
    } else if (event->type == KEY_ENTER || event->type == KEY_CANCEL) {
        if (event->type == KEY_CANCEL) write(t, "^C");
        put(t, '\n');
        if (event->type == KEY_ENTER) {
            if (t->overflow) write(t, "Input too long; command discarded.\n");
            else execute(t);
        }
        t->length = t->overflow = 0;
        t->command[0] = 0;
        write(t, "> ");
    } else if (event->type == KEY_CLEAR) {
        uint32_t i;
        blank(t);
        mirror(t, '\x1b'); mirror(t, '['); mirror(t, '2'); mirror(t, 'J');
        mirror(t, '\x1b'); mirror(t, '['); mirror(t, 'H');
        write(t, "> ");
        for (i = 0; i < t->length; i++) put(t, t->command[i]);
    }
    t->dirty[t->row] = 1;
}
