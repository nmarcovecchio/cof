# Backlog de ingenieria — CallOnFail

Estado: **2026-09-25**. Ultimo firmware publicado: **0.2.75** (en `ota/manifest.json`
y corriendo en `cof-test`).

Este archivo es la lista de trabajo tecnico pendiente (deuda, bugs conocidos,
hardening de proceso). **No** es el roadmap de producto: las funciones que
todavia no existen y no se pueden vender viven en `docs/ROADMAP.md`.

Regla: cuando un item se cierra, se borra de aca en el mismo commit que lo
arregla.

---

## P0 — Pipeline de OTA

Tres agujeros encontrados el 2026-09-21 al publicar 0.2.60. El device no tenia
forma de saber que estaba desactualizado durante **cinco releases** (0.2.55 a
0.2.59). El §0 de abajo es del 2026-09-23 y agrega un cuarto, mas de fondo.

### 0. Un equipo solo-LTE nunca se puede actualizar (hallazgo 2026-09-23)

**CONFIRMADO EN HARDWARE el 2026-09-23.** `cof-test` saliendo por LTE: el OTA desde
el panel devolvio `ota_check: accepted` y el equipo **siguio en 0.2.70** con el
manifest anunciando 0.2.72. El servidor estaba bien (el binario del VPS tenia el
sha256 correcto); el corte es del lado del equipo.

`checkManifest()` lee el manifest con `httpGetString()`, que arranca asi:

```c
bool httpGetString(const String& url, String& out, uint32_t timeoutMs) {
  if (!lanConnected()) {          // estado: ethernetConnected || wifiConnected
    return false;
  }
```

`lanConnected()` es Ethernet o WiFi **solamente**: no cuenta `lteDataUp`. Y
`checkManifest()` es quien decide si hay version nueva. Consecuencia: en un sitio
solo-LTE el manifest **nunca se lee**, el equipo nunca ve una version nueva y
**jamas se actualiza solo**.

Es la misma raiz que §8c (el audio y el OTA necesitan `HTTPClient`, que necesita
lwIP), pero aca el sintoma es mas grave porque no depende de una descarga fallida:
ni siquiera se intenta. Y como `performOta()` usa `networkConnected()` (que **si**
cuenta LTE), la asimetria es accidental, no deliberada: el OTA se negaria en
`httpGetString` mucho antes de llegar a `performOta`.

**Mitigado en el panel (commit `53b5407`).** El boton ya no publica a ciegas:
`device_ota_block_reason()` en `backend/app/main.py` rechaza antes de enviar y
avisa por que. El boton queda deshabilitado con la razon en el tooltip y una alerta
debajo, asi que ya no se puede creer que el OTA funciono cuando no puede. Sigue
siendo una mitigacion, **no un arreglo**: el equipo sigue sin poder actualizarse
solo.

**Impacto directo en el trabajo en curso:** esto es lo que hace que B tenga un
bootstrap de una sola vez. El firmware que implementa la via HTTP nativa del
modem solo puede llegar a un equipo que ya tenga LAN - que es justo el escenario
que B viene a resolver. Unico camino: LAN (o flasheo a mano) **una vez**, y a
partir de ahi B abre el OTA por LTE.

**Como cerrarlo:** darle al manifest una via que no requiera lwIP, que es el mismo
trabajo de §8c (`AT+HTTPINIT` -> `AT+HTTPPARA="URL"` -> `AT+HTTPACTION`, y leer el
cuerpo). Ojo: `COF_MANIFEST_URL` apunta a `raw.githubusercontent.com`, **no** al
VPS, asi que esa via tendria que alcanzar GitHub (o el manifest tiene que mudarse
al VPS primero). Ver §8c y la prueba end-to-end del probe en 0.2.69.

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
panic o `CFUN`.

**Instrumentacion implementada en 0.2.74.** `setup()` captura
`esp_reset_reason()` (mapeado a `power_on`/`software`/`panic`/`wdt`/...) en
`state.bootResetReason` y lo publica en el primer status (`reset_reason`), junto
con `CEREG`/`CPSI`/`CSQ` que ya iban en `discovered.cellular`. Con esto, la
proxima vez que el equipo rebootee se distingue "escalera stage 5 = ESP.restart()
= `software`" de un crash (`panic`/`wdt`). Falta una muestra en hardware para
cerrar el item.

**Segunda confirmacion en hardware: 2026-09-24.** Ese dia el modulo reporto
`NO SERVICE` (`CSQ 99,99`) en **tres clusters** separados, todos con MQTT sano por
Ethernet (telemetria fluyendo cada 60 s, sin huecos):

- `11:40:57 / 11:44:01 / 11:51:57 / 11:53:04` -03 (`14:40-14:53` UTC)
- `14:06:13 / 14:09:03 / 14:10:03` -03 (`17:06-17:10` UTC)
- `15:39:54 / 15:44:28` -03 (`18:39-18:44` UTC)

Solo el ultimo cluster termino en **reboot entero del ESP32**: `uptime_s` paso de
`68218` a `15` (`18:50:34 -> 18:52:17` UTC, o sea ~15:52 -03), consistente con la
escalera de radio llegando al **stage 5 = `ESP.restart()`**. La telemetria
descarta el watchdog de silencio MQTT (no hubo hueco de publicacion), asi que la
hipotesis es la escalera y no una caida de red.

**Hallazgo sobre la OLED (corrige el primer analisis):** el usuario vio "reset" en
la OLED **al llegar a las 15:00 -03, antes de tocar nada**, no a las 15:52. Eso es
el footer `setStatus("Modem reset")` que `resetModemRadio()` escribe al **arranque
de cada stage**, y es **pegajoso**: no se limpia cuando la radio se recupera
(`pollModem()` solo resetea `modemRecoveryStage = 0` y hace `return`, sin reescribir
el status), y el path de publicacion de telemetria no toca `setStatus`. Con lo cual
"reset" puede quedar en pantalla **horas** despues de que el modem ya esta sano
(lo dejo pegado el cluster de las 11:40 o 14:06, no el reboot de las 15:52).

**El status sticky quedo arreglado en 0.2.73:** `pollModem()` ahora restaura
`"Network OK"` cuando el status actual es `"Modem reset"`/`"Restart (modem)"` y la
radio volvio a reportar servicio.

**Tercera confirmacion en hardware: 2026-09-25 (LTE).** Con Ethernet desenchufado
(~14:00 -03) y MQTT intentando salir por LTE, el modem **flapeo** entre `NO SERVICE`
y servicio **13 veces en ~83 min** (13 eventos `modem_recovery step 1/5` en el
panel, todos al mismo timestamp porque se diferieron y flushearon en rafaga al
reconectar MQTT). El modem en el fondo estaba **sano**: el trace final muestra
`+CPSI: LTE,Online`, `CSQ 30` (senal mediocre, BAND4 2100MHz), IP asignada y
`CIPOPEN ok`. Dos efectos detectados, ambos arreglados en **0.2.75**:

