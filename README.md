# Ferrari Virtual Race - macOS Static Recompilation Port

A native macOS (`x86_64`) port of *Ferrari Virtual Race* (2009) created by statically recompiling the original 32-bit Windows executable (`FerrariVR_Hi.exe`) into C and pairing it with a custom Win32 / DirectX host runtime built on SDL3 and OpenGL 4.1 Core.

![In-game screenshot on Mugello](media/gameplay.jpg)

> **Note:** This repository contains no original game binaries, copyrighted code, or game assets. You must supply your own copy of *Ferrari Virtual Race* to generate the recompiled C sources and run the game.

## Screenshots

| Main Menu | Car Selection |
| --- | --- |
| ![Main Menu](media/first_render_menu.png) | ![Car Selection](media/first_render_car_select.png) |

## Quick Start

You need your own copy of *Ferrari Virtual Race* (the Windows release). Nothing from the game ships with this repository.

```sh
brew install sdl3 pkg-config sevenzip
git clone https://github.com/BlackLineInteractive/FerrariVirtualRace-MacOS-Port.git
cd FerrariVirtualRace-MacOS-Port
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt

# 1. unpack the Windows installer (or use an already installed game folder)
tools/extract_game.sh ~/Downloads/ferrari-virtual-race-1-0-en-win.exe ../extracted

# 2. recompile the game and build the macOS app
make gen EXE=../extracted/FerrariVR_Hi.exe
make -j
make app GAME=../extracted      # -> build/FerrariVR.app
make install                    # optional: copy it to /Applications
```

Double-click `FerrariVR.app`. It finds the game folder by itself when it sits next to the app (or at `../extracted` from the repository); otherwise it asks for the folder once and remembers it. `make app GAME=<folder> EMBED=1` copies the game files into the bundle for a fully self-contained app on your own Mac. Do not share such a bundle, because it contains the game.

## Graphics Options

All settings are in the game's own *Video settings* menu.

| Setting | What the port adds |
| --- | --- |
| **Antialias** | `FXAA` (post filter), `MSAA 2x / 4x / 8x` (real multisampling of the HDR scene, alpha-to-coverage for alpha-tested foliage), `FSR 1` (MSAA 4x + AMD FidelityFX Super Resolution 1 upscaling to the Retina pixel size and RCAS sharpening) |
| **Resolution / Window size** | Every common size up to 2560x1440 (2K), 3840x2160 (4K) and the display's native Retina resolution. Sizes larger than the screen are scaled down to fit, which works as supersampling |
| **Shadow detail** | Hardware shadow maps (depth-compare PCF) like the original Windows drivers |

Changing video settings recreates the device in-game; the port keeps running.

## How It Works

1. **Function Discovery (`ghidra/`)** - [ExportFuncs.java](ghidra/ExportFuncs.java) exports function boundaries and computed-jump targets from `FerrariVR_Hi.exe` into [funcs_hi.jsonl](ghidra/funcs_hi.jsonl).
2. **Static Recompilation (`tools/`)** - [recomp.py](tools/recomp.py) and [fpu.py](tools/fpu.py) lift x86-32 integer, x87, SSE/SSE2, and MMX instructions into C functions over the `CPU` state struct defined in [cpu.h](runtime/cpu.h). Direct calls become C calls, intra-function branches become `goto`, flags are computed lazily by liveness analysis, and indirect calls/jumps go through `call_indirect()`. [hooks.txt](tools/hooks.txt) replaces single guest instructions with calls into the runtime (for example the Antialias menu labels).
3. **Native Runtime (`runtime/`)** - Maps a 4 GiB guest address space via `mmap`, loads the original PE sections from `FerrariVR_Hi.exe` at runtime, patches IAT slots to host thunks, and implements the required Windows and DirectX APIs natively:
   - **Graphics** - Direct3D 9 on OpenGL 4.1 Core (`d3d9.cpp`): state and binding caches, MSAA render-target shadows, hardware shadow maps, instancing. Fixed-function GLSL generation (`d3d9_ff.cpp`), Shader Model 2.x/3.0 bytecode-to-GLSL 4.10 translator (`d3d9_shader.cpp`), FXAA / FSR 1 post passes (`d3d9_post.cpp`), D3DX9 helpers via ImageIO/CoreGraphics (`d3dx9.cpp`), DirectDraw memory reporting (`ddraw.cpp`).
   - **Windowing & Input** - `user32` / `gdi32` (`user32.cpp`) and DirectInput 8 (`dinput8.cpp`) implemented over SDL3 for keyboard, mouse, and gamepads/wheels.
   - **Audio** - DirectSound 8 software mixer (`dsound.cpp`) outputting 48 kHz float stereo via SDL3.
   - **OS & Filesystem** - `kernel32`, `advapi32`, `shell32`, `shlwapi`, `ole32`, and `winmm` replacements (`kernel32.cpp`, `heap.cpp`, `seh.cpp`), including case-insensitive path resolution and a copy-on-write user overlay under `~/Library/Application Support/FerrariVirtualRace`.

