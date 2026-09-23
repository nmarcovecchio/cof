# Avisos por cliente (agenda, canales, OK e histeresis)

## Estado (retomar aca)

Codigo en `main`. Proximo paso: **configurar el VPS**. Todavia no estan el bot
de Telegram ni Gmail. No probar alarmas hasta tener eso y rebuild.

En `/opt/callonfail/.env` (no commitear secretos):

```text
TELEGRAM_BOT_TOKEN=123456:ABC...
SMTP_HOST=smtp.gmail.com
SMTP_PORT=587
SMTP_USER=tu.cuenta@gmail.com
SMTP_PASSWORD=xxxx xxxx xxxx xxxx
SMTP_FROM=CallOnFail <tu.cuenta@gmail.com>
SMTP_STARTTLS=true
IMAP_HOST=imap.gmail.com
IMAP_PORT=993
```

`IMAP_*` es solo si queres responder OK por email. El enlace/boton confirma
sin IMAP. `PUBLIC_BASE_URL=https://app.callonfail.com.ar` ya tiene que estar
(sino el SMS/Telegram/email sale sin link).

Despues:

```bash
cd /opt/callonfail
git pull
docker compose up -d --build web mqtt-worker
```

Recien ahi: web → cliente (grupo Telegram + agenda) → **Probar Telegram** /
**Probar email** → **Disparar esta alarma** (en la regla). WhatsApp no va.

---

Un bot y un SMTP para todo CallOnFail. Cada **cliente** tiene una **agenda de
contactos** (nombre + telefono y/o email) y **un grupo de Telegram**. Cada
**regla** elige a quien avisar por SMS, email y llamadas. Telegram, si la
regla lo tiene tildado, va al grupo del cliente. La llamada de un freezer
solo sale si ese dispositivo tiene `Llamadas habilitadas`.

## Que configura quien

| Cosa | Donde | Quien |
| --- | --- | --- |
| Token del bot de Telegram | `.env` del VPS `TELEGRAM_BOT_TOKEN` | Una vez, CallOnFail |
| Gmail SMTP + IMAP | `.env` del VPS `SMTP_*` / `IMAP_*` | Una vez, CallOnFail |
| Agenda (Juan, Maria) | Web → Clientes → Editar | Telefono y/o email |
| Grupo de Telegram | Web → Clientes → Editar | Un chat ID del cliente |
| Quien recibe SMS/email/Telegram | Web → dispositivo → regla | Checkboxes de la agenda |
| Orden de llamadas | Misma regla, contactos con telefono | El orden es el de la agenda |
| Espera entre llamadas | Misma regla, segundos | Tiempo antes de llamar al siguiente |
| Rearme / histeresis | Misma regla, segundos | Si sigue prendida despues de OK |
| Llamadas si/no | Web → dispositivo → Publicar config | Por equipo |

Si varias alarmas piden llamada o SMS a la vez, el backend arma una **cola por
equipo**: una operacion de modem a la vez. Termina la primera y recien sale la
siguiente. No hay llamadas simultaneas (un modem no puede). Maximo 16 jobs.

## Que necesita cada canal (y donde falla)

El disparo lo hace el **servidor**: el equipo publica telemetria por MQTT y
`mqtt_worker` evalua las reglas (`evaluate_device_rules`). El firmware **no**
evalua reglas localmente, asi que sin ninguna via de MQTT (Ethernet, WiFi o LTE
del modem) **no hay alarma**, por ningun canal. Es el piso del producto, no un
detalle de configuracion.

| Canal | Que necesita | Si falta |
| --- | --- | --- |
| Email | Solo el servidor (SMTP). | Queda `skipped: SMTP no configurado`. |
| Telegram | Solo el servidor (bot). | Queda `skipped: TELEGRAM_BOT_TOKEN no configurado`. |
| SMS | El **modem** del equipo con SIM y cobertura. Sale por el bearer CS, no necesita data. | Queda en la cola y falla con el resultado del modem. |
| Llamada | El **modem** *y* que el equipo pueda **bajar el audio** del servidor, es decir lwIP: Ethernet o WiFi. | Ver abajo. |

El equipo tiene un boton **Sondear modem** en su pagina (comando MQTT
`modem_probe`, firmware >= 0.2.62) que publica eventos `modem_probe` con lo que el
modem responde sobre si mismo: `AT+FSMEM` (memoria de `C:`), `AT+CCALB?`,
`AT+HTTPINIT` y `AT+HTTPREADFILE=?`. Sirve para diagnosticar una unidad remota sin
consola serial. El ultimo evento dice si el modem puede bajar archivos por su
cuenta, que es la via para sacar a un sitio solo-LTE del problema de la llamada.

