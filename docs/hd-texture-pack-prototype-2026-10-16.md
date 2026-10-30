# Prototipo de paquetes de texturas HD (etapa 3) — 2026-10-16

Estado: **funciona** para reemplazos RGBA ×N entero de `title/title_press_a.txr` (TITLE.RAX) y de los 33 fotogramas de sprite de `006A_EV1.BRA`, con identidad estable (archivo + recurso), sin tocar la VRAM ni los archivos del juego y con el comportamiento original intacto cuando no hay paquete. Contrato: `docs/hd-texture-identity-contract-2026-10-16.md` (§8 = reglas de resolución). Alcance previo: `docs/hd-stage3-parallel-gs-scope-2026-10-16.md`. Parche de parallel-gs (LGPL): `third_party_patches/parallel-gs/` (README con obligaciones de licencia).
No es un renderizador nuevo: es un gancho acotado en parallel-gs (≈200 líneas) y la resolución de identidad en el anfitrión. El modo `indexed`, los 4 bits y la resolución interna HD quedan para etapas posteriores (la resolución interna se investiga aparte, ver el final).

## Uso
```
python -I tools/texmap/make_hd_pack.py --identities identities.json --map <carpeta PS2X_TEXMAP> --out <paquete> --scale 2 [--only TITLE.RAX] [--name title_press_a] [--style lanczos|nearest|mark|ruler] [--filter linear|nearest]   # paquete de PRUEBA
powershell -File tools/play/play.ps1 -HdPack <paquete> ...        # o PS2X_HD_PACK=<paquete>; sin la variable, nada cambia
F10                                                                # activa / desactiva los reemplazos en caliente
PS2X_HD_STATS=1   estadísticas (enlaces, motivos de rechazo, memoria de GPU, tiempos de seguimiento) en el log, cada 600 presentaciones y al salir
PS2X_HD_LOG=1     una línea por textura rechazada y motivo; fallos de coincidencia con las regiones
PS2X_HD_BUDGET_MB=<n>   presupuesto de memoria de GPU de las imágenes HD (512 por defecto; las menos usadas se expulsan)
```
`make_hd_pack.py` genera paquetes de prueba (no arte) a partir de `identities.json`: estilos `nearest` (replicación: debe verse idéntico al original), `mark` (replicación + borde magenta de 1 texel: cualquier error de posición/escala salta a la vista), `ruler` (color = fila/columna del texel original) y `lanczos` (suavizado). Los paquetes reales los genera el editor (Codex) según el contrato.

## Cómo funciona (resumen)
1. El análisis de procedencia (`PS2X_TEXMAP`, etapa 2) ya no necesita escribir informes: con `PS2X_HD_PACK` solo mantiene el libro de subidas con identidades (arranca igual, sin PNG ni `decomp/`).
2. Al enlazar una textura a un pase de dibujo, parallel-gs pregunta al anfitrión (`TextureReplacer::lookup_replacement(desc, game_tex0, out)`): el anfitrión busca la subida cuyo `dbp == tbp0` y aplica las reglas del contrato (§8): identidad única, región exacta, hashes, paleta igual…, y devuelve la entrada del paquete.
3. parallel-gs conserva la imagen original (la subida a la VRAM y el decodificado siguen ejecutándose) y solo sustituye la **vista de imagen** y los valores de normalización de `TexInfo`: UV, CLAMP y regiones siguen en texeles originales; la imagen HD abarca el rectángulo subido y se normaliza con su tamaño original. Con filtro `linear` se fuerza bilineal (`nearest` = el del juego).
4. Para que la decisión se tome con el libro correcto, con paquete activo el frontend entrega los paquetes GIF a parallel-gs **etiqueta a etiqueta**, justo después de interpretar cada una (las subidas siempre preceden a los dibujos que las usan). Sin paquete se entrega el paquete entero antes, como siempre.
5. Memoización: la decisión de cada textura se recuerda hasta que el libro cambia (subida o invalidación).

## Resultados
Reproducción determinista de la sesión grabada (`p14_user_session1.pad`, `PS2X_MODS=off`), capturas por VBlank, comparadas con la ejecución sin paquete. Los paquetes y capturas están en `out/hd/` (no se versionan).

