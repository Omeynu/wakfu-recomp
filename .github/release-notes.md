## What's new in v0.1.5

- **RecompLauncher**: the game is now set up once (recompiled on your PC) and your copy of the game is no longer
  needed afterwards; the launcher knows when setup is finished. Updates that change the generated code recompile
  the game automatically.
- **Big Picture**: the game opens borderless (unless you chose fullscreen) and does not change your saved window
  size or position.
- **Linux**: settings chosen in RecompLauncher before the first setup are now used by the game.

**No game files are included.** You need your own copy of Islands of Wakfu (XBLA, title `584109AA`) and its title update 2.

## Download

| Your system | File |
|---|---|
| **Windows 10/11 x64** | `WakfuRecomp-<version>-windows-x64.exe` — download and double-click |
| **Linux x86_64** | `WakfuRecomp-<version>-linux-x86_64` — download, `chmod +x` it, run it |
| RecompLauncher | uses the `wakfu-recomp-…` archives automatically: nothing to download by hand |

The first start asks for the game package and the title update 2 package, then recompiles the game on your PC
(a few minutes to an hour, ~3 GB of disk). Later starts are immediate. Settings and saves are kept across updates.

Windows SmartScreen or an antivirus may warn because the executable is not signed.
Linux: on Arch-based distributions the launcher can install the missing packages; elsewhere it lists them.
