# Vantage-OS

VantageOS is a Raspberry Pi version of Idkthisguy's Vantage/VantageOS

## Desktop simulator

Runs the same UI on desktop for those who want to work on Development without PI.

### Prerequisites

- CMake
- An **SDL2 Visual C++** package: the latest `SDL2-devel-*-VC.zip` from
  [libsdl-org/SDL releases](https://github.com/libsdl-org/SDL/releases).

### Build and run

From the repository root:

```powershell
cmake -S sim -B sim/build -DCMAKE_PREFIX_PATH=C:\path\to\SDL2-devel-2.32.10-VC
cmake --build sim/build --config Release
.\sim\build\Release\vantage_sim.exe
```

CMake finds Visual Studio on its own, and `SDL2.dll` is copied next to the
executable, so the simulator runs straight from the build directory.

### How it works

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

Run the headless SDL input/rendering regression test after building:

```powershell
ctest --test-dir sim/build -C Release --output-on-failure
```

### Drawing UI

`includes/video/graphics.h` is the drawing API and is portable — call it from either
build. It draws shapes only, so a bitmap font baked into a C array and a
`canvas_draw_text` blitter are needed before anything can be labelled.

`Window` in `includes/env/windowManager.h` already carries `title[32]`,
`titleColor` and an `onTouch` callback, but nothing reads them yet. That is where
UI behaviour attaches.
