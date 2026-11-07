# Desktop terminal simulator

On Windows, install CMake and Visual Studio Build Tools with C++ support.
Open PowerShell in the repository root, then use the bundled SDL2 package:

```powershell
cmake -S sim -B sim/build -DCMAKE_PREFIX_PATH="$PWD/sim/SDL2"
cmake --build sim/build --config Release
.\sim\build\Release\vantage_sim.exe
```

Keep `SDL2.dll` next to `vantage_sim.exe`; the build copies it there automatically.
The simulator opens a terminal with a graphical QWERTY keyboard. Type normally
or click its keys, then use Enter to submit `help`, `mem`, `echo hello`, `about`
or `clear`. Hold Del to keep deleting. Shift applies to the next character.

Run the automated simulator and terminal checks with:

```powershell
ctest --test-dir sim/build -C Release --output-on-failure
```

See [the terminal guide](../docs/TERMINAL.md) for input behavior and limitations.
