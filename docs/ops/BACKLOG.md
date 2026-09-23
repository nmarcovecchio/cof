# Backlog de ingenieria — CallOnFail

Estado: **2026-09-23**. Ultimo firmware publicado y desplegado: **0.2.62**.

Este archivo es la lista de trabajo tecnico pendiente (deuda, bugs conocidos,
hardening de proceso). **No** es el roadmap de producto: las funciones que
todavia no existen y no se pueden vender viven en `docs/ROADMAP.md`.

Regla: cuando un item se cierra, se borra de aca en el mismo commit que lo
arregla.

---

## P0 — Pipeline de OTA

Tres agujeros encontrados el 2026-09-21 al publicar 0.2.60. El device no tenia
forma de saber que estaba desactualizado durante **cinco releases** (0.2.55 a
0.2.59).

### 1. El VPS nunca se sincroniza solo (causa raiz)

`scripts/release-fw.py` compila, actualiza `ota/manifest.json` y pushea a
GitHub. Pero el binario lo sirve Flask desde el **checkout del VPS**
(`./ota:/srv/ota:ro`, ver `docs/ops/VPS_CONFIG.md`), y **nada** corre `git pull`
ahi. El manifest anunciaba `0.2.59` mientras el VPS servia `0.2.57`; el gate de
sha256 rechazaba el update en silencio.

Opciones (elegir una):

- **a)** `manifest.url` deja de apuntar al VPS y apunta a un CDN (raw GitHub o
  R2/S3). Elimina la dependencia del pull, pero suma latencia de CDN.
- **b)** El script falla si el sha256 servido no coincide con el local. Requiere
  un endpoint publico en el backend que devuelva el sha256 de `ota/firmware.bin`
  (no hace falta SSH).
- **c)** Cron/systemd timer en el VPS: `git pull` periodico + aviso si cambia.

Mientras tanto: **`cd /opt/callonfail && git pull` en el VPS antes de cada OTA.**

### 2. `raw.githubusercontent.com` cachea el manifest

`COF_MANIFEST_URL` (`firmware/include/cof_config.h:14`) apunta a raw GitHub, que
sirve cacheado unos minutos. Un OTA apretado justo despues del release ve la
version vieja y no baja nada.

Fix: agregar cache-busting (`?...&t=<epoch>`) al pedir el manifest, o servirlo
desde el VPS con `no-store`.

### 3. Nadie verifica que el device haya actualizado

El gate de sha256 protege contra firmware corrupto, pero rechaza **en silencio**
(`OTA bad hash`). No hay alarma de "OTA rechazada" ni de "version estancada".

Fix: (a) el firmware publica un evento `ota_rejected` con el motivo; (b) el
backend avisa si un `ota_check` fue `accepted` y la version reportada no cambio
en N minutos.

---

## P1 — Firmware

### 4. Sondeo de SMS entrante pausado bajo LTE

`pollIncomingSms()` tiene gate `!lteMqttTransport`
(`firmware/src/main.cpp:719`, comentado en `firmware/src/sms_voice.cpp:140`).
Es **deliberado**: intercalar `sendAT` con un `AT+CIPOPEN` abierto puede
corromper el socket del modem. El SMS **saliente** (la prueba manual) funciona
desde 0.2.55.

Decidir: dejar como limitacion documentada, o drenar SMS cerrando el CIPOPEN
antes del housekeeping AT y reabriendo despues.

### 5. TLS no verificado en las descargas del device

`client.setInsecure()` en `ota_config.cpp:223` y `:260` (manifest y firmware),
`sms_voice.cpp:172` (audio), `mqtt_io.cpp:605` (MQTT sobre LTE). El sha256 cubre
integridad, **no autenticidad del transporte**. Fix: pinear la cadena de Let's
Encrypt (ISRG Root X1). Ya anotado como "Pending" en
`.cursor/rules/20-firmware.mdc`.

### 6. `NO SERVICE` del modem: causa raiz desconocida

El 2026-09-21 el device quedo sin publicar con `radio: "NO SERVICE"`, `csq: 99`,
y se recupero solo tras un reinicio por watchdog. Nunca se supo si fue watchdog,
panic o `CFUN`. Instrumentar: loguear `esp_reset_reason()`, `CEREG`, `CPSI` y
`CSQ` en el primer status post-boot.

