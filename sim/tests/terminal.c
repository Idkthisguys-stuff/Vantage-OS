#include "../../user/include/terminal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static struct { unsigned char before[64]; Terminal terminal; unsigned char after[64]; } guarded;
static char transcript[32768];
static size_t written;
static uint32_t info_kind = CONSOLE_MEMORY_KERNEL;

uint64_t handle_sys_call(uint64_t id, uint64_t argument) {
    (void)id; (void)argument;
    return (uint64_t)(int64_t)FILE_UNAVAILABLE;
}

static void output(void* context, char c) {
    CHECK(context == &guarded);
    if (written + 1 < sizeof(transcript)) {
        transcript[written++] = c;
        transcript[written] = 0;
    }
}

static uint32_t info(void* context, ConsoleInfo* out) {
    CHECK(context == &guarded);
    memset(out, 0, sizeof(*out));
    out->memory_kind = info_kind;
    out->host_backbuffer_bytes = 1536000;
    out->memory.ram_bytes = 128 * 1024 * 1024;
    out->memory.free_pages = 123;
    out->memory.allocatable_pages = 456;
    out->memory.heap_used_bytes = ~(uint64_t)0;
    out->memory.heap_bytes = 8 * 1024 * 1024;
    return 1;
}

static void reset(uint32_t cols, uint32_t rows) {
    memset(&guarded, 0xa7, sizeof(guarded));
    written = 0; transcript[0] = 0;
    CHECK(terminal_init(&guarded.terminal, cols, rows, info, output, &guarded));
}

static void key(uint32_t type, uint32_t character) {
    KeyEvent event = {type, character};
    terminal_key(&guarded.terminal, &event);
}

static void text(const char* string) {
    while (*string) key(KEY_TEXT, (unsigned char)*string++);
}

static void command(const char* string) {
    text(string);
    key(KEY_ENTER, 0);
}

static void guards(void) {
    size_t i;
    for (i = 0; i < 64; i++) CHECK(guarded.before[i] == 0xa7 && guarded.after[i] == 0xa7);
    CHECK(guarded.terminal.row < guarded.terminal.rows);
    CHECK(guarded.terminal.column <= guarded.terminal.columns);
    CHECK(guarded.terminal.length <= guarded.terminal.input_limit);
    CHECK(!guarded.terminal.command[guarded.terminal.length]);
}

int main(void) {
    Terminal* t = &guarded.terminal;
    uint32_t i;
    CHECK(!terminal_init(NULL, 60, 50, NULL, NULL, NULL));
    CHECK(!terminal_init(t, 0, 50, NULL, NULL, NULL));
    CHECK(!terminal_init(t, 60, 0, NULL, NULL, NULL));
    CHECK(!terminal_init(t, TEXT_MAX_COLUMNS + 1, 50, NULL, NULL, NULL));
    CHECK(!terminal_init(t, 60, TERMINAL_MAX_ROWS + 1, NULL, NULL, NULL));
    reset(60, 50);
    CHECK(t->row == 3 && t->column == 2 && strstr(transcript, "Vantage terminal"));
    text("helx"); key(KEY_BACKSPACE, 0); text("p"); key(KEY_ENTER, 0);
    CHECK(strstr(transcript, "Show commands"));
    command("mem");
    CHECK(strstr(transcript, "RAM: 128 MiB") && strstr(transcript, "Free pages: 123/456"));
    CHECK(strstr(transcript, "18446744073709551615/8388608"));
    info_kind = CONSOLE_MEMORY_HOST;
    command("mem");
    CHECK(strstr(transcript, "Simulator: host-managed memory.") && strstr(transcript, "1536000 bytes"));
    command("  echo hello terminal  ");
    CHECK(strstr(transcript, "\nhello terminal  \n"));
    command("unknown"); CHECK(strstr(transcript, "Unknown command"));
    command("about"); CHECK(strstr(transcript, "built-in shell"));
    command("edit notes"); CHECK(!t->editing && strstr(transcript, "Storage unavailable"));
    command("clear");
    CHECK(!t->row && t->column == 2 && t->cells[0][0] == '>');
    for (i = 2; i < t->columns; i++) CHECK(t->cells[0][i] == ' ');
    key(KEY_BACKSPACE, 0); CHECK(t->column == 2); /* Cannot erase the prompt. */
    text("echo keep"); key(KEY_CLEAR, 0);
    CHECK(!strcmp(t->command, "echo keep") && !t->row && t->column == 11);
    key(KEY_CANCEL, 0); CHECK(!t->length && strstr(transcript, "^C\n> "));
    key(KEY_TEXT, 0x100); key(KEY_TEXT, 0); CHECK(!t->length);

    reset(16, 4);
    text("echo 123456789012345");
    for (i = 0; i < 15; i++) key(KEY_BACKSPACE, 0);
    CHECK(!strcmp(t->command, "echo "));
    text("OK"); key(KEY_ENTER, 0); CHECK(strstr(transcript, "\nOK\n"));
    text("clear");
    for (i = 0; i < 200; i++) key(KEY_TEXT, ' ');
    key(KEY_ENTER, 0);
    CHECK(strstr(transcript, "command discarded"));
    guards();
    for (i = 0; i < 200; i++) command("echo scroll");
    CHECK(t->row == t->rows - 1);
    guards();

    /* Deterministic mixed input exercises wraps, scrolling and bounded editing. */
    { uint32_t seed = 17;
      for (i = 0; i < 20000; i++) {
          seed = seed * 1664525U + 1013904223U;
          if (seed % 13 == 0) key(KEY_ENTER, 0);
          else if (seed % 17 == 0) key(KEY_CLEAR, 0);
          else if (seed % 7 == 0) key(KEY_BACKSPACE, 0);
          else if (seed % 19 == 0) key(KEY_CANCEL, 0);
          else key(KEY_TEXT, 'a' + seed % 26);
          guards();
      }
    }
    puts("PASS: shell commands, memory source labels, editing, wrapping, scrolling and input bounds");
    return 0;
}
