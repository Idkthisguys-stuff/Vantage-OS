#pragma once
#include "std.h"

/* Trusted, cooperative EL1 tasks, owned by core 0 with interrupts masked. */
#define TASK_LIMIT 8U
#define TASK_STACK_BYTES 16384U
typedef uint64_t TaskId;
typedef void (*TaskEntry)(void* argument);

enum TaskResult {
    TASK_OK = 1,
    TASK_INVALID = 0,
    TASK_PENDING = -1,
    TASK_BUSY = -2,
    TASK_DEADLOCK = -3
};

enum TaskState {
    TASK_FREE, TASK_RUNNABLE, TASK_RUNNING, TASK_BLOCKED, TASK_EXITED
};

int task_init(void);
TaskId task_create(TaskEntry entry, void* argument);
TaskId task_current(void);
int task_yield(void);
void task_exit(int status);
/* Join is for running tasks; reap is nonblocking and also works from boot. */
int task_join(TaskId id, int* status);
int task_reap(TaskId id, int* status);
int task_state(TaskId id, enum TaskState* state);
/* Dispatch until no task is runnable. Returns PENDING for blocked tasks. */
int task_run(void);
int task_self_test(void);
