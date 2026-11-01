# Mods: traducción al inglés y texturas HD

Esta guía explica **qué archivos** son los mods, **dónde se ponen** y **cómo comprobar** que el juego los usa. Este repositorio **no contiene** ningún mod ni ningún archivo del juego: solo el programa y esta guía. Los mods se descargan aparte (ver [Dónde descargar](#6-dónde-descargar)) y **necesitás tu propio disco** de *Shining Wind* (NTSC-J, SLPM-66671).

> Antes de empezar: el juego ya debe funcionar sin mods (ver [TUTORIAL.es.md](TUTORIAL.es.md)). Probá primero que arranca y llega al menú.

## 1. Carpeta de datos

Todo va dentro de la **carpeta de datos** del juego:

| Cómo arrancaste | Carpeta de datos |
|---|---|
| `ShiningWind.exe` sin opciones | `%APPDATA%\ShiningWind` (pegalo en el explorador) |
| `ShiningWind.exe --data-dir "D:\MisDatos"` | la que indicaste, p. ej. `D:\MisDatos` |

Dentro de ella conviven estas carpetas (las crea el juego salvo `mods` y `hd_packs`, que creás vos):

```
<carpeta de datos>\
  disc\          <- copia de los archivos de tu disco (NO tocar)
  memcards\      <- tus partidas guardadas
  logs\          <- registro de errores
  mods\          <- traducción (ver sección 2)
  hd_packs\      <- texturas HD (ver sección 3)
```

Si una carpeta no existe, creala con ese nombre exacto.

## 2. Traducción al inglés (carpeta `mods`)

La traducción consta de dos partes que se copian **tal cual vienen** dentro de `mods\`, conservando las subcarpetas:

```
mods\
  story_en.tsv         <- diálogos de la historia (un solo archivo de texto)
  ST\
    SYSTEM.RAX         <- menús, ayudas, estados, enemigos, objetos, habilidades…
    SAVELOAD.RAX       <- pantallas de guardar/cargar
    BLACKSMITH.RAX  ITEMSHOP.RAX  SOULPIECE.RAX   <- tiendas
    EQUIP.RAX  STATUS.RAX  RESULT.RAX  ITEM.RAX  …  <- otros menús
    SHOP\…   ENEMYINFO\…                          <- subcarpetas (copiarlas enteras)
    PAC_STAGESELECT_*.RAX  WORLDMAP*.RAX  …       <- selección de mapa y mapa del mundo
```

Reglas:

* **Qué hace cada cosa.** Un archivo de `mods\ST\` **sustituye** al archivo de igual nombre que está dentro del contenedor `ST.CVM` de tu disco. Si no hay archivo en `mods\ST\`, el juego usa el original. Por eso podés copiar solo algunos.
* **El nombre debe coincidir exactamente** (mayúsculas da igual; se ignoran las diferencias de mayúsculas y el sufijo `;1`). Los archivos con nombres no reconocidos se ignoran y se anotan en el registro.
* **`story_en.tsv`** va directamente en `mods\` (no dentro de `ST`). Se aplica en tiempo de ejecución sobre los diálogos; no modifica ningún archivo.
* Se rechazan rutas absolutas, con `..`, con letras de unidad o con caracteres no válidos en Windows.
* Los archivos de `mods\` no se modifican ni se bloquean mientras el juego corre. Tus partidas no se tocan.
* **Reemplazar una versión por otra:** copiá encima (Windows pregunta si sobrescribir). Para volver al original, borrá el archivo de `mods\` (o renombrá la carpeta `mods` a `mods_off`).
* **Desactivar temporalmente todos los mods:** variable de entorno `PS2X_MODS=off` antes de arrancar (`set PS2X_MODS=off` en una consola, y desde ella `ShiningWind.exe`). Para el texto de la historia: `PS2X_STORY_EN=off`.

### Comprobar que carga
Lo seguro es mirarlo en pantalla: con la traducción instalada, el menú del título y el de pausa (Start en el mapa) salen en inglés. Si querés ver el detalle técnico, al arrancar desde una consola (`ShiningWind.exe` o `play-x4.cmd`) aparecen líneas `[mods] ST/SYSTEM.RAX: <tamaño original> -> <tamaño nuevo> bytes ...` por cada archivo sustituido, y avisos de los que se ignoran. (`logs\ShiningWind.log` es un registro corto de errores y puede no incluirlas.) Si siguen en japonés, mirá la sección 5.

## 3. Texturas HD (carpeta `hd_packs`)

Un **paquete HD** es una carpeta con este aspecto:

```
hd_packs\
  NombreDelPaquete\
    manifest.json      <- lista qué textura del juego sustituye cada imagen
    textures\          <- las imágenes (PNG) con el arte nuevo
    LEEME.txt          <- (opcional) qué contiene
```

Reglas:

* **Una carpeta por paquete**, con su `manifest.json` directamente dentro. No la dejes dentro de otra carpeta adicional (`hd_packs\Paquete\Paquete\manifest.json` **no** funciona).
* El `manifest.json` identifica cada textura por el archivo y recurso del juego y por su contenido (hash); si tu disco no es la edición NTSC-J SLPM-66671, no coincidirá y no se aplicará nada (el juego se ve como el original).
* **Hace falta la resolución interna ×4:** arrancá con **`play-x4.cmd`** (no con `ShiningWind.exe`). Con ×1 los paquetes HD no se ven.
* Podés tener varios paquetes a la vez (p. ej. uno de retratos y otro de menús). **Evitá que dos paquetes reemplacen la misma textura**; si lo hacen, solo se usa una de las dos y no está garantizado cuál.
* Cuesta memoria de vídeo: cuantos más paquetes y más grandes, más VRAM. El juego descarta las texturas menos usadas si se queda sin presupuesto (`PS2X_HD_BUDGET_MB`, 512 MB por defecto).
* **F10** activa/desactiva los reemplazos HD con el juego en marcha: sirve para comparar con el original al instante.
* Para quitar un paquete: sacá su carpeta de `hd_packs\`.

## 4. Pasos resumidos

1. Instalá y probá el juego sin mods (tutorial).
2. Descargá los mods (sección 6) y descomprimilos en una carpeta temporal.
3. Copiá su carpeta `mods\` (con `story_en.tsv` y `ST\`) dentro de la carpeta de datos, y/o las carpetas de paquetes HD dentro de `hd_packs\`.
4. Arrancá con `play-x4.cmd` (o `ShiningWind.exe` si solo usás la traducción).
5. Comprobá el menú en inglés y usá F10 para ver las texturas HD.

## 5. Si algo falla

| Síntoma | Causa probable |
|---|---|
| Sigue todo en japonés | la carpeta `mods` no está en la carpeta de datos que usa el juego (¿arrancaste con `--data-dir`?), o está anidada (`mods\mods\ST`), o el nombre no coincide |
| Aviso de archivo ignorado en la consola | nombre/ruta no válidos o archivo que no existe en tu `ST.CVM` |
| Texto cortado o raro en una caja | mod hecho para otra versión del texto; avisalo al autor del mod con el nombre del archivo |
| Las texturas HD no se ven | no arrancaste con `play-x4.cmd`; `manifest.json` no está directamente en la carpeta del paquete; edición de disco distinta; o F10 las tiene desactivadas |
| Parpadeo, lentitud o error de memoria con muchos paquetes | demasiada VRAM: quitá paquetes o bajá `PS2X_HD_BUDGET_MB` |
| Quiero comprobar que el juego sin mods está sano | renombrá `mods` y `hd_packs` o usá `PS2X_MODS=off` |

Antes de reportar un error, **probá sin mods**. Si falla igual, no son los mods.

## 6. Dónde descargar

*(Enlace a completar por el autor de los mods.)*

Los mods los publica un tercero fuera de este repositorio. Este repositorio no los aloja, no los revisa y no da soporte de su contenido.

## 7. Aviso

* Este proyecto no incluye ni distribuye el juego, imágenes de disco, ni archivos extraídos o modificados de él. Necesitás tu propia copia.
* Los mods son obras de sus autores. Usalos solo con una copia legítima del juego.
* Esta guía no es asesoramiento legal.
