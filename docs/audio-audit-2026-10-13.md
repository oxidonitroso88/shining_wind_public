# Auditoría del audio — estado real y propuesta incremental, 2026-10-13

## 1. Qué hay realmente (evidencia de ejecuciones con el juego real)
* **El IOP ejecuta los módulos reales del juego** (`ps2xIOP` emula la CPU del IOP y carga los IRX de `MODULES.CIX`): `sio2man`, `Sound_Device_Library` (libsd del SDK), **`CRI_ADX_Driver` v9.75** (hilos «Main/PSM/DTX», «SPU CORE Number»), `padman`, `Sound_Data_HD` (bancos de efectos), `sdr_driver` (servidor RPC `0x80000701` del lado EE), `mcman`, `mcserv`. Se ve `sceSdInit(SD_INIT_COLD)` ejecutado por el propio código del IOP.
* **El SPU2 es solo un archivo de registros.** `iop_memory.cpp` guarda cada escritura en `m_hardware` y devuelve lo escrito; el DMA hacia el SPU solo simula *tiempo* (finalización a 48 kHz×4 bytes si el registro `ADMAS`≠0). No había memoria de sonido, ni voces, ni mezcla, ni salida: **el juego funciona «sin sonido» porque el IOP recibe respuestas coherentes de un SPU2 que no existe**, no porque se finjan respuestas del audio.
* El servicio EE→IOP de libsd (`ps2xIOP/src/modules/libsd.cpp`, SID `0x80000701`) es un resto del camino HLE antiguo: rechaza el transporte por paquetes («lacks verified packet completion») y devuelve «manejado, sin completar». No es el camino por el que el juego llega a sonar (el audio real pasa por el IOP, no por ese servicio).
* `ps2_audio.cpp`/`ps2_audio_vag.cpp` (EE) decodifican VAG y reproducen con raylib (`InitAudioDevice` ya se llama); ese código pertenece al camino HLE antiguo y puede reutilizarse como **salida** (cola de PCM → `AudioStream`), no como emulación.

## 2. Observado en una ejecución normal (arranque → FILE01, ~160 s)
* Sondeo constante de `REG_ADMAS` del core 0 (`0x1F9001B0`, ~190 lecturas/s): el bucle del driver ADX espera.
* **DMA continuo del core 0 en «ping-pong»** (`PS2X_SPU2_LOG=1`): bloques de 512 palabras (2048 bytes = 10,67 ms a 48 kHz estéreo) alternando los búferes del IOP `0x25CC0` y `0x264C0`, `ADMAS` activo, desde el arranque. Capturados 20 MB en 100 s: 9 962 bloques (ritmo en tiempo real correcto), la mayoría silencio; 625 bloques con señal (≈6,7 s) al principio.
* Inicialización estándar de libsd: `KON` de las 24 voces de cada core sobre un bloque de silencio (`SSA=0x2800`, bloque `00 07`).
* Primeras activaciones reales de voces (6 `KON` en la sesión; core 0 y 1, `SSA` entre `0x2808` y `0x998D8` palabras, tonos `0x1000`/`0xAAA`/`0x4AA`/`0x555`, ADSR `87FF/5FE6` y `AFFF/5FED`).
* **Pregunta abierta (importante):** el contenido de la RAM del SPU en esas direcciones parece **PCM de 16 bits suave** en varias (`d2 0a b7 0c 24 0e 06 0f …`) y **no** ADPCM, mientras los bloques ping-pong tienen estructura de 16 bytes con cabeceras `10 00` y banderas 1/2/7 (aspecto de ADPCM) que no se corresponde con la RAM volcada en el primer `KON`. Antes de construir un mezclador hay que aclarar cómo viaja cada dato (qué DMA va a qué dirección de RAM del SPU, si `ADMAS` activa realmente la entrada por *AutoDMA* o es una transferencia de volcado, en qué orden llegan banco y flujo). No se ha intentado adivinarlo.

