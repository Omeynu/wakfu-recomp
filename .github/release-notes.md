## What's new in v0.1.3

- **Windows: smooth 60 fps in busy areas** (1 ms timers; frames were missed in large zones, 31-48 fps before).
- **Grougal is drawn in full** (render targets split in bands are now enlarged together).
- **No console window while playing**; the game log is `journal.log` (next to your saves).
- **RecompLauncher**: settings on the game's page, no console during setup, progress bar, setup log.
- **Icons**: WakfuRecomp has its own icon; the game window uses the icon of your copy of the game.
- **Portable mode removed**: an old portable folder's saves and settings are copied once to the normal data folder
  (nothing is replaced or deleted). The game is prepared again on first start.

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
