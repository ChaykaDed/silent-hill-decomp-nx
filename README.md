# Silent Hill 1 — Nintendo Switch Port

This is a port of the Silent Hill 1 PC decompilation to Nintendo Switch, built with devkitPro and libnx. Everything lives in the `feature/switch-port` branch.

## Building

You'll need devkitPro with devkitA64, libnx, and the Switch portlibs:

```
dkp-pacman -S switch-dev switch-cmake ninja
dkp-pacman -S switch-sdl2 switch-openal-soft switch-libjpeg-turbo
```

Then clone and build:

```
git clone https://github.com/ChaykaDed/silent-hill-decomp-nx.git
cd silent-hill-decomp-nx
git checkout feature/switch-port
git submodule update --init --recursive
cd pc_port
mkdir build_switch && cd build_switch
cmake .. -DCMAKE_TOOLCHAIN_FILE=..\toolchains\switch.cmake -DCMAKE_BUILD_TYPE=Release -G Ninja
ninja
elf2nro SilentHillPC SilentHillPC.nro --nacp=SilentHillPC.nacp
python3 ../embed_icon.py SilentHillPC.nro ../SH1icon.png
```

On Linux, just run `./build_switch.sh rebuild` from `pc_port/`. There's also `build_switch.bat` for Windows.

## Game Data

Drop a Silent Hill PS1 BIN dump in `gamedata/` next to the NRO. On Ryujinx that's `%APPDATA%\Ryujinx\sdcard\gamedata\` by default.

## How It Works

The renderer uses GLES 3.0 through native `GLES3/gl3.h` headers instead of glad function pointers — the glad loader was generated for desktop GL 4.3 and didn't play nice with GLES 3.0 at runtime. The GL driver is Mesa NVN over EGL.

FMV video runs through the game's existing decoder with a shader version fix (`#version 300 es` for GLES). Audio uses audout directly instead of SDL audio, since OpenAL already claims the only SDL audio device available on Switch.

## Status

Links clean: 197/197 targets, 32 MB NRO. Boots in Ryujinx — GLES context comes up, logos display, main menu works, FMVs play with sound. In-game rendering and gameplay still need testing.

## Credits

- Decompilation: https://github.com/Vatuu/silent-hill-decomp
- PC port: https://github.com/SlickAmogus/silent-hill-decomp
- PsyCross: https://github.com/OpenDriver2/PsyCross
- devkitPro: https://devkitpro.org

Silent Hill (c) Konami. This repository contains no game assets.