---

## P1 — Backend / panel

### 7. Revisar el error del log del modem en el panel

Pendiente de la lista original de 0.2.55 y **nunca se hizo**: entrar a
`app.callonfail.com.ar` y encontrar el error del modem que reporto el usuario.
Requiere acceso al panel; el agente **no** tiene ni debe tener la credencial
(ver §Secretos). Lo mas util es pegar el error en el chat.

### 8. Verificar que las alarmas de corte disparen

Se agregaron en 0.2.55 (`Add Ethernet/WiFi/internet alarm sensors`). Falta
confirmar en el panel que efectivamente disparan y que reportan **liveness de
internet**, no presencia de link.

El codigo esta: `SENSOR_ALIASES` mapea `net_ethernet` / `net_wifi` /
`net_internet` a `network_*_ok`, el firmware los publica en cada frame
(`fillConnectivityJson`) y `ensure_network_sensors()` los inyecta en el form. Lo
que falta es la prueba end-to-end con una regla `lt 1`.

### 8b. Probar el respaldo de audio de la llamada (0.2.61, revisar con 0.2.64)

Con 0.2.64 el orden de preferencia cambio, asi que este caso quedo mas acotado. La
llamada usa, en orden: (1) el audio **pregrabado y estatico** de la regla, que no
necesita red; (2) el TTS exacto bajado del servidor, si hay ruta; (3) la
**variante generica** del audio de la regla; (4) recien ahi el
`C:/cof_fallback.wav` congelado.

O sea que el respaldo solo se activa si la regla **no tiene audio local
utilizable** (p. ej. el sync nunca corrio) **y** la descarga del TTS falla. Para
probarlo: equipo sin Ethernet ni WiFi, con `C:/cof_fallback.wav` ya sincronizado y
una regla **sin** `call_text` previamente descargado, y confirmar que la llamada
sale y el evento dice `TTS unavailable, using fallback`. Con eso se cierra tambien
la duda de que el asset llegue al modem.

### 8c. El sitio solo-LTE no puede recibir nada (hallazgo 0.2.62)

El problema del audio en sitio sin LAN no es del audio sino del **transporte**.
Todo lo que el firmware baja lo baja con `HTTPClient` + `WiFiClientSecure`, que
necesitan una interfaz lwIP (Ethernet o WiFi). En un sitio solo-LTE el MQTT viaja
por el socket `AT+CIPOPEN` del modem, que no es lwIP, asi que **no hay ruta**
para `HTTPClient`. Por el mismo motivo:

| Que baja | De donde | Via |
|---|---|---|
| Audio de llamada | `app.callonfail.com.ar` | `HTTPClient` |
| Manifest | `raw.githubusercontent.com` | `HTTPClient` |
| Firmware (OTA) | `app.callonfail.com.ar` | `HTTPClient` |

O sea que un equipo sin LAN **tampoco se puede OTA-ear solo**. Es el mismo agujero
que el P0 de arriba, con otra cara.

El modem si tiene pila HTTP propia y puede escribir directo a su C:
(`AT+HTTPINIT`, `AT+HTTPPARA="URL",…`, `AT+HTTPACTION`, y
`AT+HTTPREADFILE="x.amr",1` guarda el cuerpo en `C:/`). `AT+CFTPSGETFILE` hace lo
mismo por FTPS. Si el modem baja su propio audio, el sitio solo-LTE deja de
depender de lwIP para tener voz nueva, y de paso se abre la puerta al OTA por la
misma via.

**Confirmado en `cof-test` el 2026-09-23:** `AT+HTTPINIT` ok y
`AT+HTTPREADFILE=?` **SUPPORTED**. La via existe en hardware real. Lo que falta
es implementarla, resolviendo antes que `HTTPREADFILE` escribe respuestas HTTP en
claro (servir el audio por HTTP, o configurar el contexto SSL del modem).


**Como verificarlo sin acceso fisico:** boton **Sondear modem** en la pagina del
dispositivo (comando MQTT `modem_probe`, firmware >= 0.2.62). Publica eventos
`modem_probe` con `AT+FSMEM` (memoria libre de C:), `AT+CCALB?`,
`AT+HTTPINIT` y `AT+HTTPREADFILE=?`. El ultimo dice si la via HTTP-a-archivo
existe en esta unidad. Se saltea mientras hay llamada activa.