**La llamada es la unica que necesita las dos cosas.** `placeCallAndPlayAudio()`
descarga el AMR por HTTPS antes de marcar (`uploadAudioToModem`). En un sitio sin
Ethernet y sin WiFi, MQTT viaja por el socket AT del modem (`AT+CIPOPEN`), que
**no es una interfaz de lwIP**: `HTTPClient` no tiene ruta y la descarga falla
siempre. Ese era el caso "la alarma quedo registrada pero no sono el telefono".

Desde **0.2.61** la llamada no se pierde en ese caso:

1. Intenta bajar el TTS del servidor (audio con el texto de esa regla).
2. Si la descarga falla **y** el modem ya tiene el audio de respaldo, marca
   igual y reproduce el respaldo. El evento registra
   `TTS unavailable, using fallback`.
3. Si tampoco hay respaldo, no marca y devuelve el error de descarga.

El respaldo es el WAV que anuncia el `manifest.json`
(`ota/audio/cof_fallback.wav` -> `C:/cof_fallback.wav`), que el equipo baja en el
`checkManifest` normal. **Hoy es un WAV producido a mano y congelado**: no hay
sintesis en el ESP32 (no tiene TTS, solo reproduce archivos) ni TTS del lado del
modem (`AT+CTTS` devuelve `ERROR` en este build). Por eso el respaldo dice un
mensaje generico y **no** puede decir el nombre del sitio ni el valor medido: eso
solo lo puede hacer el TTS del servidor, que necesita la descarga.

Un sitio que vaya a operar **solo con LTE** tiene que tener el respaldo cargado.
Se verifica en la pagina del equipo: el evento de la llamada dice que audio uso.

## Texto de la llamada, por regla

