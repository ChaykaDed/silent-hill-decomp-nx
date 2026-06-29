# Silent Hill 1 — Nintendo Switch Port

Port of the Silent Hill PC port targeting Nintendo Switch via devkitPro + libnx.

**Branch:** `feature/switch-port`

---

## Status

| Component | Status |
|-----------|--------|
| Build (link) | ✅ Clean — 197/197 targets, 32 MB NRO |
| GLES context creation | ✅ SDL + EGL + GLES 3.0 |
| Audio (OpenAL) | ✅ Initializes |
| Runtime (Ryujinx) | ✅ Starts, ~55s lifespan, no null-pointer crash |
| Game data loading | ❌ Needs BIN dump in `gamedata/` |
| Map overlays | ❌ Not yet linked (static stubs) |
| Controller input | ❌ Not adapted |

### Known Issues

- `glClearDepthf` was crashing via null function pointer (fixed: replaced glad function-pointer dispatch with native GLES3 headers)
- Console `eprintf` output not visible in Ryujinx logs (goes through `svcOutputDebugString`)
- No game assets included — supply your own BIN dump

---

## Building

### Prerequisites

**devkitPro** (Windows recommended, also works on Linux/macOS):
- Download from https://devkitpro.org
- Install `devkitA64` + `libnx` + `portlibs`:
  ```bash
  dkp-pacman -S switch-dev switch-cmake ninja
  dkp-pacman -S switch-sdl2 switch-openal-soft switch-libjpeg-turbo
  ```
- Add `C:\devkitPro\tools\bin` and `C:\devkitPro\devkitA64\bin` to `PATH`
- CMake 3.16+, Ninja, Git with submodule support

### Clone

```bash
git clone https://github.com/ChaykaDed/silent-hill-decomp-nx.git
cd silent-hill-decomp-nx
git checkout feature/switch-port
git submodule update --init --recursive
```

### Build (Windows)

```powershell
cd pc_port
mkdir build_switch && cd build_switch
cmake .. -DCMAKE_TOOLCHAIN_FILE=..\toolchains\switch.cmake -DCMAKE_BUILD_TYPE=Release -DSH_SWITCH=1 -G Ninja
ninja
```

### Build (Linux)

```bash
cd pc_port
./build_switch.sh rebuild
```

### Output

`pc_port/build_switch/SilentHillPC.nro`

### Generating NRO from ELF

```bash
elf2nro build_switch/SilentHillPC build_switch/SilentHillPC.nro --nacp=build_switch/SilentHillPC.nacp
nacptool --create "Silent Hill" "ChaykaDed" "1.0" build_switch/SilentHillPC.nacp
```

---

## Graphics Notes

- Uses **GLES 3.0** via **native GLES3/gl3.h** headers (not glad function pointers)
- Switch-glad was generated for `gl=4.3` desktop profile — incompatible with GLES 3.0 runtime on Switch
- `GLAD_GL_VERSION_4_x` flags stay false on GLES (major=3), so functions like `glClearDepthf` never load through glad's loader
- Fix: include `GLES3/gl3.h` directly and skip glad loader entirely on `__SWITCH__`

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

## Technical Details

- **Cross-compiler:** devkitA64 GCC 15.2.0 (aarch64-none-elf)
- **SDK:** libnx + portlibs
- **GL:** Mesa NVN (drm_nouveau backend), GLES 3.0 via EGL
- **GL loader:** Native `GLES3/gl3.h` + `GLES3/gl3ext.h` (no glad)
- **Build system:** CMake + Ninja
- **Emulator:** Ryujinx (for development/testing)

---

## Credits

- Decompilation: [Vatuu/silent-hill-decomp](https://github.com/Vatuu/silent-hill-decomp)
- PC port: [SlickAmogus/silent-hill-decomp](https://github.com/SlickAmogus/silent-hill-decomp)
- PsyCross: [OpenDriver2/PsyCross](https://github.com/OpenDriver2/PsyCross)
- devkitPro: [devkitPro](https://devkitpro.org)

Silent Hill © Konami. No game assets are included in this repository.