- **Falta de histeresis en la escalera.** Un solo `radioReportsService() == true`
  reseteaba `modemRecoveryStage = 0`, asi que cada flap re-corria el **step 1**
  (`AT+CGATT=0/1`, detach/attach disruptivo) en vez de escalar o estabilizarse.
  Con senal marginal esto amplifica cada micro-corte en una caida de ~45 s.
  **Fix:** la etapa solo se limpia tras `kModemRecoveryHoldMs` (60 s) de servicio
  sostenido (`modemHealthySinceMs`).
- **Spam de eventos.** `publishDeviceEvent("modem_recovery")` corria en cada step
  con MQTT caido, se encolaba y flusheaba en rafaga. **Fix:** solo se publican los
  bordes del episodio ("recovery started" en step 1, "recovery exhausted" en step
  5), con rate-limit de `kModemRecoveryEventMinIntervalMs` (10 min), y
  `deferDeviceEvent()` ahora descarta duplicados (type+severity+message).

Sigue abierto lo de fondo: **por que el modem pierde servicio 13 veces** con senal
marginal. El `reset_reason` y el evento por etapa ya permiten distinguir la
proxima vez si la escalera llega a `ESP.restart()` o si es RF del sitio. La senal
`CSQ 30` / BAND4 en interior es la hipotesis principal (ambiental, no firmware).

Sin `esp_reset_reason()` no se puede distinguir "stage 5" de un crash/panic: la
instrumentacion de arriba sigue siendo el cierre real de este item.

**Cuarta confirmacion en hardware: 2026-09-25 ~22:13 -03 (solo LTE, piso 12, buena
senal).** El modem volvio a flapear `NO SERVICE` (8 eventos `modem_recovery` en
rafaga) y MQTT reconecto cada ~1,5-2,5 min. Con senal buena (piso 12, segun el
usuario) el flap dejo de ser atribuible a RF ambiental y apunto a **la propia
escalera como causa**: el step 1 mandaba `AT+CGATT=0` (detach) ante el **primer**
`CPSI: NO SERVICE`, y ese detach *generaba* el siguiente flap de registro que
re-disparaba la escalera. Es un loop de retroalimentacion autoinfligido, no un
problema de senal. **Fix en 0.2.78:**

- **Ventana de gracia (`kModemNoServiceGraceMs` = 45 s).** `pollModem()` ya no
  arranca la escalera ante un unico `NO SERVICE`; exige que persista 45 s
  (`noServiceSinceMs`). La banda base del A7672 re-selecciona sola en ~2 s.
- **Step 1 suave.** Antes era `stopLtePdp()` + `CGATT=0/1` (detach/attach). Ahora
  es solo `AT+COPS=0` (re-seleccion de operador). El re-attach PS (`CGATT=1`) pasa
  al step 2 y el ciclo de RF (`CFUN=0/1`) al step 3. El detach desaparece de la
  escalera.
- **`+IPCLOSE` vs `+CIPEVENT: NETWORK CLOSED`.** Antes ambos derribaban el PDP
  (`stopLtePdp()` ~45 s de `NETCLOSE`/`NETOPEN`). Segun el manual del A76XX,
  `+IPCLOSE` es cierre pasivo del socket (broker) con el PDP **vivo**, mientras
  `+CIPEVENT: NETWORK CLOSED UNEXPECTEDLY` es la red caida (PDP muerto). 0.2.78
  solo reconstruye el PDP en el segundo caso (`ltePdpDown`); un cierre de broker
  ahora reabre el socket (~5 s) en vez de un corte visible.

**Hallazgo de arquitectura: MQTT nativo `AT+CMQTT*` — implementado en 0.2.79,
corregido en 0.2.81.** El A7672 tiene un cliente MQTT nativo (`AT+CMQTTSTART`,
`AT+CMQTTCONNECT`, `AT+CMQTTSUB`, `AT+CMQTTPUB`, y la recepción llega por URC
`+CMQTTRXPAYLOAD` en vez de poll de `AT+CIPRXGET`; `+CMQTTNONET` avisa cuando la
red se cae). Hoy reimplementábamos MQTT a mano sobre `CIPOPEN`/`CIPSEND`/`CIPRXGET`,
que es exactamente la capa frágil que produce estos cortes. Mover la ruta LTE a MQTT
nativo elimina el polling, el keepalive manual y el estado de socket/PDP duplicado.
Es el "switch de 2 segundos" de un celular.

**Estado (0.2.79 → 0.2.81):** la ruta LTE usa `firmware/src/lte_mqtt_native.cpp`
(compilada tras `#define COF_LTE_MQTT_NATIVE 1` en `cof_config.h`); el camino LAN
(`PubSubClient`) queda intacto y el socket AT legacy queda detrás del switch.

**Causa raíz real (leída del manual A76XX ch.18, no de la app note SIM7672X):**
el `AT+CMQTTSTART -> ERROR` era una *pista falsa de diagnóstico*. El manual A76XX
(el que corresponde al A7672) dice:

- `AT+CMQTTSTART` **activa el PDP por sí mismo** y responde `OK` + `+CMQTTSTART: 0`
  en éxito. No hace falta `NETOPEN`/`CNACT` previo (el result code 23 es "network
  is opened", i.e. conflictúa).
- Un `ERROR` pelado (sin `+CMQTTSTART: <err>`) significa **"el servicio ya estaba
  arrancado"**.

El bug estaba en `cmqttResult()` (`lte_mqtt_native.cpp`): solo parseaba resultados
con coma (`+CMQTTCONNECT: 0,0`) y devolvía -1 para `+CMQTTSTART: 0` (sin coma). El
primer START **sí tuvo éxito** pero se marcó como falla, se dejó el servicio corriendo
en el módem (el OTA reinicia el ESP32, no el módem), y cada reintento dio "ya
arrancado → ERROR". `0.2.80` empeoró metiendo `NETOPEN` antes de `CMQTTSTART` (pista
falsa). `0.2.81` revierte eso y arregla:

1. `cmqttResult()` parsea resultados sin coma.
2. `AT+CMQTTPUB=<client>,<qos>,<pub_timeout>,<retained>` (orden correcto A76XX).
3. `cmqttConnect()` auto-recupera un servicio ya arrancado (STOP → retry START).

**Validado en hardware (0.2.81):** solo-LTE conectó y publicó (`LTE MQTT OK
(native)`, telemetría entró al backend). Trace real:

```
>> AT+CMQTTSTART  → ERROR          (servicio viejo del OTA anterior)
>> AT+CMQTTSTOP   → OK +CMQTTSTOP: 0
>> AT+CMQTTSTART  → OK +CMQTTSTART: 0
>> AT+CMQTTCONNECT → OK +CMQTTCONNECT: 0,0
```