#### Resultado en `cof-test` (0.2.62, 2026-09-23 07:41)

| Sonda | Respuesta | Lectura |
| --- | --- | --- |
| `AT+FSMEM` | `C:(4194304,1146880)` | 4.00 MiB total, **2.91 MiB libres** |
| `AT+CCALB?` | unsupported | No hay tono de alerta programable |
| caps | `fs=YES play=YES` | El firmware puede subir y reproducir audio |
| `AT+HTTPINIT` | ok | **El modem abre HTTP por su cuenta** |
| `AT+HTTPREADFILE=?` | **SUPPORTED** | **Puede escribir la respuesta a `C:/`** |

La via HTTP-a-archivo **existe en esta unidad**. Ojo con dos cosas antes de
disenarla:

- `AT+HTTPREADFILE` escribe el cuerpo de una respuesta HTTP **en claro**.
  `app.callonfail.com.ar` es HTTPS, asi que para bajar el AMR por esta via hay
  que servir el audio por HTTP (y el audio no es secreto), **o** configurar el
  contexto SSL del modem con `AT+CSSLCFG="authmode",<ctx>,0` (sin verificacion de
  CA, equivalente al `setInsecure()` que ya usa el firmware) y `AT+CSSLCFG="cacert"`
  para verificacion real.
- El C: real es **4.00 MiB**, no los ~10.8 MiB del ejemplo del manual: **63% mas
  chico**. Cualquier calculo de cuantos audios entran tiene que salir de `AT+FSMEM`
  de la unidad, no del ejemplo del fabricante.

### 8d. ~910 KB sin explicar en el C: del modem (0.2.62)

`AT+FSMEM` en `cof-test` reporta **1.146.880 B usados**, pero los assets que
sabemos que estan suman mucho menos:

| Archivo | Bytes |
| --- | --- |
| `C:/cof_fallback.wav` | 85.410 |
| `C:/cof_test.wav` | 40.044 |
| `C:/tts.amr` + `C:/tts.wav` (estimado) | ~90.000 |
| **Total explicado** | **~215 KB** |

Sobran **~910 KB**. No se puede afirmar que sean huerfanos: el firmware viejo
pudo dejar archivos sin enumerar, y `AT+FSMEM` cuenta en bloques. Tampoco hay hoy
forma de listar el directorio (no se probo `AT+FSLS`).

**Por que importa:** si son huerfanos, se comen el 31% de los 2,91 MiB libres, y
el reconciliador nuevo (`syncRuleAudio`) solo puede podar su propio namespace
`a_*`, no archivos desconocidos.

**Como cerrarlo:** (a) agregar `AT+FSLS` al `modem_probe` para listar de verdad;
(b) medir `FSMEM` antes/despues de borrar un archivo conocido para saber como
cuenta; (c) recien entonces decidir si se limpia a mano.

### 8e. Probar el audio pregrabado en hardware (0.2.64)

La cadena completa (guardar regla → sintetizar → publicar `call_audio` →
descargar al modem → reproducir local) esta verificada solo en banco, no en un
equipo real. **Probar con LAN conectada** (con el cable desconectado la descarga
no puede ocurrir, ver 8c). Falta:

1. Guardar una regla con texto **sin** `{valor}` y confirmar que la llamada suena
   **sin** bajar nada (probar con el cable de red desconectado).
2. Guardar una regla **con** `{valor}` y confirmar los dos caminos: con red dice
   el numero; sin red reproduce la variante generica.
3. Cambiar el texto de una regla y confirmar que el audio viejo **se borra** del
   modem (`[audio] pruned ...` en el log serie) y no queda suelto.
4. Borrar el texto de la regla y confirmar que se poda.

### 9. Alarma de OTA rechazada / version estancada

Complementa el punto 3.

### 9b. Retencion de telemetria (crece sin limite)

`Telemetry` no tiene ninguna politica de retencion: no hay job de limpieza ni
`DELETE` programado (el unico precedente es `tts.cleanup_old_audio()`).

**Numeros medidos** (no estimados): el frame que publica el firmware
(`publishTelemetryNow()` en `firmware/src/mqtt_io.cpp`) serializa a **745
bytes**; con el overhead de tabla/TOAST de Postgres redondea **~500 MB por
equipo por año** a la cadencia de 60 s.

