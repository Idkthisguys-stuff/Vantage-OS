# Developer guide

## Simulator architecture

`includes/platform.h` declares the hardware surface, and two backends implement
it:

| Backend | Files | Built by |
|---|---|---|
| HDMI framebuffer + I2C touch | `drivers/video/display.c`, `drivers/input/touch.c`, `drivers/I2C.c` | `make` → `kernel8.img` |
| SDL2 window + mouse | `sim/platform/display_sim.c` | `sim/CMakeLists.txt` → `vantage_sim.exe` |

`init/main.c`, `drivers/video/graphics.c`, `user/env/windowManager.c` and
`user/lib/user_lib.c` are shared by both targets. The simulator defines
`VANTAGE_SIM` to skip ARM user-mode startup and route syscall wrappers directly
to the same `handle_sys_call` dispatcher. `sim/main.c` starts the shared
`user_task` on desktop. This simulates the application, not ARM privilege levels
or memory protection; the Raspberry Pi build still uses its ARM startup and
`svc` instructions.

## Tests

After building the simulator, run the SDL input/rendering and mocked hardware
touch-driver tests:

```powershell
ctest --test-dir sim/build -C Release --output-on-failure
```

## Hardware user memory

The linker places the user stack in a separate, 2 MiB-aligned `.user.bss`
region. Only that data region is writable from EL0. Kernel data, page tables,
the privileged stack, the framebuffer and peripheral registers are EL1-only.
The existing shared code region remains read-only and executable in both modes.

Pointer-taking syscalls check alignment and the complete buffer range before
accessing it. Rejected buffers return 0 without reading input or drawing.
The native `user_memory` test checks page-table permissions and the syscall
dispatcher with bounded test memory; it does not emulate ARM access faults.

## Pi 4 four-core startup

Before display initialization enables the MMU and data cache, core 0 releases
cores 1, 2 and 3 sequentially through the firmware spin-table slots at `0xe0`,
`0xe8` and `0xf0`. Each secondary checks its ID and execution state, exercises
its separate 4 KiB privileged stack, signals its own 64-byte status slot, and
then waits for the common MMU setup. Only core 0 writes UART output.
`secondary_core_start` is a one-shot boot operation, called once for each
secondary; it is not a restart API.

Core 0 waits up to one second per secondary using the Pi system timer, with an
iteration limit as a fallback. On failure, it identifies the unconfirmed core
and halts before display/MMU initialization, without releasing further cores.
A late secondary can still arrive because firmware may already have read its
release slot. These entry messages are not worker-ready or completed-job
signals. The early flags are not used for post-boot jobs.

This expects the normal 64-bit Pi 4 firmware spin-table protocol, with the MMU
and data cache disabled on all participating cores during the entry handshake.
It also relies on firmware having configured the Pi 4 CPU cluster for hardware
coherency and having loaded the image without stale cache lines. Verify on Pi 4 by checking
the serial startup message across repeated cold boots; a successful compile or
simulator test does not establish that a secondary core ran. The maintainer
reported successful hardware startup for the original core-1 implementation;
the new core-2/core-3 paths and cached worker system require hardware validation.

Record the tested commit, image hash, board and firmware version, and the full
serial log. Across repeated cold boots, expect all three messages before the
display-driver message:

```text
[OK] Core 1 entry reached
[OK] Core 2 entry reached
[OK] Core 3 entry reached
```

## Four-way CPU jobs and rendering

Each secondary enters EL1h, masks interrupts, installs the exception vectors,
enables FP/SIMD access and uses a private stack. Before accessing cached C job
state it waits on `secondary_mmu_ready`. The linker reserves a separate 2 MiB
`.smp.boot` block, mapped as privileged Device-nGnRnE memory to match MMU-off
data accesses. Core 0 builds immutable tables with caches off, enables its MMU,
then publishes this gate with a barrier/event. Workers enable the same tables,
MAIR and TCR before publishing their acquire/release worker-online signals.
Do not rebuild these shared tables after workers join.

After display/input setup, `parallel_init` waits for all three workers and
`parallel_self_test` runs eight four-way checksum batches. Each batch requires
all four core IDs to enter its callback before any can finish. Boot continues
only after the following message:

```text
[OK] Four-core parallel self-test passed
```

`parallel_run` has one bounded slot per secondary and one coordinator. Core 0
publishes callback/context before releasing each sequence number, wakes workers,
and executes slice 0 itself. Workers acquire their sequence, run their slice,
complete outstanding writes, then release completion. Core 0 acquires every
completion before returning. Contexts may live on its stack because the call
joins all work. A completion timeout halts without returning or reusing that
context. Nested submissions and submissions from a worker are rejected. No
dynamic queue, work stealing, task migration or preemption is implemented.

Large physical rectangle fills and framebuffer copies partition rows across
all four cores. Small spans stay local. Jobs receive immutable geometry/color
snapshots rather than modifying shared canvas state. Input, UART, mailbox
requests and page flips stay on core 0, after all row writes complete. These
are CPU operations, not GPU-accelerated rendering. The desktop application
renderer remains unchanged; separate SDL-thread tests execute the real batch
dispatcher and raster callbacks concurrently.

The native tests cover 256 repeated batches, core identity, serial fallback,
nested rejection, missing-worker and completion-timeout paths, and padded/odd
row counts compared with serial pixels. They do not test ARM memory ordering.
On Pi, run repeated cold boots, then sustained touch/rendering, and compare
frame output with a disposable test build that invokes the same raster callbacks
with `(part=0, parts=1)`. Record results and timings separately; no speedup is
claimed without measurement. Withhold a worker's online signal and, separately,
its job completion in disposable failure-path builds to verify bounded failure.

For a hardware failure-path test, use a disposable test build that withholds
one secondary's release. Confirm its timeout message and that display/MMU
initialization is never reached. Do not ship that test modification.

The optional assembled-code regression harness requires Python, Unicorn and
Capstone. Use a fresh build directory to avoid the repository's tracked build
artifacts:

```sh
python -m pip install unicorn==2.1.4 capstone==5.0.7
make BUILD_DIR=out/arm-validation
python tests/arm/secondary_startup.py out/arm-validation/kernel8.elf
```

It checks linked stack layout, EL1 entry, MMU register setup and gate ordering,
per-core startup, acquire/release and completion instructions, rejection paths,
timer wrap, timeout and boot sequencing. Privileged registers, timers and event
delivery are mocked; it does not test firmware, cache coherence or concurrent hardware.

For remaining worker validation, GPU and process-management stages, see the
[parallelism roadmap](PARALLELISM.md).

## Touch events

`sys_touch_read(&event)` returns 1 when a `TouchEvent` is available, or 0 with
type `TOUCH_EVENT_NONE` otherwise. A press emits `TOUCH_EVENT_DOWN`, a change
in position while pressed emits `TOUCH_EVENT_MOVE`, and a release emits
`TOUCH_EVENT_UP`. Each event includes `x` and `y` in canvas coordinates.
The caller keeps its own pressed state between events. The hardware reports
the last known position on release and ignores failed I2C reads; it can only
detect transitions observed when polled. The desktop simulator preserves
queued mouse transitions across reads.

## Drawing UI

`includes/video/graphics.h` is the drawing API and is portable across both
builds. It draws shapes only, so a bitmap font baked into a C array and a
`canvas_draw_text` blitter are needed before anything can be labelled.

`Window` in `includes/env/windowManager.h` already carries `title[32]`,
`titleColor` and an `onTouch` callback, but nothing reads them yet. That is where
UI behaviour attaches.
