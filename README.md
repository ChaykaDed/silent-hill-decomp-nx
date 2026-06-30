# Silent Hill 1 — Nintendo Switch Port

Branch `feature/switch-port`. Port of the PC port to Nintendo Switch via devkitPro + libnx.

## Building

### Prerequisites

- devkitPro with devkitA64, libnx, portlibs
- CMake 3.16+, Ninja
- Git with submodule support

```
dkp-pacman -S switch-dev switch-cmake ninja
dkp-pacman -S switch-sdl2 switch-openal-soft switch-libjpeg-turbo
```

### Clone

```
git clone https://github.com/ChaykaDed/silent-hill-decomp-nx.git
cd silent-hill-decomp-nx
git checkout feature/switch-port
git submodule update --init --recursive
```

### Build

```
cd pc_port
mkdir build_switch && cd build_switch
cmake .. -DCMAKE_TOOLCHAIN_FILE=..\toolchains\switch.cmake -DCMAKE_BUILD_TYPE=Release -G Ninja
ninja
elf2nro SilentHillPC SilentHillPC.nro --nacp=SilentHillPC.nacp
```

On Linux: `./build_switch.sh rebuild` from `pc_port/`.

### Game Data

Place a Silent Hill PS1 BIN dump in `gamedata/` next to the NRO, or under `sdmc:/gamedata/` on the emulated Switch SD card (Ryujinx maps this to `%APPDATA%\Ryujinx\sdcard\` by default).

## Technical Notes

- GLES 3.0 via native `GLES3/gl3.h` (no glad function pointers)
- glad loader was generated for `gl=4.3` desktop — incompatible with GLES 3.0 at runtime
- Cross-compiler: devkitA64 GCC 15.2.0 (aarch64-none-elf)
- GL driver: Mesa NVN over EGL

## Status

Full clean link (197/197 targets, 32 MB NRO). Boots in Ryujinx, creates GLES context, audio works. In-game rendering and gameplay not yet verified.

## Credits

- Decompilation: https://github.com/Vatuu/silent-hill-decomp
- PC port: https://github.com/SlickAmogus/silent-hill-decomp
- PsyCross: https://github.com/OpenDriver2/PsyCross
- devkitPro: https://devkitpro.org

Silent Hill © Konami. No game assets included in this repository.