| Alcance | 1 año |
|---|---|
| 1 equipo | ~0,5 GB |
| 10 equipos | ~5 GB |
| 100 equipos | ~50 GB |
| 1000 equipos | ~496 GB (~41 GB/mes) |

Lo que duele no es el disco sino las **filas**: a 60 s son 525.600 filas por
equipo por año, y el `DELETE` de retencion tiene que mover esa misma cantidad.

**Decidido para el MVP (2026-09-22): no hacer nada todavia.** Con un solo
equipo el volumen es irrelevante. Se revisa cuando haya clientes reales, con
datos de uso en mano.

**Opcion a evaluar cuando toque** (piramide de agregados): el firmware publica
cada 60 s una humedad/temperatura que cambia lento, asi que una muestra cada 5
min es indistinguible para un grafico o una auditoria pero son **1/5 de los
datos** (~100 MB/año por equipo). Crudo 60 s por 30 dias -> promedio 5 min por
1 año -> promedio 1 h por 5 años. La contra: se pierde el detalle minuto a
minuto mas alla de los 30 dias, asi que hay que confirmar que ningun cliente lo
necesite antes de aplicarlo.

El grafico y el export CSV ya acotan el rango (tope de 30 dias, ver
`TELEMETRY_MAX_RANGE_DAYS` en `backend/app/main.py`), pero **eso solo protege la
consulta, no el disco**.

### 9c. `mains_voltage` siempre `null`

El firmware publica `doc["mains_voltage"] = nullptr` y solo manda `zmpt_raw`
(lectura cruda del ZMPT101B, sin calibrar). Consecuencias:

- Las reglas de alarma sobre `mains_1` nunca disparan (el panel ya las marca
  como inertes en `config_form.html`).
- El grafico de la linea de 220V muestra el ADC crudo, rotulado como tal.

Falta calcular RMS en el firmware para tener voltios reales. El backend ya lo
soporta sin tocar codigo: alcanza con reasignar el `payload_key` de la ventana
`mains_1` de `zmpt_raw` a `mains_voltage` desde el formulario de config.

### 9e. `payload_key` derivado del driver — RESUELTO

Al crear `SensorWindow` para equipos que ya existian, el backfill de
`init_db.py` no tenia un `payload_key` explicito en la config y caia al
`source` del sensor. Pero `source` es el **driver** (`ds18b20`,
`sht31_temperature`, `zmpt101b`), **no** la clave que el firmware emite
(`temperature_1`, `temperature_2`, `zmpt_raw`).

El sintoma era muy confuso: el grafico mostraba los ejes y las fechas pero
**ninguna linea**, porque las series existian con todos los valores en `null`.
No es un problema del rango del eje Y. El resumen lo delataba con
`N sensor(es) sin datos`.

Arreglado con un mapa driver -> clave real (`FIRMWARE_PAYLOAD_KEYS`, espejo de
`firmware/src/mqtt_io.cpp`) y una pasada de reparacion
(`repair_sensor_window_keys`) que corre en cada arranque y reescribe solo las
ventanas cuyo `payload_key` es un nombre de driver conocido. Un `payload_key`
editado a mano no se toca, y la operacion es idempotente.

### 9f. Ventanas de sensor: timezone, frontera y CSV — RESUELTO

Tres bugs encadenados en `telemetry_series.py`, todos alrededor de las fechas
de las ventanas. Aparecieron recien al probar con el tipo real de Postgres
(`timestamptz` -> `datetime` **aware**); con SQLite los datetimes son *naive* y
los tres quedaban ocultos.

1. `isoformat() + "Z"` sobre un datetime aware genera `...+00:00Z`, que no es
   parseable. `_parse_iso_epoch` devolvia `None` en silencio, los limites de
   ventana quedaban en `None` y **el filtro por ventana se desactivaba por
   completo**: tras una reasignacion, el alias nuevo mostraba los datos del
   viejo. Arreglado con `_iso_utc()`, que normaliza a UTC antes de serializar.
