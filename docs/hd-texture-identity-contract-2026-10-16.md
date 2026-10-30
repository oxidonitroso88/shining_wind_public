# Contrato de paquetes de texturas HD (versión 1, definitivo) — 2026-10-16

Contrato entre el **editor de archivos del CVM (Codex)** y el **runtime del port**: el editor exporta un paquete (`manifest.json` + PNG) y el port lo aplica sin depender de la resolución interna, de direcciones de VRAM ni de hashes frágiles. Implementado y verificado (ver `docs/hd-texture-pack-prototype-2026-10-16.md` y `docs/hd-4bpp-indexed-2026-10-16.md`). Esquema legible por máquina: `docs/hd-pack-manifest.schema.json`; validador: `tools/texmap/validate_hd_pack.py`.
Principio: la identidad de una textura es **archivo + región dentro del recurso**; offsets y hashes son comprobaciones. **Ante cualquier duda el port usa la textura original**: un paquete nunca puede romper el juego, solo dejar de aplicarse.

## 0. Qué tiene que producir el editor (resumen)
```
<paquete>/manifest.json
<paquete>/textures/**/*.png          (rutas relativas, dentro de la carpeta del paquete)
```
Por cada imagen reemplazada una entrada de `textures[]` con: el **archivo lógico** y la **región de píxeles** (`file`, `stream_offset`, `offset`, `length`, `sha1`), el rectángulo original (`width`, `height`, `psm`), la imagen (`path`, `width`, `height` = múltiplo entero), el `mode` (`rgba` o `indexed`) y, para `rgba` de texturas con paleta, el **SHA-1 de la paleta** para la que se pintó. Nada más es obligatorio. El PNG cubre exactamente el rectángulo original (misma posición, mismas proporciones); no hay márgenes, ni desplazamientos, ni recortes.

## 0b. Instalación (para los usuarios)
Un paquete es una carpeta (`manifest.json` + `textures/`). Para instalarlo basta **copiar la carpeta dentro de `hd_packs/`**, junto al ejecutable del port (el directorio de trabajo): no hay que tocar el ejecutable ni los archivos del juego. Se cargan todas las subcarpetas de `hd_packs/` que tengan `manifest.json`, **por orden alfabético; si dos paquetes reemplazan la misma textura gana el último** (`00-base`, `10-interfaz`…). `PS2X_HD_PACK=<carpeta>` (o `play.ps1 -HdPack`) usa otra ubicación —un paquete o una carpeta de paquetes— y `PS2X_HD_PACK=off` los desactiva; `F10` activa/desactiva en caliente. Un paquete roto se ignora con un aviso y el juego sigue con sus texturas originales.

## 1. Identidad de un recurso
```
file            "ST/TITLE.RAX"          ruta lógica en mayúsculas con '/' (ST/…, VOICE/…, DISC/…): archivo servido al juego
resource        {"container_kind":"RAX","index":1,"type":"TXR","name":"title/title_press_a.txr"}      informativo
                {"container_kind":"LZSS-blocks","index":32}                                           (BRA: ordinal del bloque LZSS)
stream_offset   45004    offset del bloque 'LZSS' (cabecera de 0x10 bytes) dentro de `file`; AUSENTE si la región son bytes planos del archivo
pixels          {"offset":16,"length":8064,"sha1":"…"}    offset dentro del flujo DESCOMPRIMIDO (o del archivo si no hay stream_offset)
```
* **Clave de emparejamiento en ejecución**: `file` + `stream_offset` + `pixels.offset` + `pixels.length` (los bytes son los que recibe el GS en la transferencia: ráster lineal, 1 byte por texel en PSMT8, 2 texeles por byte —nibble bajo primero— en PSMT4, 4 bytes RGBA en CT32).
* `pixels.sha1` es la comprobación: si los bytes reales no coinciden, el archivo no es el que el paquete espera y la entrada no se aplica.
* Los offsets son los del archivo **tal como lo ve el juego** (con mods `PS2X_MODS` pueden diferir del original: generar el paquete sobre el mismo archivo que se sirve).
* **Buffers que el juego modifica tras cargarlos** (estado `modified`): p. ej. el atlas de la fuente de `SYSTEM.RAX`, al que el juego escribe los glifos de kanji al cargar. Siguen atados a su región de origen, pero lo que recibe el GS ya no coincide con el archivo, así que la entrada se identifica además por el **contenido realmente subido**: `pixels.sha1` = SHA-1 de los bytes que recibe el GS (obligatorio para estas entradas). Ese contenido es determinista (un único contenido por región en toda la sesión grabada) y no puede calcularse desde el archivo: sale de `identities.json` (`pixels_identity: "modified"`, con el resumen `modified` de qué filas cambia el juego) y de su `png` decodificado. Si el juego subiera otro contenido (otra modificación), el SHA-1 no coincide y se usa la original.
* Si el mismo contenido (bytes idénticos) existe en varios recursos (p. ej. la misma imagen duplicada en dos archivos), el port lo considera **orígenes equivalentes** (§4.3): puede haber una entrada por origen; si los candidatos tienen entradas con la misma imagen (o solo uno tiene entrada) se aplica; si se contradicen, se usa la original.

