# LittleBigPlanet 3 Recompiled

A native Windows runtime for LittleBigPlanet 3 (PS4, CUSA00473). It loads the game's own
executable and modules from **your own dump of the game**, links them against
high-level reimplementations of the PS4 system libraries (`runtime/src/hle`), and
renders through [shadPS4](https://github.com/shadps4-emu/shadPS4)'s GPU emulation.

Made with [Claude Code](https://claude.com/claude-code) on the Opus 5.5 model. `NOTES.md` is
the running log of that work (what was tried, found and fixed), useful for picking it up.

**No game files are included or will be accepted.** You need a legally obtained dump of
your own copy (base game CUSA00473 and its update).

## Setup

1. Clone with the shadPS4 fork: `git clone --recursive <this repo>` (or run
   `git submodule update --init --recursive` after cloning).
2. Download SDL3 3.4.16 for Visual Studio (`SDL3-devel-3.4.16-VC.zip` from
   [SDL's releases](https://github.com/libsdl-org/SDL/releases)) and extract it so that
   `third_party/SDL3/cmake` exists.
3. Put your dump in a folder that holds `CUSA00473` (base game) and `CUSA00473-patch`
   (the update). By default the runtime looks in `LBP3 Game Files` next to this project
   folder. Anywhere else: write the folder's path on the first line of `game_dir.txt` in
   the project root (git-ignored), or set `LBP3_GAME_DIR`.
4. Build with Visual Studio 2026 (C++ workload with Clang): `build.bat`.
5. Run: `play.bat`. Logs go to `analysis\play_log.txt`.

Controls are configured in `user\config\input.ini` (written on first start).

## Layout

| Path | What |
|---|---|
| `runtime/` | The runtime: ELF loader and linker, HLE libraries, input, video, audio |
| `third_party/shadPS4` | [Our shadPS4 fork](https://github.com/PurpleGitty/shadPS4/tree/lbp3) (GPU command processor, Vulkan renderer), branch `lbp3` |
| `third_party/SDL3` | SDL3 (window, input), downloaded separately |
| `tools/` | Test and analysis scripts (self-driving test runs, disassembly helpers) |
| `NOTES.md` | Detailed working notes: what was found, fixed and still open |

Local output (`build/`, `analysis/`, `user/` saves and caches) is git-ignored and can
contain game data; don't commit it.

## Status

Boots to the Pod, menus, the Prologue levels and create mode. See `NOTES.md` for known
issues.

## License

GPL-2.0-or-later (see `LICENSE`), the same as shadPS4. This covers the project's own code
only; LittleBigPlanet 3 and its files belong to Sony Interactive Entertainment.