## Repository Layout

```text
.
├── Makefile          - gen (recompile), build, app, install, shadertest
├── ghidra/           - Ghidra headless export script and exported function map
├── media/            - Screenshots of the native macOS build
├── packaging/        - Info.plist template for the app bundle
├── runtime/          - C++ Win32, Direct3D 9, DirectInput 8 and DirectSound 8 runtime
├── tests/            - Standalone shader translation and GL compilation test
├── tools/            - Static recompiler, instruction hooks, app bundler, installer extractor
└── docs/ROADMAP.md   - What is planned next
```

`gen/` (recompiled C, about 95 MB) and `build/` are generated and not tracked.

## Prerequisites

- An Intel Mac, or Apple Silicon with Rosetta 2 (the binary is `x86_64`)
- Xcode Command Line Tools (`clang`, `clang++`, `make`)
- [SDL3](https://libsdl.org/) via `pkg-config` (`brew install sdl3 pkg-config`)
- Python 3 for the recompiler, 7-Zip (`brew install sevenzip`) to unpack the installer
- The original *Ferrari Virtual Race* files (`FerrariVR_Hi.exe` and the asset folders)

[Makefile](Makefile) targets `-arch x86_64 -mmacosx-version-min=26.0`. Adjust `ARCH` for an older macOS SDK.

## Running From the Terminal

```sh
FVR_GAME_DIR=/path/to/game ./build/fvr
```

On first launch, [config.cpp](runtime/config.cpp) writes a default windowed `Config.ini` under `~/Library/Application Support/FerrariVirtualRace/docs/Synetic/FerrariVR/`. Files the game saves go to `overlay/` and `docs/` there; the game folder is never modified. When started as an app, the log is written to `~/Library/Application Support/FerrariVirtualRace/fvr.log`.

### Environment Variables

| Variable | Purpose |
| --- | --- |
| `FVR_GAME_DIR` | Game folder (skips auto-detection) |
| `FVR_AA` | Force anti-aliasing: `off`, `fxaa`, `msaa2`, `msaa4`, `msaa8`, `fsr` |
| `FVR_FPS=1` | Print FPS once a second and explain slow frames |
| `FVR_GL_MT=0` | Disable Apple's multithreaded GL engine |
| `FVR_KEYS` | Scripted key presses `ms:Key[:holdms],...`; the real keyboard and mouse are ignored while set. `9000:Return,12000:Return,15000:Return,18000:Return,21000:Return,24000:Return` drives the menus into a race |
| `FVR_SHOT_DIR` + `FVR_SHOT_FRAMES` / `FVR_SHOT_SECS` | Dump the back buffer as PPM at given frame numbers or seconds; `FVR_SHOT_POST=1` also dumps the window after post-processing |
| `FVR_TRACE=1`, `FVR_D3D_TRACE=1` | Log Win32 / Direct3D calls (slow) |
| `FVR_RT_LOG=N` | Log render-target switches during frame N |

## Testing & Utilities

- **Shader Translation Test** - translates all D3D9 shaders embedded in `FerrariVR_Hi.exe` to GLSL and compiles them with macOS OpenGL 4.1 Core:

  ```sh
  make build/shadertest
  ./build/shadertest /path/to/FerrariVR_Hi.exe
  ```

- **Inspection Scripts (`tools/`)** - PE layout ([peinfo.py](tools/peinfo.py)), embedded shader finder ([find_shaders.py](tools/find_shaders.py)), quick disassembly ([x86dis.py](tools/x86dis.py)).

## Roadmap

See [docs/ROADMAP.md](docs/ROADMAP.md): more Ferrari models, higher track detail and new tracks, and what temporal upscalers (FSR 2/3) and screen-space reflections would need.

## Support & Links

If you would like to support the project or follow updates:

- Ko-fi - <https://ko-fi.com/blacklineinteractive>
- Patreon - <https://www.patreon.com/BlacklineInteractive>
- YouTube - <https://youtube.com/@blacklineinteractive>
- X - <https://x.com/demonv559>
- Instagram - <https://www.instagram.com/blacklineinteractive>
- TikTok - <https://www.tiktok.com/@blacklineinteractive>
- LinkedIn - <https://linkedin.com/in/blacklineinteractive>

## License

This project is licensed under the GNU General Public License v3.0 - see the [LICENSE](LICENSE) file for details.
