# wakfu-recomp

A static recompilation of **Islands of Wakfu** (Xbox Live Arcade, Xbox 360, title ID `584109AA`) to native
PC code. The game's PowerPC code is translated to C++ with a patched
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp), then linked against a hand-written runtime that
emulates the Xbox 360 kernel, GPU (Vulkan), audio (XMA) and input (SDL2).

> **No game files are included or distributed.** You need your own copy of the game (XBLA package) and its
> title update 2. Everything game-specific, including the recompiled C++, is generated on your machine.

This project is not affiliated with Ankama or Microsoft.

## Status

Early, but playable:

- Boots, plays the intro videos, reaches the menus and starts a new game; the first zone is playable.
- Save games and profile settings are written and read back.
- Controller (SDL2, with rumble) and keyboard; XMA audio; window, borderless and fullscreen modes;
  optional AMD FSR 1 upscaling.
- Windows x64 (main target) and Linux.
- Frame rate: about 30–55 fps in game on a Radeon RX 6700 XT. Below 60 fps the game runs in slow motion
  (fixed 1/60 s step per frame).

Known gaps and details: [`docs/etat.md`](docs/etat.md).

## Playing on Windows

1. Download `WakfuRecomp.exe` from the [Releases](https://github.com/Omeynu/wakfu-recomp/releases) page. It contains the build tools and the
   runtime sources only (no game data); Windows SmartScreen will warn because the file is not signed.
2. Run it and give it your copy of the game and of title update 2 (STFS package, ISO/XISO image or extracted
   folder; drag and drop works). Files are checked by content and SHA-1; an unknown version is refused.
3. It recompiles the game locally (a few minutes to an hour, ~1.5 GB of disk) and then shows a **Play** button.

Settings live in `%APPDATA%\WakfuRecomp\config.toml` (also editable from the launcher).
Bug reports: open an issue with `journal.txt`, a screenshot and what you were doing — see
[`docs/tester-sur-pc.md`](docs/tester-sur-pc.md).

## Building from source

- Windows installer (built on Linux, pinned and SHA-256-checked downloads):
  `packaging/windows/build-installer.sh <build dir>` — see [`docs/build-windows.md`](docs/build-windows.md).
- Linux, step by step (Clang ≥ 18, CMake, Ninja, Python 3):

```sh
mkdir -p ~/wakfu-work && ln -s ~/wakfu-work work           # outside the repo; `work` is git-ignored

# XenonRecomp at the commit pinned in tools.lock, with this project's patch
git clone --recursive https://github.com/hedge-dev/XenonRecomp work/tools/XenonRecomp
git -C work/tools/XenonRecomp checkout <commit from tools.lock>
git -C work/tools/XenonRecomp apply "$PWD/patches/xenonrecomp-wakfu.patch"
CC=clang CXX=clang++ cmake -S work/tools/XenonRecomp -B work/tools/XenonRecomp/build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C work/tools/XenonRecomp/build

# Prepare the game: extracts STFS/ISO/folder, applies the title update
CC=clang CXX=clang++ cmake -S tools-src/wakfu-prepare -B work/build-prepare -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DXENONRECOMP_DIR=$PWD/work/tools/XenonRecomp && ninja -C work/build-prepare
work/build-prepare/wakfu-prepare -o work/game <your game files>

# Generate the C++ (empty work/out/ppc first: XenonRecomp does not delete stale files)
work/tools/XenonRecomp/build/XenonRecomp/XenonRecomp config/wakfu.toml work/tools/XenonRecomp/XenonUtils/ppc_context.h

# Kernel import stubs, then the runtime (S = work/tools/XenonRecomp)
S=$PWD/work/tools/XenonRecomp
python3 -I runtime/tools/gen_imports.py work/game/patched/default.xex work/game/patched/patched.txt $S/XenonUtils/xbox work/gen
CC=clang CXX=clang++ cmake -S runtime -B work/build-runtime -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DPPC_DIR=$PWD/work/out/ppc -DSIMDE_DIR=$S/thirdparty/simde -DGEN_DIR=$PWD/work/gen
cmake --build work/build-runtime --target wakfu
```

On Arch-based distributions, `packaging/linux/build-paquet.sh` produces a package whose `installer.sh` does all
of the above (see `packaging/linux/LISEZMOI.txt`).

Details of each step and how it was verified: [`docs/methode.md`](docs/methode.md).

## Repository layout

| Path | Contents |
|---|---|
| `runtime/` | The runtime: kernel/XAM emulation, PM4 command processor, Vulkan renderer, audio, input, tests |
| `config/` | XenonRecomp configuration and jump tables (`switches_manual.toml`: hand-checked cases) |
| `patches/xenonrecomp-wakfu.patch` | Instructions and fixes added to XenonRecomp for this game |
| `tools-src/wakfu-prepare/` | Game preparation: STFS / ISO-XISO / folder → base, title update, patched XEX |
| `tools-src/wakfu-installer/`, `packaging/` | Windows one-file installer and launcher; Linux package |
| `scripts/` | Analysis helpers: STFS extraction, XEX analysis, jump tables, helper functions |
| `tests/insn/` | Differential test of the added instructions (QEMU vs recompiled C++) |
| `check/` | Compile and link check of the generated C++ |
| `tools.lock` | Pinned tool commits |

Documentation in `docs/` is written in French.

## Credits

- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) (MIT) — PowerPC to C++ recompiler.
- [Xenia](https://github.com/xenia-project/xenia) (BSD) — shader microcode analysis files in `runtime/third_party/xenia`.
- [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp) — architecture reference (no code copied).
- AMD FidelityFX FSR 1 (MIT), SDL2, FFmpeg (LGPL, XMA2 decoder only), glslang, Dear ImGui, toml++, zstd,
  llvm-mingw — see `packaging/windows/LICENCES-TIERS.txt`.

## License

The code in this repository is under the [MIT license](LICENSE), except `runtime/third_party/`, which keeps
its own licenses. Islands of Wakfu and its assets belong to their respective owners.
