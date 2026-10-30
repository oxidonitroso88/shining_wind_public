# Sustitución de archivos y recursos (mods) — auditoría y prototipo, 2026-10-13

## 1. Cadena real de carga de recursos (verificada dinámicamente)
Traza con `PS2X_TRACE_CALLS` (nuevo, ver §6) y registros del CDVD en el juego real:

1. **Gestor de recursos del juego** (`0x12A8E8` abre por nombre: `a0` = cadena `"system.rax"`, `"title.rax"`, `"pac_town03_01.rax"`…; crea el *handle* `0x37D39C` con `0x12A738`). Los nombres son relativos a la raíz de `ST:`. `0x12A940` (el hook histórico de Codex) es solo el punto **inmediatamente posterior a la creación del handle** (ahí `v0` = handle, `s1` = ruta): sirve para observar aperturas, **no** para sustituir contenido. `0x12B4C8` es un envoltorio con bloqueo (`0x12C5A0/0x12C5B8`) de `0x12B520` (exige `a2 & 0x3F == 0`) que salta a `0x12B328`; `0x19F5C0` (y su interior `0x19F5D0`) es una función de **sondeo de estado** que se llama cientos de veces por archivo (`a0` = gestor `0x6B3EB0`, `a1` = handle, `a2` = ruta, devuelve 1 mientras no termina). Es una máquina de estados asíncrona, no una lectura.
2. **Capa CRI `cvFs`** (`0x131D90`, `a0` = objeto `0x389890`, `a1` = ruta): encola la petición; los dispositivos `ST:`/`VOICE:` son contenedores **CVM** (cabecera `CVMH` de 0x1800 bytes + ISO9660 plano; `ROFS` en la cabecera).
3. **CDVDFSV** (RPC al IOP): `read lsn N xM`. El disco virtual (`ps2xIOP/.../iop_cdvd.cpp`, modo directorio) sirve cada contenedor como un archivo de la raíz: `ST.CVM` en el LSN 2280, `VOICE.CVM` en 3 277 658; LSN de disco = base del nodo + 3 (cabecera) + LSN ISO.
4. **Dato clave:** el juego **no lee los registros del directorio raíz del CVM**. Lleva una **copia estática en su ejecutable**: 1 515 registros de 0x30 bytes `{u32 tamaño, u32 0, u32 lsn, 2 bytes, nombre[34]}` en `0x3A8B48–0x3BA758` (empieza por `.` y `..`; 1 503 coinciden archivo por archivo con `ST.CVM`, los otros 12 son directorios). Los subdirectorios (`ADX/`, `COMMON/`…) sí se leen del disco (sus extents LSN 61, 64, 73…).

## 2. Dónde interceptar (comparación)
| Punto | Veredicto |
|---|---|
| Gestor de recursos tras la asignación (`0x12A940`/`0x12B4C8`/`0x19F5D0`) | Descartado como mecanismo. `0x12A940` es previo a la lectura; `0x19F5C0` es un sondeo; copiar bytes en el buffer del propietario exige conocer capacidad, tamaño y momento de finalización de una máquina de estados asíncrona y no cubre archivos mayores. Se conserva para **diagnóstico** (`[ShiningWind] OPEN …`). |
| FILEIO/`cdrom0:` del runtime | Cubre solo la raíz real del disco (`OVERLAY/`, `MODULES/`, `SYSTEM.CNF`): útil y barato (se usa para `DISC/`), pero los recursos del juego no pasan por ahí. |
| **Sectores del disco virtual (debajo de `cvFs`)** | **Elegido.** Todo consumidor (lecturas parciales, `seek`, EOF, alineación, errores, asincronía) sigue siendo el original porque lo único que cambia son los bytes servidos. No toca overlays ejecutables (usan `DISC/` y `FILEIO`, y el emisor de overlays identifica el código por contenido). |
| + parche de la tabla raíz en RAM del invitado | Necesario solo para tamaño/extent de archivos de la **raíz** de `ST.CVM`, porque el juego no los lee del disco. Datos-dirigido, sin tocar código generado. |

