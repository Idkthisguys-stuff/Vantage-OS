#include "user/apps/terminal/terminal.h"
#include <vantage/user.h>
#include <vantage/kernel/files.h>
#include "../platform/storage_host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
static struct { unsigned char before[32]; Terminal terminal; unsigned char after[32]; } guarded;
static char transcript[65536];
static size_t written;

static void output(void* context, char c) {
    (void)context;
    if (written + 1 < sizeof(transcript)) {
        transcript[written++] = c; transcript[written] = 0;
    }
}
static void command(const char* line) {
    while (*line) {
        KeyEvent event = {KEY_TEXT, (unsigned char)*line++};
        terminal_key(&guarded.terminal, &event);
    }
    KeyEvent enter = {KEY_ENTER, 0};
    terminal_key(&guarded.terminal, &enter);
    for (unsigned int i = 0; i < 32; i++) CHECK(guarded.before[i] == 0xa7 && guarded.after[i] == 0xa7);
}
static void contents(const char* expected) {
    unsigned char data[FILE_IO_MAX];
    int64_t handle = sys_file_open("/ram/notes", FILE_READ);
    CHECK(handle > 0);
    int count = sys_file_read((uint64_t)handle, data, sizeof(data));
    CHECK(count == (int)strlen(expected) && !memcmp(data, expected, (size_t)count));
    CHECK(sys_file_close((uint64_t)handle) == 0);
}

static void action(uint32_t which) { terminal_editor_action(&guarded.terminal, which); }
static void close_editor(void) {
    action(EDIT_EXIT);
    if (guarded.terminal.editing) action(EDIT_EXIT);
}
static void clear_document(void) {
    while (guarded.terminal.document_size) action(EDIT_DROP);
}
static void text(const char* line) {
    while (*line) {
        KeyEvent event = {KEY_TEXT, (unsigned char)*line++};
        terminal_key(&guarded.terminal, &event);
    }
}
static void tap(int x, int y) {
    TouchEvent e = {0};
    e.type = TOUCH_EVENT_DOWN; e.x = x; e.y = y;
    terminal_touch(&guarded.terminal, &e);
    e.type = TOUCH_EVENT_UP;
    terminal_touch(&guarded.terminal, &e);
}

