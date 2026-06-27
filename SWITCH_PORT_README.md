# Silent Hill 1 - Nintendo Switch Port

Status: Work in Progress - Feature Branch: feature/switch-port

This is a port of the original Silent Hill 1 (PS1) to Nintendo Switch, built on the basis of decompilation and PC port.

Note: This build infrastructure was created with AI assistance using GitHub Copilot.

## Contents

- [System Requirements](#system-requirements)
- [Installation](#installation)
- [Building](#building)
- [Project Structure](#project-structure)
- [What Changed for Switch](#what-changed-for-switch)
- [Troubleshooting](#troubleshooting)
- [Resources](#resources)

---

## System Requirements

### Required
- devkitPro with Switch (NX) support
- CMake 3.16+
- Ninja (for fast builds)
- Git with submodule support

### On Switch
- 4 GB RAM
- Tegra GPU (built-in to every Switch)
- 8 GB free storage for the game

---

## Installation

### Step 1: Install devkitPro

#### Windows
```bash
# Download installer from
# https://github.com/devkitPro/installer/releases

# Run the installer, select:
# > Switch Development
# > Build Tools
# > libnx
```

#### Linux / macOS
```bash
# Follow official instructions
https://devkitpro.org/wiki/Getting_Started

# On macOS via Homebrew (additional):
brew install devkitpro-meta-switch
```

### Step 2: Install Packages

```bash
# Main tools
dkp-pacman -S switch-dev
dkp-pacman -S switch-cmake
dkp-pacman -S ninja

# Libraries
dkp-pacman -S switch-sdl2
dkp-pacman -S switch-openal-soft
dkp-pacman -S switch-libjpeg-turbo
```

### Step 3: Clone Repository

```bash
# Main fork
git clone https://github.com/ChaykaDed/silent-hill-decomp-nx.git
cd silent-hill-decomp-nx

# Switch port branch
git checkout feature/switch-port

# Initialize submodules (PsyCross)
git submodule update --init --recursive
```

### Step 4: Verify Installation

```bash
# Check DEVKITPRO
echo $DEVKITPRO
# Result: /opt/devkitpro (or other path)

# Check compiler
$DEVKITPRO/devkitARM/bin/aarch64-none-elf-gcc --version
# Result: gcc 14.x.x (arm-none-eabi)
```

---

## Building

### Quick Start

```bash
cd pc_port
bash build_switch.sh rebuild
```

This will take 30-60 minutes on first run (depends on CPU).

### Detailed Build

```bash
cd pc_port

# 1. Configuration (run once)
bash build_switch.sh configure

# 2. Build
bash build_switch.sh build

# 3. Clean (if needed)
bash build_switch.sh clean

# 4. Full rebuild
bash build_switch.sh rebuild
```

### Build Output

```
pc_port/build_switch/
├── SilentHillPC          <- Executable (ELF)
├── SilentHillPC.nro      <- Switch cartridge (NRO)
└── ... (object files)
```

SilentHillPC.nro is the file you copy to Switch.

---

## Project Structure

```
silent-hill-decomp-nx/
├── SWITCH_BUILD_GUIDE.md          <- Detailed guide
├── SWITCH_PORT_README.md          <- THIS FILE
│
├── pc_port/
│   ├── CMakeLists.txt             <- Updated for Switch
│   ├── build_switch.sh            <- Build script
│   ├── toolchains/
│   │   └── switch.cmake           <- CMake toolchain for devkitPro
│   ├── src/                       <- PC-specific code
│   ├── include/                   <- PC-specific headers
│   └── PsyCross/                  <- PSX emulator (submodule)
│
├── src/
│   ├── bodyprog/                  <- Main game engine code
│   ├── maps/                      <- Maps (only map0_s00 for now)
│   ├── screens/                   <- Menu screens
│   └── ...
│
└── include/
    ├── decomp/                    <- Decompilation headers
    └── psyq_compat/               <- PSX compatibility
```

---

## What Changed for Switch

### Platform Detection

```cmake
# CMakeLists.txt (lines 28-35)
if(CMAKE_SYSTEM_NAME STREQUAL "Generic" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    set(SH_SWITCH ON)
    add_compile_definitions(__SWITCH__ RENDERER_OGLES OGLES_VERSION=3)
endif()
```

### Compiler

| PC | Switch |
|----|--------|
| x86_64 GCC | ARM64 (aarch64-none-elf) |
| -march=native | -march=armv8-a -mtune=cortex-a57 |
| OpenGL 3.3 | OpenGL ES 3 |

### Libraries

| Component | PC | Switch |
|-----------|----|----|
| Window/Input | SDL2 (dynamic) | SDL2 (static) |
| Rendering | OpenGL 3.3 | OpenGL ES 3 (EGL) |
| Audio | OpenAL | OpenAL (switch version) |
| Video | libjpeg-turbo | libjpeg-turbo (switch) |

### Output

- PC: SilentHillPC.exe (Windows) or SilentHillPC (Linux/macOS)
- Switch: SilentHillPC.nro (Nintendo Runtime Object)

---

## Commit Log

```
71c31f43 - Add Switch/NX CMake toolchain for devkitPro
275b3196 - Add Switch/NX platform support to CMakeLists
2a9e0cae - Add Switch build script for devkitPro
58284cda - Add Nintendo Switch build guide
```

All commits are in branch feature/switch-port.

---

## Troubleshooting

### DEVKITPRO not set

Error:
```
Error: DEVKITPRO not set
```

Solution:
```bash
# Linux/macOS
export DEVKITPRO=/opt/devkitpro
export PATH=$DEVKITPRO/devkitARM/bin:$PATH

# Make permanent (add to ~/.bashrc or ~/.zshrc)
echo 'export DEVKITPRO=/opt/devkitpro' >> ~/.bashrc
source ~/.bashrc
```

Windows (MSYS2):
- Reinstall devkitPro from installer
- Or set manually in System Environment Variables

### aarch64-none-elf-gcc not found

Error:
```
Error: aarch64-none-elf-gcc not found
```

Solution:
```bash
dkp-pacman -S switch-gcc
dkp-pacman -S switch-binutils
```

### SDL2 / OpenAL / libjpeg not found

Error:
```
Could not find SDL2
Could not find OpenAL
Could not find JPEG
```

Solution:
```bash
# Install all portlibs for Switch
dkp-pacman -S switch-sdl2
dkp-pacman -S switch-openal-soft
dkp-pacman -S switch-libjpeg-turbo
dkp-pacman -S switch-glm
```

### CMake errors

Error:
```
cmake: command not found
ninja: command not found
```

Solution:
```bash
dkp-pacman -S cmake
dkp-pacman -S ninja
```

### Compilation errors (undefined reference)

Usually caused by: PsyCross not found

Solution:
```bash
# Check submodules
git submodule update --init --recursive
ls -la PsyCross/CMakeLists.txt
# File should exist
```

### Build is slow

- First time: normal (entire codebase compiles)
- Subsequent times: only changed files
- Delete build_switch/ for full rebuild

```bash
bash build_switch.sh clean
bash build_switch.sh rebuild
```

---

## Next Steps

1. DONE - Compilation infrastructure
2. TODO - Testing on Switch hardware
3. TODO - Optimization for 720p handheld display
4. TODO - Complete map coverage (not just map0_s00)

---

## Resources

- devkitPro: https://devkitpro.org
- libnx API: https://libnx.readthedocs.io
- SDL2 for Switch: https://github.com/devkitPro/SDL2
- PsyCross (PSX HAL): https://github.com/OpenDriver2/PsyCross
- Silent Hill Decomp: https://github.com/SlickAmogus/silent-hill-decomp
- OpenGL Tutorials: https://learnopengl.com

---

## License

This port is based on:
- Silent Hill Decomp (GPL-3.0 decompilation code)
- PsyCross (Apache 2.0)
- devkitPro tools (GPL-2.0+)

---

## Contributing

The feature/switch-port branch is open for experiments!

If you have improvements:
1. Fork the repository
2. Create a feature branch
3. Push and create a PR

---

## Questions?

- Read SWITCH_BUILD_GUIDE.md for details
- GitHub Issues for bugs
- Check Troubleshooting section above

---

Good luck with the port!

Last updated: 2026-06-27
Build infrastructure created with GitHub Copilot AI assistance
