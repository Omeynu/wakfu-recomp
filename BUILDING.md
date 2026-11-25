# Building wakfu-recomp

You need your own copy of Islands of Wakfu (XBLA package, title ID `584109AA`) and its title update 2.
No game file is ever needed at build time of the Windows installer; it is only needed to build and run the game.

## Windows installer (`WakfuRecomp.exe`)

Built on Linux x86_64 (this is what the release workflow does). Requirements: cmake ≥ 3.20, ninja, make, curl,
python3, zstd, git. Every download is pinned and checked by SHA-256 (`packaging/windows/deps.lock`).

```sh
packaging/windows/build-installer.sh <build dir>
packaging/recomplauncher/package.sh <build dir>/WakfuRecomp.exe <version> <output dir>   # release zip
```

The release workflow (`.github/workflows/release.yml`) runs these two commands on every `v*` tag.

Details (French): [`docs/build-windows.md`](docs/build-windows.md).

## Linux

The Linux launcher is the same program as `WakfuRecomp.exe`. It is built by:

```sh
packaging/linux/build-paquet.sh <output dir>    # -> WakfuRecomp-linux.tar.zst
```

Host requirements: git, tar, zstd, curl, python3, clang ≥ 19 (`--embed-dir`), cmake, ninja, zlib and Vulkan headers, X11/Wayland and audio development headers (the pinned SDL3 is built statically into the launcher), network access
(XenonRecomp, XenosRecomp and DXC at the versions in `tools.lock`; zstd, Dear ImGui and Inter at the versions in
`packaging/windows/deps.lock`). Extract the archive and run `WakfuRecomp/WakfuRecomp`: the window guides you
through choosing the game files, recompiling and playing. On Arch-based distributions it can install the missing
runtime packages (`clang cmake ninja pkgconf vulkan-headers vulkan-icd-loader glslang ffmpeg`) with pacman; elsewhere it lists them.
Without a system SDL3, the launcher builds the official SDL3 archive shipped in the package (checksum verified) and links the game statically against it.
Details (French): [`packaging/linux/LISEZMOI.txt`](packaging/linux/LISEZMOI.txt).

### What the launcher does

For reference, the steps it runs on the player's machine:

1. **Prepare the game** with `tools-src/wakfu-prepare`: STFS packages, ISO/XISO image or folder → `game/base`
   (game), `game/tu2` (title update) and `game/patched` (patched XEX and image); unknown versions are refused by SHA-1.
2. **Recompile** with XenonRecomp at the commit in `tools.lock` plus `patches/xenonrecomp-wakfu.patch`, using
   `config/wakfu.toml`, then generate the kernel import stubs (`runtime/tools/gen_imports.py`).
3. **Translate the game's shaders** to SPIR-V with XenosRecomp and DXC (`packaging/shaders/precompile-shaders.sh`
   does the same as a standalone script).
4. **Build the runtime** (`runtime/`, CMake + Ninja + Clang) into `wakfu`, and run it.

## Tests

- `tests/insn/run_tests.py`: differential test of the instructions added to XenonRecomp (needs QEMU and
  `binutils-powerpc64-linux-gnu`).
- `runtime/tests/`: unit tests (`unit_*.cpp`, built with the runtime) and scripted runs `j1.sh` … `j5-*.sh`
  (need the prepared game); `batterie.sh` runs them all.

How each step was designed and verified (French): [`docs/methode.md`](docs/methode.md).