2. `int(received_at.timestamp())` interpreta un datetime naive como **hora
   local**. Con el servidor en `America/Argentina/Buenos_Aires` cada comparacion
   se corria 3 h respecto de los limites de la ventana. Ahora todo pasa por
   `_to_epoch()`, que trata naive como UTC.
3. La ventana era cerrada (`at <= ends_at`). En una reasignacion el instante del
   traspaso pertenece a las dos ventanas, asi que la misma muestra se contaba
   dos veces. Ahora es semiabierta `[starts_at, ends_at)`.

Ademas el **CSV no filtraba por ventana** (`extract_value` solo lee la clave del
payload): la misma fila aparecia con valor bajo los dos alias, y el CSV no
coincidia con el grafico. Ahora cada columna respeta su ventana.

Nota de proceso: los tests de la sesion anterior pasaban porque SQLite devuelve
datetimes naive. Cualquier test futuro de ventanas tiene que forzar datetimes
aware (o correr contra Postgres) para no repetir este falso verde.

### 9g. Pagina de dispositivo: duplicaciones y orden — RESUELTO

La pagina de detalle mostraba el mismo dato en varios lugares y enterraba el
grafico. Medido en `app.callonfail.com.ar/devices/cof-test` (1440 px):

- **Estado celular 3 veces**: badge en el header, la card "Red celular" y chips
  dentro de "Recursos descubiertos" (operador, CSQ, CREG, CEREG, APN, SMSC,
  modelo, radio, IMS, voz, CS). Los chips eran copia literal de la card. Se
  eliminaron: "Recursos descubiertos" ahora solo lista **hardware** (SHT31,
  Modem, PCF8574, DS18B20 y sus direcciones).
- **Estado de config 2 veces**: la card "Configuracion" (arriba, col 1) y
  "Ultimas configs" (abajo, col 2) mostraban ambas `version` + badge de status,
  a ~1500 px de distancia, obligando a scrollear para cruzarlas. El historial se
  fusiono dentro de "Configuracion".
- **Grafico a media pagina**: vivia en un `col-xl-7` compitiendo con Modem,
  Eventos y Configs, y medía 688 px de 1256. Ahora es ancho completo (1222 px) y
  subio al segundo lugar, debajo del estado del equipo.
- **Tercera representacion de la telemetria**: la tabla de columnas fijas
  (`Temp 1`, `Temp 2`, `Hum`, `VAC`) repetia lo del grafico y encima usaba el
  modelo viejo de columnas, no los alias. `VAC` ademas salia siempre `-` porque
  `mains_voltage` es null. Se elimino; el CSV sigue existiendo para el crudo.
- **Capacidades + Recursos** ocupaban lugar privilegiado con chips crudos: ahora
  estan detras de un `<details>`. El alto del documento bajo de 3778 a 3058 px.

Ademas se agrego al grafico la **leyenda** (mapeaba color -> sensor solo por el
picker) y el **titulo del eje X** ("Fecha y hora"); los dos ejes Y ya tenian
titulo. Los ticks pasaron de 8 a 12 con `autoSkip`, sin solape verificado.

Queda pendiente decidir si la tabla de crudo vuelve **generada desde los alias**
(no columnas fijas) con paginacion, para inspeccionar valores exactos sin bajar
el CSV.

### 9i. Apagon invisible en el grafico — RESUELTO

Con el equipo apagado varias horas, el grafico unia los puntos a ambos lados
como si nunca hubiera dejado de reportar. La causa no era la distancia entre
puntos: era que **`bucket_rows` solo emitia los buckets que tenian filas**
(`for slot in sorted(buckets)`, y `buckets` solo se poblaba con
`setdefault(slot, {})` al aparecer una muestra). Los slots del apagon nunca se
creaban, asi que ningun `null` llegaba al renderer y `spanGaps: false` no tenia
nada que cortar. Con `type: 'time'` Chart.js interpola por posicion en el eje X,
de modo que la linea cruzaba el hueco en diagonal.

Arreglado con `_fill_gaps()`: emite los buckets faltantes entre la primera y la
ultima lectura. Los bordes no se rellenan (no se inventa tiempo antes de la
primera lectura ni despues de la ultima, y un rango entero sin datos sigue
vacio).