## 2. `manifest.json`
```json
{
  "format": "ps2x-hd-pack",
  "version": 1,
  "game": "SLPM-66671",
  "textures": [
    {
      "id": "ST/TITLE.RAX@0xafcc+0x10:pixels",
      "file": "ST/TITLE.RAX",
      "resource": {"index": 1, "type": "TXR", "name": "title/title_press_a.txr"},
      "pixels": {"stream_offset": 45004, "offset": 16, "length": 8064, "sha1": "61016736ebc2e2c8347059961591cf0e20af5cf3"},
      "palette": {"sha1": "edb9412628e77d5798004e42f749dc29b4208e48", "length": 1024},
      "original": {"width": 336, "height": 24, "psm": 19},
      "replacement": {"path": "textures/ST/TITLE.RAX/0001_title_press_a.png", "width": 672, "height": 48, "mode": "rgba", "filter": "linear"}
    },
    {
      "id": "ejemplo ilustrativo: glifo de interfaz en modo indexed",
      "file": "ST/SYSTEM.RAX",
      "pixels": {"stream_offset": 646144, "offset": 16, "length": 72, "sha1": "…"},
      "original": {"width": 16, "height": 9, "psm": 20},
      "replacement": {"path": "textures/system/glyph.png", "width": 64, "height": 36, "mode": "indexed", "filter": "linear"}
    }
  ]
}
```
| Campo | Reglas |
|---|---|
| `format`, `version` | `"ps2x-hd-pack"`, `1`. Otra cosa: el paquete se rechaza entero. Propiedades desconocidas se ignoran (compatibilidad hacia delante). |
| `file` | obligatorio, ver §1. |
| `pixels` | obligatorio: `offset`, `length` (>0); `stream_offset` solo si la región está dentro de un bloque LZSS (también admitido a nivel de entrada); `sha1` (recomendado: 40 hex). |
| `palette` | para **`rgba` de texturas con paleta (psm 19/20): obligatorio `sha1`** (SHA-1 de los bytes de la subida de la paleta tal como los recibe el GS: 1 024 bytes en PSMT8 —CT32 16×16—, 64 bytes en PSMT4 —CT32 8×2—). Se empareja **por contenido**, procedan de donde procedan. `file`/`stream_offset`/`offset`/`length` son informativos. Para `indexed` no se usa. |
| `original` | `width`, `height`, `psm` del rectángulo que sube el juego (`19` PSMT8, `20` PSMT4, `0` CT32, `1` CT24, `2` CT16). Deben coincidir con la subida real. |
| `replacement.path` | PNG relativo a la carpeta del paquete (debe quedar dentro). |
| `replacement.width/height` | **múltiplo entero** de `original.width/height` (factor ≥ 1 por eje; se recomiendan ×2 y ×4; también ×1 es válido). |
| `replacement.mode` | `"rgba"` (por defecto) o `"indexed"` (§3). |
| `replacement.filter` | `"linear"` (por defecto: bilineal aunque el juego use *nearest*) o `"nearest"` (respeta el filtro que programó el juego). Con `nearest` y una imagen obtenida replicando píxeles el resultado es **idéntico bit a bit** al original (prueba de exactitud). |
| `replacement.mipmaps` | ignorado en la versión 1. |
| `id`, `resource`, `authored_palette_sha1` | informativos. |
Un paquete con entradas inválidas **carga el resto**: cada entrada descartada se avisa por el log (`[hdpack] entry … skipped: motivo`) y esa textura queda con su original.

