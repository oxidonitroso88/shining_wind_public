# Shining Wind: arquitectura nativa, compatibilidad SDK y recursos

## Decisión propuesta

**Recomendación / HYPOTHESIS por validar: C, híbrida.** Usar A como capa acotada
de compatibilidad durante el arranque y mantenerla disponible después para los
consumidores que sigan usando SDK/sectores. Añadir B por interfaces de recursos
cuya semántica esté demostrada, sin sustituir de golpe el cargador ni el IOP.

Sí podemos proporcionar servicios nativos sin ejecutar todos los módulos originales.
HLE ya es una forma de sustitución nativa: no obliga a emular instrucciones IOP ni
mecánica CD/DVD. El criterio es conservar los efectos observables en la frontera
elegida, no conservar todas las capas originales ni devolver el valor esperado.
Los requisitos de esa frontera incluyen memoria, handles, errores, callbacks,
orden, bloqueos, lifecycle y buffers, además de bytes de archivo.

## Evidencia utilizada y límite de esta reanudación

Se reutilizaron las lecturas estáticas realizadas antes del corte, registradas en
la conversación: informes CDVD/RPC, decompilados guardados de la copia aislada,
instrucciones de C++ generado y código actual runtime/IOP. No se volvió a abrir
Ghidra ni se repitió análisis del juego. La lectura adicional de archivos en esta
reanudación falló antes de iniciar proceso con `setup refresh had errors`.
Por ello las afirmaciones se limitan a la evidencia ya inspeccionada; no se afirma
haber comprobado nuevos hashes o inventariado todos los callsites posteriores.

Fuentes durables: docs/cdvd-rpc-root-cause-2026-10-07.md,
docs/cdvdfsv-contract-audit-2026-10-07.md (limitaciones históricas, parcialmente
resueltas por el modelo), docs/cdvd-device-implementation-2026-10-07.md,
docs/cdvd-sector-implementation-2026-10-08.md; consultas guardadas
tools/ghidra/runs/20261007T175758621Z,20261007T175859272Z,20261007T180020867Z.
Referencia SDK: ps2dev/ps2sdk@2c670453980fcc3fe46ead6399b730b12c8556eb,
CDVDMAN/CDVDFSV y cliente EE, disponibles localmente en external-evidence.

## Efectos concretos del arranque

**CONFIRMED / GAME-SPECIFIC**, estático; ejecución posterior al BIND no observada:

| Etapa | Evidencia y obligación observable |
|---|---|
| InitRpc | Handshake SIF ya demostrado dinámicamente en la observación anterior. Conservar dispatcher guest, handlers y SREG; no modificar directamente RPCINIT. |
| CdInit(0), FUN11C960 | Desde FUN1A8F50, BIND80000592 y CALL0, request4/response16. El cliente lee resultado/versiones/diagnóstico e instala semáforos y handler SIFCMD80000012. Sustituir toda la función por return1 perdería esos efectos. |
| Mmode(2), FUN11D538 | CALL0x22 a80000593,4/4 bytes. Guardar el modo es una operación legítima; no equivale a montar DVD. |
| DiskReady(0), FUN11CE60 | Consulta8000059C y fallback a59A. El backend debe aplicar su contrato de disponibilidad/completion; no equiparar ready a existe archivo o PVD válido. |
| Reinicio IOP | FUN115910 recibe cdrom0:\\MODULES\\IOPRP300.IMG;1, construye ruta UDNL y pasa por115740/SIFCMD80000003. Caller repite solicitud y espera FUN1158C0; ésta consulta SIF GetReg(4), máscara0x40000, y ejecuta acciones adicionales al sincronizar. No basta devolver true de un stub de reboot. |
| Después del reinicio | FUN1A8F50 repite CdInit/Mmode/DiskReady, InitRpc y llamadas111300/114490. Hay reinicialización de interfaces; no sólo una espera que se pueda suprimir. No se afirma que todo el grafo posterior esté identificado. |

El caller inicial no necesita comprobar todos los retornos para que los efectos
sean necesarios: otros consumidores observan estado cliente, handles y callbacks.
Un CDVDFSV HLE conserva el cliente guest y sustituye el servidor; tiene menor
superficie de equivalencia que reemplazar el cliente o todo FUN1A8F50.

**CONFIRMED / PS2RECOMP-GENERIC:** el adaptador actual sólo enruta INIT,
ChangeAddress y BIND/CALL/OtherData; otros comandos se diagnostican como no
solicitados. No se ha demostrado soporte de reboot80000003 en esa ruta. Hay
stubs EE reboot/reset que llaman al reset host, pero eso no demuestra equivalencia
con el protocolo guest usado. Diseñar el lifecycle de reboot es una tarea separada:
cancelar transacciones antiguas, invalidar handles/colas, restablecer servicios del
perfil y publicar sincronización sólo cuando ese nuevo entorno esté operativo.
Mantener el medio seleccionado no implica conservar las sesiones IOP antiguas.

## Dependencias posteriores: qué sabemos y qué falta

**CONFIRMED / GAME-SPECIFIC por instrucciones/strings, no cobertura dinámica:**

