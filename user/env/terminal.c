#include "../include/terminal.h"
#include "../include/user_lib.h"

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

static void file_error(Terminal* t, int error) {
    const char* message = "File operation failed";
    if (error == FILE_UNAVAILABLE) message = "Storage unavailable (not mounted)";
    else if (error == FILE_NOT_FOUND) message = "File not found";
    else if (error == FILE_BAD) message = "Invalid filename or request";
    else if (error == FILE_NO_SPACE) message = "Storage or file capacity exhausted";
    else if (error == FILE_BUSY) message = "File is busy";
    else if (error == FILE_IO_ERROR) message = "Storage I/O failed; remount required";
    else if (error == FILE_CORRUPT) message = "Storage is corrupt";
    write(t, message);
    write(t, ".\n");
}

static int file_path(const char* argument, char path[FILE_PATH_MAX]) {
    uint32_t length = 0;
    while (*argument == ' ') argument++;
    if (!*argument) return 0;
    if (*argument != '/') {
        const char* prefix = "/ram/";
        while (*prefix) path[length++] = *prefix++;
    }
    while (*argument && *argument != ' ') {
        if (length == FILE_PATH_MAX - 1) return 0;
        path[length++] = *argument++;
    }
    while (*argument == ' ') argument++;
    path[length] = 0;
    return !*argument;
}

static void show_bytes(Terminal* t, const unsigned char* data, uint32_t length) {
    for (uint32_t i = 0; i < length; i++) {
        unsigned char c = data[i];
        /* Never send stored control/escape sequences to the serial terminal. */
        put(t, c == '\n' || (c >= 32 && c <= 126) ? (char)c : '?');
    }
}

static void list_files(Terminal* t) {
    FileEntry entry = {0};
    int result;
    while ((result = sys_file_list(&entry)) > 0) {
        write(t, entry.path); write(t, "  "); number(t, entry.size); write(t, " bytes\n");
    }
    if (result < 0) file_error(t, result);
}

static void open_text(Terminal* t, const char* path, int edit) {
    int64_t handle = sys_file_open(path, FILE_READ);
    uint32_t used = 0;
    int result = 0;
    if (handle < 0 && !(edit && handle == FILE_NOT_FOUND)) {
        file_error(t, (int)handle); return;
    }
    if (handle >= 0) {
        unsigned char chunk[FILE_IO_MAX];
        while ((result = sys_file_read((uint64_t)handle, chunk, sizeof(chunk))) > 0) {
            if (!edit) { show_bytes(t, chunk, (uint32_t)result); continue; }
            if ((uint32_t)result > FILE_CAPACITY - used) { result = FILE_NO_SPACE; break; }
            uint32_t i;
            for (i = 0; i < (uint32_t)result; i++) {
                unsigned char c = chunk[i];
                if (c != '\n' && c != '\r' && c != '\t' && (c < 32 || c > 126)) break;
            }
            if (i != (uint32_t)result) {
                write(t, "Not an ASCII text file; left unchanged.\n");
                (void)sys_file_close((uint64_t)handle); return;
            }
            memcpy(t->document + used, chunk, (uint32_t)result);
            used += (uint32_t)result;
        }
        int closed = sys_file_close((uint64_t)handle);
        if (result < 0 || closed < 0) { file_error(t, result < 0 ? result : closed); return; }
    }
    if (!edit) { put(t, '\n'); return; }
    for (uint32_t i = 0; i < FILE_PATH_MAX; i++) {
        t->document_path[i] = path[i];
        if (!path[i]) break;
    }
    t->document_size = used;
    t->editing = 1;
    write(t, "Editing "); write(t, path);
    write(t, "\nLines append. .show .clear .save .delete .quit\n");
    show_bytes(t, t->document, used);
    if (used && t->document[used - 1] != '\n') put(t, '\n');
}