## 3. Imágenes
### 3.1 PNG `rgba`
* RGBA8 (cualquier PNG que se convierta a RGBA). Origen = esquina superior izquierda del rectángulo subido; filas de arriba abajo. Mismo tamaño que `replacement.width × height`.
* **Alfa**: el GS usa 0..0x80 (0x80 = opaco). Un PNG con A=255 equivale a 0x80 y A=0 a 0; los 129 valores del GS viajan sin pérdida (`A = min(255, a·255/128)` al exportar, `a = (A·128+127)/255` al importar). Los valores de alfa **superiores a 0x80** (efectos que sobreexponen) no caben en un PNG: esas texturas deben ir en modo `indexed`.
* Una entrada `rgba` de textura con paleta solo se aplica cuando la paleta en uso tiene exactamente el SHA-1 del manifiesto; con otra paleta (fundidos, recolores…) se usa la original. Se pueden dar varias entradas con los mismos píxeles y paletas distintas.
### 3.2 PNG `indexed` (paleta dinámica)
* PNG **en escala de grises de 8 bits** (colour type 0, o gris+alfa), del tamaño de `replacement`; el **valor de cada texel es el índice de paleta** del texel original (0..15 en PSMT4, 0..255 en PSMT8). No se admiten PNG con paleta (PLTE) ni RGB: se descartan.
* En ejecución el port aplica la **paleta viva** del juego (la subida actual de la paleta, con su diseño CSM1 y su CSA) a ese mapa de índices y filtra *después* de aplicar la paleta: fundidos, recolores, ciclos de paleta y efectos de alfa de la paleta siguen funcionando con **una sola imagen** para todas las paletas (p. ej. un glifo de la interfaz que el juego colorea con paletas distintas, o un efecto de combate con 15 paletas).
* Mapa de índices ampliado: normalmente replicación por píxel del mapa original (`tools/texmap/make_hd_pack.py --mode indexed`); el editor puede usar cualquier proceso que conserve índices válidos (p. ej. reescalado por reglas de borde de los índices o repintado a mano con la misma paleta).
### 3.3 Geometría
El reemplazo cubre el rectángulo subido tal cual: posición, proporciones y UV del juego no cambian (el sombreador sigue en texeles originales y la imagen HD se normaliza con el tamaño original). El juego **renderiza por campos** (búferes de 640×224); un reemplazo ×N luce a su densidad completa con resolución interna mayor (`PS2X_GS_SCALE=2`, independiente de los paquetes).