- FUN1A0580 materializa rutas SIO2MAN.IRX, LIBSD.IRX y CRI_ADXI.IRX en
  3E5530/3E5550/3E5570, llama1153E0 y toma ramas de error por retorno negativo.
  La presencia de esos módulos señala contratos de input/sonido/CRI que no se
  eliminan sustituyendo lecturas del disco. No se ha completado su ABI/RPC.
- FUN1A0490 pasa CDV:/st.cvm y CDV:/voice.cvm a FUN129148 desde1A0504/1A0528,
  con bucles sobre el resultado. Es evidencia de una frontera de montaje/registro
  de contenedores; llamarla exactamente API de montaje sigue siendo interpretación
  hasta completar FUN129148 y su estado/ownership.
- FUN1A8AF0 usa la ruta raíz cdrom0:\\MODULES.CIX;1, llamadas111568/111BB0/1117F8,
  comprueba cantidad leída y construye transferencias mediante10C5A0. No tratar
  todo archivo como textura/texto: también alimenta inicialización de módulos.
- El ELF contiene rutas OVERLAY/OVLxx.BIN. No demuestra ejecución de overlays,
  pero obliga a distinguir contenido de recursos de código/cargas relocables.
- Direcciones y rutinas SDK80000595/597 existen; no está probado qué subconjunto
  de Read/Search/streaming se ejecutará a continuación ni sus callbacks efectivos.

**CONFIRMED / PS2RECOMP-GENERIC:** existen gestor de módulos, carga IRX/imports,
servicios HLE mcserv/dbcman/libsd, acceso host y transporte SIF. Aceptar un nombre
en loadHle o devolver un ID no acredita las capacidades del módulo original.
No se ha demostrado que libsd nativo cubra CRI_ADXI, ni que servicios genéricos de
input cubran toda la interacción SIO2/PAD. Esas equivalencias siguen abiertas.

Reemplazables conceptualmente: transporte de bytes por archivos PC, implementación
servidora CDVDFSV, asignación de handles host detrás de IDs guest y ejecución de
servicios concretos por backends nativos. Se conservan sus efectos guest. No se
pueden declarar reemplazados el reinicio, audio/input o el filesystem completo
sin sus contratos. Ejecutar módulos originales queda como alternativa por servicio,
no como obligación global ni como solución trivial.

## Comparación A/B/C

| Alternativa | Reutilización y beneficio | Trabajo adicional y riesgo |
|---|---|---|
| A. CDVDFSV HLE | Reutiliza SDK guest, SIF/RPC, estado CDVD y Search→Read. Preserva la frontera wire conocida; no requiere CDVDMAN físico. | Frontend RPC, perfil/versiones veraces, bootstrap/reset, buffers/completion y operaciones según consumidores demostrados. Puede crecer si se intenta cubrir todo el SDK sin priorizar. Por sí sola no permite archivos internos CVM modificados. |
| B. Recursos nativos | Permite resolver nombres/IDs a archivos PC y cambiar tamaños sin fingir sectores físicos, si la frontera elegida conoce tamaños y asignación. | RE de contenedores/handles/read/seek/close, errores, sincronización y allocator. No resuelve el primer CdInit ni reinicio/audio/input. Reemplazar SDK entero para llegar al loader amplía mucho el alcance. Alto riesgo hoy porque no hay ABI completo demostrado. |
| C. Híbrida | A para compatibilidad observable; B por recurso validado, manteniendo fallback original coherente. Aprovecha todo el trabajo actual y permite traducción incremental. | Exige enrutamiento explícito y un único propietario por handle/transacción. No dos fuentes que respondan al mismo LSN ambiguamente. Coste inicial de A más una capa de recursos, pero reduce la necesidad de emular funciones no observadas. |

La propuesta no es «HLE ahora, desactivar SDK después». Algunos consumidores
pueden necesitar SDK siempre; cada interfaz sustituida tiene pruebas propias.
No prometer que C sea menos trabajo total antes de medir los contratos de audio/CRI.

## Fronteras de sustitución, de menor a mayor incertidumbre

1. **Backend de sectores IOP ya corregido.** Fuente única por generación para
   Search/Read, ISO o directorio virtual. Adecuado para compatibilidad óptica;
   no mezclar con pseudo-LSN EE ni usar la ISO interna CVM como disco completo.
2. **Servidor RPC CDVDFSV detrás del cliente guest.** Mantiene memoria/semáforos/
   callbacks del cliente y permite backend PC. Falta exponer API tipada de CDVD,
   sin invocar imports con un CPU ficticio como contrato permanente.
3. **Handle de archivo/recurso con nombre, tamaño y offset conocidos.** Candidato
   preferido para overrides: elegir origen al abrir; size/read/seek/close del mismo
   handle usan esa fuente hasta cerrar. No cambiar de origen en medio de una lectura.
4. **Buffer cargado tras asignación guest.** Seguro sólo si capacidad, tamaño real,
   formato y propietario están demostrados. Si el override crece, interceptar también
   la consulta de tamaño/asignación anterior; no copiar más bytes sobre el buffer.

