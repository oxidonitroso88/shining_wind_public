# Mods: English translation and HD textures

This guide explains **which files** the mods are, **where to put them** and **how to check** the game is using them. This repository contains **no mods and no game files**, only the program and this guide. Mods are downloaded separately and you **need your own copy** of *Shining Wind* (NTSC-J, SLPM-66671). Make sure the game runs without mods first (see the README / tutorial).

## 1. Data folder
Everything goes in the game's **data folder**: `%APPDATA%\ShiningWind` by default, or the one you passed with `ShiningWind.exe --data-dir "D:\MyData"`. Create `mods` and `hd_packs` there if they do not exist:

```
<data folder>\
  disc\ memcards\ logs\     <- created by the game, do not touch
  mods\                     <- translation
  hd_packs\                 <- HD textures
```

## 2. English translation (`mods`)
Copy the translation **as shipped**, keeping subfolders:

```
mods\
  story_en.tsv              <- story dialogue (one text file, directly in mods\)
  ST\
    SYSTEM.RAX SAVELOAD.RAX BLACKSMITH.RAX ITEMSHOP.RAX SOULPIECE.RAX EQUIP.RAX STATUS.RAX ...
    SHOP\...  ENEMYINFO\...   <- subfolders: copy them whole
```

* A file in `mods\ST\` **replaces** the file with the same name inside your disc's `ST.CVM`. Files you do not provide stay original, so partial installs work.
* Names must match (case and a trailing `;1` are ignored). Unknown names, absolute paths, `..` or invalid Windows names are ignored and reported on the console.
* Nothing in your disc copy or your saves is modified. To go back to the original, delete the file(s) or rename `mods` to `mods_off`.
* Disable all mods for one run: set `PS2X_MODS=off` before starting. Story text only: `PS2X_STORY_EN=off`.
* Check: the title menu and the pause menu appear in English. From a console you also see `[mods] ST/...: old -> new bytes` lines.

## 3. HD textures (`hd_packs`)
One folder per pack, with `manifest.json` directly inside (not in an extra nested folder):

```
hd_packs\
  PackName\
    manifest.json
    textures\*.png
```

* Start with **`play-x4.cmd`** (×4 internal resolution); at ×1 HD packs are not shown.
* The manifest matches textures by game file, resource and content hash: with a different disc edition nothing applies and the game looks original.
* Avoid two packs replacing the same texture (only one is used, which one is not guaranteed).
* More/larger packs use more VRAM (`PS2X_HD_BUDGET_MB`, default 512).
* **F10** toggles HD replacements while playing. To remove a pack, delete its folder.

## 4. Troubleshooting
| Symptom | Likely cause |
|---|---|
| Still Japanese | `mods` is not in the data folder the game uses (check `--data-dir`), is nested (`mods\mods\ST`) or names do not match |
| HD textures not visible | not started with `play-x4.cmd`, `manifest.json` not directly inside the pack folder, other disc edition, or F10 toggled off |
| Slowdowns / memory errors with many packs | too much VRAM: remove packs or lower `PS2X_HD_BUDGET_MB` |

Always test **without mods** before reporting a bug.

## 5. Download
*(Link to be added by the mod author.)* The mods are published by a third party outside this repository, which does not host or review them.

## 6. Notice
This project does not include or distribute the game, disc images, or extracted/modified game files. Use mods only with a legitimate copy. This is not legal advice.
