# Soporte de overlays de código (OVL00–OVL10) — diseño y resultados, 2026-10-12/13

Sustituye la propuesta de `overlays-missing-target-2026-10-12.md` (opción A, implementada).

## Formato `MWo3` (verificado en los 11 archivos de `OVERLAY/`)
| Offset | Campo |
|---|---|
| +0x00 | magia `MWo3` (0x336F574D) |
| +0x04 | índice (OVLnn → nn+1) |
| +0x08 | base de carga `0x452200` (igual en todos: el ELF declara 11 `PT_LOAD` con ese vaddr, `filesz=0`) |
| +0x0C / +0x10 | tamaño de código / de datos |
| +0x18 / +0x1C | dirección final (informativa; en OVL06 y OVL10 difiere del tamaño real por relleno) |
| +0x20 | nombre original (`ovl09.bin`) |
| +0x40.. | código (`+0x0C` bytes), luego datos (`+0x10` bytes) |

Se carga **todo el archivo** (cabecera incluida) en `0x452200`. No hay relocaciones: el código está enlazado en la base fija y llama al programa principal con `jal` absolutos. Los 11 rangos de código no contienen opcodes inválidos (se comprobó palabra a palabra); ~10 % son nops de relleno.

## Cómo los carga el juego (desensamblado de `0x376F90`–`0x3771C4`)
`0x376F90(idx)`: lee el nombre de la tabla `0x3D8E30` (11 punteros a `cdrom0:\OVERLAY\OVLnn.BIN`), `open` + `lseek` para el tamaño, **`malloc(size)`** (buffer guardado en `0x451F60+idx*4`, tamaño en `0x451F90+idx*4`), `read` y `close`. El arranque precarga OVL07 y OVL08 (`0x377150`). `0x377090(idx)` los activa: copia el buffer a la dirección guardada en `[0x41DF34]` (= `0x452200`), `FlushCache`, y anota el índice activo en `[gp-0x7CF8]`. Conclusión: **abrir el archivo no instala el overlay**; el código solo es ejecutable tras la copia + flush, y entre uno y otro puede pasar tiempo.

## Diseño
1. **Herramientas** (`tools/overlay/`, sin tocar `D:\NO_LABEL`): `ovl_tool.py analyze|build|supplement` descubre funciones (llamadas `jal` internas, referencias desde el ELF, punteros en datos, constantes `lui/addiu|ori` con ventana de 40 instrucciones, prólogos y «código tras `jr ra`»), fusiona divisiones erróneas con el análisis de saltos, y escribe por overlay un ELF sintético (`.hdr`, `.text` ejecutable, `.data`), un CSV de funciones y un TOML. `ovl_install.py` copia lo generado a `ps2xRuntime/src/runner_overlays/` (git-ignorado, derivado del juego).
2. **Recompilador** (`overlay_name` en `[general]`): mismo traductor; cabeceras `ps2_<ovl>_functions.h` con guarda propia, sin cabecera de stubs, y en lugar de la tabla densa emite un `ps2_overlay::Descriptor` (nombre, rango de código, FNV-1a 64 del código, 32 palabras de muestra, entradas ordenadas incluyendo los puntos de reanudación) que se registra en la inicialización estática.
3. **Runtime** (`ps2_overlay.h/.cpp`, sin tocar `ps2_runtime.h`, así que no obliga a recompilar las 568 unidades): `lookupFunction`/`hasFunction` consultan primero la región de overlays. `resolve()` identifica el overlay **por su contenido en la memoria del invitado**: comprobación rápida de las muestras en cada llamada, hash completo en la primera resolución, al fallar las muestras y tras cada `FlushCache/iFlushCache` (el punto en que el hardware real hace ejecutable el código). Cada cambio queda en el log (`[overlay] installed code: ovlNN (was ovlMM)`). Una dirección compartida resuelve solo al overlay instalado; los datos tras el rango de código pueden cambiar sin invalidarlo.
4. **Funciones suplementarias del programa principal** (`supplement_name`): el mapa de Ghidra omite funciones del ELF que solo se alcanzan desde overlays o por punteros en tiempo de ejecución. `mainx` (64 funciones, llamadas `jal` de overlays) y `mainorph` (440, código en huecos del mapa que parece código real) se compilan aparte y se escriben en la tabla principal solo si la ranura está vacía. Cada función suplementaria se compila **completa** (sigue sus saltos hasta su `jr ra`), porque Ghidra parte funciones grandes en trozos.

## Resultados
* Los 11 overlays recompilan: 224 funciones, 0 instrucciones sin soporte (60 avisos, todos `jalr` a punteros, resueltos en ejecución).
* Pruebas: `ps2x_tests` 467/467 (9 nuevas: hash de referencia, selección, sustitución, corrupción/restauración, datos fuera del rango, `PS2Runtime` integrado, emisión del recompilador con hash idéntico al del runtime).
* Verificación manual del usuario (sesiones play4–play8): Items, Equipo y Status funcionan (OVL09, OVL08, OVL07 se instalan y alternan); combate y recogida de la bandera funcionan tras los suplementos.
* Errores encontrados y corregidos en el camino: constantes `lui/addiu` separadas por más de 7 instrucciones (función `0x456880`), funciones del ELF omitidas por Ghidra (`0x2CB8D0`, `0x265870` partida en trozos), colisión de nombres entre grupos suplementarios (mi herramienta lo generó con el nombre equivocado).

## Mantenimiento
* Regenerar: `ovl_tool.py build/supplement --out out/overlay-build`, `ps2_recomp <ovl>.toml` por cada TOML, `ovl_install.py --build out/overlay-build`, compilar el runner. Si tras añadir/quitar archivos falla el enlace con `PS2DebugPanel`, tocar el `unity_N_cxx.cxx` que contiene `ps2_debug_panel.cpp` y `ps2_debug_panel.cpp`.
* Cambiar la lista de `target_include_directories` fuerza recompilar todo (24 min); los cambios de fuentes del runner solo recompilan lo tocado.
* Límites: la comprobación rápida solo mira 32 palabras; dos overlays que coincidieran en ellas se distinguen solo tras un `FlushCache` (el juego lo llama). Funciones omitidas por el descubrimiento aparecen como `missing-target` con su dirección; se añaden regenerando.
