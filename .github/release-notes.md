## What's new in v0.1.4

- **Local co-op works**: a second controller (or keyboard + controller) can now join. Before, player 2's START did
  nothing. Choose who uses what in Settings > Sound and controller (up to 4 controllers); player 2 has its own profile.
- **1440p**: new *Internal resolution* setting (720p, 1440p, 2160p). 1440p holds 60 fps on a mid-range card.
- **Graphics presets** in the launcher: Economy (laptops), Balanced, Quality (1440p, MSAA 4x); MSAA 2x/4x/8x,
  anisotropic filtering up to 16x and sharper shadows can also be set one by one.
- **Lighter on the CPU**: the renderer's thread uses about 30% less CPU per frame in busy areas (helps laptops).
- **New options**: mute when the window is in the background, stick dead zone, rumble strength, screenshot key
  (F12, saved in `captures`, with a "Screenshot saved" message for 2 s), screen choice and window position remembered.
- **Save backups**: a copy of your saves is made at each start (last 5 kept) and can be restored from the launcher.
- Your `config.toml` is never reset to defaults: a file with an invalid value is kept, and copied to
  `config.toml.bak` before the launcher rewrites it.

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
