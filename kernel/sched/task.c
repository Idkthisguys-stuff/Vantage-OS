#include "internal.h"
#include <vantage/kernel/files.h>
#include <vantage/kernel/parallel_port.h>

#define ID_BITS 6U
#define ID_MASK 63U
#define GUARD_BYTES 64U
#define GUARD_VALUE 0xa7U

typedef char task_ids_must_fit[(TASK_LIMIT < ID_MASK) ? 1 : -1];
typedef char task_cores_must_match[(TASK_CORES == PARALLEL_CORES) ? 1 : -1];

static Task tasks[TASK_LIMIT];
static TaskContext dispatcher[TASK_CORES];
/* Each current slot is written by its core under the lock. Other cores may
 * inspect slots only under the lock, when reserving idle workers for a batch. */
static Task* current[TASK_CORES];
/* The table, cursors and dispatching are protected by scheduler_lock. */
static unsigned int cursor[TASK_CORES];
static uint32_t scheduler_lock, initialized;
static int dispatching, workers_reserved;
/* Only core 0 blocks or wakes timed event waiters. */
static unsigned int timed_waiters;

static int owned(void) { return task_port_owned() && parallel_load(&initialized); }
static void lock(void) { task_port_lock(&scheduler_lock); }
static void unlock(void) { task_port_unlock(&scheduler_lock); }

static Task* lookup(TaskId id) {
    unsigned int slot = (unsigned int)(id & ID_MASK);
    if (!slot || slot > TASK_LIMIT) return NULL;
    Task* task = &tasks[slot - 1];
    return task->state != TASK_FREE && task->id == id ? task : NULL;
}

static void check_stack(const Task* task) {
    uintptr_t bottom = (uintptr_t)task->stack + GUARD_BYTES;
    uintptr_t top = (uintptr_t)task->stack + TASK_STACK_BYTES;
    for (unsigned int i = 0; i < GUARD_BYTES; i++)
        if (task->stack[i] != GUARD_VALUE)
            task_port_halt("[ERR] Task stack guard damaged; halted\n");
    if (task->context.sp < bottom || task->context.sp > top || (task->context.sp & 15U))
        task_port_halt("[ERR] Task stack pointer invalid; halted\n");
}

static void bootstrap(void) {
    Task* task = current[task_port_core()];
    task->entry(task->argument);
    task_exit(0);
}

/* Lock held; the exited task's context is saved and it is off every core. */
static void finish(Task* task) {
    Task* waiter = lookup(task->waiter);
    if (waiter && waiter->state == TASK_BLOCKED && waiter->waiting_for == task->id)
        waiter->state = TASK_RUNNABLE;
    if (task->process) process_thread_exited(task->process);
}

/* Called by exactly one dispatcher per core. A yielding/exiting/blocking task
 * hands the lock to its dispatcher across the context switch. Until its entire
 * context is saved, no other core may dispatch, join or reap that task. */
static uint32_t dispatch_one(unsigned int core) {
    lock();
    Task* next = NULL;
    if (dispatching && (!core || !workers_reserved)) {
        for (unsigned int offset = 1; offset <= TASK_LIMIT; offset++) {
            unsigned int slot = (cursor[core] + offset) % TASK_LIMIT;
            Task* candidate = &tasks[slot];
            if (candidate->state != TASK_RUNNABLE) continue;
            if (candidate->process && candidate->process->terminating) {
                /* A thread of a terminated process never runs again. It is
                 * off every core, so any dispatcher may retire it. */
                candidate->status = candidate->process->status;
                candidate->state = TASK_EXITED;
                finish(candidate);
                continue;
            }
            if (candidate->affinity & (1U << core)) {
                next = candidate;
                cursor[core] = slot;
                break;
            }
        }
    }
    if (!next) { unlock(); return 0; }
    check_stack(next);
    current[core] = next;
    next->state = TASK_RUNNING;
    next->core = core;
    if (next->dispatches != ~(uint64_t)0) next->dispatches++;
    task_port_address_space(next->process ? next->process->root : 0);
    unlock();
    /* The dispatcher keeps IRQs masked: the tick can only be taken once the
     * task runs EL0 code or opens a preemptible region. */
    task_port_slice_start(TASK_SLICE_US);
    task_context_switch(&dispatcher[core], &next->context);
    task_port_slice_stop();
    /* The resumed dispatcher owns the lock handed over by this core's task.
     * Drop the user half before the task can be observed off this core: an
     * exited thread's address space may be freed as soon as the lock opens. */
    task_port_address_space(0);
    check_stack(next);
    next->core = TASK_CORES;
    current[core] = NULL;
    if (next->state == TASK_EXITED) finish(next);
    unlock();
    parallel_wake();
    return 1;
}