Cada regla puede llevar un **Texto de la llamada** (opcional, hasta 400
caracteres). Si esta vacio se usa el texto generico de la alarma, leido en voz
alta con el alias del sensor y el operador en palabras ("Camara A mayor que
-18"), no con los ids de la config (`temp_1 gt -18`).

Acepta placeholders:

```text
{equipo}   {sitio}   {cliente}   {sensor}   {umbral}   {valor}   {regla}
```

Ejemplo:

```text
Alarma en {sitio}. {sensor} marca {valor} grados. Revise la camara.
```

El texto custom afecta **solo la llamada**. El email, Telegram y el SMS siguen
mandando el texto completo de la alarma con el sitio, la regla y el enlace de
confirmacion. Un placeholder desconocido se descarta en vez de leerse.

El escalamiento al siguiente telefono reusa el mismo texto (viaja en el job), asi
que no se re-evalua la regla entre llamadas.

### El audio se pregrabra al guardar (no se sintetiza al llamar)

Al guardar la config, el servidor sintetiza el texto de cada regla **una sola
vez** y lo deja en un store permanente, direccionado por el hash del texto
(`<sha256>.amr`). La config baja al equipo la URL estable y el nombre en el
modem (`C:/a_<sha16>.amr`), y el equipo lo descarga al aplicar la config. Despues
esa descarga **queda hecha**: una caida de red en el momento de la alarma ya no
degrada la llamada, porque el texto esta en el modem.

La llamada reproduce ese archivo **local**, sin bajar nada en el momento de la
llamada.

**Ojo con lo que esto resuelve y lo que no.** La descarga del asset al aplicar la
config usa el mismo `HTTPClient` de siempre, que necesita lwIP (Ethernet/WiFi).
Entonces:

- **Sitio con LAN (aunque sea intermitente):** gana. El audio queda en el modem al
  guardar la regla, y despues la llamada suena **aunque la red se haya caido**.
  Antes, una caida de red en el momento de la alarma degradaba la llamada al
  respaldo generico.
- **Sitio solo-LTE:** **todavia no.** No puede bajar el audio al guardar, por el
  mismo motivo por el que no podia antes (`HTTPClient` sin ruta). Sigue sonando el
  respaldo. La via para cerrarlo es el HTTP nativo del modem
  (`AT+HTTPREADFILE`, ver `docs/ops/BACKLOG.md` 8c), que es trabajo aparte.

Dos reglas con el mismo texto (en el mismo equipo o en otro) comparten **un solo
archivo**, tanto en el servidor como en el modem.

### `{umbral}` vs `{valor}`: la unica diferencia que importa

| Placeholder | Cuando se conoce | Que se pregrabra |
|---|---|---|
| `{umbral}` y el resto | Al guardar la regla | Se graba con el numero ya dicho |
| `{valor}` | Recien al dispararse | Se graba la **variante generica**, sin el numero |

Si el texto lleva `{valor}`, hay **dos** audios: la variante generica pregrabada
y el texto exacto que el servidor sintetiza al disparar. El equipo intenta bajar
el exacto:

- Si tiene internet (Ethernet/WiFi) → baja el exacto y **dice el numero**.
- Si no puede bajarlo (solo-LTE) → reproduce la **variante generica**.

En los dos casos la llamada se hace. Lo unico que cambia es si dice el numero.

En la web hay un boton **Escuchar** al lado del texto: sintetiza y reproduce lo
que va a decir la llamada. Si el texto ya estaba guardado, suena exactamente el
archivo pregrabado.

### Boton "Escuchar" y que es cada cosa

- Texto sin `{valor}`: lo que se escucha es **exactamente** lo que va a sonar,
  siempre, con o sin internet.
- Texto con `{valor}`: lo que se escucha es la **variante generica**. El numero
  exacto depende de la lectura del momento y no se puede pregrabar.

### Archivos en el modem: por que no quedan sueltos

Todo lo que baja el equipo por esta funcion se llama `a_<sha16>.amr`, con el
prefijo `a_` como namespace propio. Al aplicar una config, el equipo descarga lo
que falta y **borra los `a_` que esa config ya no pide**. Nunca toca nada que no
empiece con `a_`: ni el audio de respaldo (`C:/cof_fallback.wav`) ni cualquier
resto de una version vieja. Un archivo desconocido en el modem se conserva, no
se "limpia" a ciegas.

El indice `sha -> archivo` sobrevive un corte de luz porque se guarda en las
Preferences del ESP32; al arrancar se reconstruye sin volver a bajar los audios.

**Limite:** el ESP32 bufferiza el archivo en RAM antes de pasarlo al modem, con
un tope de 180 KB. A 12.2 kbps eso da ~118 s de audio por regla; un texto de 400
caracteres esta muy por debajo. Ademas se topea en 40 archivos distintos por
equipo, por los 4 MiB del modem (2,91 MiB libres medidos en `cof-test`).


Al normalizarse se puede avisar por email, Telegram y/o SMS (configurable en la
regla). El ciclo se ve en **Alarmas**.

Cada aviso lleva un **enlace para confirmar y silenciar** (SMS, Telegram con
boton, email). Hay que tocarlo y confirmar. Eso silencia las alarmas abiertas
de **ese equipo**. El enlace muere cuando esa alarma se cierra. En el grupo
de Telegram no vale escribir ok: solo el boton. En SMS/email, un mensaje con
ok sigue de respaldo. Atender una llamada no corta. En la web hay
**Silenciar alarmas de este equipo**.
El aviso de detencion va al grupo de Telegram y queda en el ciclo (auditoria).
Si el sensor sigue mal, se repite el ciclo cada N segundos (mismo N que
despues de un OK). 0 = no se repite hasta que se normalice.

No hace falta un bot por cliente. Si un token se filtra, un solo bot ve todos
los grupos; por eso el token vive en el VPS, no en la web.

## 1. Telegram — un bot

1. En Telegram, abrí [@BotFather](https://t.me/BotFather).
2. `/newbot` → nombre `CallOnFail` → username tipo `CallOnFailAlertasBot`.
3. Copiá el token (`123456:ABC...`). **No lo subas a git.**
4. En el VPS, `/opt/callonfail/.env`:

   ```text
   TELEGRAM_BOT_TOKEN=123456:ABC...
   ```

5. El bot no lee mensajes del grupo: solo manda el aviso con el boton
   **Confirmar y silenciar**. No hace falta `/setprivacy` Disable.

## 2. Telegram — grupo o chat de cada contacto

1. Creá un grupo, por ejemplo `Alertas Heladería Centro`, o usa un chat
   privado con el bot.
2. Agregá el bot al grupo.
3. Mandá un mensaje en el grupo (`hola`).
4. En el VPS (reemplazá el token):

   ```bash
   curl -s "https://api.telegram.org/botTOKEN/getUpdates"
   ```

5. Buscá `"chat":{"id":-100...`. Ese número es el chat ID.
6. En la web: **Clientes → Editar** → grupo Telegram del cliente → pegalo → Guardar → **Probar Telegram**.

Otras formas de ver el ID: agregar [@userinfobot](https://t.me/userinfobot) o
[@RawDataBot](https://t.me/RawDataBot) al grupo y leer el `id` (después los
podés sacar).

Los supergrupos empiezan con `-100`. Si reenviás el grupo a un canal, el ID
cambia: volvé a leer `getUpdates`.

Para detener el escalamiento: tocar **Confirmar y silenciar** en el aviso.

## 3. Gmail SMTP (pruebas) e IMAP (respuesta OK)

Gmail no acepta la contraseña normal. Hace falta 2FA + App Password.

1. En la cuenta Gmail, activá
   [verificación en 2 pasos](https://myaccount.google.com/signinoptions/two-step-verification).
2. Creá una
   [contraseña de aplicación](https://myaccount.google.com/apppasswords)
   (aplicación: Correo, dispositivo: Otro → `CallOnFail`).
3. En Gmail: Settings → See all settings → Forwarding and POP/IMAP →
   **Enable IMAP**.
4. Copiá las 16 letras. En el VPS `.env`:

   ```text
   SMTP_HOST=smtp.gmail.com
   SMTP_PORT=587
   SMTP_USER=tu.cuenta@gmail.com
   SMTP_PASSWORD=xxxx xxxx xxxx xxxx
   SMTP_FROM=CallOnFail <tu.cuenta@gmail.com>
   SMTP_STARTTLS=true
   IMAP_HOST=imap.gmail.com
   IMAP_PORT=993
   IMAP_USER=tu.cuenta@gmail.com
   IMAP_PASSWORD=xxxx xxxx xxxx xxxx
   ```

   Podés pegar la app password con o sin espacios. Si `IMAP_USER` /
   `IMAP_PASSWORD` van vacíos, se reusan los de SMTP.
5. El **destinatario** no va acá. Va en la ficha del contacto.
6. Web: **Clientes → Editar** → contacto con email → Guardar → **Probar email**.

Si Gmail bloquea: revisá que 2FA esté on, que la app password sea de esa
cuenta, y que `SMTP_USER` / `SMTP_FROM` coincidan con esa cuenta.

El mail de alarma pide responder **OK** o abrir el enlace
`/alarms/<id>/ack/<token>`. El enlace no pide login. IMAP lee las respuestas
OK; el enlace funciona aunque IMAP no esté.

## 4. Aplicar env en el VPS

Despues de `git pull` y de editar `.env`:

```bash
cd /opt/callonfail
git pull
docker compose up -d --build web mqtt-worker
```

Hace falta rebuild porque cambiaron variables, la agenda y el worker. IMAP
sigue leyendo OK por email. `init_db` agrega la columna `contacts`; no toca
datos. Los emails/telefonos viejos se leen como contactos hasta que guardes
el cliente de nuevo.

## 5. En la web

1. Login admin.
2. **Clientes** → crear o **Editar** el cliente (ej. Demo).
3. Cargar el grupo de Telegram del cliente y contactos con telefono/email.
4. **Probar email** y **Probar Telegram**.
5. En el dispositivo: **Publicar config** → habilitar llamadas si ese equipo
   debe llamar → en cada regla elegir contactos, espera entre llamadas y
   rearme.
6. En el dispositivo: **Disparar esta alarma** en la regla a probar.
   - Email y Telegram salen ya (a los contactos de esa prueba: todos).
   - SMS si hay telefono.
   - Llamada solo si `Llamadas habilitadas` y hay telefono. Mismo camino CSFB
     que **Probar llamada**. Si la regla tiene **Texto de la llamada**, es lo que
     se escucha; si no, el texto generico de la alarma.

`Durante (seg)` es cuanto tiene que estar mal el sensor antes del primer
disparo. `Rearmar si sigue prendida` es cuanto esperar, despues de un OK o
de que se normalice, para volver a sonar si el sensor sigue mal. `0` = no
vuelve a sonar hasta que se normalice y se vuelva a romper.

Una alarma de regla no se vuelve a disparar mientras sigue abierta (salvo
rearme por histeresis). **Disparar esta alarma** no espera la condicion del sensor: es una prueba.

## 6. Checklist rapido

- [ ] `TELEGRAM_BOT_TOKEN` en `.env` y compose rebuild
- [ ] Bot agregado al grupo del cliente
- [ ] Chat ID `-100...` guardado en el cliente
- [ ] **Probar Telegram** llega al grupo
- [ ] **Sondear modem** en un equipo 0.2.62+: los eventos `modem_probe` responden
      (memoria de `C:`, y si `HTTPREADFILE` esta soportado)
- [ ] Gmail 2FA + App Password en `SMTP_*` e IMAP habilitado
- [ ] Email del contacto cargado
- [ ] **Probar email** llega
- [ ] Telefono `+549...` en el contacto
- [ ] En la regla, tildar quien recibe SMS/email/Telegram/llamada
- [ ] En el equipo, llamadas habilitadas si corresponde
- [ ] Si el sitio opera **solo con LTE**, verificar que el respaldo de audio este
      en el modem (`checkManifest`); sin eso la llamada no sale
- [ ] **Disparar esta alarma** genera evento `alarm` y los avisos de esa regla
- [ ] El enlace/boton confirma y silencia; al cerrarse la alarma el enlace muere
