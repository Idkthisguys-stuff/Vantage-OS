#include "../../includes/task.h"

typedef struct {
    unsigned int count[2];
    unsigned int failed;
} Progress;

typedef struct { Progress* progress; unsigned int slot; } Participant;

static void probe(void* argument) {
    Participant* participant = argument;
    Progress* progress = participant->progress;
    unsigned int slot = participant->slot;
    volatile uint64_t local = 0x123456789abcdef0ULL + slot;
    for (unsigned int i = 0; i < 8; i++) {
        if (!task_current() || progress->count[slot ^ 1U] < i ||
            progress->count[slot ^ 1U] > i + 1U ||
            local != 0x123456789abcdef0ULL + slot) progress->failed = 1;
        progress->count[slot]++;
        if (task_yield() != TASK_OK) progress->failed = 1;
    }
    task_exit((int)slot + 10);
}

int task_self_test(void) {
    static Progress progress;
    static Participant participants[2] = {{&progress, 0}, {&progress, 1}};
    progress = (Progress){{0, 0}, 0};
    TaskId first = task_create(probe, &participants[0]);
    TaskId second = task_create(probe, &participants[1]);
    int a = 0, b = 0;
    if (!first || !second) {
        return 0;
    }
    return task_run() == TASK_OK && task_reap(first, &a) == TASK_OK &&
           task_reap(second, &b) == TASK_OK && a == 10 && b == 11 &&
           progress.count[0] == 8 && progress.count[1] == 8 && !progress.failed;
}
