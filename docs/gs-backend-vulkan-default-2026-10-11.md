# parallel-gs (Vulkan) como backend de GS predeterminado — 2026-10-11

## Decisión y selección
* **Vulkan (parallel-gs) es el backend predeterminado** cuando la compilación lo incluye y puede inicializarse. El rasterizador por software sigue construyéndose siempre y no se tocó.
* Selección en ejecución (`ps2xRuntime/src/lib/gs/gs_frontend.cpp`, `makeDefaultBackend`):
  * sin variables: Vulkan; si Vulkan no arranca → **respaldo automático al software** con mensajes (`[gs] parallel-gs (Vulkan) could not start: <motivo>` y `[gs] FALLBACK: Vulkan backend unavailable, using the software rasteriser`);
  * `PS2X_GS_BACKEND=CPU` (o `SOFTWARE`): software explícito (`[gs] backend: software rasteriser (selected ...)`);
  * `PS2X_GS_SYNC` o `PS2X_GS_CAPTURE` también implican software (trabajan sobre su flujo de primitivas; con Vulkan la captura saldría vacía);
  * un valor desconocido de `PS2X_GS_BACKEND` se avisa y se usa el predeterminado.
* Compilación: `PS2X_GS_PARALLEL` está **ON por defecto en Windows cuando existe `third_party/parallel-gs`** (o `-DPS2X_PARALLEL_GS_DIR=<ruta>`); si no hay copia, la compilación queda solo con software y lo dice (no falla). `tools/perf/fetch_parallel_gs.ps1` descarga el commit fijado (3a66c19) con solo los submódulos de Granite necesarios (~146 MB). La copia está ignorada por git (LGPL, no se versiona).
* Los ejecutables de pruebas fijan `PS2X_GS_BACKEND=CPU` (no deben crear un dispositivo Vulkan por cada instancia de GS).

## Validación antes de dejarlo como predeterminado
| Comprobación | Resultado |
|---|---|
| Arranque por defecto (35 s) | `[gs] backend: parallel-gs (Vulkan)`, fotogramas, 0 `missing-target` |
| `PS2X_GS_BACKEND=CPU` (35 s) | software seleccionado, funciona |
| Vulkan inutilizado (`VK_ICD_FILENAMES` a un ICD inexistente) | `Failed to create Vulkan instance` → mensaje claro → **respaldo a software**, el juego corre (35 s) |
| Recorrido guionizado de 471 s con Vulkan (`out/evidence/movement/run3_vulkan`) | mismas escenas que el software en los mismos instantes (intro, cristal, camino, puerta, vestíbulo, caminata con el stick, evento con diálogo, menú de campo; hoja `out/evidence/movement/compare_sw_vulkan.png`), 58-59 fps constantes, saltos de volcado ≤1,3 s, 0 `missing-target` |
| 4 ejecuciones de 105 s con 8 procesos de CPU ocupados (`b0010/hunt5_vulkan_load`) | 0 paradas, 0 `missing-target` |
| Pruebas | GS golden 11/11 (software, hash de `slow1` idéntico), `ps2_gs_pool_tests`, IPU 15/15, SIF 30/30, `ps2x_tests` 447/447 |
Rendimiento (medido antes, misma configuración): CPU del proceso 82 % frente a 221 % de un núcleo; 116 % frente a 310 % en el diálogo del cristal (`docs/perf-branch-evaluation-2026-10-11.md`).

## Límites conocidos (no bloquean)
* Probado solo en NVIDIA RTX 5060 Ti (driver 595.97); AMD/Intel sin verificar. Si el dispositivo Vulkan no cumple los requisitos de parallel-gs (descriptor indexing, subgrupos, descriptor buffers) se cae al software con el motivo.
* Las primeras ejecuciones con la caché de pipelines del driver en frío consumen más CPU durante minutos.
* Diferencia sistemática de 1 px con el software en coordenadas fraccionarias (el software es el que se aparta; ver pendientes).
* Transferencias local→host y `WriteVram` pasan por un espejo de VRAM de 4 MiB (raras en lo observado). El parche de colas de transferencia de la rama de performance no se aplicó.
* Distribución: LGPLv3+ enlazada estáticamente (ver informe de rendimiento, apartado de licencias).
