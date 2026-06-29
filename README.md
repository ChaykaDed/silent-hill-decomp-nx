# Silent Hill 1 — Nintendo Switch Port

Port of the Silent Hill PC port targeting Nintendo Switch via devkitPro + libnx.

**Branch:** `feature/switch-port`

---

## Status

Build infrastructure is complete. The port compiles but has not been tested on hardware yet.

- [x] CMake + Ninja build for aarch64 (devkitPro)
- [x] SDL2, OpenAL, libjpeg-turbo (static linking)
- [x] OpenGL ES 3.0 via PsyCross
- [ ] Hardware testing on Switch
- [ ] All 42 maps (only map0_s00 compiled)
- [ ] Joy-Con / Pro Controller mapping
- [ ] FMV playback on ARM
- [ ] Save/load (memory card)

---

## Building

### Prerequisites

- [devkitPro](https://devkitpro.org) with Switch packages:
  ```
  dkp-pacman -S switch-dev switch-cmake ninja
  dkp-pacman -S switch-sdl2 switch-openal-soft switch-libjpeg-turbo
  ```
- CMake 3.16+, Ninja
- Git with submodule support

### Steps

```bash
git clone https://github.com/ChaykaDed/silent-hill-decomp-nx.git
cd silent-hill-decomp-nx
git checkout feature/switch-port
git submodule update --init --recursive

cd pc_port
./build_switch.sh rebuild
```

**Output:** `pc_port/build_switch/SilentHillPC.nro`

---

## Controls (Switch)

| PSX Button | Switch |
|------------|--------|
| Cross | A |
| Circle | B |
| Triangle | X |
| Square | Y |
| Start | + |
| Select | - |
| D-Pad | D-Pad |
| L1 / L2 | L / ZL |
| R1 / R2 | R / ZR |

---

## Game Data

Place a Silent Hill (PS1) disc dump (BIN format) in the `gamedata/` directory next to the NRO file.

---

## Project Structure

```
pc_port/                  Port layer (PC/Switch shims)
├── CMakeLists.txt        Build system
├── build_switch.sh       Switch build script
├── toolchains/           devkitPro cmake toolchain
├── PsyCross/             PSX hardware abstraction layer (submodule)
└── src/                  Stubs, reformatters, FMV player
src/                      Decompiled game code
├── bodyprog/             Core engine
└── maps/                 42 map overlays
include/                  Headers
```

---

## Credits

- Decompilation: [Vatuu/silent-hill-decomp](https://github.com/Vatuu/silent-hill-decomp)
- PC port: [SlickAmogus/silent-hill-decomp](https://github.com/SlickAmogus/silent-hill-decomp)
- PsyCross: [OpenDriver2/PsyCross](https://github.com/OpenDriver2/PsyCross)
- devkitPro: [devkitPro](https://devkitpro.org)

Silent Hill © Konami. No game assets are included in this repository.
