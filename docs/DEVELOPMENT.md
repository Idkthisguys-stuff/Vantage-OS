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

## Pi 4 second core

Before display initialization enables the MMU and data cache, core 0 writes the
core 1 entry address to the firmware spin-table slot at `0xe0` and sends an
event. Core 1 checks its ID and execution state, uses a separate 4 KiB stack,
signals that it reached the entry point, and parks. Only core 0 writes UART
output. A bounded wait reports an unconfirmed startup without hanging boot.
Core 1 does not run application work, enable its MMU, or participate in a
scheduler; cores 2 and 3 are not released.

This expects the normal 64-bit Pi 4 firmware spin-table protocol, with the MMU
and data cache disabled on both cores during the handshake. It is not a general
multicore synchronization primitive. Verify on actual Pi 4 hardware by checking
the serial startup message across repeated cold boots; a successful compile or
simulator test does not establish that a second core ran.

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