## 4. Resolución en ejecución
`PS2X_HD_PACK=<carpeta>` (o `play.ps1 -HdPack <carpeta>`); `F10` activa/desactiva en caliente. Sin paquete o con paquete inválido el juego es idéntico al original.
### 4.1 Condiciones (todas; si falla una, textura original)
1. Formato: PSMT8 con paleta (CSM1, CT32), **PSMT4 con paleta (CSM1, CT32, CSA 0 o 16×16)** o formatos directos (CT32/CT24/CT16). No admitidos: PSMT8H/PSMT4HL/PSMT4HH, paletas CT16/CSM2.
2. Hay una subida seguida cuya base es exactamente `tbp0`, con el mismo formato, el mismo ancho de búfer y sin desplazamiento: el espacio de texeles de la textura es el rectángulo subido (no sirven las subidas como CT32 muestreadas como PSMT8/PSMT4 —atlas—, ni las texturas sin subida seguida).
3. Los píxeles tienen identidad **única**, **equivalente** (§4.3) o **modificada** (§1: la entrada debe llevar `pixels.sha1` igual al contenido subido) y proceden de **una región contigua** de un recurso. Los bytes que la transferencia GIF añade para redondear a quadword se descartan. Texturas generadas por completo en memoria (sin origen en un archivo) o cuyo origen no se puede seguir no tienen identidad → original.
4. El paquete tiene una entrada con esa región exacta y los SHA-1 coinciden; `original.*` coincide con la subida.
5. Paleta: `rgba` → el SHA-1 de la subida de paleta coincide con `palette.sha1`; `indexed` → existe una subida de paleta entera (16×16 CT32 para PSMT8; 16×16 u 8×2 CT32 para PSMT4) a la base `cbp`, legible (con `csa`≠0 solo en subidas 16×16).
6. El rectángulo muestreado, o el recuadro UV del dibujo (`uv_bb`), está dentro del rectángulo reemplazado.
7. No hay retroalimentación del framebuffer, mipmaps del juego ni super-muestreo de texturas (`PS2X_GS_SSTEX=1`).
8. El PNG existe, se decodifica y su tamaño/formato es el del manifiesto (si no, la entrada se desactiva).
### 4.2 Orden de decisión y diagnóstico
`PS2X_HD_STATS=1` imprime contadores por motivo (`applied`, `applied (indexed)`, `applied (equivalent origins)`, `no tracked upload (dynamic or unknown texture)`, `refused: …`); `PS2X_HD_LOG=1` lista cada textura rechazada con su motivo.
### 4.3 Orígenes equivalentes
Si los mismos bytes proceden de más de un recurso (o el contenido se halla en varios), el libro registra los candidatos (hasta 6). Se buscan entradas del paquete para cada candidato: una sola imagen distinta → se aplica; imágenes distintas → «equivalent origins are replaced with different images» → original. (Texto del contrato anterior: «ambiguo ⇒ original» sigue valiendo para el resto de ambigüedades: el contenido se modificó tras registrarse, o no se sabe de dónde viene.)
### 4.4 Paletas
Las paletas se identifican por **contenido**, no por origen: la subida de paleta (CT32) y su SHA-1. El diseño CSM1 se resuelve en el port: en una subida 16×16 la entrada *i* está en la posición `(i&0xE7)|((i&0x10)>>1)|((i&0x08)<<1)` del ráster de la subida; en una subida 8×2 (PSMT4) la entrada *i* es el texel *i*.

## 5. Cómo obtener los datos (para el editor)
1. Para cada TXR/imagen editable el editor conoce `file`, el recurso y, si está comprimido, `stream_offset`; `pixels.offset/length` son la región de píxeles dentro del flujo descomprimido (cabecera de 0x10 bytes en los TXR) y la paleta (si la hay) va en el mismo recurso (TXR 8 bpp: 1 024 bytes tras los píxeles) o en otro lugar (BRA: bytes 24..1048 del archivo). Sus SHA-1 se calculan con su propio LZSS; `decomp/*.bin` de una captura da la salida de referencia del descompresor del juego.
2. Lo que ve realmente el GS (verdad de referencia) se captura con `play.ps1 -TexMap <carpeta> -Replay … -FastForwardToEnd` y `python -I tools/texmap/texmap_report.py --map <carpeta> --cvm D:/NO_LABEL/ST.CVM --out identities.json`: cada identidad con su región, SHA-1 verificados contra el archivo original, rectángulo/formato, paleta (SHA-1, forma) y, si procede, orígenes equivalentes.
3. Un paquete de prueba completo se genera con `tools/texmap/make_hd_pack.py` (estilos `nearest`, `lanczos`, `mark`, `ruler`; modos `rgba` e `indexed`) y se **valida** con `python -I tools/texmap/validate_hd_pack.py <paquete> [--identities identities.json] [--strict]` (campos, factores enteros, PNG de gris para `indexed`, índices dentro de rango, identidades duplicadas y, con la captura, coincidencia de región/hash/rectángulo/paleta).
4. Prueba de referencia de todo el sistema: `python -I tools/validation/hd_pack_exactness.py --map <captura> --identities identities.json`: paquetes ×1 de todas las texturas identificadas (`rgba` e `indexed`) no deben cambiar ni un píxel del juego.

## 6. Salida del seguimiento (`PS2X_TEXMAP`)
`textures.jsonl` (una línea por textura distinta dibujada: `tex0`, `pixels` y `palette` con `identity` = `unique` | `equivalent` | `ambiguous` | `none`, `reason`, `pieces[]`, `candidates[]`, `sha1`, `upload{dbp,dpsm,dbw,x,y,w,h,bytes}`, `png`), `events.jsonl` (`PS2X_TEXMAP_TRACE=1`), `decomp/<archivo>@<offset>.bin` y `png/*.png` (decodificación de referencia con la paleta aplicada). `pieces[]`: `{"stage":"lzss","stream_file","stream_offset","raw_offset","bytes","verified"}` o `{"stage":"file","file","offset","bytes","verified"}`.
Eslabones de la cadena: lectura de disco del IOP → descompresión LZSS del juego (`0x221FF0`) → copias por contenido (anclas de contenido, 48 MiB) → subida DMA al GIF → subida al GS (libro de la VRAM) → dibujo (TEX0). Las regiones del libro se invalidan cuando algo posterior las pisa.