| Prueba | Resultado |
|---|---|
| TITLE.RAX `title_press_a.txr`, ×2 y ×4, estilo `mark` | exactamente **716 píxeles** distintos = el anillo de 1 texel de 336×24 en x 152..487, y 352..375; ni un píxel de desplazamiento, mismo resultado en ×2 y ×4 |
| ídem, estilo `nearest` ×2 y ×4 | **0 píxeles distintos** (idéntico bit a bit al original), también transparencias (alfa del sombreado del texto) |
| ídem, estilo `ruler` | los texeles se muestrean exactamente donde el original: 1 texel por columna, 2 líneas de pantalla por texel (el juego dibuja en modo campo: sprite de 24 texeles en 12 líneas del GS) |
| ídem, `lanczos` ×2 / ×4 | solo cambia la zona del texto (máx. diferencia 46 / 23 niveles); bordes limpios |
| 006A_EV1.BRA, 33 fotogramas (Kiriya de campo), ×2 `mark`/`nearest`/`lanczos`, ×4 `lanczos`, ×8 `lanczos` | 114 enlaces en 10 360 vblanks; el área distinta es siempre el rectángulo 40×72 del sprite; con **`filter: nearest`** y replicación: **0 píxeles distintos** en los 4 fotogramas (poses distintas de la animación); con `linear` hay diferencias de ≤63 niveles (el filtro bilineal suaviza lo que el juego muestrea *nearest*) |
| desplazamiento | la mejor alineación entre original y reemplazo es (0,0) en ×2 (error medio 3,2 nearest / 5,1 lanczos) |
| alfa | los 129 valores de alfa del GS (0..0x80) sobreviven al viaje PNG→GS sin pérdida (se comprueba en las pruebas) |
| memoria | ×8 de los 33 fotogramas: 24,2 MiB residentes en GPU (33 imágenes, exacto 40·72·64·4·33); con `PS2X_HD_BUDGET_MB=2`: se expulsan 52 imágenes (1,5 MiB residentes) y **la imagen es idéntica** |
| sin paquete / paquete que no coincide | **31 capturas idénticas** (vblank 100..3100) entre «sin paquete» y «paquete con SHA-1 incorrecto» (seguimiento y entrega etiqueta a etiqueta activos, 0 reemplazos); y capturas idénticas en `none`, `none2`, paquete real, solo seguimiento en el vblank 6000 |
| rendimiento | llegar al vblank 6000 de la sesión: **24,6 s sin paquete**, 57 s con la primera versión del seguimiento, **28,7 s** tras corregir dos costes (reconstrucción del índice de contenido en cada expulsión y hashes lentos); el seguimiento cuesta ≈2,6 s de esos 28,7 s, el resto son las entregas por etiqueta y las consultas |
| pruebas | `ps2x_tests` **488/488** (4 nuevas: manifiesto, coincidencia exacta, `find()` con hashes/paleta, `pixels()` alfa/tamaño, desactivación), `ps2_sif_runtime_tests` OK, las 13 suites del IOP OK, `tools/validation/audio_baseline.py`: **ALL PASS** |

Contadores de rechazo en la sesión completa (con el paquete del BRA): no se sustituye lo que no debe: texturas de 4 bits («aún no soportadas», la mayoría), identidades ambiguas, sin subida seguida, sin paleta seguida, o píxeles de varias regiones.

## Límites y decisiones
* **Solo `rgba` entero ≥ 1 por eje.** `indexed` (paleta viva), 4 bits y factores no enteros: no implementados (las entradas se descartan con aviso).
* **Mipmaps**: ignorados. El sombreador de parallel-gs no tiene derivadas (shader de cómputo), así que elegir el nivel por derivadas no es posible sin tocarlo; el filtro bilineal a ×2 equivale a un promedio de bloque 2×2 en el centro de los texeles, a ×4 muestrea 4 de los 16 sub-texeles (aliasing leve a resolución interna 1×; a resolución interna ×2/×4 desaparece).
* **Densidad real**: a resolución interna 1× el juego dibuja a baja resolución (campo de 224 líneas); un reemplazo ×4 solo luce a su densidad con resolución interna mayor. Por eso la investigación de la resolución interna es el siguiente paso, **independiente** de los reemplazos (ninguna depende de la otra).
* Texturas con super-muestreo (`PS2X_GS_SCALE` + sprites de 8 bits): no se sustituyen todavía (usan una imagen en array).
* Si una textura reutiliza el mismo índice y luego se dibuja con UV fuera del rectángulo reemplazado, mostrará el contenido envuelto de la HD (no el del original): solo se comprueba el recuadro UV del dibujo que enlaza la textura. No se ha observado.
* **Combinación con la resolución interna HD** (independientes, se activan por separado): sprite de 006A_EV1.BRA con paquete `lanczos` ×4 y `PS2X_GS_SCALE=2` (salida 1280×447): el reemplazo se aplica (4 002 píxeles distintos, el recuadro del sprite: x 636..717, y 203..273) y se ve más suave que sin paquete. Con `PS2X_GS_SSTEX=1` (texturas super-muestreadas) los sprites de 8 bits no se reemplazan (siguen el camino de imágenes en array); por eso esa opción está apagada por defecto. Ver `docs/hd-internal-resolution-2026-10-16.md`.
* `006A.BRA` (sin `_EV1`) sigue sin cargarse en esta sesión: el mecanismo es el mismo para cualquier bloque LZSS de cualquier contenedor.
* La tecla F10 no se ha podido probar de forma automática (solo su efecto sobre `hdpack::setEnabled`, cubierto por pruebas); hay que probarla jugando.