## 3. Implementado ahora (hito M0, sin cambiar el comportamiento audible)
* `IopMemory`: RAM de sonido real de 2 MB (`spuRam()`), **copia de los DMA del SPU** (IOP RAM ⇄ RAM del SPU a través del `TSA` de cada core, que ahora se sigue: `0x1A8/0x1AA`), **puerto de datos PIO** (`0x1AC`), y registro de `KON` (`PS2X_SPU2_LOG=1`: voz, `SSA`, tono, ADSR, volumen, cabecera del primer bloque). Diagnósticos: `PS2X_SPU2_DUMP=<archivo>` (bytes de cada bloque DMA), `PS2X_SPU2_RAM_DUMP=<archivo>` (RAM del SPU en el primer `KON` real).
* No hay mezcla ni salida de audio: el juego sigue en silencio; los registros se comportan como antes (misma escritura read-modify-write de palabras de 32 bits). El juego arranca y llega a FILE01 con normalidad (160 s, 0 `missing-target`).

## 4. Propuesta de implementación incremental (diseño; requiere tu aprobación antes de M1+)
| Hito | Contenido | Verificación | Riesgo |
|---|---|---|---|
| **M1** | Aclarar los datos (pregunta abierta de §2): rastrear TSA de cada DMA y de cada `KON`, comprobar que cada `SSA` apunta a datos válidos. | Informe de cobertura: % de `KON` cuyo bloque inicial es ADPCM válido o PCM de entrada. | Bajo: solo diagnóstico. |
| **M2** | Voces: decodificador ADPCM del SPU (28 muestras/bloque, filtros, banderas fin/bucle), paso de tono (interpolación), ADSR, volúmenes, `KON/KOFF/ENDX`; mezcla de las 48 voces a 48 kHz. | Capturar la salida a un WAV (`PS2X_SPU2_WAV`) y compararla con la decodificación independiente de las muestras del banco (`.HD/.BD` extraídos con `cvm_tool`). Prueba unitaria con un banco sintético. | Medio: sincronía (la mezcla corre al ritmo de tiempo real de los DMA ya modelados; sin hilo de audio propio hay deriva). |
| **M3** | Entrada por AutoDMA (`MEMIN`) para flujos PCM del driver ADX/AHX y mezcla del core 1 con la entrada del core 0. | Música y voces en vivo; WAV de referencia. | Medio. |
| **M4** | Salida: cola de PCM con relleno de silencio hacia `AudioStream` de raylib (ya inicializado), hilo de audio con búfer de 100–200 ms; reverb después. | Escucha + WAV. | Medio-alto: latencia/subdesbordamientos. |
Alternativas descartadas por ahora: (B) HLE del `CRI_ADX_Driver` (reemplazar el IRX por una implementación del host): rompe el principio de ejecutar el código real y obliga a reimplementar un formato propietario; (C) portar código de otros emuladores (PCSX2 es GPL): no se copia código, se reimplementa desde la documentación de registros.

## 5. Relación con el bloqueo de pantalla negra
No hay relación con el audio: la causa demostrada es la corrupción del heap del juego por el heap del host (`docs/heap-corruption-host-arena-2026-10-13.md`). La falta de audio no altera las esperas del IOP: el SPU2 simulado responde `ADMAS`/DMA con el tiempo correcto.

## 6. Riesgos
* Sincronización: si algún evento del juego espera a que una voz termine (`ENDX`/`NAX`), la mezcla debe avanzar al ritmo del reloj del IOP y no del reloj del host; hoy `NAX/ENDX` no avanzan (devuelven lo escrito), lo que ya es una limitación respecto al hardware.
* Rendimiento: 48 voces + interpolación en el hilo del IOP; mezclar en un hilo aparte con cola lockeada.
* Datos: bancos de efectos y música de VOICE.CVM/ST.CVM son material con derechos: las pruebas deben generarse desde los archivos locales, nunca subirse.
