# Avisos por cliente (email, Telegram, telefono)

Un bot y un SMTP para todo CallOnFail. Cada **cliente** tiene emails, grupos
de Telegram y telefonos (uno por linea). Los equipos heredan eso. Email y
Telegram van a todos. SMS y llamada usan el primer telefono. La llamada de un
freezer solo sale si ese dispositivo tiene `Llamadas habilitadas`.

## Que configura quien

| Cosa | Donde | Quien |
| --- | --- | --- |
| Token del bot de Telegram | `.env` del VPS `TELEGRAM_BOT_TOKEN` | Una vez, CallOnFail |
| Gmail SMTP | `.env` del VPS `SMTP_*` | Una vez, CallOnFail |
| Emails del cliente | Web → Clientes → Editar, uno por linea | Se avisa a todos |
| Chat ID del grupo | Web → Clientes → Editar, uno por linea | Se avisa a todos |
| Telefonos | Web → Clientes → Editar, uno por linea | Hoy SMS/llamada usan el **primero** |
| Llamadas si/no | Web → dispositivo → Publicar config | Por equipo |

La cascada de llamadas (primero X, si no atiende Y, luego Z) **todavia no esta**.
Los telefonos ya se guardan en orden para cuando se arme.

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

5. `/setprivacy` en BotFather → Disable, para que el bot vea mensajes del grupo
   si más adelante hace falta. Para **enviar** alarmas no es obligatorio.

## 2. Telegram — grupo de cada cliente

1. Creá un grupo, por ejemplo `Alertas Heladería Centro`.
2. Agregá el bot al grupo.
3. Mandá un mensaje en el grupo (`hola`).
4. En el VPS (reemplazá el token):

   ```bash
   curl -s "https://api.telegram.org/botTOKEN/getUpdates"
   ```

5. Buscá `"chat":{"id":-100...`. Ese número es el chat ID.
6. En la web: **Clientes → Editar** → pegalo → Guardar → **Probar Telegram**.

Otras formas de ver el ID: agregar [@userinfobot](https://t.me/userinfobot) o
[@RawDataBot](https://t.me/RawDataBot) al grupo y leer el `id` (después los
podés sacar).

Los supergrupos empiezan con `-100`. Si reenviás el grupo a un canal, el ID
cambia: volvé a leer `getUpdates`.

## 3. Gmail SMTP (pruebas)

Gmail no acepta la contraseña normal. Hace falta 2FA + App Password.

1. En la cuenta Gmail, activá
   [verificación en 2 pasos](https://myaccount.google.com/signinoptions/two-step-verification).
2. Creá una
   [contraseña de aplicación](https://myaccount.google.com/apppasswords)
   (aplicación: Correo, dispositivo: Otro → `CallOnFail`).
3. Copiá las 16 letras. En el VPS `.env`:

   ```text
   SMTP_HOST=smtp.gmail.com
   SMTP_PORT=587
   SMTP_USER=tu.cuenta@gmail.com
   SMTP_PASSWORD=xxxx xxxx xxxx xxxx
   SMTP_FROM=CallOnFail <tu.cuenta@gmail.com>
   SMTP_STARTTLS=true
   ```

   Podés pegar la app password con o sin espacios.
4. El **destinatario** no va acá. Va en la ficha del cliente.
5. Web: **Clientes → Editar** → email → Guardar → **Probar email**.

Si Gmail bloquea: revisá que 2FA esté on, que la app password sea de esa
cuenta, y que `SMTP_USER` / `SMTP_FROM` coincidan con esa cuenta.

## 4. Aplicar env en el VPS

Despues de `git pull` y de editar `.env`:

```bash
cd /opt/callonfail
git pull
docker compose up -d --build web mqtt-worker
```

Hace falta rebuild porque cambiaron variables y el worker ahora evalua reglas.
`init_db` agrega solo las columnas nuevas (`notify_email`, `telegram_chat_id`,
`phone`); no toca datos.

## 5. En la web

1. Login admin.
2. **Clientes** → crear o **Editar** el cliente (ej. Demo).
3. Cargar emails, chat IDs y telefonos `+549...` (uno por linea).
4. **Probar email** y **Probar Telegram**.
5. En el dispositivo: **Publicar config** → habilitar llamadas si ese equipo
   debe llamar → guardar reglas (sensor, umbral, acciones).
6. En el dispositivo: **Disparar alarma**.
   - Email y Telegram salen ya.
   - SMS si hay telefono.
   - Llamada solo si `Llamadas habilitadas` y hay telefono. Mismo camino CSFB
     que **Probar llamada**.

Una alarma de regla no se vuelve a disparar hasta que la condicion se
normaliza (debounce). **Disparar alarma** no espera eso: es una prueba.

## 6. Checklist rapido

- [ ] `TELEGRAM_BOT_TOKEN` en `.env` y compose rebuild
- [ ] Bot agregado al grupo del cliente
- [ ] Chat ID `-100...` guardado en el cliente
- [ ] **Probar Telegram** llega al grupo
- [ ] Gmail 2FA + App Password en `SMTP_*`
- [ ] Email del cliente cargado
- [ ] **Probar email** llega
- [ ] Telefono `+549...` en el cliente
- [ ] En el equipo, llamadas habilitadas si corresponde
- [ ] **Disparar alarma** genera evento `alarm` y los avisos