**Observado en ese trace (0.2.82):** dos CONNECT seguidos con `REL → ERROR` y
`STOP → ERROR` en el medio. La segunda conexión la disparó `cmqttBrokerUp=false`
tras la primera (publicación inicial fallida o `+CMQTTCONNLOST` del broker); el
teardown previo salteaba `DISC` porque el flag estaba apagado pero el módem seguía
conectado, así que REL/STOP respondían "client is busy". Autorecuperó, pero **0.2.82**
endurece `cmqttTearDown()` (DISC incondicional antes de REL/STOP) y además lee el IP
real del PDP (`queryLteIp()` tras CONNECT) para que el OLED muestre `L <ip>` en vez
de `L --` (el camino nativo nunca corre `NETOPEN`, así que `lteIpAddress` quedaba "-").

**Pendiente:** re-probar solo-LTE en 0.2.82 y confirmar (a) una sola secuencia de
CONNECT, (b) `L <ip>` en el OLED, (c) `+CMQTTNONET` en la caída de red.

**OLED `L --` arreglado en 0.2.84.** El `queryLteIp()` de 0.2.82 seguía devolviendo
false en el camino nativo porque `parseLteIp()` tomaba el **primer** campo tras el
tag: `AT+CGPADDR=1` responde `+CGPADDR: 1,10.83.214.110` (CID primero, sin
comillas) y el parser agarraba `"1"`. Además `AT+IPADDR` en modo sin NETOPEN
responde `+IP ERROR: Network not opened` (el camino nativo no abre NETOPEN). Fix:
`parseLteIp()` toma el **último** campo separado por coma (la dirección), y la
consulta se movió **antes** de los `CMQTTSUB` — justo tras CONNECT la UART está
quieta, así el `sendAT` no puede robarse un `+CMQTTRX` entrante (un config/desired
retenido llega inmediatamente tras el SUB).

**SMS/llamada sobre LTE (0.2.85).** SMS y llamada son operaciones del módem
(`AT+CMGS` / `ATD`), a diferencia de Telegram/email que salen por MQTT del backend.
Cuando MQTT viaja por LTE (CMQTT nativo), el único UART del A7672 está compartido
entre la sesión CMQTT y el diálogo de SMS: `transmitSms()` intentaba coexistir
("SMS y PDP coexisten con `AT+CGSMS=1`") y solo soltaba el socket en el reintento.
Resultado en hardware: el comando `test_sms` no mandó nada y el resultado se perdió
porque el `readModemUntil(60 s)` del SMS se bloqueó más que el keepalive de 30 s
del broker y tiró la conexión. Fix: `sendTestSms()`/`transmitSms()` llaman
`releaseLteMqttForModem()` **antes** del diálogo de SMS (módem quieto), el caller
reconecta (`connectMqttIfNeeded`) y `publishDeviceEvent` difiere el resultado
(`deferDeviceEvent`) hasta que MQTT vuelve. Pendiente de re-validar solo-LTE.

**Endurecido en 0.2.83: el equipo solo-LTE nunca queda colgado.** El hueco que
quedaba era el **monitoreo proactivo de radio mientras MQTT viaja por LTE**: con
`lteMqttTransport == true`, `pollModem()`/`refreshCellularStatus()` están gateados
(§6c), así que una radio que muere *en silencio* — sin emitir `+CMQTTNONET` —
dejaba al equipo "conectado" sobre un bearer muerto, dependiendo solo del watchdog
de silencio de 6 min (`ESP.restart()`, que además no toca el módem). 0.2.83 cierra
eso con dos mecanismos:

1. **`serviceLteMqttHealth()`** (en `lte_mqtt_native.cpp`, corre cada pass): un
   `AT+CSQ` cada `kLteHealthProbeMs` (30 s) mientras `cmqttIsConnected()`. Si
   `+CSQ: 99,99` persiste `kModemNoServiceGraceMs` (45 s), suelta el PDP
   (`stopLtePdp()`) y arma `noServiceSinceMs` para que la escalera de `pollModem()`
   dispare de inmediato. Es URC-safe: `cmqttLoop()` drena antes del probe y el probe
   se saltea si hay un mensaje a medio ensamblar (`cmqttIsRxBusy()`).
2. **Escalada de fallo de connect** en `connectMqttNativeIfNeeded()`: si tras
   `kLteMqttConnectFailLimit` (3) fallos se derriba el PDP y `refreshCellularStatus()`
   (seguro, CMQTT ya está caído) muestra que la radio **sigue "registered + Online"**
   pero el plano de datos no conecta, eso es un data-plane wedged, no un corte de
   radio: tras `kLteMqttRebuildEscalateCycles` (3) ciclos de rebuild sin conectar,
   fuerza un ciclo de RF (`resetModemRadio(3)`). Si en cambio reporta NO SERVICE,
   cede a la escalera (`noServiceSinceMs = 1`).

Con esto, en cualquier estado el equipo o publica por LTE, o está activamente
reconectando/escalando (radio cycle → modem reset → ESP32 restart), nunca idle-muerto.

**Pendiente:** validar en hardware solo-LTE: cortar el servicio de la SIM y
confirmar que (a) aparece `+CSQ: 99,99` en el trace del probe, (b) en ~45 s suelta
el PDP y la escalera arranca, (c) al volver la señal reconecta sin tocar nada.

### 6c. La escalera de recuperacion del modem no corre mientras MQTT usa LTE

`pollModem()` (`firmware/src/main.cpp`) tiene gate `!state.lteMqttTransport`:

```cpp
const uint32_t modemEvery = lanConnected() ? kModemIntervalMs : kModemRetryNoLanMs;
if (now - lastModemMs >= modemEvery && !state.callInProgress && !state.audioSyncInProgress &&
    !state.lteMqttTransport) {
  lastModemMs = now;
  pollModem();
}
```

Y la escalera `resetModemRadio()` vive **dentro** de `pollModem()`, despues de
`refreshCellularStatus()`. O sea: con el PDP de LTE arriba y MQTT montado sobre el
socket AT (`lteMqttTransport == true`), **la unica recuperacion de radio que tiene
el firmware queda deshabilitada** - justo en el escenario de un sitio sin LAN,
donde no hay a quien recurrir.

Importa porque la causa conocida de "modem sordo" (`NO SERVICE`, `CSQ 99,99`,
`AT+CNACT?` ERROR, ver §6) necesita esa escalera. El gate esta para no intercalar
`sendAT` con un `AT+CIPOPEN` abierto (mismo motivo que §4), asi que no es un
descuido: es una limitacion asumida. Pero el efecto neto es que un equipo
solo-LTE que se queda sin radio no se recupera solo.

