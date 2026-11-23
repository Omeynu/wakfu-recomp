# wakfu-recomp

[![Support on Patreon](https://img.shields.io/badge/Patreon-support%20the%20project-F96854?logo=patreon&logoColor=white)](https://www.patreon.com/Omeynu)

A static recompilation of **Islands of Wakfu** (Xbox Live Arcade, Xbox 360, title `584109AA`) to native PC code.
The game's PowerPC code is translated to C++ with a patched [XenonRecomp](https://github.com/hedge-dev/XenonRecomp);
its Direct3D calls are intercepted and rendered with Vulkan; the Xbox 360 kernel, audio (XMA) and input are
reimplemented in a runtime (SDL2).

**[Download the latest release](https://github.com/Omeynu/wakfu-recomp/releases/latest)** ·
**[Support the project on Patreon](https://www.patreon.com/Omeynu)**

> [!IMPORTANT]
> This project does not include any game assets. You must provide the files from your own legally acquired copy
> of the game (XBLA package) and its title update 2. The recompiled code is generated on your PC from those files
> and is never distributed.

This project is not affiliated with, endorsed or sponsored by Ankama or Microsoft. *Islands of Wakfu* and *Wakfu*
are trademarks of their respective owners. Donations support development time only: they never buy access to the
game, its files or any build.

**AI disclosure:** most of the code and documentation was written with the help of an AI coding assistant (Claude).
Each step is covered by the tests and checks described in [`docs/`](docs/) (French) and [`BUILDING.md`](BUILDING.md).

## Table of contents

- [System requirements](#system-requirements)
- [How to install](#how-to-install)
- [Features](#features)
- [Performance](#performance)
- [Problems](#problems)
- [Roadmap](#roadmap)
- [FAQ](#faq)
- [Building](#building)
- [Credits](#credits)

## System requirements

| | |
|---|---|
| OS | Windows 10/11 x64, or Linux x86_64 (missing packages installed automatically on Arch-based distributions) |
| GPU | Vulkan 1.3 (tested on AMD Radeon RX 6700 XT, RX 7900 XT, Vega 8 integrated) |
| Memory | at least 4 GB of free virtual memory (RAM + page file): the emulated console memory is committed at start |
| Disk | ~3 GB free (build tools, prepared game, build) |
| Game | Islands of Wakfu XBLA package + title update 2 (STFS packages, ISO/XISO image or extracted folder) |

## How to install

### With RecompLauncher

Add **Islands of Wakfu** in [RecompLauncher](https://recomplauncher.com) and give it a folder holding the game
package and the title update 2 package. The first start recompiles the game (a few minutes to an hour, depending on
the CPU); later starts are immediate.

### Manually

Pick the file for your system on the [releases page](https://github.com/Omeynu/wakfu-recomp/releases/latest):

| System | File | How to start it |
|---|---|---|
| Windows 10/11 x64 | `WakfuRecomp-<version>-windows-x64.exe` | double-click |
| Linux x86_64 | `WakfuRecomp-<version>-linux-x86_64` | `chmod +x` it, then run it |

1. Drop your game package and title update 2 package on the window (or use *Browse*). Each file is checked by
   content; an unknown version is refused with the reason.
2. Click *Recompile*, wait, then *Play*.

Windows SmartScreen or an antivirus may warn because the executable is not signed.
Settings, saves and logs are in `%APPDATA%\WakfuRecomp` (Windows) or `~/.local/share/wakfu-recomp` (Linux) and survive updates.

## Features

- Plays from boot to in-game: intro videos, menus, new game, first zone; save games and profile settings.
- Native rendering: Direct3D calls are translated to Vulkan (no GPU emulation), MSAA, optional AMD FSR 1 upscaling.
- Game language: French, English, German, Spanish, Italian, Japanese (or the system language).
- Controllers through SDL2 (hot-plug, rumble) and a remappable keyboard.
- Windowed, borderless and fullscreen modes; GPU selection; frame rate counter; skippable intro logos.
  All settings in `config.toml` or the launcher window ([`docs/options.md`](docs/options.md), French).
- One-file Windows installer and launcher that ships its own compiler toolchain: nothing else to install.

## Performance

First zone, 1280x720. Frame rate: median of the game's 5-second log samples.

| PC | Frame rate | GPU time per frame |
|---|---|---|
| Radeon RX 6700 XT, Linux | **60 fps** locked | 2.7 ms |
| Ryzen 7 5700U + Radeon Vega 8 (laptop), Linux | **60 fps** median, occasional drops to 33–40 | 5.5–6.7 ms |
| Radeon RX 7900 XT, Windows | **~45 fps** (31–60) — see [Problems](#problems) | 0.7 ms |

Below 60 fps the game runs in slow motion: it advances a fixed 1/60 s per frame, like on the console.
For comparison, the first versions emulated the Xbox 360 GPU and ran the same scene at 6 to 55 fps.
Details: [`docs/etat.md`](docs/etat.md#performances-actuelles).

## Problems

### Solved along the way

- **Recompilation**: no maintained XBLA package extractor on Linux (written for the project); XenonRecomp found 1 of
  147 jump tables (new detector) and could not translate 52 instructions used by the game (added and tested against
  QEMU); the time base ran at the host CPU frequency instead of 50 MHz, which froze the first zone's scripts.
- **Kernel**: a memory flag misread as "16 MB pages" exhausted memory when starting a game; a never-updated kernel
  clock froze videos; sound sources were appended instead of mixed, so audio stuttered and videos played at 1/4 speed.
- **Rendering**: emulating the Xbox 360 GPU capped the game near 30 fps (CPU and GPU waited on each other ~9 times
  per frame). The game's statically linked, symbol-less Direct3D was mapped function by function and replaced by
  native Vulkan rendering: 60 fps, and ~5,400 fewer lines of code.

### Current

- **Windows is slower than Linux** (~45 fps on a fast GPU that is almost idle). Suspected cause, to be confirmed:
  the emulated 60 Hz vertical sync relies on Windows' default 15.6 ms timer resolution.
- Short stutters while loading (title screen, start of cutscenes, save notice).
- Few tests on real Windows PCs so far; saving at an original checkpoint not yet verified by playing through;
  no online features (the service no longer exists); Linux package for Arch-based distributions only; no macOS.

Full list (French): [`docs/etat.md`](docs/etat.md#problèmes-actuels). Reports are welcome:
[open an issue](https://github.com/Omeynu/wakfu-recomp/issues/new/choose) with `%APPDATA%\WakfuRecomp\journal.txt`.

## Roadmap

1. **In progress**: Windows frame pacing, laptop CPU usage, 1440p rendering (internal resolution x1–x4).
2. **Quick options**: anisotropic filtering, mute when unfocused, stick dead zone, rumble strength, screenshot key,
   monitor choice and remembered window position, automatic save backups, quality presets.
3. **Image**: MSAA levels (2x/4x/8x), higher-resolution shadows.
4. **In-game checks**: local co-op with two controllers, voice languages, full-screen effects toggles, audio channels.
5. **Bigger options**: controller remapping, in-game performance overlay, achievement notifications, transparency
   anti-aliasing, Xbox/PlayStation button icons, color-blind filters.
6. **Later**: 5.1/7.1 surround, mod support (file replacement from a mods folder), higher than 60 fps.
7. **RecompLauncher**: settings in the launcher, then full install with progress bar.

## FAQ

**Is this legal?** The repository contains only original code, documentation and third-party code under open source
licenses. No game file, extracted asset or code generated from the game is distributed: you need your own copy,
and everything game-specific is produced on your PC. This is the same model as other recompilation projects.

**Why do I need the title update?** The recompiled code targets the game patched with title update 2; the files are
checked by SHA-1 before recompiling.

**Why does the first start take so long?** The game's code is recompiled and compiled on your PC, because the
generated C++ is derived from the game and cannot be distributed.

**Where are the logs?** `%APPDATA%\WakfuRecomp\journal.txt` on Windows,
`~/.local/share/wakfu-recomp/userdata/journal.txt` on Linux.

## Building

See [`BUILDING.md`](BUILDING.md).

## Credits

- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) —
  PowerPC and Xenos shader recompilers.
- [Xenia](https://github.com/xenia-project/xenia) — texture layout code adapted in `runtime/third_party/xenia`.
- [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp) — architecture reference.
- AMD FidelityFX FSR 1, SDL2, FFmpeg (XMA2 decoder only), glslang, DirectXShaderCompiler, Dear ImGui, toml++, zstd,
  llvm-mingw — see [`packaging/windows/LICENCES-TIERS.txt`](packaging/windows/LICENCES-TIERS.txt).

## License

[GPL-3.0](COPYING), except `runtime/third_party/`, which keeps its own licenses (BSD, MIT).
Islands of Wakfu and its assets belong to their respective owners.
