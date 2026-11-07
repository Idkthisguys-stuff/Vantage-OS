# Vantage-OS

VantageOS is a Raspberry Pi version of Idkthisguy's Vantage/VantageOS.

## Desktop simulator

You can run the simulator on a Windows PC without a Raspberry Pi.

### Prerequisites

- [CMake](https://cmake.org/download/)
- Visual Studio or Visual Studio Build Tools with C++ support
- The bundled SDL2 Visual C++ package in `sim/SDL2`.

### Build and run

Open PowerShell in the repository folder, then run:

```powershell
cmake -S sim -B sim/build -DCMAKE_PREFIX_PATH="$PWD/sim/SDL2"
cmake --build sim/build --config Release
.\sim\build\Release\vantage_sim.exe
```

After the first build, use the last two commands to rebuild and run it again.
The simulator opens the Vantage terminal. Click its window and type `help`,
`clear`, `mem`, `echo hello` or `about`, then press Enter. Backspace edits the
current line; Ctrl+C cancels it and Ctrl+L clears the screen while keeping it.
Input is printable ASCII. The simulator's `mem` output describes host-managed
display memory; the Pi reports its kernel allocator statistics.

Click the graphical QWERTY keyboard to type with the mouse; on Pi it accepts
touch input. Shift applies to the next character, Del deletes and Enter submits.
Keys highlight while pressed; drag off a key to cancel a tap.
Hold Del to keep deleting after a short initial delay.

The Pi terminal uses the existing serial connection at 115200 baud, 8N1, with
local echo disabled. See the [terminal guide](docs/TERMINAL.md) for scope and tests.

For implementation details and tests, see the [developer guide](docs/DEVELOPMENT.md).
