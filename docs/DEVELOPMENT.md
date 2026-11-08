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

The C subsystem in [`mm/`](../mm/README.md) discovers firmware RAM/reservations,
manages physical pages, and supplies the renderer's kernel heap. It runs discovery
before secondary release, finalizes reservations before MMU enablement, and keeps
the shared tables immutable. Before clearing BSS, a file-backed bootstrap stack
supports read-only firmware discovery with stack-local metadata, rejecting boot
assets that would be overwritten by zeroing. DTB bounds are checked before any
stack writes. Whole RAM holes and blocks overlapping `no-map` are
left invalid; allocation excludes all reservations. Allocator calls are restricted
to masked EL1 execution on core 0, with workers using caller-owned buffers.

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

## Cooperative tasks

`kernel/sched/` implements round-robin EL1 tasks on core 0. Boot runs a two-task
progress test, then starts the built-in EL0 app inside a scheduled task. Cores
1–3 retain their existing synchronous worker ownership.

`task_init` is one-shot. `task_create` takes a trusted kernel entry and argument;
the caller keeps the argument alive until exit. Eight fixed slots each have a
16 KiB privileged stack with a 64-byte diagnostic guard. Task IDs include a
generation; reaped IDs cannot address a reused slot. Generation exhaustion
permanently retires the slot rather than wrapping.

Tasks move through runnable, running, blocked and exited states. Yield returns
to the boot-stack dispatcher. Returning from an entry exits with status zero.
`task_join` blocks a running task, rejects self/cyclic waits and reserves the
target for one waiter. `task_reap` is nonblocking and also works from boot; it
rejects running or waiter-owned targets. Exited stacks remain reserved until
reaped, after execution has switched away. Join/reap status pointers are trusted
kernel pointers, not syscall payloads. `task_run` dispatches until quiescent;
blocked-only state returns `TASK_PENDING` instead of spinning.

All scheduler calls require masked EL1 execution on core 0. Drivers and parallel
jobs must finish before yielding; there is no preemption, migration, timer wakeup
or interrupt-safe locking. The context switch saves the AAPCS64 callee-saved
integer registers, SP, lower halves of v8–v15, FPCR/FPSR and the pending
ELR_EL1/SPSR_EL1/SP_EL0. This is a cooperative call-boundary context, not a full
interrupt frame. Stack guards detect some corruption; they are not guard pages.

| Syscall | ID | Result |
|---|---|---|
| `sys_task_id()` | 16 | Current task ID; zero when unavailable |
| `sys_task_yield()` | 17 | One after resuming; zero when unavailable |
| `sys_task_exit(status)` | 18 | Does not return on success; zero when unavailable |

IDs 6–15 are reserved for console extensions. Presentation also yields, after
the renderer and parallel jobs finish. EL0 return exits through SVC; interrupt
masks stay set because interrupt delivery is not configured. Task creation and
join are kernel-only. The desktop app does not emulate scheduling and returns
zero for these three syscalls.

The shared code and single writable user region are unchanged. These tasks are
not isolated processes: there are no independent address spaces, user account
permissions, executable loading or user-fault recovery.

```sh
python tests/arm/tasks.py /path/to/fresh/kernel8.elf -v
```

This executes the linked scheduler, context-switch assembly and SVC wrappers,
with scripted task bodies and modeled exception delivery. It checks register and
stack preservation, round-robin progress, join/exit/reaping, stale IDs, ownership
checks, damaged guards and return to EL0 after a yield. It does not model Pi
caches, GIC interrupts or ARM memory-permission enforcement.

Before hardware acceptance, record image/commit hashes and serial output across
cold boots. Expect `[OK] Cooperative task self-test passed` before app entry.
Exercise sustained touch/rendering and a disposable two-task yield/exit build;
do not infer hardware compatibility from mocked tests.

## Application files

Apps access a small **persistent snapshot filesystem** through `sys_file_*` in
`user/include/user_lib.h` after an explicit successful storage mount. Normal Pi
boot keeps SD disabled pending physical validation, so files are unavailable
there. The simulator can mount a regular image through `VANTAGE_STORAGE_IMAGE`.
The legacy block-device RAM disk is separate. Provisioning, crash recovery,
limits and the hardware acceptance gate are documented in [STORAGE.md](STORAGE.md).

