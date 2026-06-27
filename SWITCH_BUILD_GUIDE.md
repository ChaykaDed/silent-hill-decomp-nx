# Silent Hill - Nintendo Switch Port Build Guide

## Overview

This guide explains how to build Silent Hill for Nintendo Switch using devkitPro.

## Prerequisites

### 1. Install devkitPro

**Windows:**
- Download from: https://github.com/devkitPro/installer/releases
- Run the installer and follow the setup wizard
- Select "Switch" when asked which platforms to install

**Linux/macOS:**
- Follow: https://devkitpro.org/wiki/Getting_Started

### 2. Install Switch Development Packages

```bash
dkp-pacman -S switch-dev
dkp-pacman -S switch-sdl2
dkp-pacman -S switch-openal-soft
dkp-pacman -S switch-libjpeg-turbo
dkp-pacman -S ninja
dkp-pacman -S cmake
```

### 3. Verify Installation

```bash
echo $DEVKITPRO
# Should output your devkitPro installation path (e.g., /opt/devkitpro)

ls $DEVKITPRO/devkitARM/bin/aarch64-none-elf-gcc
# Should show the compiler exists
```

## Building

### Quick Build

```bash
cd silent-hill-decomp-nx/pc_port
bash build_switch.sh rebuild
```

### Step-by-step Build

**1. Configure (one-time setup):**
```bash
cd silent-hill-decomp-nx/pc_port
bash build_switch.sh configure
```

**2. Build:**
```bash
bash build_switch.sh build
```

**3. Output:**
```
build_switch/SilentHillPC.nro
```

### Build Commands

- `bash build_switch.sh configure` - Initial configuration
- `bash build_switch.sh build` - Build (configures if needed)
- `bash build_switch.sh clean` - Remove build directory
- `bash build_switch.sh rebuild` - Clean + build

## Troubleshooting

### DEVKITPRO not set

```bash
# On Linux/macOS:
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=$DEVKITPRO/devkitARM
export PATH=$DEVKITARM/bin:$PATH

# Add to ~/.bashrc or ~/.zshrc to make permanent
```

**Windows (MSYS2):**
- devkitPro installer sets this automatically
- If not, set in System Environment Variables or MSYS2 profile

### Missing packages

```bash
# Check installed packages:
dkp-pacman -Q

# Reinstall a package:
dkp-pacman -S switch-sdl2
```

### Compiler errors

**"aarch64-none-elf-gcc not found"**
```bash
dkp-pacman -S switch-gcc
```

**"SDL.h not found"**
```bash
dkp-pacman -S switch-sdl2
```

**"libjpeg not found"**
```bash
dkp-pacman -S switch-libjpeg-turbo
```

### CMake errors

**"Could not find CMake"**
```bash
dkp-pacman -S cmake ninja
```

**"Ninja not found"**
```bash
dkp-pacman -S ninja
```

## Changes Made for Switch Support

### 1. CMake Toolchain (`pc_port/toolchains/switch.cmake`)
- Configures ARM64 compiler settings for Switch
- Sets up devkitPro library paths
- Defines `__SWITCH__` and `RENDERER_OGLES`

### 2. Build Script (`pc_port/build_switch.sh`)
- Automated configure/build process
- Error checking for prerequisites
- Colored output for clarity

### 3. CMakeLists.txt modifications
- Platform detection (Switch vs PC)
- Conditional library linking
- Output name: `SilentHillPC.nro`
- Disabled map DLLs (not applicable on Switch)

## Next Steps

1. **Obtain game data:**
   - Place your Silent Hill (USA) disc image at `pc_port/build_switch/gamedata/Silent Hill (USA).bin`

2. **Install on Switch:**
   - Copy `SilentHillPC.nro` to your Switch homebrew directory
   - Use a compatible mod manager or place in the applications folder

3. **Testing:**
   - Boot the game and verify graphics render correctly
   - Test controller input
   - Monitor performance

## Known Limitations

- Map overlays (DLLs) are not supported on Switch
- Only starting map (map0_s00) is included
- Rendering scaled to Switch resolution (1280x720 handheld, 1920x1080 docked)
- Audio may require optimization for Switch audio hardware

## Performance Notes

- Switch: ARM64, 4GB RAM, Tegra custom GPU
- Original PS1: 34 MB RAM, 4 MB VRAM
- Expected: 30 FPS target, 1280x720 resolution

## Resources

- devkitPro: https://devkitpro.org
- libnx: https://github.com/switchbrew/libnx
- PsyCross: https://github.com/OpenDriver2/PsyCross
- Silent Hill Decomp: https://github.com/SlickAmogus/silent-hill-decomp