static int reserve_workers(int claim) {
    lock();
    if (claim) {
        for (unsigned int core = 1; core < TASK_CORES; core++) {
            if (current[core]) { unlock(); return 0; }
        }
    }
    workers_reserved = claim != 0;
    unlock();
    if (!claim) parallel_wake();
    return 1;
}

int task_init(void) {
    if (!task_port_owned() || task_port_core() || parallel_load(&initialized)) return TASK_INVALID;
    if (!parallel_task_install(dispatch_one, reserve_workers)) return TASK_INVALID;
    for (unsigned int core = 0; core < TASK_CORES; core++) cursor[core] = TASK_LIMIT - 1;
    parallel_store(&initialized, 1);
    return TASK_OK;
}

TaskId sched_spawn(TaskEntry entry, void* argument, uint32_t affinity, Process* process) {
    if (!owned() || !entry || !affinity || (affinity & ~parallel_task_mask())) return 0;
    lock();
    TaskId id = 0;
    for (unsigned int i = 0; i < TASK_LIMIT; i++) {
        if (process && (process->state != PROCESS_LIVE || process->terminating)) break;
        Task* task = &tasks[i];
        if (task->state != TASK_FREE || task->generation == (~(TaskId)0 >> ID_BITS)) continue;
        task->generation++;
        task->id = (task->generation << ID_BITS) | (i + 1U);
        task->entry = entry;
        task->argument = argument;
        task->waiting_for = task->waiter = 0;
        task->events = task->event_interests = task->wait_duration = 0;
        task->status = 0;
        task->affinity = affinity;
        task->core = TASK_CORES;
        task->dispatches = task->preemptions = 0;
        task->process = process;
        if (process) process->live++;
        for (unsigned int j = 0; j < GUARD_BYTES; j++) task->stack[j] = GUARD_VALUE;
        task_context_init(&task->context, task->stack + GUARD_BYTES,
                          TASK_STACK_BYTES - GUARD_BYTES, bootstrap);
        task->state = TASK_RUNNABLE;
        id = task->id;
        break;
    }
    unlock();
    if (id) parallel_wake();
    return id;
}

TaskId task_create_affinity(TaskEntry entry, void* argument, uint32_t affinity) {
    return sched_spawn(entry, argument, affinity, NULL);
}

TaskId task_create(TaskEntry entry, void* argument) {
    return task_create_affinity(entry, argument, TASK_CORE0);
}

unsigned int task_core(void) { return task_port_core(); }

TaskId task_current(void) {
    if (!owned()) return 0;
    Task* task = current[task_port_core()];
    return task ? task->id : 0;
}

int task_services_allowed(void) {
    if (!task_port_owned() || task_port_core()) return 0;
    Task* task = current[0];
    return !task || task->affinity == TASK_CORE0;
}

int task_yield(void) {
    if (!owned()) return TASK_INVALID;
    unsigned int core = task_port_core();
    Task* task = current[core];
    if (!task) return TASK_INVALID;
    lock();
    task->state = TASK_RUNNABLE;
    task_context_switch(&task->context, &dispatcher[core]);
    return TASK_OK;
}

static void wake_events(Task* task) {
    task->wait_duration = 0;
    timed_waiters--;
    task->state = TASK_RUNNABLE;
}