**Como cerrarlo (parcialmente resuelto en 0.2.74):** encadenar la comprobacion de
servicio, no `pollModem()` entero. `radioReportsService()` ya existe y es barata,
pero hoy la alimenta `refreshCellularStatus()`, que tambien es `sendAT` y tiene el
mismo problema. Hace falta una via que no use el UART compartido: leer si el socket
AT sigue vivo (un `CIPEVENT`/`IPCLOSE` inesperado, o el keepalive de MQTT fallando
de forma sostenida) y, si lo esta, soltar el PDP a proposito para poder correr la
escalera. Decidir junto con §4.

**Resuelto en 0.2.74 (el caso "socket muerto").** Cuando MQTT viaja por LTE y el
socket AT muere (`lteMqttClient.sockOpen == false`, detectado por los URC
`+IPCLOSE`/`+CIPEVENT`/`+CASTATE` o por `mqttClient.loop()` fallando),
`connectMqttIfNeeded()` ahora derriba el PDP de inmediato (`stopLtePdp()`). Eso
libera el UART y habilita `pollModem()` -> la escalera arranca en vez de esperar
los tres reintentos de connect (~15 s) antes. Si el socket sigue vivo pero el
broker no respondio el ping, no se derriba el PDP (se hace el reconnect normal).

Queda como **limitacion asumida** el monitoreo *proactivo* de radio mientras el
socket AT esta vivo: no se puede mandar `refreshCellularStatus()` con un
`AT+CIPOPEN` abierto sin arriesgar la perdida de URC de datos MQTT (ver §4). En la
practica la radio muerta mata el socket, asi que el camino de 0.2.74 cubre el caso
real; lo que no cubre es detectar `NO SERVICE` *antes* de que el socket caiga.

**Confirmado en hardware el 2026-09-24 (ver §6h).** La escalera **si corre**
durante una salida por LTE, y el gate no es el que este texto sugiere:

- El gate real es `!state.lteMqttTransport` (MQTT ya montado sobre el socket AT),
  **no** `state.lteDataUp` ni "hay un attach en curso". Entre que el PDP sube
  (`lteDataUp`) y que MQTT se reencamina, `pollModem()` corre normalmente.
- En ese hueco la escalera puede disparar y su **stage 4 es `AT+CFUN=1,1`**, o sea
  un reinicio del modulo en medio del attach. Los URC de arranque del A7672
  (`*ATREADY`, `*ISIMAID`, `+CPIN: READY`) caen entonces dentro de la respuesta de
  un `AT+CIPOPEN` y lo rompen. Es la hipotesis principal del evento del 2026-09-24.
- Corolario: la pantalla OLED con el footer **"Modem reset"** es la evidencia de
  que la escalera se esta ejecutando (`setStatus()` en `resetModemRadio()`). Ese
  footer, y no un string "MQTT reset" que no existe en el firmware, es lo que hay
  que buscar la proxima vez.

La instrumentacion de §6 (`reset_reason` en el status post-boot + evento
`modem_recovery` por stage) quedo implementada en 0.2.74; falta una muestra en
hardware para distinguir "el modulo se colgo solo" de "la escalera lo reinicio".

**Segundo dato en hardware (mismo dia, 2026-09-24 ~15:52 -03, ver §6).** Esta vez
la escalera corrio **por Ethernet** (no en la ventana del attach LTE) y escalo
hasta el final: el equipo rebooteo entero (`uptime_s` 68218 -> 15). Refuerza que el
gate `!lteMqttTransport` no protege contra una escalera que sube peldaños por
Ethernet y termina en `ESP.restart()`; el unico "escudo" es que `radioReportsService()`
vuelva a dar true y resetee `modemRecoveryStage`. El riesgo real de §6c se mantiene
para el caso **solo-LTE**: ahi el gate si deja al modem sin recuperacion de radio
mientras MQTT viaja por el socket AT.

### 6d. RESUELTO en 0.2.71: la IP cacheada del broker

**Resuelto y compilado (sin verificar en hardware).** La correccion importante de
este analisis, porque el diagnostico inicial estaba mal en dos puntos.

**Lo que se creia:** que con MQTT por LTE la IP quedaba congelada y el equipo
quedaba "pegado a LTE sin salida automatica".

**Lo que es en realidad:**

1. **El equipo si se recuperaba solo**, en 6 min, por el watchdog de silencio
   (`kMqttSilenceRestartMs`). `cachedMqttIp` vive en RAM, asi que el reboot la
   limpia y `serviceBrokerResolve()` la rellena desde DNS. No era un equipo
   perdido; era un recovery feo que dependia del reboot que el propio diseño
   queria evitar.

2. **Rompe mas de lo que se pensaba.** `cachedMqttIp` no lo usan solo los probes
   de LAN: `resolveLteMqttPeer()` la prefiere **por encima del DNS del modulo**
   (`lte_pdp.cpp:20`). Con la IP vieja, el connect de LTE tambien fallaba, asi que
   el sintoma no era "LTE anda pero no vuelvo a la LAN" sino "se cae todo".

3. **El disparador es angosto.** Hace falta LAN conectada *pero degradada* (para
   que LTE lleve MQTT) **y** que el broker cambie de IP. En un sitio solo-LTE no
   ocurre: `serviceBrokerResolve()` retorna temprano sin LAN, asi que el modulo
   resuelve fresco en cada intento. Por eso no es critico.

**Causa raiz:** la cache se aprendia del connect por LAN y, mientras MQTT iba por
LTE, no habia **ninguna** via que la actualizara ni que la invalidara: el connect
solo cachea con `!lteMqttTransport` (`mqtt_io.cpp:530`) y el resolver solo adopta
un cambio con MQTT caido (`net_paths.cpp:124`).

**Fix (0.2.71):** flag `lteForceDnsResolve`.

- `connectMqttIfNeeded()` lo levanta cuando un connect por LTE falla, y lo baja
  cuando un connect sale bien.
- `resolveLteMqttPeer()` con el flag puesto consulta `AT+CDNSGIP` en vez de
  confiar en la cache, y **guarda la respuesta fresca en `cachedMqttIp`** - es la
  unica via que puede repuntar la cache con el MQTT arriba por LTE.
- Si el DNS del modulo no contesta, prefiere la IP vieja antes que el hostname:
  `CIPOPEN` quiere un literal, asi que un hostname fallaria el open de una.

**Bug hermano encontrado en el camino:** `saveMqttConfig()`
(`ota_config.cpp:176`) cambiaba `state.mqttHost` sin invalidar `cachedMqttIp`, o
sea que la cache quedaba apuntando al broker **anterior**. Ahora la limpia, y
`serviceBrokerResolve()` la rellena desde el hostname nuevo.

---

### 6e. WiFi sano tarda en promoverse desde LTE, y no se reporta

**Estado 2026-09-24: RESUELTO en 0.2.73 (el fix de bajo riesgo, sin flag optimista).**