Paths are `/ram/<name>` with 1–31 ASCII letters, digits, `.`, `_` or `-`.
`.` and `..` are rejected. There are no subdirectories or relative paths. Limits
are 16 files, 4096 bytes per file, 16 open handles and 256 bytes per I/O syscall.

| Operation | ID | Payload alignment | Result |
|---|---|---|---|
| Open | 32 | `FileOpen`, 4 bytes | Positive handle |
| Read/write | 33/34 | `FileIo`, 8 bytes | Bytes transferred; zero at read EOF |
| Seek | 35 | `FileSeek`, 8 bytes | Zero on success |
| Close | 36 | Scalar handle | Zero on success |
| List | 37 | `FileEntry`, 4 bytes | One entry, or zero at end |
| Remove | 38 | `FilePath`, 1 byte | Zero on success |

Failures are negative `FileError` values. Open requires `FILE_READ` and/or
`FILE_WRITE`; `FILE_CREATE` and `FILE_TRUNCATE` require write access. Open checks
handle capacity before creating/truncating a file. A write exceeding capacity
fails without changing data or position. Seek accepts offsets through current
EOF; no sparse files are supported. Truncation leaves other handles' positions
unchanged: reads past EOF return zero, and writes there require a seek first.
The `/ram/` spelling is retained for ABI compatibility, not volatile storage.
Successful mutations commit synchronously. I/O failures quarantine the mount
until handles are released and storage is remounted; they are not guaranteed
to have left the disk unchanged. Corruption and I/O errors are reported explicitly.

Handles include a non-wrapping generation and belong to the current task. Exit
closes that task's handles but retains named files. Remove rejects files with
any open handles. The simulator uses one host-app owner. Files share a namespace;
this ownership check is not user-account permission or process memory isolation.

Requests have no nested pointers. The dispatcher validates the full range and
alignment, snapshots input on the kernel stack, checks reserved fields and
lengths, then copies successful output back. Read clears unused payload bytes.
Operations do not yield, preserving the scheduler's single-owner rules.

```c
int64_t handle = sys_file_open("/ram/note", FILE_READ | FILE_WRITE | FILE_CREATE);
if (handle > 0) {
    char text[8];
    sys_file_write((uint64_t)handle, "hello", 5);
    sys_file_seek((uint64_t)handle, 0);
    int count = sys_file_read((uint64_t)handle, text, sizeof(text));
    /* count is a byte count; file contents are not automatically terminated. */
    (void)count;
    sys_file_close((uint64_t)handle);
}
```

For listing, start with `FileEntry entry = {0}` and call `sys_file_list(&entry)`
until it returns zero. Entries include a full path, byte size and next cursor.
Listing is not a snapshot across concurrent file creation/removal. Calls after
an error should inspect the error rather than treating it as EOF.

Native tests cover paths, permissions, capacity, stale handles, syscall buffers,
5000 deterministic read/write/seek operations against a byte-array oracle,
fresh-process persistence and interrupted snapshot commits.
They can run without SDL:

```sh
cmake -S kernel/fs -B out/files-host
cmake --build out/files-host
ctest --test-dir out/files-host --output-on-failure
python tests/arm/files.py /path/to/fresh/kernel8.elf -v
```

The linked tests exercise the real EL0 wrappers, syscall handler, automatic
task-exit cleanup and malformed buffers, using modeled exception delivery.
Physical SD read/write/erase and reboot-persistence acceptance remain pending.

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

Optional Pi 4 V3D clear/copy and CYW43455 Ethernet transport are documented in
[the hardware driver guide](HARDWARE.md). Both are experimental and disabled at
boot by default. The V3D option accelerates full-canvas clears only after its
pixel self-test; normal shapes and presentation still use the CPU path.

`includes/video/graphics.h` is the drawing API and is portable across both
builds. It draws shapes only, so a bitmap font baked into a C array and a
`canvas_draw_text` blitter are needed before anything can be labelled.

`Window` in `includes/env/windowManager.h` already carries `title[32]`,
`titleColor` and an `onTouch` callback, but nothing reads them yet. That is where
UI behaviour attaches.
