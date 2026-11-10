#pragma once
#include <vantage/kernel/storage.h>

/* Mount a provisioned partition after confirming the board handoff. */
int storage_start(uint32_t base_clock_hz, unsigned int partition);
/* Validation volume only: create a deterministic probe (0), or check an
 * existing probe (1). A match only proves a reboot if separate logs show one. */
int storage_reboot_probe(void);