**El sintoma.** `markEthernetUp()` marca la salud optimista al obtener IP:

```cpp
// Optimistic: a fresh IP is treated as a working path until the probe says
// otherwise. This keeps boot fast and avoids double-switching.
ethInternetUp = true;
```

El evento de WiFi **no hace eso**: `ARDUINO_EVENT_WIFI_STA_GOT_IP` setea
`wifiConnected`, `wifiSsid` y `wifiIpAddress`, y nunca `wifiInternetUp`. Como
`serviceNetworkPaths()` exige el flag para liberar LTE, asociarse a un WiFi **sano**
estando en LTE no libera LTE hasta que `pollWifiPath()` logre un probe: 10 s de
intervalo + 3 s de settle, ~13 s en el mejor caso. En esa ventana el panel reporta
"sale por LTE" aunque el WiFi este perfecto.

**La asimetria NO es deliberada: es un olvido.** El commit que introdujo los flags
(`ec4fc57`, fw 0.2.55) dice que se setean *"optimistically on IP and demoted by a
failed connect, publish or broker probe"*. Ethernet cumple; WiFi no se actualizo.

**Por que igual no se toca.** Marcar `wifiInternetUp = true` sin probar reabre el bug
que ese mismo commit arreglo: *"a WiFi associated to a router with no uplink used to
keep lanConnected() true forever, so LTE never engaged"*. Seria dar por sano un AP
sin uplink, que es exactamente el caso que el probe existe para detectar. El
comportamiento actual (probar primero, confiar despues) es el correcto; el problema
es solo que prueba tarde.

**Costo real:** ~13 s de LTE de mas en una transicion poco frecuente. No rompe nada,
no pierde datos, no deja el equipo incomunicado. Cosmetico.

**Fix aplicado (0.2.73).** El fix de bajo riesgo, no el flag optimista: en
`ARDUINO_EVENT_WIFI_STA_GOT_IP` se limpia `wifiProbeFails` y, si MQTT va por LTE
(`state.lteMqttTransport`), se resetea `lastWifiProbeMs = 0` para que `pollWifiPath()`
pruebe en la pasada siguiente en vez de esperar hasta 10 s. Eso baja la espera a
~3 s (settle) **sin asumir salud**: el probe sigue decidiendo, asi que un AP sin
uplink no se promueve. Es viable porque `pollWifiPath()` corre antes de
`serviceNetworkPaths()` en el loop, y desde 0.2.71 `cachedMqttIp` queda seteada tras
un connect por LTE, asi que su gate de IP no lo bloquea. Compilado, sin verificar en
hardware (igual que 0.2.71/0.2.72).

### 6f. RESUELTO en 0.2.72: `canUseLan()` hacia probes bloqueantes en el hot path

**Resuelto y compilado (sin verificar en hardware).**

**El problema.** `canUseLan()` delega en `lanPathReachable()`, que llama
`probeMqttOnInterface()` hasta dos veces, cada una con `probe.connect(..., 1500)`
mas dos `applyPreferredRoute()`: hasta ~3 s bloqueando el loop. No tenia timestamp
de throttle, a diferencia de `pollEthernetPath()` / `pollWifiPath()`.

Se llama desde `maintainLteFallback()`, que es la primera linea de
`connectMqttIfNeeded()` y corre **en cada pasada del loop** (~20 ms). El estado que
lo dispara es "LAN conectada pero marcada sin internet", y estando en LTE se alcanza
asi (`net_paths.cpp`):

```cpp
} else if (ethInternetUp) {
  ethInternetUp = false;      // baja la salud sin tocar ethernetConnected
```

O sea: **cada pasada pagaba el probe completo**, estancando sensores, display y el
propio MQTT sobre LTE. Se dispara al enchufar el cable en un router sin uplink
mientras el equipo va por LTE.

**El fix (0.2.72).** Throttle por timestamp en `lanPathReachable()`, con el veredicto
latcheado entre probes (`kLanReachableProbeIntervalMs`, 10 s - la misma cadencia que
los polls). El costo pasa de ~3 s por pasada a ~3 s cada 10 s.

Vale anotar lo que hizo que el fix sea tan chico: **el trabajo ya estaba duplicado**.
`pollEthernetPath()` ya prueba Ethernet cada `kPathRecoverProbeIntervalMs` (60 s)
mientras MQTT va por LTE y mantiene `ethInternetUp` - el mismo veredicto que
`lanPathReachable()` recalculaba en cada pasada. Ademas `pollEthernetPath()` corre
justo **antes** de `connectMqttIfNeeded()` en el loop, asi que los flags que mira
`canUseLan()` ya estan frescos cuando llega.

**Efecto limitado a `canUseLan()`.** `serviceNetworkPaths()` (el que libera LTE) no
usa `lanPathReachable()`: usa `ethInternetUp` / `wifiInternetUp`, que mantienen los
polls. Asi que latchear este veredicto no puede liberar LTE por error.

### 6g. Deuda: `pauseWiFiRadio()` y el respaldo de WiFi temporizado no se usan

**Hallazgo 2026-09-23.** Codigo sin callers: `pauseWiFiRadio()`
(`net_paths.cpp:234`, solo declarada en `cof_api.h:135`) y
`scheduleWifiBackup()` / `maintainWifiBackup()` (`net_paths.cpp:272-288`). Como nada
setea `wifiBackupDueMs`, `maintainWifiBackup()` retorna en su primera linea.

No es un bug de switching, pero engana al que lee: parece que existe un respaldo de
WiFi temporizado y no existe. Sacarlo o conectarlo.

### 6h. El eco del modem tras un reinicio rompe el `AT+CIPOPEN`, y filtra el CONNECT

**Hallazgo 2026-09-24, en hardware (`cof-test`, 0.2.72). RESUELTO en 0.2.74 (a y b).**
Al pasar de Ethernet a LTE, MQTT por LTE fallo ~6 min. El evento `lte_data` del
panel (15:20:23 -03) trae el `handshake` completo y permite reconstruir la secuencia:

```text
open mqtt.callonfail.com.ar:1883 netopen
CIPOPEN no response: AT+CIPOPEN=0,"TCP","54.207.204.86",1883\r\r\nOK\r\n
\u0000\r\n*ATREADY: 1\r\n\r\n*ISIMAID: "A0000000871004FF54F00189000001FF"\r\n\r\n
+CPIN: READY\r\n\r\nSMS DONE\r\n ... +CGEV: EPS PDN ACT 1
mqtt connect failed, state=-2
open mqtt.callonfail.com.ar:1883 netopen
CIPOPEN err 2: AT+CIPOPEN=0,"TCP","54.207.204.86",1883
+CIPOPEN: 0,2
mqtt connect failed, state=-2
...
CIPOPEN ok 54.207.204.86
```