int main(int argc, char** argv) {
    CHECK(argc == 3);
    memset(&guarded, 0xa7, sizeof(guarded));
    Terminal* t = &guarded.terminal;
    CHECK(terminal_init(t, 60, 33, NULL, output, NULL));
    CHECK(sim_storage_open(argv[2], 0) >= 0);
    if (!strcmp(argv[1], "capacity")) {
        int64_t h = sys_file_open("/ram/large", FILE_READ | FILE_WRITE | FILE_CREATE);
        char chunk[FILE_IO_MAX]; memset(chunk, 'a', sizeof(chunk));
        CHECK(h > 0);
        for (unsigned int i = 0; i < 16; i++) CHECK(sys_file_write((uint64_t)h, chunk, sizeof(chunk)) == sizeof(chunk));
        CHECK(sys_file_write((uint64_t)h, "z", 1) == 1 && sys_file_close((uint64_t)h) == 0);
        command("edit large"); CHECK(!t->editing && strstr(transcript, "Editor limit is 4096 bytes"));
        FileDirectoryLong stat = {0}; memcpy(stat.path, "/ram/large", 11);
        CHECK(sys_file_directory_long(&stat) == 0 && stat.size == 4097);
        command("cat large"); CHECK(strstr(transcript, "aaaaz")); command("rm large");
        char path[FILE_PATH_MAX] = "/ram", line[TERMINAL_COMMAND_SIZE]; unsigned int length = 4;
        for (unsigned int depth = 0; depth < 7; depth++) {
            path[length++] = '/'; memset(path + length, (int)('a' + depth), 31); length += 31; path[length] = 0;
            snprintf(line, sizeof(line), "mkdir %s", path); command(line);
        }
        path[length++] = '/'; memset(path + length, 'z', 26); length += 26; path[length] = 0;
        CHECK(length == 255);
        snprintf(line, sizeof(line), "edit %s", path); command(line); CHECK(t->editing);
        text("long path"); action(EDIT_SAVE); close_editor();
        snprintf(line, sizeof(line), "cat %s", path); command(line); CHECK(strstr(transcript, "long path"));
        snprintf(line, sizeof(line), "rm %s", path); command(line);
        snprintf(line, sizeof(line), "mkdir %s", path); command(line);
        snprintf(line, sizeof(line), "cd %s", path); command(line); CHECK(!strcmp(t->cwd, path));
        command("cd ."); CHECK(!strcmp(t->cwd, path));
        command("cd .."); CHECK(strlen(t->cwd) == 228);
        snprintf(line, sizeof(line), "rmdir %s", path); command(line);
        while (length > 4) {
            while (length > 4 && path[length - 1] != '/') length--;
            if (length > 4) length--;
            path[length] = 0;
            if (length > 4) { snprintf(line, sizeof(line), "rmdir %s", path); command(line); }
        }
        command("cd /ram"); CHECK(!strcmp(t->cwd, "/ram"));
    } else if (!strcmp(argv[1], "write")) {
        command("pwd"); CHECK(!strcmp(t->cwd, "/ram"));
        command("mkdir empty"); command("mkdir docs"); command("cd docs");
        CHECK(!strcmp(t->cwd, "/ram/docs"));
        command("mkdir sub"); command("cd sub"); command("cd ..");
        CHECK(!strcmp(t->cwd, "/ram/docs"));
        command("edit nested"); CHECK(t->editing); text("nested text"); action(EDIT_SAVE); close_editor();
        command("cd ../.."); CHECK(!strcmp(t->cwd, "/ram"));
        command("edit notes"); CHECK(t->editing && !t->document_size);
        command("hello storage"); command("second line"); command(".save");
        action(EDIT_SAVE); CHECK(strstr(transcript, "Saved."));
        contents("hello storage\nsecond line\n.save\n");
        close_editor(); CHECK(!t->editing);
    } else if (!strcmp(argv[1], "read")) {
        command("ls"); CHECK(strstr(transcript, "/ram/empty/"));
        command("cd docs"); CHECK(!strcmp(t->cwd, "/ram/docs"));
        command("cat ./nested"); CHECK(strstr(transcript, "nested text"));
        command("rmdir ."); CHECK(strstr(transcript, "File is busy"));
        command("cd nested"); CHECK(!strcmp(t->cwd, "/ram/docs"));
        command("rm nested"); command("rmdir sub"); command("cd /ram");
        command("rmdir docs"); command("rmdir empty"); command("cd ..");
        CHECK(!strcmp(t->cwd, "/ram"));
        contents("hello storage\nsecond line\n.save\n");
        command("ls"); CHECK(strstr(transcript, "/ram/notes"));
        command("cat notes"); CHECK(strstr(transcript, "hello storage\nsecond line\n.save\n"));
        command("edit /ram/notes"); CHECK(t->editing);
        /* Tap the second line and insert in its middle; no pointer is drawn. */
        tap(8 * TEXT_CELL_WIDTH, 4 * TEXT_CELL_HEIGHT);
        CHECK(t->edit_position == 16 && terminal_editor_selected(t, 4));
        text("!"); CHECK(!memcmp(t->document + 14, "se!cond", 7));
        KeyEvent backspace = {KEY_BACKSPACE, 0}; terminal_key(t, &backspace);
        action(EDIT_UP); CHECK(t->edit_position == 2);
        action(EDIT_LEFT); action(EDIT_RIGHT); CHECK(t->edit_position == 2);
        action(EDIT_DOWN); action(EDIT_NEW); text("new line");
        CHECK(!memcmp(t->document + 26, "new line\n.save", 14));
        action(EDIT_DROP);
        clear_document(); CHECK(!t->document_size);
        command("replacement"); action(EDIT_SAVE); contents("replacement\n");
        clear_document(); command("unsaved");
        action(EDIT_EXIT); CHECK(t->editing && t->edit_confirm);
        action(EDIT_EXIT); CHECK(!t->editing); contents("replacement\n");
        command("edit notes"); clear_document();
        /* Fill across syscall chunks and reach the separate editor capacity. */
        char line[128]; memset(line, 'a', 127); line[127] = 0;
        for (unsigned int i = 0; i < 32; i++) command(line);
        CHECK(t->document_size == TERMINAL_DOCUMENT_CAPACITY);
        command("overflow"); CHECK(t->document_size == TERMINAL_DOCUMENT_CAPACITY);
        action(EDIT_SAVE); close_editor(); command("edit notes");
        CHECK(t->document_size == TERMINAL_DOCUMENT_CAPACITY);
        for (unsigned int i = 0; i < TERMINAL_DOCUMENT_CAPACITY; i++) CHECK(t->document[i] == (i % 128 == 127 ? '\n' : 'a'));
        CHECK(t->edit_top > 0 || t->edit_position == 0);
        /* A single full-length line is editable too; shell limits do not apply. */
        clear_document();
        for (unsigned int i = 0; i < 32; i++) text(line);
        text("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        CHECK(t->document_size == TERMINAL_DOCUMENT_CAPACITY && t->edit_left > 0);
        action(EDIT_NEW); CHECK(t->document_size == TERMINAL_DOCUMENT_CAPACITY);
        terminal_key(t, &backspace); text("z");
        action(EDIT_SAVE); close_editor(); command("edit notes");
        CHECK(t->document_size == TERMINAL_DOCUMENT_CAPACITY && t->document[4095] == 'z');
        for (unsigned int i = 0; i < TERMINAL_DOCUMENT_CAPACITY - 1; i++) CHECK(t->document[i] == 'a');
        clear_document(); action(EDIT_SAVE); contents("");
        close_editor(); command("rm notes");
        CHECK(sys_file_open("/ram/notes", FILE_READ) == FILE_NOT_FOUND);
        command("edit temporary"); command("discard"); close_editor();
        CHECK(sys_file_open("/ram/temporary", FILE_READ) == FILE_NOT_FOUND);
        command("edit delete-me"); command("data"); action(EDIT_SAVE); close_editor(); command("rm delete-me");
        CHECK(!t->editing && sys_file_open("/ram/delete-me", FILE_READ) == FILE_NOT_FOUND);
        command("edit missing/../invalid");
        CHECK(!t->editing); /* Missing parents cannot be bypassed with ... */
        int64_t binary = sys_file_open("/ram/binary", FILE_WRITE | FILE_CREATE);
        CHECK(binary > 0 && sys_file_write((uint64_t)binary, "\x1b[2J", 4) == 4);
        CHECK(sys_file_close((uint64_t)binary) == 0);
        command("edit binary"); CHECK(!t->editing && strstr(transcript, "Not an ASCII text file"));
        command("cat binary"); CHECK(strstr(transcript, "?[2J"));
        command("rm binary");
    } else {
        CHECK(!strcmp(argv[1], "deleted"));
        CHECK(sys_file_open("/ram/notes", FILE_READ) == FILE_NOT_FOUND);
        command("cat notes"); CHECK(strstr(transcript, "File not found"));
        command("edit notes"); command("keep this buffer");
        CHECK(t->editing && t->document_size == 17);
        CHECK(files_unmount() == 0);
        action(EDIT_SAVE); CHECK(t->editing && t->document_size == 17);
        CHECK(strstr(t->edit_status, "Save failed"));
        CHECK(!memcmp(t->document, "keep this buffer\n", 17));
        close_editor();
        command("edit notes"); CHECK(!t->editing && strstr(transcript, "Storage unavailable"));
        puts("PASS: editor create/read/replace/delete persists across fresh process mounts");
        return 0;
    }
    CHECK(files_unmount() == 0);
    return 0;
}