static void edit_line(Terminal* t, const char* line) {
    if (line[0] == '.' && line[1] != '.') {
        if (command_is(line, ".quit") && !line[5]) {
            t->editing = 0; write(t, "Editor closed; unsaved changes discarded.\n");
        } else if (command_is(line, ".show") && !line[5]) {
            show_bytes(t, t->document, t->document_size); put(t, '\n');
        } else if (command_is(line, ".clear") && !line[6]) {
            t->document_size = 0; write(t, "Buffer cleared; .save to replace file.\n");
        } else if (command_is(line, ".delete") && !line[7]) {
            int result = sys_file_remove(t->document_path);
            if (result < 0) file_error(t, result);
            else { t->editing = 0; write(t, "File deleted.\n"); }
        } else if (command_is(line, ".save") && !line[5]) {
            int64_t handle = sys_file_open(t->document_path, FILE_WRITE | FILE_CREATE | FILE_TRUNCATE);
            if (handle < 0) { file_error(t, (int)handle); return; }
            uint32_t offset = 0;
            int result = 0;
            while (offset < t->document_size) {
                uint32_t count = t->document_size - offset;
                if (count > FILE_IO_MAX) count = FILE_IO_MAX;
                result = sys_file_write((uint64_t)handle, t->document + offset, count);
                if (result != (int)count) { if (result >= 0) result = FILE_IO_ERROR; break; }
                offset += count;
            }
            int closed = sys_file_close((uint64_t)handle);
            if (result < 0 || closed < 0) {
                file_error(t, result < 0 ? result : closed);
                write(t, "Buffer retained; disk file may be incomplete.\n");
            } else write(t, "Saved.\n");
        } else write(t, ".show .clear .save .delete .quit; .. escapes a leading dot.\n");
        return;
    }
    if (line[0] == '.' && line[1] == '.') line++;
    uint32_t length = 0;
    while (line[length]) length++;
    if (length + 1 > FILE_CAPACITY - t->document_size) {
        write(t, "Buffer full (4096 bytes); line not added.\n"); return;
    }
    memcpy(t->document + t->document_size, line, length);
    t->document_size += length;
    t->document[t->document_size++] = '\n';
}

static void execute(Terminal* t) {
    const char* command = t->command;
    if (t->editing) { edit_line(t, command); return; }
    while (*command == ' ') command++;
    if (!*command) return;
    if (command_is(command, "help")) {
        write(t, "help   Show commands\nclear  Clear the terminal\nmem    Memory statistics\necho   Print text\nls     List files\ncat    Read a file\nedit   Open text editor\nrm     Delete a file\nabout  About this shell\n");
    } else if (command_is(command, "ls")) list_files(t);
    else if (command_is(command, "cat") || command_is(command, "edit") || command_is(command, "rm")) {
        char path[FILE_PATH_MAX];
        int edit = command_is(command, "edit"), remove = command_is(command, "rm");
        if (!file_path(command + (edit ? 4 : remove ? 2 : 3), path)) {
            write(t, "Usage: cat/edit/rm filename (or /ram/filename).\n"); return;
        }
        if (remove) {
            int result = sys_file_remove(path);
            if (result < 0) file_error(t, result); else write(t, "File deleted.\n");
        } else open_text(t, path, edit);
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
        write(t, "Vantage-OS built-in shell.\nASCII text editor; files require mounted storage.\n");
    } else write(t, "Unknown command. Type 'help'.\n");
}

uint32_t terminal_init(Terminal* t, uint32_t columns, uint32_t rows,
                       TerminalInfoFn info, TerminalOutputFn output, void* context) {
    if (!t || columns < 16 || columns > TEXT_MAX_COLUMNS || rows < 4 || rows > TERMINAL_MAX_ROWS) return 0;
    t->columns = columns;
    t->rows = rows;
    t->length = t->overflow = 0;
    t->editing = t->document_size = 0;
    t->command[0] = 0;
    t->input_limit = columns * (rows - 1) - 6; /* Reserve the longer edit prompt. */
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
        write(t, t->editing ? "edit> " : "> ");
    } else if (event->type == KEY_CLEAR) {
        uint32_t i;
        blank(t);
        mirror(t, '\x1b'); mirror(t, '['); mirror(t, '2'); mirror(t, 'J');
        mirror(t, '\x1b'); mirror(t, '['); mirror(t, 'H');
        write(t, t->editing ? "edit> " : "> ");
        for (i = 0; i < t->length; i++) put(t, t->command[i]);
    }
    t->dirty[t->row] = 1;
}
