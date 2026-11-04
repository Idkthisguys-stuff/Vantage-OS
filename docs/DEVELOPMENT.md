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
