# Developer guide

## Simulator architecture

`includes/platform.h` declares the hardware surface, and two backends implement
it:

| Backend | Files | Built by |
|---|---|---|
| HDMI framebuffer + I2C touch | `drivers/video/display.c`, `drivers/input/touch.c`, `drivers/I2C.c` | `make` → `kernel8.img` |
| SDL2 window + mouse | `sim/platform/display_sim.c` | `sim/CMakeLists.txt` → `vantage_sim.exe` |

`init/main.c`, `drivers/video/graphics.c`, `user/env/windowManager.c`,
`user/env/terminal*.c` and `user/lib/user_lib.c` are shared by both targets. The simulator defines
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
builds. Its bounded `canvas_text_row` API draws 8x16 ASCII cells using a
JetBrains Mono bitmap font (SIL Open Font License). The terminal app accesses it through a validated
fixed-size syscall payload, rather than drawing directly from EL0.

`Window` in `includes/env/windowManager.h` already carries `title[32]`,
`titleColor` and an `onTouch` callback, but nothing reads them yet. That is where
UI behaviour attaches.

## Terminal

See [the terminal guide](TERMINAL.md) for commands and input setup.

### Application and input

- `user/env/terminal.c` owns the bounded screen model and command editing.
  Commands have a 127-character limit, reduced on smaller screens; overflow
  discards the command. It has no SDL or kernel-global dependencies.
- `user/env/terminal_app.c` stores mutable state in `.user.bss` on Pi. It polls
  at most 64 keyboard and 64 touch events per step and redraws only dirty content.
  On the 480x800 canvas, the keyboard uses 272 pixels, leaving 33 text rows.
- Keyboard rendering and hit testing share geometry. Ordinary taps commit on
  release; dragging off cancels them. Del acts on press and repeats after 400 ms,
  then every 70 ms. Unsigned clock subtraction handles rollover; overdue repeats
  are dropped rather than replayed in a burst.
- The simulator keeps keyboard and touch queues separate. Keyboard overflow
  cancels the affected command; touch overflow resets the gesture. The terminal
  entry point disables the touch-coordinate overlay.
- `drivers/input/key.c` converts nonblocking UART input into key events. It
  accepts CR/LF/CRLF, filters unsupported ANSI navigation, and discards commands
  affected by receive overruns. Serial output mirrors terminal text. Bulk pastes
  have no flow control.

The terminal accepts printable ASCII and has no history, scrollback, cursor
navigation, Unicode, quoting, pipelines or executable loading. Its static cursor
and single-app interface do not provide PTYs or process isolation.

### Syscalls and rendering

| ID | Operation | Payload alignment |
|---|---|---|
| 6 | Poll key input (`KeyEvent`) | 4 bytes |
| 7 | Draw text row (`TextRow`) | 4 bytes |
| 8 | Read geometry/memory (`ConsoleInfo`) | 8 bytes |
| 9 | Wait for frame pacing | None |
| 10 | Write bounded serial bytes (`ConsoleWrite`) | 4 bytes |
| 11 | Draw rounded button (`CanvasButton`) | 4 bytes |
| 12 | Read wrapping 32-bit microsecond clock | None |

Payloads contain no nested pointers. The dispatcher validates full user-buffer
ranges and alignment, snapshots drawing inputs on the kernel stack, and writes
only validated outputs. Existing touch and shape syscalls remain available.

The renderer checks row, cursor, pixel bounds, radius and label size while
preserving canvas color. Button labels hold seven characters plus a terminator;
text scale is native for 0/1 and doubled for 2. `kernel/memory.c` provides the
freestanding copy/zero routines needed by compiler-generated structure operations;
its Makefile flag prevents recursive library-call optimization.

Pi memory statistics come from the core-0 EL1 allocator, not per-process
accounting. The simulator reports host display allocation and does not emulate
the kernel allocator.

### Tests

CTest covers the terminal model, serial parsing, syscall buffer bounds and
SDL input-to-pixel rendering, including key feedback, scaled glyphs and drag
cancellation. Model tests also run 20,000 deterministic mixed input events.

With Unicorn/Capstone installed and a fresh AArch64 build:

```sh
python tests/arm/terminal.py /path/to/kernel8.elf -v
```

The linked tests cover user-stack guards, syscall validation, memory statistics,
copy/zero routines and Del repeat timing, including rollover and empty input.
Exceptions and devices are mocked. Validate touch coordinates, serial/display
output, scrolling and memory counters on a physical Pi before claiming hardware
compatibility.

### Font regeneration

Retain `drivers/video/OFL.txt` when distributing the JetBrains Mono bitmap.
Pillow is needed only to regenerate the checked-in header:

```sh
python -m pip install pillow
python drivers/video/gen_font8x16.py --ttf /path/to/JetBrainsMono-Regular.ttf
```