Candidatos locales **PARTIAL / GAME-SPECIFIC**:
`shining_wind_overrides.cpp` ya tiene un hook de observación en12A940, que vuelve al
owner registrado. La tabla lo asigna a entry_0012a930; el código conserva v0→s0,
path en s1, chequea handle nulo y llama12A738. No demuestra por sí mismo que allí
haya un buffer asignado para el recurso ni que sea un interceptor de lectura completo.
FUN12B4C8 envuelve12B520 con llamadas12C5A0/12C5B8;12B520 comprueba a2&0x3F.
Conservar esas llamadas y restricción hasta entender si son sincronización y
alineación/longitud. Su significado exacto sigue pendiente; no poner un fopen
directo en esas direcciones por el nombre del hook histórico.

## st.cvm / voice.cvm y traducción

**HYPOTHESIS viable:** proveedor por identidad de contenedor+ruta interna, con
prioridad de override explícita y fallback al contenedor original antes de crear
el handle. Mantener nombres/case/versiones, tamaño, offset, EOF, alineación y
orden de completion. Archivos extraídos deben conservar la jerarquía/identidad,
no hacer búsquedas ambiguas por basename. Validar primero un archivo sin cambios,
luego otro modificado de igual tamaño y después tamaño variable.

No se deduce que extraer CVM elimine ROFS/CRI: puede haber índices por ID,
compresión, cifrado, offsets relativos, audio en streaming o dependencias entre
recursos. RAX/TXR extraídos siguen pudiendo requerir decodificación guest. Traducción
requiere además encoding, punteros/longitudes y fuentes; datos externos no garantizan
texto español correcto. Voice debe probarse por separado por buffering/reloj/audio.

Para la siguiente fase de RE, sería útil el **manifiesto de rutas y tamaños internos
de st.cvm y voice.cvm**, más un archivo de texto RAX identificado como candidato y
su equivalente original. No se requiere volcar todo el contenido ahora. La evidencia
de MODULES.CIX aquí apunta al archivo raíz del disco; no confundirlo automáticamente
con un homónimo dentro del CVM. Los módulos disponibles en NO_LABEL permiten
seguir investigando arranque sin solicitar esos recursos internos.

## Riesgos y controles

- Sincronización: completion inline sólo si conserva estados y orden observable;
  callbacks no reentrantes arbitrarios desde threads host. Si hay Pending, retener
  request/buffers/epoch y completar exactamente una vez a través del scheduler.
- Memoria: direcciones guest y handles host separados, validación completa de rango,
  alineación, endian, caché/aliases y duración. Alloc/free del propietario original.
- Sectores: namespaces ISO/directorio/EE explícitos; no transformar una dirección
  por heurística numérica. Reset invalida contratos anteriores; no inferir medio de ELF.
- Compatibilidad: perfiles/capacidades comprobables; módulo cargado no implica
  servidor funcional. Unsupported no equivale a completed/error operativo.
- Overrides: fijar fuente por handle, fallback sólo en apertura según política;
  un archivo modificado corrupto no debe ocultarse cambiando silenciosamente de origen.
- Runtime: conservar PR271 y B+C para callbacks/reanudación. Más PC alcanzados no
  sustituye pruebas de bytes, estado, ownership y orden de eventos.

## Plan incremental sujeto a decisión

1. **Cerrar contratos sin implementar:** tabla de efectos Init/Mmode/Ready/reboot,
   perfil de versiones, servicios tras reset; inventario de operaciones realmente
   solicitadas por módulos/cliente. Identificar size/open/read/seek/close de un recurso
   CVM y quién asigna/libera. No ampliar a todos los módulos por anticipación.
2. **A mínimo funcional:** frontend CDVDFSV para el tramo probado, sobre API CDVD
   tipada. BIND sólo con proveedor preparado; preservar semáforos/handler guest.
   Tests RPC extremo a extremo, formatos4→16/4, errores, reset y ausencia de END
   antes de completion. Resolver Pending explícitamente o demostrar perfil síncrono.
3. **Reboot como hito separado:** probar80000003, cancelación/handles/colas,
   restauración real de servicios y sincronización. No sustituir IOPRP por un literal
   de éxito; elegir por servicio HLE o ejecución original según contrato probado.
4. **Ruta de recursos nativa equivalente:** primero sin overrides, comparar hashes,
   offsets, EOF, errores y trazas de ownership contra la ruta original en tests.
   Probar cruces de buffers, lecturas parciales, cancelación/reset y concurrencia.
5. **Override acotado:** un recurso st.cvm con fallback original; después tamaños
   variables, traducción y voice como validaciones distintas. Mantener streaming,
   MPEG/audio/input en su ruta existente hasta demostrar sustitución específica.
6. **Validación dinámica futura autorizada por separado:** observación limitada,
   trazas guest→servicio→completion, checks de contenido y nuevo blocker clasificado.
   Regresiones: recompilador90, Pointer25, CDVD35+sectores73, IOP7 y SIF23 como
   referencias actuales, sin ejecutarlas durante esta investigación.

**Detención para decisión:** se recomienda C con alcance inicial A acotado y una
auditoría de contrato de recurso antes de B. No se implementó ninguna alternativa.
