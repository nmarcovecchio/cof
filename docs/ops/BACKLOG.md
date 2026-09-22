# Backlog de ingenieria — CallOnFail

Estado: **2026-09-21**. Ultimo firmware publicado y desplegado: **0.2.60**.

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

### 9j. Cortes cortos invisibles en rangos largos — RESUELTO

Pedido: "si en 30 dias algo se corto 1 hora, querria verlo". Con 9i no se podia,
y la razon no era el umbral.

**Por que el bucketing lo impide.** Un bucket que contiene un corte de 1 h *mas*
lecturas igual tiene datos, asi que nunca queda vacio:

```
30 d -> 120 buckets de 6 h
  bucket 00:00-06:00: lecturas 00:00..03:20 | CORTE | 04:20..06:00
                       -> promedia las 340 y da UN punto
```

A 6 h de bucket un corte tendria que durar 6 h para vaciarlo. La informacion la
tira el promedio, no el umbral: bajarlo no lo arregla.

**Solucion: medir los cortes sobre el crudo, no sobre los buckets.**
`detect_outages()` recorre las muestras crudas -que ya se traen para armar los
buckets, asi que no agrega queries- y mira el delta entre consecutivas. Es exacto
al segundo e independiente del zoom. Se separan asi las dos resoluciones que
estaban atadas:

| | Resolucion | De que depende |
|---|---|---|
| La linea (valor) | bucket de 6 h a 30 dias | cuantos puntos se quieren |
| Los cortes (silencio) | precision de segundos | los datos crudos |

**Cadencia medida, no leida de la config.** El intervalo es configurable por
equipo (10-300 s) y cambia en el tiempo, y no esta persistido por muestra, asi
que leerlo de la config actual responderia con el valor de hoy para un hueco de
hace meses. Se usa la **mediana global** de los deltas. Una mediana *local* se
probo primero y era fragil: una racha de muestras puntuales al lado de un
reintento honesto de 180 s bajaba la referencia, el umbral caia justo sobre el
reintento y se reportaban ~5% de falsos positivos. La global no la mueven unos
pocos valores lentos.

Umbral: `max(3 x cadencia, 180 s) + 60 s` de margen. La duracion reportada resta
una cadencia (el delta incluye el silencio *mas* el intervalo normal: un corte de
1 h daba 3660 s a 60 s de cadencia).

**Marcas en el grafico.** El corte se dibuja como banda vertical superpuesta al
canvas, con **ancho minimo de 3 px**: una hora en 30 dias ocupa ~1,7 px de escala
real, o sea menos que un pixel. La exageracion es deliberada - la alternativa era
detectarlo bien y no verlo igual. Ademas hay un contador arriba del grafico
("2 cortes de datos - 5 h sin lecturas en total") con las tres fechas mas largas,
que es lo que garantiza que no se escape aunque las bandas se pisen.

**Limite que queda:** la deteccion no distingue que sensor falto, solo que no
llego ninguna muestra. Y si el rango supera el cap de 50.000 filas (a 10 s de
cadencia, 30 dias = 259.200) el scan ve solo la parte que entra.

Costo medido: 34 ms sobre 50.000 filas (0,68 us/fila), contra una query a
Postgres que ya se hacia.

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
