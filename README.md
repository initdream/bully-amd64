# bully-amd64

Port of Bully: Anniversary Edition (Android x86_64) to Linux x86_64.

## Requirements

- An **x86_64** GNU/Linux distribution
- Game files from a legitimate copy of **Bully: Anniversary Edition** for Android (**v1.4.311**)
- Build and runtime dependencies:
  - CMake (>= 3.10)
  - C11 and C++14 compatible compiler (GCC/Clang)
  - SDL2
  - EGL / OpenGL ES 2.0
  - OpenAL
  - libmpg123

### Installing dependencies

**Debian / Ubuntu / Linux Mint:**
```bash
sudo apt update
sudo apt install build-essential cmake libsdl2-dev libopenal-dev libmpg123-dev libegl1-mesa-dev libgles2-mesa-dev
```

**Arch Linux:**
```bash
sudo pacman -S base-devel cmake sdl2 openal mpg123 mesa
```

## Game Files Layout

Obtain the binaries (`libGame.so`, `libc++_shared.so`) and asset archives from the Android APK/OBB (**v1.4.311**). 

Place the compiled `bully` executable in the root directory alongside your game files:

```text
.
├── assets/
│   ├── data_0.zip
│   ├── data_0.zip.idx
│   ├── data_1.zip
│   ├── data_1.zip.idx
│   ├── data_2.zip
│   ├── data_2.zip.idx
│   ├── data_3.zip
│   ├── data_3.zip.idx
│   ├── data_4.zip
│   └── data_4.zip.idx
├── bully
├── libc++_shared.so
└── libGame.so
```

## Build

```bash
mkdir build
cd build
cmake ..
make
```

After building, copy the `bully` binary to the directory containing your game assets and shared libraries (`.so` files).

## Run

Navigate to the directory containing your game files and executable, then run:

```bash
./bully
```

## Legal

This repository does not contain any copyrighted game assets or proprietary libraries. You must own a legitimate copy of **Bully: Anniversary Edition** (Android v1.4.311) to extract and run the game files.