Umbral `_GAP_MIN_EMPTY_BUCKETS = 3`: un hueco menor a 3 buckets se deja
puenteado, para que un salto de una lectura no ensucie el grafico. **3 es un
valor inicial a calibrar**: un equipo configurado a 300 s (el maximo) puede dejar
un bucket vacio por desfasaje de reloj, y uno o dos de esos no deben leerse como
apagon.

**Limite del feature, medido y aceptado**: no se puede representar un apagon mas
corto que el bucket, y el bucket lo fija el ancho del rango.

| Rango | Bucket | Detecta apagones de |
|---|---|---|
| 24 h | 300 s | > 15 min |
| 7 d | 3600 s | > 3 h |
| 30 d | 21600 s | > 18 h |

O sea: un apagon de 12 h se ve en 24 h pero **desaparece** al mirar el mismo
periodo en 30 dias (2 buckets de 6 h, debajo del umbral). Cubrir eso requiere un
tramo punteado/gris propio, que se decidio no hacer por ahora.

El **CSV no cambia**: sigue siendo una fila por muestra real, sin filas de
relleno. El grafico rellena, el CSV es el crudo. Verificado que no quedan filas
con `-`.

Impacto medido: 24 h con un apagon de 4 h -> 281 puntos (51 nulos, 2 cortes, el
mayor de 4.00 h). 30 dias a resolucion de 6 h -> ~120 puntos, muy por debajo del
cap de 50k filas, asi que no hay riesgo de inflar el payload.

### 9j. El corte depende del rango (bucket) — RESUELTO

Sintoma: un apagon se veia en 24 h y 7 d pero **no en 30 d**. Medido con un
corte de 4 h: 60 nulos a 24 h, 4 a 7 d, **0 a 30 d**.

**No era el umbral, era de donde salia la decision.** `_fill_gaps` contaba
buckets vacios, y un bucket que contiene un corte *mas* lecturas igual tiene
datos, asi que nunca queda vacio. A 6 h de bucket solo un silencio de ~6 h
vaciaba uno. La informacion la tiraba el promedio.

**Se mide el silencio sobre el crudo.** `silence_spans()` recorre las muestras
crudas -que ya se traen para armar los buckets, sin queries extra- y mide el
delta entre consecutivas. `_fill_gaps` anula despues el bucket que **se solapa**
con un silencio. El grafico no cambia de tamaño: siguen siendo ~120 puntos, lo
que cambia es en cual se corta la linea.

Solapamiento y no "el timestamp del bucket cae dentro del tramo": el bucket se
etiqueta con su inicio, asi que un silencio que empieza un minuto antes de un
limite se escapaba y la linea quedaba sin cortar.

**El umbral deja de depender del bucket.** Antes habia una tabla
(`_GAP_MIN_EMPTY_BUCKETS_BY_BUCKET`) que daba 3 buckets a 1 min y 1 a 6 h. Ahora
el criterio es tiempo real e igual para todos los rangos: **3x la cadencia
medida**, con piso de 180 s y 60 s de margen. La cadencia se mide (mediana
global de los deltas), no se lee de la config: el intervalo es configurable por
equipo y no esta persistido por muestra.

Un silencio que cruza un limite de bucket anula dos puntos contiguos; en el
grafico se ve como **un solo corte**, porque son nulos seguidos.

**Limite que queda:** el corte es granular al bucket. En 30 d la linea se corta
una vez en el bucket de 6 h que contiene el hueco, pero **no se puede leer
cuanto duro**: un corte de 30 min y uno de 5 h se ven igual. Y sigue en pie el
cap de filas crudas (50.000): a 10 s de cadencia, 30 d son 259.200 y el analisis
ve solo la parte que entra.

Comparacion sobre el mismo dataset (cortes de 30 min, 1 h y 4 h), nulos:

| Rango | Bucket | Antes | Ahora |
|---|---|---|---|
| 24 h | 5 min | 0 | 0 |
| 7 d | 1 h | 0 | 0 |
| 30 d | 6 h | **0** | **4 (3 cortes)** - incluye el de 30 min |

### 9k. Ultima lectura por sensor — RESUELTO

`last_readings()` devuelve el valor mas reciente de cada ventana activa, para
responder "en cuanto esta la camara ahora" sin abrir el grafico.

- Las tarjetas van **debajo del estado del equipo**, no grandes: el valor manda
  pero la pagina sigue liderando con el estado.