Lectura:

1. **El modulo se reinicio en medio del primer `CIPOPEN`.** `*ATREADY` / `*ISIMAID`
   / `+CPIN: READY` son los URC de arranque del A7672 y no aparecen en operacion
   normal. `state=-2` (`MQTT_CONNECT_FAILED`) confirma que el TCP nunca llevo el
   CONNECT: no fue DNS, APN ni credenciales.
2. **El reinicio dejo el modulo en eco.** La respuesta llega con el comando
   devuelto y un `OK` suelto, **sin el tag `+CIPOPEN:`** que `sendAT()` espera, mas
   bytes de control (`\u0000`, `\u0001`). Eso es exactamente "eco ON".
3. **Por que el firmware no lo apaga de nuevo.** `ATE0` se manda una sola vez,
   dentro de `initModem()` (`modem_at.cpp:421`), y `state.modemReady` sigue en
   `true`, asi que `initModem()` no se re-ejecuta nunca. **El firmware no reconoce
   `*ATREADY` en ningun lado** (grep: 0 matches), asi que tampoco se entera de que
   el modulo se reinicio.

**Hipotesis principal de la causa del reinicio: la propia escalera.** Ver §6c,
confirmado en hardware: durante la ventana entre `lteDataUp` y el reencaminado de
MQTT, `pollModem()` corre, y el **stage 4 de `resetModemRadio()` es `AT+CFUN=1,1`**,
que es un reinicio del modulo. El usuario vio en la OLED el footer **"Modem reset"**
(`setStatus()` en `resetModemRadio()`), que es la evidencia directa de que la
escalera corrio. No se puede distinguir de un cuelgue espontaneo sin la
instrumentacion de §6.

**Dos efectos a arreglar (ambos resueltos en 0.2.74):**

- **(a) Robustez:** re-aplicar `ATE0` cuando el modulo responde con eco, y
  reconocer `*ATREADY` como "el modulo se reinicio" -> re-init. Un reinicio del
  modulo dejaba el firmware hablandole mal el resto del arranque.
- **(b) SEGURIDAD - fuga de credencial:** el `verdict` que se guardaba en el
  `handshake` incluia los **bytes binarios del CONNECT MQTT** (client id, will,
  usuario y **password en texto plano**), porque el eco los devuelve. Con el
  default `change-me-device-mqtt` no se filtro nada real, pero con la credencial
  puesta **el panel la mostraba**. Ademas esos bytes de control son los que
  **rompian el JSON**: el worker lo envolvia en `{"raw": ...}` (`mqtt_worker.py:91`),
  el evento se guardaba como `event` / "Sin mensaje" y **perdia su `severity:
  warning`** (por eso el fallo aparece en el panel como `info`, ver §7).

**Como se cerro (0.2.74):**

- **(a)** `noteModemRebootDetected()` (`modem_at.cpp`): detecta `*ATREADY` en
  `readModemUntil()`, `flushModemInput()` y `LteMqttClient::noteUrc()`, y marca
  `modemReady/simReady/lteDataUp/lteMqttTransport = false` + cierra el socket.
  El proximo `pollModem()` o `ensureLtePdp()` re-ejecuta `initModem()`, que
  re-aplica `ATE0`, `CGDCONT`, `CGAUTH`, `CGSMS`, `CMGF`, `CSCA` y voz. Ademas
  `sendAT()` detecta eco residual (respuesta que empieza con el comando) y
  re-manda `ATE0` sin re-emitir el comando (ya se ejecuto).
- **(b)** `LteMqttClient::write()` ya no vuelca la respuesta cruda al `handshake`:
  guarda solo la forma del ACK (`+CIPSEND: 0,n,n` / `OK` / `ERROR`), nunca el eco
  del comando ni el payload. Y `connect()` pasa la respuesta de un `CIPOPEN` fallido
  por `sanitizeHandshake()` (solo ASCII imprimible, truncado) para que el evento
  siempre serialice como JSON valido.

### 6i. El UART colgaba el loop entero: lecturas sin cota y PDP stale post-llamada

**Hallazgo 2026-09-25, arreglado en 0.2.77.** Cuatro bucles de lectura del UART
del modem (`ModemSerial`) no tenian cota, mas un flag de LTE que quedaba stale
despues de una llamada:

1. **`readModemUntil()`** (`modem_at.cpp`): el `while (ModemSerial.available())`
   interno no re-chequeaba el timeout ni alimentaba el watchdog, y `response`
   crecia sin limite. Un modem que escupe (eco ON, rafaga de URC de arranque,
   ruido de linea) dejaba la funcion girando para siempre: el loop cooperativo
   se estancaba (watchdog de tarea a los 60 s -> `task_wdt`) o agotaba el heap
   (OOM). Es la causa mas probable de "el UART cuelga todo".
   **Fix:** drenar un chunk acotado (256 B) por pasada re-chequeando el deadline,
   y truncar `response` a 4096 B conservando la cola (el token siempre llega
   ultimo).
2. **`flushModemInput()`**: mismo `while` sin cota; `sendAT()` lo llama primero,
   asi que bloqueaba todo comando AT. **Fix:** tope de 1024 B por llamada.
3. **`LteMqttClient::pumpUrcs()`**: bucle externo sin cota; una rafaga de URC
   colgaba `available()` y la bomba de MQTT. **Fix:** tope de 16 lineas por
   llamada.
4. **`LteMqttClient::recvChunk()`**: `header` crecia sin limite y el drenaje no
   tenia cota. **Fix:** tope de 1024 B.

**Coherencia LTE/alarma (corte de luz):** `placeCallAndPlayAudio()` libera el
socket LTE (`releaseLteMqttForModem()`) pero no el PDP, y `bounceRadioForCsfb()`
(`CFUN=4/1`, el camino comun en Claro con CSFB) **mata el NETOPEN**. El flag
`state.lteDataUp` quedaba `true` con el PDP muerto, asi que el primer
`connectMqttIfNeeded()` tras la llamada intentaba `CIPOPEN` sobre un PDP caido y
necesitaba 3 connects fallidos (~15 s) para reconstruir. En una alarma eso son
~15 s en los que el resultado de la llamada no puede publicarse.
**Fix:** capturar `wasOnLte = state.lteDataUp` antes de liberar y, al terminar,
`stopLtePdp()` si `wasOnLte`. Si Ethernet/WiFi volvio durante la llamada,
`maintainLteFallback()` ve la LAN y no re-engancha LTE; si no, `ensureLtePdp()`
reconstruye el PDP limpio en el siguiente loop.

**Starvation del loop con modem muerto:** `pollModem()` re-ejecutaba `initModem()`
completo (hasta ~7 s en 5 intentos AT) en cada poll — 5 s sin LAN, 30 s con LAN —
cuando el modem no responde, estancando sensores, display y MQTT.
**Fix:** throttle `kModemInitRetryMs` (15 s) en el reintento de `initModem()`.

