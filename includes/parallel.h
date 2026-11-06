#pragma once

#include "std.h"

#define PARALLEL_CORES 4

typedef void (*parallel_job)(unsigned int part, unsigned int parts, void* context);

/* Core 0 only, after the common MMU is enabled. Does not start/restart cores. */
uint32_t parallel_init(void);
uint32_t parallel_self_test(void);

/* Synchronous fork/join: cores 1..3 receive one slice each; core 0 runs slice 0.
 * Before init, runs one serial slice. Rejects NULL, nested or non-core-0 calls.
 * Context must remain valid until return; callbacks may not submit more jobs,
 * call blocking drivers, or touch overlapping writable memory without ordering.
 * A missing completion is fatal: a timed-out context cannot safely be reused. */
uint32_t parallel_run(parallel_job job, void* context);
void parallel_halt(const char* message);