- Muestran valor + unidad, la **hora exacta** de la muestra y **cuanto hace**
  (esto ultimo se calcula en el navegador), con color por antiguedad: gris al
  dia, ambar pasados 15 min, rojo pasada 1 h. El dato viejo tiene que verse viejo.
- Solo ventanas **abiertas**: un alias retirado no anuncia una lectura que ya no
  produce.
- Busqueda acotada a 7 dias y a las 500 filas mas recientes, en **una sola
  query** (no una por sensor); si no encuentra nada, la tarjeta dice "sin datos".

### 9d. Borrado de historial por alias — NO implementado

Cuando un sensor se reasigna (camara A -> camara B), el historial viejo queda
bajo el alias anterior y el nuevo empieza a acumular. Falta poder **borrar el
historial de un alias** ("ya no voy a loggear mas en esta camara").

Depende de `SensorWindow` (ver `docs/ROADMAP.md`): el alias no es una unidad de
almacenamiento, porque dos alias pueden leer el mismo `payload_key`. Lo unico
que los separa es la **ventana** `[starts_at, ends_at)`. Entonces:

- Borrar "el historial de Camara A" = `DELETE` del crudo en
  `[starts_at, ends_at)`, **no** todas las filas con ese `payload_key` (eso
  borraria tambien el de Camara B).
- Si dos ventanas se solapan en el tiempo con el mismo `payload_key`, hay que
  avisarlo en la confirmacion, porque el borrado se va a llevar datos de las dos.
- Decisiones tomadas para cuando se implemente: alcance = **todo el historial de
  ese alias**, y confirmacion **escribiendo el nombre** (no un clic).
- Ya existe el boton **Cerrar** (deja de registrar sin borrar). El borrado es la
  operacion destructiva y va aparte.

### 9h. Reset de config desde el panel — NO implementado

Pedido para poder rearmar sensores y alias desde cero. Alcance acordado: reset
de **config** (vuelve a los sensores por defecto y cierra las ventanas abiertas)
**sin borrar telemetria**. Va en la card de Sensores de la config del equipo.

Ojo: hoy no se puede editar la config **desde ningun lado** — no hay endpoint
POST para el JSON (se descubrio al escribir el plan de las ventanas). Antes de
que el reset sea util, el panel tiene que poder **crear** config, no solo
resetearla. Un reset tiene que ademas **cerrar las ventanas abiertas** y abrir
las nuevas, o las ventanas viejas seguirian reclamando las muestras nuevas.

---

## P2 — Diferido a proposito (requiere acceso fisico al device)

### 10. MQTT TLS + auth + credenciales por device

Detalle completo en `docs/ops/VPS_CONFIG.md` §"Deferred: finish MQTT security".
**No** OTA-ear un firmware que migre el default a `:8883` sin acceso serial: ya
paso con `0.2.11` y dejo `cof-test` offline.

### 11. Rotar la credencial del panel admin

Sigue siendo la provisoria de la puesta en marcha. Rotarla antes de que el panel
quede expuesto a terceros.

### 12. Infra pendiente

Migraciones de DB, backups, monitoreo basico, `ota.callonfail.com.ar` como
gestor de releases.

---

## Secretos — regla dura

Este repo es **publico**. Nunca commitear valores de credenciales, tokens, APN,
SIM o claves de API: ni en docs, ni en codigo, ni en mensajes de commit, ni en
ejemplos "de prueba".

- Las credenciales reales viven en el `.env` del VPS y en los `Preferences` del
  device, nunca en el repo.
- Cuando una tarea necesite una credencial, el agente la **pide en el chat** o
  el usuario ejecuta el paso. No se guarda en un archivo del repo.
- Plantillas (`.env.example`, docs) usan `<placeholder>`, nunca el valor real.
- Si una credencial se filtra a un commit, **rotarla** aunque se reescriba la
  historia: el valor ya estuvo publico.

---

## Verificado OK en 0.2.60 (no re-tocar)

- Split de `firmware/src/main.cpp`: 5245 -> 880 lineas en 11 modulos + 3
  headers. Verificado por tabla de simbolos: 0 simbolos desaparecidos, +0.08%
  de codigo (diferencia esperada de inlining, LTO esta off).
- OTA a 0.2.60 aplicada y funcionando en el device remoto.