---

## P1 — Backend / panel

### 7. Revisar el error del log del modem en el panel

Pendiente de la lista original de 0.2.55 y **nunca se hizo**: entrar a
`app.callonfail.com.ar` y encontrar el error del modem que reporto el usuario.

**Resuelto el 2026-09-24 en la parte de "encontrar el rastro".** El usuario paso
la credencial en el chat y el panel si guarda la traza: el evento `lte_data`
contiene el `handshake` (head del intento MQTT) y el `modem_log` (ring del attach),
y la vista de dispositivo los muestra en la tarjeta "Modem". Ver §6h para el
hallazgo que salio de ahi.

Queda pendiente lo de fondo: el evento llego **mal clasificado** (`event`, mensaje
"Sin mensaje", severity perdida) porque el payload no era JSON valido - el eco del
modem metio bytes de control (`mqtt_worker.py:91` lo envuelve en `{"raw": ...}`).
La causa raiz (eco crudo en el `handshake`) quedo **resuelta en 0.2.74** con
`sanitizeHandshake()` y el `write()` que ya no vuelca el payload (ver §6h(b)).
Falta verificar en hardware que un `lte_data` de fallo ya no se guarda como
`event`/`info`.

Nota de proceso: la credencial del panel se pasa **en el chat**, nunca al repo (ver
§Secretos).

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

#### Prueba end-to-end del probe (0.2.69)

Las sondas de arriba confirman que los **comandos existen**, pero no que un `GET`
funcione en esta unidad: no dicen que CID necesita el stack HTTP, donde cae el
archivo, ni que hace el flag de `HTTPREADFILE`. Por eso 0.2.69 extiende
**Sondear modem** para correr la secuencia completa contra `example.com`
(estable, ~1 KB, HTTP plano, sin depender de nuestro DNS ni de Caddy mientras el
transporte mismo no esta probado):

```text
AT+FSMEM                          -> memoria antes
AT+HTTPPARA="CID",1
AT+HTTPPARA="URL","http://example.com/"
AT+HTTPACTION=0                   -> espera +HTTPACTION: <method>,<status>,<len>
AT+HTTPREADFILE="C:/probe_http.txt",1
AT+FSMEM                          -> memoria despues (el delta = lo que escribio)
AT+FSLS=C:/                       -> el nombre real; puede no estar soportado
```

El probe sigue siendo **de solo lectura** sobre los assets de alarma: escribe
`probe_http.txt`, un nombre fuera del namespace `a_`. Nada del audio se toca.

**Interpretacion:** si `FSMEM after` sube ~1 KB y `FSLS` lista el archivo, la via
es real y el audio por LTE se puede implementar con ella. Si `HTTPACTION` devuelve
`status` 0 o no llega la URC, el problema es el PDP/CID del stack HTTP y hay que
resolverlo antes de tocar el camino del audio.

#### Decision 2026-09-23: NO implementar. El hotspot cierra el caso.

Evaluado a fondo al confirmarse que un equipo solo-LTE no se puede OTA-ear (§0).
**Conclusion: no se implementa esta via**, por el hotspot.

**El hotspot resuelve el OTA sin codigo.** `lanConnected()` es
`ethernetConnected || wifiConnected`, asi que el firmware trata WiFi como LAN para
el OTA. Un tecnico prende un hotspot en el sitio, el operador carga SSID/password
desde el panel (comando MQTT que **si** viaja por LTE), el equipo asocia, y el OTA
sale por el camino normal. Cero codigo nuevo, cero riesgo nuevo. El aviso del
panel ya lo dice: *"Enchufa Ethernet, o dale una WiFi con internet"*.

**Por que no vale la pena la via del modem**, aunque exista en hardware:

- **El modem no puede flashear al ESP32.** Los bytes tienen que volver por el UART
  a `115200` (`modem_at.cpp:401`). Bajar 1.15 MB al `C:` del modem y despues
  leerlos de vuelta por `AT+FSREAD` troceado son **minutos**, contra un par de
  segundos por Ethernet. Es un camino de descarga ~30x mas lento, y con el UART
  ocupado todo ese rato - el mismo UART que MQTT necesita para no caerse.
- **Es un code path nuevo entero** para usar en una sola cosa.
- **El riesgo no es el argumento** (esto se descarto explicitamente): `performOta()`
  hashea al vuelo y hace `Update.abort()` si no coincide, y el rollback de
  `serviceOtaRollbackGuard()` exige MQTT OK + 20 s de uptime o vuelve a la imagen
  vieja. No se brickearia. El argumento es costo/beneficio, no seguridad.
- **Nadie actualiza firmware en una emergencia.** Siendo el OTA un evento
  planificado, en ese momento planificado tambien se puede prender un hotspot.

**Cuando habria que revisarlo:** sitios **desatendidos, sin internet fijo y sin
nadie que pueda ir** (un contenedor, una camara en el campo). Hoy no hay ninguno.
Si aparecen, el orden sugerido es **PPP del modem** (le da una interfaz lwIP real
al ESP32 y `HTTPClient` funciona sin tocar nada mas: sirve para OTA, audio y
manifest a la vez) antes que esta via, que solo arregla un caso de uso.

### 8d. Tope de audios por equipo (resuelto 0.2.68, silencioso hasta 0.2.67)

El equipo guarda **40 audios distintos** (`kRuleAudioMax` en `ota_config.cpp`).
Hasta 0.2.67 pasarse del tope era **completamente silencioso**, y por tres razones
apiladas:

1. El `continue` que saltea un asset no tocaba `installed`, `pruned` ni `failed`.
2. El evento `call_audio` solo se publicaba si alguno de esos tres era `> 0`.
3. El backend **no conocía el tope**: no hay nada en `main.py` ni en el modelo que
   lo refleje.

Resultado: una config con 45 reglas y 40 ya en el modem no publicaba **ningun**
evento. La pagina del equipo no mostraba nada, esas 5 reglas quedaban mudas, y el
unico rastro era la consola serie (inalcanzable en un sitio sin acceso fisico).
Peor: cual regla quedaba sin voz dependia del orden de iteracion del `std::map`,
no de un criterio.

**Resuelto:**

- **Firmware:** el salteo incrementa `skipped`, se reporta en el evento
  `call_audio` como `skipped (cap)`, y ese evento pasa a `warning` (antes solo lo
  era con descargas fallidas).
- **Backend:** `MAX_RULE_AUDIO_ASSETS = 40` en `main.py`, espejo de `kRuleAudioMax`
  (si se cambia uno hay que cambiar el otro). `count_call_audio_assets()` cuenta
  los assets **por contenido** - dos reglas con el mismo texto resuelto comparten
  un archivo - y el guardado se **rechaza** si la config pide mas de los que
  entran. El conteo se hace antes de sintetizar, para no pagar un TTS de una
  config que se va a rechazar.
