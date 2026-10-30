# data/

* `shining_wind_function_map.csv`: function map of the main program of SLPM-66671 (`Name,Start,End,Size`: start/end addresses and sizes of 16 469 functions and automatic names such as `FUN_001002f0`). It holds no code bytes or game text. It is the input of the recompiler; it was produced with Ghidra (`tools/ghidra/ExportPS2Functions.active-20261027.java`) plus `tools/ghidra/add_materialized_entries.py`.
* `shining_wind.toml.in`: recompiler configuration template (`@ELF@`, `@OUT@`, `@CSV@` are filled in by `tools/port/make_port.py`).

Regenerating the runner from these two files and the user's own disc reproduces the `register_functions.cpp` table byte for byte.
