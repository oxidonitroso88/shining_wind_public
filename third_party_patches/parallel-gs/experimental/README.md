# Parche experimental: salida de alta resolución ×4 (parallel-gs)

`0003-x4-scanout-output.patch` es **opcional y no se aplica automáticamente** (`tools/perf/apply_parallel_gs_patches.ps1` solo lee esta carpeta, no `experimental/`). Se aplica sobre el commit fijado `3a66c19` + parches 0001 y 0002. Licencia: LGPL-3.0-or-later, como parallel-gs; conserva los avisos SPDX de los archivos.

Qué hace
* `VSyncInfo::high_resolution_scanout_log2` (por defecto 1 = comportamiento original, salida ×2 = 1280×447). Con 2 y super-muestreo X16 (rejilla ordenada 4×4) la salida es de **una muestra por píxel** a 2560×894; sin X16 vuelve a 1.
* `gs/shaders/sample_circuit.frag`: constante de especialización 3 `SCANOUT_SHIFT` y, con `SUPER_SAMPLES == 16 && SCANOUT_SHIFT == 2`, `sample_id = (y&1) | ((x&1)<<1) | ((y>>1)<<2) | ((x>>1)<<3)` (el mapa de `compute_sample_points()`).
* `gs/gs_renderer.cpp/.hpp`: el factor ×2 fijo de la salida pasa a `1 << scanout_shift` (imágenes de circuito, ventanas de mezcla, `horiz_resolution`, desfase entre campos `(1<<shift)>>1` filas).
* `Hacks::hd_per_sample_sprites` (`PS2X_GS_HD_SSAA=1` en el runtime): los sprites que muestrean una sustitución HD pierden `SNAP_ATTRIBUTE` (interpolación de UV por muestra); en `ubershader.comp` la rama «cobertura anclada + interpolación por muestra» ya no exige textura en array. Solo afecta a texturas reemplazadas.
* Define `PARALLEL_GS_SCANOUT_LOG2` y `PARALLEL_GS_HD_PER_SAMPLE` en `gs_interface.hpp`: el runtime (`PS2X_GS_OUTPUT=4`, `PS2X_GS_HD_SSAA=1`, `gs_parallel_backend.cpp`) solo usa esos campos si las macros existen, así que compila también contra parallel-gs sin el parche.

Después de aplicarlo hay que **regenerar el SPIR-V embebido** (el shader está precompilado):
```
# slangmosh se compila desde Granite/ (necesita los submódulos third_party/spirv-cross y third_party/shaderc, y glslang):
cmake -S <parallel-gs>/Granite -B <build> -G "Visual Studio 17 2022" -DGRANITE_PLATFORM=null -DGRANITE_RENDERER=OFF -DGRANITE_TOOLS=ON -DGRANITE_VULKAN_SPIRV_CROSS=ON -DGRANITE_VULKAN_SHADER_MANAGER_RUNTIME_COMPILER=ON -DGRANITE_VULKAN_SYSTEM_HANDLES=OFF -DGRANITE_VULKAN_FOSSILIZE=OFF -DGRANITE_SHIPPING=ON -DGRANITE_AUDIO=OFF -DGRANITE_FFMPEG=OFF -DGRANITE_INSTALL_TARGETS=OFF
cmake --build <build> --config Release --target slangmosh
cd <parallel-gs>/gs && <build>/Release/slangmosh.exe --namespace ParallelGS shaders/slangmosh.json --output shaders/slangmosh.hpp --output-interface shaders/slangmosh_iface.hpp -O --strip
```
Comprobado: regenerar con los shaders sin modificar reproduce `slangmosh.hpp` y `slangmosh_iface.hpp` del commit byte a byte (salvo fin de línea).
Copia de trabajo usada: `C:\ps2x-gpu\parallel-gs-x4` (la copia estable `C:\ps2x-gpu\parallel-gs` no se modificó).

Resultados y por qué todavía no aporta detalle visible: `docs/x4-output-prototype-2026-10-23.md`.

## 0004-x4-uncommitted-work-20261027.patch (instantánea de seguridad)
Se aplica **después** de `0003`. Recoge los cambios que a 2026-10-27 solo existían sin commitear en `C:\ps2x-gpu\parallel-gs-x4` (5 archivos: `gs_interface.cpp/.hpp`, `shaders/data_structures.h`, `triangle_setup.comp`, `ubershader.comp`; incluye las correcciones de desplazamiento y filas repetidas de retratos de `docs/hd-bustup-seams-and-shift-2026-10-26.md`). Se comprobó con `git apply --check` sobre «commit fijado + 0001 + 0002 + 0003». **Excluye** `slangmosh.hpp` y `slangmosh_iface.hpp` (SPIR-V generado: regenerar con `slangmosh`, ver `docs/guias/01-ps2recomp.md` §6). Sin probar compilando desde cero; es una copia de respaldo del trabajo, no una versión validada. LGPL-3.0-or-later como parallel-gs.
