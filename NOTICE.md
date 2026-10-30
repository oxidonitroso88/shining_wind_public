# Notices

* **PS2Recomp** (https://github.com/ran-j/PS2Recomp), GPL-3.0: the recompiler, the base runtime and the analyser this project started from.
* **paraLLEl-GS** (Arntzen Software AS, https://github.com/Arntzen-Software/parallel-gs), LGPL-3.0-or-later, with its Granite submodules: not included. `tools/perf/fetch_parallel_gs.ps1`
  downloads the pinned commit `3a66c1976170cbc2cb53a3593fabbc7c4b2ccfbd`; the files in `third_party_patches/parallel-gs` are our modifications of it and keep its licence and SPDX headers.
  Anyone who receives a binary of the port can obtain the corresponding source of the modified library from the pinned commit plus these patches.
* **raylib**, **Dear ImGui**, **rlImGui** (fetched by CMake at configure time) and **FFmpeg** (built by CMake): each under its own licence.
* **DejaVu fonts** (`third_party_fonts/dejavu`): Bitstream Vera / DejaVu licence (`LICENSE` next to the fonts).
* *Shining Wind* is a trademark and a copyrighted work of its owners. This project is not affiliated with or endorsed by them; it contains none of their material.
