#pragma once
#include "../../includes/console.h"

#define TERMINAL_MAX_ROWS 60U
#define TERMINAL_COMMAND_SIZE 128U

typedef uint32_t (*TerminalInfoFn)(void* context, ConsoleInfo* info);
typedef void (*TerminalOutputFn)(void* context, char character);

typedef struct {
    uint32_t columns, rows, column, row;
    uint32_t length, input_limit, overflow;
    char cells[TERMINAL_MAX_ROWS][TEXT_MAX_COLUMNS];
    char command[TERMINAL_COMMAND_SIZE];
    uint8_t dirty[TERMINAL_MAX_ROWS];
    TerminalInfoFn info;
    TerminalOutputFn output;
    void* context;
} Terminal;

uint32_t terminal_init(Terminal* terminal, uint32_t columns, uint32_t rows,
                       TerminalInfoFn info, TerminalOutputFn output, void* context);
void terminal_key(Terminal* terminal, const KeyEvent* event);

void terminal_app_init(void);
void terminal_app_step(void);
