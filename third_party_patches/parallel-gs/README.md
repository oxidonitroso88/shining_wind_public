# Parches locales de parallel-gs

parallel-gs (paraLLEl-GS, Arntzen Software AS, https://github.com/Arntzen-Software/parallel-gs) es el rasterizador Vulkan del GS que usa el port. Este directorio contiene **las modificaciones propias** como parches sobre el commit fijado **`3a66c1976170cbc2cb53a3593fabbc7c4b2ccfbd`**. La copia de parallel-gs no se versiona en este repositorio (`third_party/` está ignorado); solo estos parches.

| Parche | Qué hace |
|---|---|
| `0001-hd-texture-replacement.patch` | Gancho de reemplazo de texturas HD: interfaz `TextureReplacer` / `TextureReplacement` / `TextureReplacementStats` (`gs/gs_renderer.hpp`), `GSInterface::set_texture_replacer()` y `get_texture_replacement_stats()`, y la sustitución de la imagen muestreada al enlazar una textura (`gs/gs_interface.cpp`, `drawing_kick_update_texture`). Sin reemplazador instalado el comportamiento es idéntico al original. No toca los shaders (solo añade dos constantes de indicadores host en `gs/shaders/data_structures.h` que los shaders nunca leen). |
| `0002-debug-super-sample-readback.patch` | Ayuda de depuración: `GSInterface::debug_read_vram_slice(slice, offset, size, dst)` (y `GSRenderer::debug_read_gpu_vram`) para leer de vuelta cualquier capa de la VRAM de la GPU (la VRAM normal, el plano de marcas del super-muestreo y las capas de muestras). Solo se usa con `PS2X_GS_SLICE_DUMP`; no afecta al renderizado. |

## Cómo se aplican (reproducible)
```
powershell -File tools/perf/fetch_parallel_gs.ps1                 # descarga el commit fijado y aplica los parches (third_party/parallel-gs)
powershell -File tools/perf/apply_parallel_gs_patches.ps1 -Dest <carpeta de parallel-gs> [-Check]   # aplicarlos a una copia existente (idempotente)
```
`apply_parallel_gs_patches.ps1` detecta los parches ya aplicados y se niega a continuar si un parche no encaja en el commit esperado. Para regenerar el parche tras cambios: `git -C <parallel-gs> diff > third_party_patches/parallel-gs/0001-hd-texture-replacement.patch` (partiendo del commit fijado con los parches anteriores aplicados) y comprobarlo con `git apply --check` sobre un `git archive` limpio del commit.

## Qué hace exactamente el parche 0001 (resumen técnico)
* parallel-gs ya convierte cada textura de la VRAM en una imagen Vulkan en caché y la muestrea con descriptores `bindless` más una tabla `TexInfo` (tamaños, región, sesgo). El parche solo **sustituye la vista de imagen y los valores de normalización de `TexInfo`** de la textura enlazada; la imagen original y la VRAM no se tocan, las coordenadas UV/CLAMP/región siguen en texeles originales.
* Normalización: `sizes.zw = 1 / tamaño del rectángulo reemplazado (texeles originales)` y `bias = -origen / tamaño`; la imagen HD abarca ese rectángulo, así que el muestreo en espacio de texeles originales cae en la posición correcta sea cual sea el factor (entero ≥ 1 por eje).
* La decisión (qué textura es) la toma el anfitrión (`lookup_replacement(desc, game_tex0, out)`), con el TEX0 tal como lo escribió el juego (el descriptor normaliza CBP). parallel-gs comprueba que la ventana muestreada, o el recuadro UV del dibujo, caben dentro del rectángulo reemplazado; si no, usa la original.
* No se sustituye: texturas con super-muestreo (`samples > 1`), con mipmaps del juego, ni texturas con posible retroalimentación del framebuffer.
* Caché de imágenes HD con presupuesto (`budget_bytes`, 512 MiB por defecto) y expulsión por menos uso reciente; los píxeles se piden al anfitrión (`replacement_pixels(key)`) solo cuando la imagen no está en la caché.

## Obligaciones de licencia (LGPL-3.0-or-later)
parallel-gs se distribuye bajo **LGPL-3.0+** (cada archivo lleva `SPDX-License-Identifier: LGPL-3.0+`; el texto está en `COPYING.LGPLv3` de su repositorio). Lo que implica para este proyecto:
1. **Las modificaciones de parallel-gs siguen siendo LGPL-3.0+.** Estos parches son obra derivada y se publican bajo la misma licencia; conservan los avisos de copyright y SPDX de los archivos originales (el parche no los elimina).
2. **Quien reciba un binario del port que incluya parallel-gs debe poder obtener el código fuente correspondiente de la biblioteca modificada**: el commit fijado de Arntzen Software + estos parches (y los scripts de `tools/perf/`) son esa fuente. Si algún día se distribuyen binarios, hay que acompañarlos de este directorio (o una oferta escrita equivalente) y del texto de la licencia LGPL-3.0, e indicar los cambios (este README + el parche).
3. **Enlace**: la LGPL permite enlazar con código propio de otra licencia siempre que el usuario pueda sustituir la biblioteca modificada por otra versión. El port la compila como biblioteca aparte (`parallel-gs` en CMake, estática hoy); si se distribuyen binarios habría que proporcionar los objetos o el medio para reenlazar (o pasar a enlace dinámico).
4. **El código del port (PS2Recomp, `ps2xRuntime`, etc.) no cambia de licencia por usar parallel-gs**, pero cualquier archivo que se copie *dentro* de parallel-gs (como estas modificaciones) sí es LGPL.
5. **Nada de esto se publica ni se sube** mientras el usuario no lo decida: los parches viven solo en el repositorio local. Los datos del juego (texturas, paquetes HD generados desde el disco) nunca forman parte de los parches.
6. Pendiente si se quiere contribuir a upstream: abrir un pull request a Arntzen-Software/parallel-gs con el parche (decisión del usuario; **no se ha enviado nada**).
