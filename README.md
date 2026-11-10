# Vantage-OS

VantageOS is a Raspberry Pi version of Idkthisguy's Vantage/VantageOS.

## Raspberry Pi 4 kernel

With Python 3, GNU Make, a POSIX shell and a GNU AArch64 cross-toolchain installed:

```sh
python tools/build.py
```

The kernel image is `out/pi4/prod/kernel8.img`; the linked test image is
`out/pi4/kernel8.elf`. The kernel image is not a complete bootable SD-card image.
V3D rendering is probed at boot by default and falls back to CPU rendering;
`python tools/build.py --v3d 0` builds the CPU renderer only. Wi-Fi and storage
support default on, but Wi-Fi skips initialization without supplied assets and
storage skips mounting without the confirmed SD clock and primary partition.
Physical validation remains pending; see
[the hardware guide](docs/HARDWARE.md) and [storage guide](docs/STORAGE.md).

New V3 storage images support 256 files/folders, 255-byte paths and 1 MiB per file
(subject to shared disk space). The text editor retains its 4 KiB document limit.
Existing volumes keep their old limits; use the storage guide's explicit
[migration command](docs/STORAGE.md#migrate-an-existing-data-image) for more capacity.

## Desktop simulator

You can run the simulator on a Windows PC without a Raspberry Pi.

### Prerequisites

- [CMake](https://cmake.org/download/)
- Visual Studio or Visual Studio Build Tools with C++ support
- An SDL2 development package; supply its CMake prefix with `--sdl` or
  `CMAKE_PREFIX_PATH`.

### Build and run

Open PowerShell in the repository folder, then run (provide your SDL2 package path):

```powershell
python tools/build.py sim --sdl C:/path/to/SDL2
.\out\sim\sim\Release\vantage_sim.exe
```

Type `help` to get started, using your keyboard or the on-screen keys. With a
storage image mounted, `edit`, `cat`, `ls` and `rm` read and write persistent
files. See the [terminal guide](docs/TERMINAL.md) for usage.

Experimental persistent SD files and the physical Pi validation gate are
documented in [the storage guide](docs/STORAGE.md).

Preemptive four-core scheduling, processes with isolated address spaces and
their hardware acceptance checklist are documented in
[the scheduler guide](docs/SCHEDULER.md). They are validated on host tests and
ARM models; physical Pi validation is pending.

For implementation details and tests, see the [developer guide](docs/DEVELOPMENT.md).

## Headless tests and source layout

Requires CMake, a host C compiler and Python 3.11+.

```sh
python tools/build.py test
```

Add `-DVANTAGE_BUILD_SIMULATOR=ON` and an SDL2 package path to include the desktop
application and rendering tests in the root build. Headless tests do not require
SDL2. Use CTest labels such as `-L storage` or `-L scheduler` for a focused run.

See [architecture and ownership](docs/ARCHITECTURE.md) for the directory map and
[contributing](CONTRIBUTING.md) for change and verification conventions.
