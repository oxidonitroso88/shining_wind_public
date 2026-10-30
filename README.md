# Shining Wind (SLPM-66671): native Windows port

A static recompilation of the PlayStation 2 game *Shining Wind* (SEGA / Alvion, 2007, NTSC-J disc **SLPM-66671**) into a native Windows program, built on top of
[ran-j/PS2Recomp](https://github.com/ran-j/PS2Recomp). It has an emulated IOP with the original IRX modules, a virtual CD-ROM, a Vulkan GS backend
([paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs), patched), WASAPI audio, resource replacement hooks and an HD texture replacement path (x4 output).

> **You need your own copy of the game.** This repository contains **no game data at all**: no disc image, no executable, no recompiled game code, no textures, no text, no audio.
> Everything derived from the game is generated on your computer, from your own disc image, by one script, and stays on your computer.

Español: [tutorial completo en español](docs/TUTORIAL.es.md).

---

# Tutorial: build the port from your disc

The whole process is one command. It takes about 40 minutes (most of it compiling) and about 8 GB of disk space.

## 0. What you need
| | |
|---|---|
| Windows | 10 or 11, 64 bit |
| Visual Studio 2022 or **Build Tools for Visual Studio 2022** | with the workload **Desktop development with C++** (it includes the compiler, the Windows SDK and CMake). Free download: <https://visualstudio.microsoft.com/downloads/> (scroll to "Tools for Visual Studio") |
| Git | <https://git-scm.com/download/win> |
| Python 3.9 or newer | <https://www.python.org/downloads/> (tick "Add python.exe to PATH") |
| A GPU with a Vulkan driver | any recent NVIDIA / AMD / Intel driver; without Vulkan the program falls back to a slow CPU renderer |
| Disk space | about 8 GB free for the build, plus about 4.5 GB for the game copy the program keeps in `%APPDATA%\ShiningWind` (can be moved, see step 6) |
| Internet | the build downloads paraLLEl-GS and a few libraries (raylib, Dear ImGui, FFmpeg) |
| **Your disc image** | `Shining Wind.iso`, a dump of your own NTSC-J disc (SLPM-66671). See step 1 |

## 1. Get your disc image
Dump your own disc to an `.iso` file with any disc-ripping tool (a 4.18 GB ISO9660 image; 2048-byte-sector `.iso` and raw 2352-byte `.bin`/`.img` images are understood).
Put it anywhere you like, for example `C:\Games\Shining Wind.iso`. **The file never leaves your computer and is never modified.**

Only the NTSC-J edition **SLPM-66671** works. The script checks the size and the SHA-256 of the game's executable (`SLPM_666.71`) before doing anything and stops with a clear message if the disc is a different edition.

## 2. Download this repository
Open **PowerShell** (Start menu, type "PowerShell") and run:
```
cd C:\
git clone https://github.com/oxidonitroso88/shining_wind_public.git ShiningWind
cd ShiningWind
```

## 3. Run the script and tell it where your ISO is
**This is the step where you load the `.iso`**: its path goes after `--iso` (keep the quotes if the path has spaces).
```
python tools/port/make_port.py --iso "C:\Games\Shining Wind.iso"
```
![First run](docs/img/01-first-run.png)

The script checks your tools and the disc, then extracts only what it needs (the executable and the code overlays, 13 small files) into `port-work\game`.
If something is missing, it prints a `PROBLEM:` line that says what to install.

## 4. Wait: it builds everything
The script now runs by itself. Every stage writes a log to `port-work\logs`.

**Recompiling the game's code** (about 2 minutes). The recompiler turns the game's executable into C++ using the function map in `data/`. About 16,500 source files appear in `ps2xRuntime\src\runner`; they are derived from the game, are ignored by git and **must never be published**:

![Recompile](docs/img/02-recompile.png)

**The HD renderer** (about 2 minutes). paraLLEl-GS is downloaded at a fixed version, our patches (HD texture replacement and x4 output) are applied and its shaders are regenerated:

![HD renderer](docs/img/03-hd-renderer.png)

**Compiling** (about 25 minutes, uses 2 CPU jobs so the PC stays usable). This is the long part. Warnings `LNK4006` and `LNK4088` at the end are normal:

![Build](docs/img/04-build.png)

## 5. The result
When the script prints `done`, your port is in `ShiningWind-port`:

![Result](docs/img/05-result.png)

You can copy that folder anywhere (about 160 MB, it needs nothing installed). It contains no game data.

## 6. Start the game for the first time
Double-click **`ShiningWind.exe`**. The first time it asks for the game:
* choose **"disc image"** and select the same `.iso` (or **"extracted folder"** if you have the disc files extracted in a folder),
* it verifies the disc (size and SHA-256 of the executable) and copies the game files to `%APPDATA%\ShiningWind\disc` (about 4 GB, takes around 10 seconds on an SSD),
* it remembers your choice, so next time it just starts.

To keep the data on another drive, start it from a shortcut or from PowerShell with:
```
ShiningWind.exe --disc "C:\Games\Shining Wind.iso" --data-dir "D:\ShiningWindData"
```
Saves (a virtual memory card) go to `<data folder>\memcards`; a short error log is kept in `<data folder>\logs\ShiningWind.log`.

## 7. Play in HD (x4)
Use **`play-x4.cmd`** instead of `ShiningWind.exe`. It starts the same program with the x4 internal resolution, which the HD texture packs need (it is also sharper without any pack).

**Controls** (keyboard, used when no gamepad is connected; with a gamepad connected only the gamepad is used):

| Key | Button | Key | Button |
|---|---|---|---|
| C | Circle | Enter | Start |
| X or Space | Cross | Tab | Select |
| Z | Square | Q / E | L1 / R1 |
| V | Triangle | Left / Right Shift | L2 / R2 |
| Arrow keys | D-pad | WASD | Left stick (walk) |

Do not use Esc as a button: it closes the window. **F10** turns the HD textures on and off; **F7** opens the extras panel.

## 8. Optional add-ons (separate downloads)
The English translation and the HD texture packs are not code and are **not** in this repository; they are published separately. When you have them, drop them in the data folder (`%APPDATA%\ShiningWind`, or the one you gave with `--data-dir`):
```
%APPDATA%\ShiningWind\mods\        <- translation files (ST\*.RAX and story_en.tsv)
%APPDATA%\ShiningWind\hd_packs\    <- one folder per texture pack (each with its manifest.json)
```
They are picked up the next time the game starts. The HD packs need `play-x4.cmd`.

## 9. If something goes wrong
| Message or symptom | What to do |
|---|---|
| `PROBLEM: Visual Studio 2022 ... not found` | install the **Desktop development with C++** workload (step 0) and run the command again |
| `PROBLEM: less than 8 GB free` | free disk space or use `--work D:\port-work` (another drive) |
| `unsupported edition` / `SHA-256 is ...` | your disc is not the NTSC-J SLPM-66671, or the image is damaged or modified |
| `not an ISO9660 image` | the file is not a plain disc image (a `.cue` is only a text file: use the `.bin`/`.iso` next to it) |
| A stage fails | the last lines of its log are printed; open `port-work\logs\<stage>.log`. You can run the same stage again: `python tools/port/make_port.py --iso "..." --stage build` (stages: `check extract recomp generate overlays gs build package`) |
| The build seems stuck | it is compiling about 17,000 files with 2 jobs; the `build` stage takes 25-30 minutes on a typical PC. You can raise `--jobs 4` if you have 16 GB of RAM or more |
| Black window or very slow | update your GPU driver (Vulkan). `ShiningWind.exe` falls back to the CPU renderer if Vulkan is missing |
| A rebuild after updating this repository | just run the command again; finished stages are repeated quickly except `build`, which only recompiles what changed |

More detail on every stage and what was verified: [docs/BUILDING.md](docs/BUILDING.md).

---

## What is in this repository
| Folder | Contents |
|---|---|
| `ps2xRecomp`, `ps2xAnalyzer` | the recompiler (MIPS R5900 to C++) and the ELF analyser |
| `ps2xRuntime`, `ps2xIOP` | the runtime: EE memory/syscalls/scheduler, GS, audio, pad, IOP (R3000A + virtual kernel, CDVD, SIF) |
| `data/` | function map of the executable (addresses and sizes only) and the recompiler configuration |
| `tools/port` | `make_port.py` (disc to executable) and an ISO9660 extractor |
| `tools/overlay` | code overlay discovery and recompilation (OVL00-OVL10 and supplementary functions) |
| `tools/perf`, `third_party_patches` | fetch of the pinned paraLLEl-GS commit and our patches (HD texture replacement, x4 scan-out) |
| `tools/ghidra` | the Ghidra exporter used to produce the function map |
| `ps2xTest` | tests |

## Licence
GPL-3.0 (see `LICENSE`), inherited from PS2Recomp. paraLLEl-GS is LGPL-3.0-or-later and is fetched from its own repository; our changes to it are the patches in
`third_party_patches/parallel-gs` under the same licence. See `NOTICE.md`.