## 7. Casos reales de Shining Wind (sesión grabada completa, FILE02, ≈6 min de juego)
* Texturas: 8 bpp con paleta y 4 bpp (9 373 combinaciones de 4 bpp, todas paletas de 16 entradas y CSA 0). 1 478 identidades distintas verificadas (1 326 PSMT8, 147 PSMT4, 4 con orígenes equivalentes, 1 CT32). Archivos: sprites de enemigos (`PAC_ENM_*`), campo (`PACK006A`, `PACK015A`, `PAC_TOWN*`, `PAC_ROUTE*`), selección de personaje (`CHARACTERSELECT`), animaciones de personajes (`CHARA/PLAYER/*_EV1.BRA`), retratos, efectos (`PAC_EFF_*`), interfaz (`TITLE`, `SYSTEM`, `RESULT`, `SAVELOAD`…).
* Paletas dinámicas: en esta sesión solo **1 región de píxeles con 2 paletas** cumple la geometría exacta (un elemento de menú de `TITLE.RAX`, normal/resaltado); el resto de las texturas con paleta usan una sola. Las demás combinaciones "píxeles con varias paletas" que se ven en bruto eran subidas de paleta confundidas con texturas (se descartan por la geometría). Otras partes del juego que esta sesión no recorre (recolores, fundidos) usarán el modo `indexed` con una sola imagen.
* Cobertura en ejecución con un paquete completo: 72 % de las consultas del GS se sustituyen; 22 % son texturas sin subida seguida (dinámicas); 5 % ambiguas (el juego modifica el búfer tras cargarlo: sobre todo `SYSTEM.RAX`: texto/ventanas generados); < 1 % sin entrada o sin paleta seguida.
* Ejemplo con paleta en el mismo flujo: `ST/TITLE.RAX` entrada 1 `title/title_press_a.txr` (336×24, 8 bpp): flujo LZSS en `0xAFCC`, píxeles en `[0x10, 0x10+8064)`, paleta en `[8080, 8080+1024)` del mismo flujo. Ejemplo con paleta en el archivo: `ST/CHARA/PLAYER/006A_EV1.BRA`, bloque LZSS 32 (`0x21204`): píxeles 40×72, paleta = bytes 24..1048 del propio archivo.

## 8. Límites conocidos
* Sin identidad (siempre original): texturas generadas o modificadas por la CPU tras cargarlas, render-a-textura, vídeo, atlas subidos como CT32 y muestreados como PSMT8/PSMT4, PSMT8H/4HL/4HH, paletas CT16/CSM2, texturas con super-muestreo (`PS2X_GS_SSTEX=1`) o con mipmaps del juego.
* `rgba` no representa alfa > 0x80 (usar `indexed`); `rgba` solo vale para la paleta del manifiesto.
* El borde de una textura **parcialmente subida** muestreada con filtro bilineal puede mostrar un tono (1 nivel) distinto al original (la imagen HD se envuelve en el borde en lugar de leer la memoria vecina). Medido en la sesión completa con ×1 y todo el paquete: 2 de 323 capturas (412 píxeles de 1 nivel en una columna; 1 464 píxeles en la barra inferior de la interfaz, hasta 45 niveles).
* Los mipmaps no se usan (el sombreador no tiene derivadas): a densidades altas con resolución interna 1× puede verse aliasing leve con `nearest`; con `linear` y resolución interna ×2 no.
* Los archivos modificados con `PS2X_MODS` se indexan con su extensión servida.
* Las identidades por contenido dependen de que el origen siga entre los últimos 48 MiB registrados.

## 9. Versionado
Versión 1 = este documento. Campos nuevos futuros serán opcionales (los desconocidos se ignoran); un cambio incompatible incrementa `version` y el runtime rechazará paquetes de versión superior. Cualquier modo nuevo (`replacement.mode`) desconocido descarta solo esa entrada.