uint32_t task_wait_events(uint32_t interests, uint32_t timeout_us) {
    if (!owned() || task_port_core() || timeout_us > TASK_WAIT_MAX_US) return 0;
    Task* task = current[0];
    if (!task || task->affinity != TASK_CORE0) return 0;
    lock();
    if (!(task->events & interests) && timeout_us) {
        task->event_interests = interests;
        task->wait_started = parallel_time_us();
        task->wait_duration = timeout_us;
        timed_waiters++;
        task->state = TASK_BLOCKED;
        task_context_switch(&task->context, &dispatcher[0]);
        lock();
    }
    uint32_t events = task->events & interests;
    task->events &= ~events;
    unlock();
    return events;
}

void task_notify_events(uint32_t events) {
    if (!events || !owned() || task_port_core()) return;
    lock();
    for (unsigned int i = 0; i < TASK_LIMIT; i++) {
        Task* task = &tasks[i];
        if (task->affinity != TASK_CORE0 || task->state == TASK_FREE ||
            task->state == TASK_EXITED) continue;
        task->events |= events;
        if (task->wait_duration && (task->event_interests & events)) wake_events(task);
    }
    unlock();
}

int task_preempt_begin(void) {
    if (!owned() || !current[task_port_core()]) return TASK_INVALID;
    task_port_irq_enable();
    return TASK_OK;
}

void task_preempt_end(void) {
    task_port_irq_disable();
}

void task_trap_boundary(void) {
    if (!owned()) return;
    Task* task = current[task_port_core()];
    /* A terminated process never executes another EL0 instruction. */
    if (task && task->process && parallel_load(&task->process->terminating))
        task_exit(0);
}

void task_tick(void) {
    if (!owned()) return;
    unsigned int core = task_port_core();
    Task* task = current[core];
    if (!task) { parallel_wake(); return; }
    task_trap_boundary();
    /* The interrupted context is complete in the trap frame on this task's
     * kernel stack, so this is a yield from inside the interrupt handler. */
    lock();
    task->state = TASK_RUNNABLE;
    if (task->preemptions != ~(uint64_t)0) task->preemptions++;
    task_context_switch(&task->context, &dispatcher[core]);
}

void task_exit(int status) {
    if (!owned() || !current[task_port_core()])
        task_port_halt("[ERR] Task exit outside scheduler; halted\n");
    unsigned int core = task_port_core();
    Task* task = current[core];
    Process* process = task->process;
    uint64_t owner = task->id;
    if (process) {
        /* Exit ends the whole process; the first request fixes its status. */
        lock();
        if (!process->terminating) {
            process->status = status;
            parallel_store(&process->terminating, 1);
        }
        status = process->status;
        unlock();
        owner = process->id;
    }
    /* Only immutable core-0-affinity tasks may own file handles. No other
     * thread of a terminating process can issue another file call. */
    if (task->affinity == TASK_CORE0) files_release(owner);
    lock();
    task->status = status;
    task->state = TASK_EXITED;
    task_context_switch(&task->context, &dispatcher[core]);
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
    lock();
    Task* task = lookup(id);
    int result = TASK_INVALID;
    if (task && !task->process && task != current[task_port_core()]) {
        result = task->waiter ? TASK_BUSY : task->state != TASK_EXITED ? TASK_PENDING : reap(task, status);
    }
    unlock();
    return result;
}

int task_join(TaskId id, int* status) {
    if (!owned()) return TASK_INVALID;
    unsigned int core = task_port_core();
    Task* self = current[core];
    if (!self) return TASK_INVALID;
    lock();
    Task* task = lookup(id);
    /* Process threads are reaped with their process, never individually. */
    if (!task || task->process) { unlock(); return TASK_INVALID; }
    if (task == self) { unlock(); return TASK_DEADLOCK; }
    if (task->waiter) { unlock(); return TASK_BUSY; }
    Task* dependency = task;
    for (unsigned int i = 0; dependency && i < TASK_LIMIT; i++) {
        if (dependency == self) { unlock(); return TASK_DEADLOCK; }
        dependency = lookup(dependency->waiting_for);
    }
    if (task->state != TASK_EXITED) {
        task->waiter = self->id;
        self->waiting_for = id;
        self->state = TASK_BLOCKED;
        task_context_switch(&self->context, &dispatcher[core]);
        /* May have resumed on a different core. Never reuse the old dispatcher. */
        lock();
        self->waiting_for = 0;
    }
    int result = reap(task, status);
    unlock();
    return result;
}