- **UI:** aviso en vivo mientras se edita, y el submit se corta con el aviso a la
  vista. El contador del JS es aproximado (cuenta textos crudos, no resueltos), asi
  que puede subcontar; la autoridad es el chequeo del servidor.

**Por que se rechaza en vez de avisar:** una regla guardada sin voz no falla, hace
la llamada y dice el **texto de respaldo**. Es exactamente el fallo silencioso que
motivo retirar `{valor}`.

**Pendiente:** el tope de 40 es conservador y fijo, calculado sobre ~1,5 KB/s de
AMR-NB contra los ~2,91 MiB libres medidos. Con el `C:` real de cada unidad
(`AT+FSMEM`, boton **Sondear modem**) podria subir, pero antes conviene resolver
8d-910KB: si esos ~910 KB son huerfanos, recuperarlos da mas margen que reajustar
el numero.

### 8e. ~910 KB sin explicar en el C: del modem (0.2.62)

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

### 8f. Probar el audio pregrabado en hardware (0.2.64)

La cadena completa (guardar regla → sintetizar → publicar `call_audio` →
descargar al modem → reproducir local) esta verificada solo en banco, no en un
equipo real. **Probar con LAN conectada** (con el cable desconectado la descarga
no puede ocurrir, ver 8c). Falta:

1. Guardar una regla con texto con el numero escrito a mano y confirmar que la
   llamada suena **sin** bajar nada (probar con el cable de red desconectado).
2. Confirmar que **no existe** `{valor}` en la lista de placeholders del
   formulario ni en el ejemplo, y que escribir `{valor}` a mano lo descarta el
   sintetizador en vez de leerlo.
3. Cambiar el texto de una regla y confirmar que el audio viejo **se borra** del
   modem (`[audio] pruned ...` en el log serie) y no queda suelto.
4. Borrar el texto de la regla y confirmar que se poda.
5. Probar una llamada de prueba **antes** de que el asset haya bajado (p.ej. con el
   cable de red desconectado): tiene que decir `Audio not on device` y sonar el
   respaldo, **no** quedarse sin llamada.

### 8g. Limpieza de la migracion de audio

`{valor}` se retiro (ver `NOTIFICATIONS.md`) y no habia ninguna regla guardada
usandolo, asi que **no queda codigo de compatibilidad**: se borraron
`has_retired_placeholder()`, el aviso del formulario, el campo `dynamic` del
payload, `has_dynamic` del modelo, `ruleAudioDynamic`/`ruleAudioIsDynamic()` y la
rama de descarga de TTS en `placeCallAndPlayAudio()`. La llamada ahora solo usa el
archivo local, y si no esta pide un sync de config en vez de intentar una descarga
que en solo-LTE no puede funcionar.

Lo unico que sobrevive a proposito es `loadRuleAudioIndex()` en el firmware, que
sabe leer el formato viejo `{"path":...,"dynamic":...}` ademas del nuevo (string
plano). Es una decena de lineas y evita que un equipo que actualiza por OTA pierda
el audio que ya tenia y lo vuelva a bajar entero por LTE.

**Resuelto en `init_db.py`:** `ensure_schema_columns()` ahora inspecciona
`audio_assets` y, si la columna sigue ahi, corre
`ALTER TABLE audio_assets DROP COLUMN has_dynamic`. Es idempotente (guardado por
la inspeccion) y corre en el mismo arranque del contenedor que el codigo nuevo,
asi que no hay ventana en la que el modelo y la tabla no coincidan.

Hacia falta hacerlo, no era cosmetico: la columna se creo como `nullable=False`
con default del lado de Python, o sea `NOT NULL` **sin** default en el DDL. Una
vez que el modelo deja de declararla, cada `INSERT` en `audio_assets` deja de
nombrarla y Postgres rechaza la fila por violacion de not-null. El `DROP` no
pierde informacion util: el unico valor era `False`.

Verificacion opcional despues del deploy:

```sql
-- No debe existir. Si existe, init_db no corrio.
SELECT column_name FROM information_schema.columns
WHERE table_name = 'audio_assets' AND column_name = 'has_dynamic';
```

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

**Correccion 2026-09-23: son DOS vias distintas, y el arreglo que decia este item
solo cubre una.**

- **Grafico:** resuelve por `payload_key` de la ventana. Reasignar el `payload_key`
  de `zmpt_raw` a `mains_voltage` lo arregla, como decia el item.
- **Alarmas:** **no** usan `payload_key`. `sensor_value()` (`alarms.py:156`) resuelve
  por `SENSOR_ALIASES`, y para `mains_1` los alias son
  `("mains_1", "mains_voltage")`. El firmware manda `mains_voltage: null` y
  `zmpt_raw: <crudo>`, asi que la busqueda no encuentra **ninguno** de los dos y
  devuelve `None`; `condition_holds()` devuelve `None` y
  `evaluate_device_rules()` **saltea la regla en silencio**.

O sea: reasignar el `payload_key` **no** hace disparar la alarma. Para que dispare
hay que calcular RMS real en el firmware (y publicarlo en `mains_voltage`), o bien
agregar `zmpt_raw` a los alias de `mains_1` en `alarms.py` - pero eso haria comparar
un umbral contra un ADC crudo sin calibrar, que no es comparable entre equipos.

**El backend ya lo admite.** `main.py:1977` marca estas reglas como inertes y el
formulario avisa que el firmware todavia no calcula la tension. El sistema sabe que
no funciona; ver abajo el desalineo con la web.

**Falta:** calcular RMS en el firmware para tener voltios reales.

### 9d. La web vende "corte de red electrica" y hoy no puede detectarlo

**Hallazgo 2026-09-23.** `web/energia/index.html` lista **"Corte de red electrica en
el tablero"** como alarma del pack de energia, y `docs/ROADMAP.md` lo reconoce sin
marcarlo como no vendible. Hoy **no hay ninguna via que lo detecte**:

- Por ZMPT (tension): depende de §9c, y la alarma no puede disparar.
- Por contacto seco: las alarmas vecinas de esa misma pagina dicen "via contacto /
  senal", pero IN1 (`P0` del PCF) esta mapeado a **fuga de agua** y IN2 (`P1`) es
  **spare sin borne asignado** (`HARDWARE_V1.md`). O sea que no hay una entrada
  libre para el corte de red.

Es el mismo patron que §1 y §2 del roadmap (Modbus y SNMP): se anuncian funciones que
no existen. La diferencia es que aca es una **alarma de seguridad**, no un extra.

**A decidir (no es una decision de codigo):** o se implementa (RMS en firmware para
la via ZMPT, o reasignar IN2 a corte de red), o se saca de la web y del roadmap hasta
que exista.

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
