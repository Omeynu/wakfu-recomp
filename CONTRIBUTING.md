# Contributing

Bug reports, tests on other PCs and pull requests are welcome.

## Reporting a bug

Open an [issue](https://github.com/Omeynu/wakfu-recomp/issues/new/choose) with:

- `%APPDATA%\WakfuRecomp\journal.txt` (its last line, `[sortie] code N : raison`, says why the game stopped)
  and `config.toml`;
- your GPU, driver version and OS;
- a screenshot and one sentence on what you did (menu, zone, action) and what happened.

For more detail, set `console = false` under `[confort]` in `config.toml`; for the game's internal log, start
`jouer.cmd` from a command prompt after `set WAKFU_GAMELOG=1`.

## Pull requests

- Never commit game files, extracted assets or generated C++ (`work/` is git-ignored for that reason).
- Keep changes small and focused; describe how you tested them (see `BUILDING.md`, *Tests*).
- Changes to the XenonRecomp patch need a case in `tests/insn/cases.py`.
- Code comments and docs in `docs/` are in French; English is fine in issues and pull requests.

By contributing you agree that your contribution is licensed under the GPL-3.0 (`COPYING`).