void sched_lock(void) { lock(); }
void sched_unlock(void) { unlock(); }
Task* sched_running(void) { return owned() ? current[task_port_core()] : NULL; }
Task* sched_lookup(TaskId id) { return lookup(id); }
void sched_release(Task* task) { (void)reap(task, NULL); }

int task_info(TaskId id, TaskInfo* info) {
    if (!owned() || !info) return TASK_INVALID;
    lock();
    Task* task = lookup(id);
    if (task) {
        info->id = task->id;
        info->dispatches = task->dispatches;
        info->affinity = task->affinity;
        info->core = task->core;
        info->state = task->state;
        info->preemptions = task->preemptions;
    }
    unlock();
    return task ? TASK_OK : TASK_INVALID;
}

int task_state(TaskId id, enum TaskState* state) {
    if (!state) return TASK_INVALID;
    TaskInfo info;
    int result = task_info(id, &info);
    if (result == TASK_OK) *state = info.state;
    return result;
}

static uint32_t remaining(uint32_t now, uint32_t started, uint32_t duration) {
    uint32_t elapsed = now - started;
    return elapsed < duration ? duration - elapsed : 0;
}

/* Core 0 rechecks deadlines before dispatch. The result is the earliest
 * remaining interval; zero means there is no timed waiter left. */
static uint32_t wake_timed_waiters(void) {
    if (!timed_waiters) return 0;
    uint32_t now = parallel_time_us(), next = 0;
    lock();
    for (unsigned int i = 0; i < TASK_LIMIT; i++) {
        Task* task = &tasks[i];
        if (!task->wait_duration) continue;
        uint32_t left = remaining(now, task->wait_started, task->wait_duration);
        if (!left || (task->process && task->process->terminating)) wake_events(task);
        else if (!next || left < next) next = left;
    }
    unlock();
    return next;
}

int task_run_with_service(void (*service)(void), uint32_t interval_us) {
    if (!owned() || task_port_core() || current[0]) return TASK_INVALID;
    if (service && (!interval_us || interval_us > TASK_WAIT_MAX_US)) return TASK_INVALID;
    lock();
    if (dispatching) { unlock(); return TASK_INVALID; }
    dispatching = 1;
    unlock();
    if (!parallel_task_start()) {
        lock(); dispatching = 0; unlock();
        return TASK_INVALID;
    }
    uint32_t last_service = service ? parallel_time_us() - interval_us : 0;
    for (;;) {
        uint32_t until_service = 0;
        if (service) {
            uint32_t now = parallel_time_us();
            until_service = remaining(now, last_service, interval_us);
            if (!until_service) {
                last_service = now;
                service();
                /* Recompute with the time spent servicing I/O included. */
                until_service = remaining(parallel_time_us(), last_service, interval_us);
                if (!until_service) until_service = 1;
            }
        }
        uint32_t until_waiter = wake_timed_waiters();
        if (dispatch_one(0)) continue;
        lock();
        int live = 0, blocked = 0;
        for (unsigned int i = 0; i < TASK_LIMIT; i++) {
            if (tasks[i].state == TASK_RUNNING || tasks[i].state == TASK_RUNNABLE) live = 1;
            if (tasks[i].state == TASK_BLOCKED) blocked = 1;
        }
        if (!live && !until_waiter) {
            dispatching = 0;
            unlock();
            parallel_task_stop();
            return blocked ? TASK_PENDING : TASK_OK;
        }
        unlock();
        /* Publication -> SEV pairs with WFE, including check/sleep races. */
        uint32_t interval = until_waiter;
        if (until_service && (!interval || until_service < interval)) interval = until_service;
        if (interval) task_port_idle_for(interval);
        else parallel_idle();
    }
}

int task_run(void) { return task_run_with_service(NULL, 0); }
