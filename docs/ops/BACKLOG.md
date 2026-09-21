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
`app.callonfail.com.ar` con `admin` / `123QAZwsx!` y encontrar el error del
modem que reporto el usuario. Requiere acceso al panel; el agente no puede
autenticarse solo.

### 8. Verificar que las alarmas de corte disparen

Se agregaron en 0.2.55 (`Add Ethernet/WiFi/internet alarm sensors`). Falta
confirmar en el panel que efectivamente disparan y que reportan **liveness de
internet**, no presencia de link.

### 9. Alarma de OTA rechazada / version estancada

Complementa el punto 3.

---

## P2 — Diferido a proposito (requiere acceso fisico al device)

### 10. MQTT TLS + auth + credenciales por device

Detalle completo en `docs/ops/VPS_CONFIG.md` §"Deferred: finish MQTT security".
**No** OTA-ear un firmware que migre el default a `:8883` sin acceso serial: ya
paso con `0.2.11` y dejo `cof-test` offline.

### 11. Rotar la password del panel admin

El usuario ya aviso que `123QAZwsx!` es provisoria.

### 12. Infra pendiente

Migraciones de DB, backups, monitoreo basico, `ota.callonfail.com.ar` como
gestor de releases.

---

## Verificado OK en 0.2.60 (no re-tocar)

- Split de `firmware/src/main.cpp`: 5245 -> 880 lineas en 11 modulos + 3
  headers. Verificado por tabla de simbolos: 0 simbolos desaparecidos, +0.08%
  de codigo (diferencia esperada de inlining, LTO esta off).
- OTA a 0.2.60 aplicada y funcionando en el device remoto.
