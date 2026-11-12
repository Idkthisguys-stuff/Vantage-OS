#pragma once
#include <vantage/abi/console.h>
#include <vantage/abi/files.h>
#include <vantage/abi/net.h>
#include <vantage/abi/touch.h>

#define TERMINAL_MAX_ROWS 60U
#define TERMINAL_COMMAND_SIZE 288U
#define TERMINAL_DOCUMENT_CAPACITY 4096U
#define TERMINAL_JOB_STATE 1280U

typedef struct Terminal Terminal;
typedef uint32_t (*TerminalInfoFn)(void* context, ConsoleInfo* info);
typedef void (*TerminalOutputFn)(void* context, char character);
/* Receives the masked line; text is scrubbed after the call. */
typedef void (*TerminalSecretFn)(Terminal* terminal, void* state, const char* text);

/* A job runs across frames so the display and Cancel stay live; input is held
 * meanwhile. step runs once per frame. cancel (optional) must end the job. */
typedef struct {
    void (*step)(Terminal* terminal, void* state);
    void (*cancel)(Terminal* terminal, void* state);
} TerminalJobType;
typedef struct {
    const TerminalJobType* type;
    uint32_t started;
    uint64_t state[TERMINAL_JOB_STATE / 8];
} TerminalJob;

struct Terminal {
    uint32_t columns, rows, column, row;
    uint32_t length, input_limit, overflow;
    char cells[TERMINAL_MAX_ROWS][TEXT_MAX_COLUMNS];
    char command[TERMINAL_COMMAND_SIZE];
    uint8_t dirty[TERMINAL_MAX_ROWS];
    uint32_t editing, document_size;
    uint32_t edit_position, edit_top, edit_left, edit_modified, edit_confirm;
    uint32_t edit_touch;
    const char* edit_status;
    char document_path[FILE_PATH_MAX];
    char cwd[FILE_PATH_MAX];
    unsigned char document[TERMINAL_DOCUMENT_CAPACITY];
    TerminalJob job;
    TerminalSecretFn secret;           /* The input line is a passphrase. */
    TerminalInfoFn info;
    TerminalOutputFn output;
    void* context;
};

uint32_t terminal_init(Terminal* terminal, uint32_t columns, uint32_t rows,
                       TerminalInfoFn info, TerminalOutputFn output, void* context);
void terminal_key(Terminal* terminal, const KeyEvent* event);

enum { EDIT_SAVE, EDIT_EXIT, EDIT_UP, EDIT_DOWN, EDIT_LEFT, EDIT_RIGHT, EDIT_NEW, EDIT_DROP };
void terminal_editor_action(Terminal* terminal, uint32_t action);
void terminal_touch(Terminal* terminal, const TouchEvent* event);
uint32_t terminal_editor_selected(const Terminal* terminal, uint32_t row);

void terminal_app_init(void);
void terminal_app_step(void);

void terminal_write(Terminal* terminal, const char* text);
void terminal_put(Terminal* terminal, char character);
void terminal_number(Terminal* terminal, uint64_t value);
void terminal_prompt(Terminal* terminal);
const char* terminal_file_error_text(int error);
/* Resolves a path argument against the current directory; 0 if invalid. */
int terminal_file_path(Terminal* terminal, const char* argument, char path[FILE_PATH_MAX]);
void terminal_erase(void* memory, size_t bytes);

/* Returns zeroed job state; the job starts with terminal_job_run. */
void* terminal_job_state(Terminal* terminal);
void terminal_job_run(Terminal* terminal, const TerminalJobType* type);
void terminal_job_end(Terminal* terminal);
void terminal_job_step(Terminal* terminal);
void terminal_ask_passphrase(Terminal* terminal, TerminalSecretFn secret);

void terminal_net_command(Terminal* terminal, const char* arguments);
void terminal_net_startup(void);
