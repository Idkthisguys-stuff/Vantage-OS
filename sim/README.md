# Vantage OS Desktop Simulator Guide

This took me a while to actually get this up and running because it would run only on Rennex's pc

SDL2 is added manually in sim so nobody has to suffer and install like I (Idkthisguy) did :/

### Configure command

```powershell
cd sim
cmake -B build -DCMAKE_PREFIX_PATH="./SDL2"
```

### Build comand

```powershell
cmake --build build --config Release
```

The .exe file is in sim/build/, but I couldn't find the right one..