## 3. Diseño implementado
* `ps2xIOP/include/ps2x/iop/mod_overlay.h`, `src/mod_overlay.cpp`: `normalizeLogicalPath`, `ModRoot`, `CvmOverlay`; integrados en `iop_cdvd.cpp` (`applyMods` al construir el disco virtual) y `shining_wind_overrides.cpp` (`applyResourceTablePatches`).
* **Carpeta de mods** (`PS2X_MODS=<carpeta>`; `off`/`0` la desactiva; sin variable se usa `./mods` si existe; sin carpeta no hay mods):
  * `mods/DISC/<ruta>` sustituye un archivo del árbol raíz del disco (`DISC/OVERLAY/OVL09.BIN`, `DISC/MODULES/…`). Cualquier tamaño (el disco virtual calcula su disposición).
  * `mods/ST/<ruta>` y `mods/VOICE/<ruta>` sustituyen un archivo dentro de `ST.CVM`/`VOICE.CVM`. Si cabe en los sectores originales se escribe en el sitio; si no, se **añade tras los datos originales** (el tamaño del volumen y los registros ISO se parchean al servirlos, nada se escribe en disco). El nodo del contenedor crece en el disco virtual.
* **Seguridad de rutas:** mayúsculas/barras normalizadas, sufijo `;1` ignorado, se rechazan vacías, absolutas, `..`, `.`, unidades, caracteres de control o ilegales en Windows y nombres con espacio/punto final; enlaces simbólicos ignorados; cada archivo debe resolver dentro de la carpeta. Archivos no presentes en el contenedor, solapados o ilegibles se registran y se ignoran.
* **Diagnóstico:** `[mods] ST/TITLE.RAX: 652688 -> 660943 bytes, appended at ISO sector 1637524 <- …`, `[mods] serving …` (primera lectura), `[mods] game file table: … size a -> b, lsn c -> d`, avisos de archivos descartados. Los archivos de mods se abren por lectura y se cierran (no quedan bloqueados mientras el juego corre).
* **Dependencia de datos del juego:** la dirección de la tabla raíz es de esta versión del ejecutable; si el primer registro no es `.` no se parchea y se avisa.

## 4. Resultados
* Pruebas: `ps2_mod_overlay_tests` 67 comprobaciones (seguridad de rutas, índice, parches en sitio/menor/mayor/reubicado, registros ISO coherentes en ambos órdenes de bytes, tamaño de volumen, integración con el disco virtual, `DISC/`, desactivable), `ps2_cdvd_sector_tests` 73 y `ps2_cdvdfsv_tests` 74 siguen pasando.
* **Juego real, archivo real** (`TITLE.RAX`, 652 688 bytes, cargado al arrancar):
  1. +63 bytes con marcador (cabe en los 319 sectores): el marcador aparece en la RAM del invitado (`0x9E5B60`).
  2. +8 255 bytes (323 sectores → reubicado en el sector ISO 1 637 524): el juego lee el tamaño y el LSN nuevos de su tabla, el disco sirve el archivo de la carpeta, el marcador y los 8 192 bytes añadidos están íntegros en RAM, 0 `missing-target`, el juego arranca y llega al menú.
  3. `PS2X_MODS=off` con la carpeta presente: sin líneas `[mods]`, sin marcador. Sin la variable y sin carpeta `mods`, comportamiento original.
* Corregido de paso: el override de Shining Wind era **código muerto** (la librería estática descartaba `shining_wind_overrides.o` por no estar referenciado; no aparecía nunca `[ShiningWind] path hook installed`). Se añadió un ancla de enlace; desde ahora el hook de aperturas y las nuevas funciones están activos.

## 5. Límites y pendientes
* Archivos de **subdirectorios** de `ST.CVM`: probado a nivel de sectores/registros en pruebas unitarias; no probado en el juego (todavía no hay un recurso de subdirectorio con efecto visible a mano).
* `VOICE.CVM`: el mismo mecanismo de sectores existe (`mods/VOICE/…`); no se ha buscado su tabla estática ni se ha probado.
* Los contenedores internos (RAX/PAC/DPK/TXR…) siguen siendo formato propio del juego: la sustitución es de **archivos completos**; editar su contenido es la Prioridad 4.
* Sin guardados: el sistema no escribe nada en `D:\NO_LABEL`, la tarjeta ni los mods.

## 6. Herramientas
* `tools/mods/cvm_tool.py list|extract|check`: lista/extrae archivos de un CVM (solo lectura) y dice si un archivo modificado cabe en sitio o se añade.
* `PS2X_TRACE_CALLS="0xADDR,…"` (+ `PS2X_TRACE_MAX`): envuelve funciones recompiladas con un trazador que imprime `a0–a3`, `ra`, `sp`, cadenas apuntadas y `v0` (`ps2_call_trace.cpp`).
* `tools/perf/load_slot1.ps1` acepta `-McRoot` (copia de la tarjeta), `-Env @{…}` y `-NoLoad`.
