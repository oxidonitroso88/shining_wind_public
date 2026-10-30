# Shining Wind (SLPM-66671): native Windows port

A static recompilation of the PlayStation 2 game *Shining Wind* (SEGA / Alvion, 2007, NTSC-J disc SLPM-66671) into a native Windows program, built on top of
[ran-j/PS2Recomp](https://github.com/ran-j/PS2Recomp). It includes an emulated IOP with the original IRX modules, a virtual CD-ROM, a Vulkan GS backend
([paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs), patched), WASAPI audio, resource replacement hooks and an HD texture replacement path (x4 output).

**You need your own copy of the game.** This repository contains **no game data at all**: no disc image, no executable, no recompiled game code, no textures, no text, no audio.
Everything derived from the game is generated on your computer from your own disc by one script, and stays there.

## Build it from your disc

```
git clone <this repository> ShiningWind
cd ShiningWind
python tools/port/make_port.py --iso "C:\path\to\Shining Wind.iso"
```

Needs Windows 10/11 x64, Visual Studio 2022 (Desktop development with C++), CMake 3.20+, Git, Python 3.9+, a GPU with Vulkan, and about 8 GB of free disk space.
The whole run takes roughly 40 minutes (most of it compiling). Details, stages and troubleshooting: [docs/BUILDING.md](docs/BUILDING.md).
The result is `ShiningWind-port\ShiningWind.exe`; the first start asks for your disc image (or an extracted folder), verifies it (size and SHA-256 of the executable) and
keeps a copy of the game files in `%APPDATA%\ShiningWind`.

Only the NTSC-J edition SLPM-66671 is supported.

## What is here
| Folder | Contents |
|---|---|
| `ps2xRecomp`, `ps2xAnalyzer` | the recompiler (MIPS R5900 to C++) and the ELF analyser |
| `ps2xRuntime`, `ps2xIOP` | the runtime: EE memory/syscalls/scheduler, GS, audio, pad, IOP (R3000A + virtual kernel, CDVD, SIF) |
| `data/` | function map of the executable (addresses and sizes only) and the recompiler configuration |
| `tools/port` | `make_port.py` (disc to executable) and an ISO9660 extractor |
| `tools/overlay` | code overlay discovery/recompilation (OVL00-OVL10 and supplementary functions) |
| `tools/perf`, `third_party_patches` | fetch of the pinned paraLLEl-GS commit and our patches (HD texture replacement, x4 scan-out) |
| `tools/ghidra` | the Ghidra exporter used to produce the function map |
| `ps2xTest` | tests |

The English translation and the HD texture packs are **separate downloads** (they are not code and are not part of this repository). Put them in
`%APPDATA%\ShiningWind\mods` and `%APPDATA%\ShiningWind\hd_packs`; the program picks them up on start. The HD packs need the x4 mode (`play-x4.cmd`).

## Licence
GPL-3.0 (see `LICENSE`), inherited from PS2Recomp. paraLLEl-GS is LGPL-3.0-or-later and is fetched from its own repository; our changes to it are the patches in
`third_party_patches/parallel-gs` under the same licence. See `NOTICE.md`.

---

## Resumen en español
Port nativo de Windows de *Shining Wind* (SLPM-66671) hecho con PS2Recomp. **No incluye nada del juego**: necesitás tu propio disco. Ejecutá
`python tools/port/make_port.py --iso "<tu imagen>.iso"` y el script verifica el disco, recompila el programa, compila el ejecutable (con render HD ×4) y lo deja en
`ShiningWind-port\ShiningWind.exe`. La traducción al inglés y los paquetes de texturas HD se distribuyen aparte.
