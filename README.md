# Vantage-OS

VantageOS is a Raspberry Pi version of Idkthisguy's Vantage/VantageOS.

## Desktop simulator

You can run the simulator on a Windows PC without a Raspberry Pi.

### Prerequisites

- [CMake](https://cmake.org/download/)
- Visual Studio or Visual Studio Build Tools with C++ support
- The SDL2 Visual C++ package: download `SDL2-devel-*-VC.zip` from
  [SDL releases](https://github.com/libsdl-org/SDL/releases) and extract it.

### Build and run

Open PowerShell in the repository folder. Replace the SDL2 path below with the
location where you extracted the package, then run:

```powershell
cmake -S sim -B sim/build -DCMAKE_PREFIX_PATH=C:\path\to\SDL2-devel-2.32.10-VC
cmake --build sim/build --config Release
.\sim\build\Release\vantage_sim.exe
```

After the first build, use the last two commands to rebuild and run it again.
Use the left mouse button as touch input. The simulator shows the most recent
touch coordinates at the top of the window.

For implementation details and tests, see the [developer guide](docs/DEVELOPMENT.md).


