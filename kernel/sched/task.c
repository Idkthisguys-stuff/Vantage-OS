#include "../../includes/task_internal.h"

#define ID_BITS 4U
#define ID_MASK 15U
#define GUARD_BYTES 64U
#define GUARD_VALUE 0xa7U

#ifdef _MSC_VER
#define TASK_ALIGN __declspec(align(16))
#else
#define TASK_ALIGN __attribute__((aligned(16)))
#endif

typedef struct {
    TaskContext context;
    TaskEntry entry;
    void* argument;
    TaskId id, generation, waiting_for, waiter;
    enum TaskState state;
    int status;
    TASK_ALIGN unsigned char stack[TASK_STACK_BYTES];
} Task;

static Task tasks[TASK_LIMIT];
static TaskContext dispatcher;
static Task* current;
static unsigned int cursor;
static int initialized, dispatching;

static int owned(void) {
    return initialized && task_port_owned();
}

static Task* lookup(TaskId id) {
    unsigned int slot = (unsigned int)(id & ID_MASK);
    if (!slot || slot > TASK_LIMIT) return NULL;
    Task* task = &tasks[slot - 1];
    return task->state != TASK_FREE && task->id == id ? task : NULL;
}

static void check_stack(const Task* task) {
    uintptr_t bottom = (uintptr_t)task->stack + GUARD_BYTES;
    uintptr_t top = (uintptr_t)task->stack + TASK_STACK_BYTES;
    for (unsigned int i = 0; i < GUARD_BYTES; i++) {
        if (task->stack[i] != GUARD_VALUE)
            task_port_halt("[ERR] Task stack guard damaged; halted\n");
    }
    if (task->context.sp < bottom || task->context.sp > top ||
        (task->context.sp & 15U))
        task_port_halt("[ERR] Task stack pointer invalid; halted\n");
}

static void bootstrap(void) {
    current->entry(current->argument);
    task_exit(0);
}

int task_init(void) {
    if (!task_port_owned() || initialized) return TASK_INVALID;
    cursor = TASK_LIMIT - 1;
    initialized = 1;
    return TASK_OK;
}

TaskId task_create(TaskEntry entry, void* argument) {
    if (!owned() || !entry) return 0;
    for (unsigned int i = 0; i < TASK_LIMIT; i++) {
        Task* task = &tasks[i];
        if (task->state != TASK_FREE || task->generation == (~(TaskId)0 >> ID_BITS))
            continue;
        task->generation++;
        task->id = (task->generation << ID_BITS) | (i + 1U);
        task->entry = entry;
        task->argument = argument;
        task->waiting_for = task->waiter = 0;
        task->status = 0;
        for (unsigned int j = 0; j < GUARD_BYTES; j++) task->stack[j] = GUARD_VALUE;
        task_context_init(&task->context, task->stack + GUARD_BYTES,
                          TASK_STACK_BYTES - GUARD_BYTES, bootstrap);
        task->state = TASK_RUNNABLE;
        return task->id;
    }
    return 0;
}

TaskId task_current(void) {
    return owned() && current ? current->id : 0;
}

int task_yield(void) {
    if (!owned() || !current) return TASK_INVALID;
    current->state = TASK_RUNNABLE;
    task_context_switch(&current->context, &dispatcher);
    return TASK_OK;
}

void task_exit(int status) {
    if (!owned() || !current)
        task_port_halt("[ERR] Task exit outside scheduler; halted\n");
    current->status = status;
    current->state = TASK_EXITED;
    Task* waiter = lookup(current->waiter);
    if (waiter && waiter->state == TASK_BLOCKED && waiter->waiting_for == current->id)
        waiter->state = TASK_RUNNABLE;
    task_context_switch(&current->context, &dispatcher);
    task_port_halt("[ERR] Exited task resumed; halted\n");
}

static int reap(Task* task, int* status) {
    if (status) *status = task->status;
    task_context_dispose(&task->context);
    task->state = TASK_FREE;
    return TASK_OK;
}

int task_reap(TaskId id, int* status) {
    if (!owned()) return TASK_INVALID;
    Task* task = lookup(id);
    if (!task || task == current) return TASK_INVALID;
    if (task->waiter) return TASK_BUSY;
    if (task->state != TASK_EXITED) return TASK_PENDING;
    return reap(task, status);
}

int task_join(TaskId id, int* status) {
    if (!owned() || !current) return TASK_INVALID;
    Task* task = lookup(id);
    if (!task) return TASK_INVALID;
    if (task == current) return TASK_DEADLOCK;
    if (task->waiter) return TASK_BUSY;
    /* Reject cycles before adding this edge to the single-waiter graph. */
    Task* dependency = task;
    for (unsigned int i = 0; dependency && i < TASK_LIMIT; i++) {
        if (dependency == current) return TASK_DEADLOCK;
        dependency = lookup(dependency->waiting_for);
    }
    if (task->state != TASK_EXITED) {
        task->waiter = current->id;
        current->waiting_for = id;
        current->state = TASK_BLOCKED;
        task_context_switch(&current->context, &dispatcher);
        current->waiting_for = 0;
    }
    return reap(task, status);
}

int task_state(TaskId id, enum TaskState* state) {
    if (!owned() || !state) return TASK_INVALID;
    Task* task = lookup(id);
    if (!task) return TASK_INVALID;
    *state = task->state;
    return TASK_OK;
}

int task_run(void) {
    if (!owned() || dispatching || current) return TASK_INVALID;
    dispatching = 1;
    for (;;) {
        Task* next = NULL;
        int blocked = 0;
        for (unsigned int offset = 1; offset <= TASK_LIMIT; offset++) {
            unsigned int slot = (cursor + offset) % TASK_LIMIT;
            if (tasks[slot].state == TASK_BLOCKED) blocked = 1;
            if (!next && tasks[slot].state == TASK_RUNNABLE) {
                next = &tasks[slot];
                cursor = slot;
                break;
            }
        }
        if (!next) {
            dispatching = 0;
            return blocked ? TASK_PENDING : TASK_OK;
        }
        check_stack(next);
        current = next;
        current->state = TASK_RUNNING;
        task_context_switch(&dispatcher, &current->context);
        check_stack(current);
        current = NULL;
    }
}
