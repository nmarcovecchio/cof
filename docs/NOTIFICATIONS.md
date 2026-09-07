# Avisos por cliente (agenda, canales, OK e histeresis)

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

5. `/setprivacy` en BotFather → Disable, para que el bot vea el **OK** en el
   grupo. Sin eso solo ve comandos.

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
6. En la web: **Clientes → Editar** → contacto → pegalo → Guardar → **Probar Telegram**.

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

Hace falta rebuild porque cambiaron variables, la agenda y el worker ahora
escucha OK por Telegram/IMAP. `init_db` agrega la columna `contacts`; no toca
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
6. En el dispositivo: **Disparar alarma**.
   - Email y Telegram salen ya (a los contactos de esa prueba: todos).
   - SMS si hay telefono.
   - Llamada solo si `Llamadas habilitadas` y hay telefono. Mismo camino CSFB
     que **Probar llamada**.

`Durante (seg)` es cuanto tiene que estar mal el sensor antes del primer
disparo. `Rearmar si sigue prendida` es cuanto esperar, despues de un OK o
de que se normalice, para volver a sonar si el sensor sigue mal. `0` = no
vuelve a sonar hasta que se normalice y se vuelva a romper.

Una alarma de regla no se vuelve a disparar mientras sigue abierta (salvo
rearme por histeresis). **Disparar alarma** no espera eso: es una prueba.

## 6. Checklist rapido

- [ ] `TELEGRAM_BOT_TOKEN` en `.env` y compose rebuild
- [ ] Bot agregado al grupo del cliente y privacy Disable
- [ ] Chat ID `-100...` guardado en un contacto
- [ ] **Probar Telegram** llega al grupo
- [ ] Gmail 2FA + App Password en `SMTP_*` e IMAP habilitado
- [ ] Email del contacto cargado
- [ ] **Probar email** llega
- [ ] Telefono `+549...` en el contacto
- [ ] En la regla, tildar quien recibe SMS/email/Telegram/llamada
- [ ] En el equipo, llamadas habilitadas si corresponde
- [ ] **Disparar alarma** genera evento `alarm` y los avisos
- [ ] El enlace/boton confirma y silencia; al cerrarse la alarma el enlace muere
