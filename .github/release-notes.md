## What's new in v0.1.6

- **Controller remapping**: a new *Controller* tab in the launcher (click a line, press a button; Reset), also on
  the game's RecompLauncher page. Works for both players.
- **PlayStation button icons**: Auto / Xbox / PlayStation. The cross, circle, square and triangle are drawn by the
  project (no Sony artwork); the controller picture, sticks and D-pad stay Xbox.
- **Performance overlay** (frames per second, GPU and CPU time) and **achievement notifications**
  ("Achievement unlocked: <name>").
- **Colour-blind filter** (protanopia, deuteranopia, tritanopia) and **transparency anti-aliasing** (MSAA).
- **SDL3** for the game and the launcher. On Linux, the launcher builds SDL3 itself when your distribution has no
  package; on Wayland the window position is no longer overwritten.
- Fixes: MSAA "On" is now shown as 4x in RecompLauncher; FSR is only used to enlarge the image (a larger internal
  resolution is scaled down smoothly); a button cleared in RecompLauncher now means "none".

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
