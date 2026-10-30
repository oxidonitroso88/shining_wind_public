# Building the port from your disc

```
python tools/port/make_port.py --iso "C:\path\to\Shining Wind.iso" [--work port-work] [--dist ShiningWind-port] [--jobs 2] [--cmake <cmake.exe>]
```

The script runs these stages in order. `--stage <name>` runs only one (every stage can be repeated). Logs: `<work>\logs\*.log`.

| Stage | What it does |
|---|---|
| `check` | looks for git, CMake, Visual Studio 2022 C++ tools, Python; needs 8 GB free |
| `extract` | reads the ISO9660 image and extracts the executable (`SLPM_666.71`), `OVERLAY\` and `SYSTEM.CNF` into `<work>\game`; **stops if the executable is not the supported edition** (3 270 888 bytes, SHA-256 `805c3830...5437`) |
| `recomp` | builds the recompiler into `<work>\recomp-build` (about 1 minute) |
| `generate` | recompiles the executable using `data/shining_wind_function_map.csv` and `data/shining_wind.toml.in` into `ps2xRuntime/src/runner` (about 16 500 files, 20 seconds) |
| `overlays` | discovers and recompiles the 11 code overlays and the supplementary functions into `ps2xRuntime/src/runner_overlays` |
| `gs` | downloads paraLLEl-GS at the pinned commit, applies our patches (HD texture replacement, x4 output), builds `slangmosh` and regenerates the embedded shaders |
| `build` | configures and builds `ps2EntryRunner.exe` with 2 parallel jobs (about 25-30 minutes) |
| `package` | copies the executable and its DLLs to `<dist>` as `ShiningWind.exe`, with `play-x4.cmd` |

Notes
* The generated sources (`ps2xRuntime/src/runner*`) are derived from the game: they stay on your computer and are ignored by git. **Do not publish them.**
* `generate` and `overlays` replace a folder only if an earlier run of this script made it (marker file `.generated-by-make_port`); any other folder with content is never touched.
* Use at most 2 parallel jobs unless you have plenty of RAM: the generated code is large.
* Network access is needed for `gs` (GitHub) and for CMake (raylib, ImGui, FFmpeg sources).
* Running: start `ShiningWind.exe` (it asks for your disc once) or `play-x4.cmd` for the x4 internal resolution that the HD texture packs need.
* Saves go to `%APPDATA%\ShiningWind\memcards`. Keyboard: arrows/WASD, C = circle, X/Space = cross, Z = square, V = triangle, Enter = Start, Tab = Select, Q/E = L1/R1.

If a stage fails, the last lines of its log are printed; open an issue with that log (it contains no game data).

## What has been verified
Run from a clean checkout of this tree with a real disc image (Windows 11, Visual Studio 2022 Build Tools, CMake 4.x):
* `extract`: the 13 needed files, ELF SHA-256 verified. `recomp`: about 2 minutes. `generate`: 16 471 files in 17 s; the function table (`register_functions.cpp`) is byte-identical to the one the developers use.
* `overlays`: 1 407 files; the 11 overlays and the `mainx`/`maincross` supplements match the developers' tree; `mainorph` differs in 17 functions because it is regenerated with the current discovery limits.
* `gs`: the patched paraLLEl-GS tree and the regenerated shaders are byte-identical to the developers' x4 copy.
* `build`: 24 minutes with 2 jobs; `ps2EntryRunner.exe` (132 MB). Replaying the same recorded inputs (load a save, field map) gives the same screens as the developers' build and no `missing-target`.
Not verified: a different PC, a GPU other than the developers', long play sessions with the regenerated supplementary functions.
