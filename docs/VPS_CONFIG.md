# CallOnFail VPS configuration

This file is the shared source of truth for VPS setup and operations. Update it
whenever the server, deployment, DNS, MQTT, TLS, backups, or Docker layout
changes.

## Current goal

Run CallOnFail on a single AWS Lightsail VPS (2 vCPU / 2 GB) using Docker
Compose:

- Flask + Gunicorn web/API service (`app.callonfail.com.ar`).
- Public static marketing site (`www.callonfail.com.ar`, apex redirects to www).
- PostgreSQL database.
- Redis.
- Mosquitto MQTT broker.
- MQTT worker.
- Caddy reverse proxy + Let's Encrypt TLS.

Current Lightsail public IP (update if the instance changes):

```text
54.207.204.86
```

## VPS baseline

Recommended baseline (current Lightsail matches the minimum):

- Ubuntu 24.04 LTS.
- 2 vCPU.
- 2 GB RAM minimum, 4 GB RAM preferred.
- 60 GB SSD minimum.
- Static public IP.

The marketing site is static and cheap. The tight resource is RAM for
Postgres + Gunicorn + MQTT worker. Keep Gunicorn at 2 workers on 2 GB. Add
1–2 GB swap if the instance OOMs under load.

## DNS plan

Cloudflare manages `callonfail.com.ar`.

Recommended DNS records (A only; do **not** publish AAAA unless the VPS has
working IPv6 and Caddy listens on it):

```text
callonfail.com.ar        A    <VPS_STATIC_IP>
www.callonfail.com.ar    A    <VPS_STATIC_IP>
app.callonfail.com.ar    A    <VPS_STATIC_IP>
api.callonfail.com.ar    A    <VPS_STATIC_IP>
mqtt.callonfail.com.ar   A    <VPS_STATIC_IP>
ota.callonfail.com.ar    A    <VPS_STATIC_IP>
```

Cloudflare proxy mode for Caddy + Let's Encrypt:

```text
callonfail.com.ar        DNS only   (grey cloud)
www.callonfail.com.ar    DNS only
app.callonfail.com.ar    DNS only
api.callonfail.com.ar    DNS only
mqtt.callonfail.com.ar   DNS only
ota.callonfail.com.ar    DNS only
```

Use **DNS only** while Caddy obtains/renews certificates with HTTP-01. If a
hostname is still Proxied (orange cloud), Cloudflare may publish AAAA to their
edge; Let's Encrypt prefers IPv6 and the ACME challenge returns 404 → no cert
→ browser errors (Cloudflare 525 if proxied, or TLS handshake errors if DNS
only).

MQTT must stay `DNS only` because the normal Cloudflare proxy does not proxy
raw MQTT on port 8883.

After certificates exist, orange-cloud + SSL mode Full (strict) is optional.
While certificates are missing, Flexible can hide the problem temporarily but
is not the target setup.

## Public marketing website

Static HTML lives in the repo under `website/` and is served by Caddy:

```text
https://www.callonfail.com.ar/     -> website/index.html
https://www.callonfail.com.ar/sala/    -> pack Sala / Rack
https://www.callonfail.com.ar/energia/ -> pack Energía
https://www.callonfail.com.ar/ot/      -> pack OT / Tablero
https://www.callonfail.com.ar/frio/    -> pack Cadena de frío
https://callonfail.com.ar/         -> redirect to www
https://app.callonfail.com.ar/     -> Flask app
```

SEO helpers in `website/`:

```text
robots.txt
sitemap.xml
```

After deploy, submit `https://www.callonfail.com.ar/sitemap.xml` in Google Search Console.
For ads, point campaigns to the pack URLs (`/sala/`, `/energia/`, `/ot/`, `/frio/`)
with unique WhatsApp prefill text per landing.
Do **not** open the Flask app by raw IP (`https://<VPS_IP>/...`). After
`APP_DOMAIN=app.callonfail.com.ar`, Caddy only routes the app on that hostname.
Use:

```text
https://app.callonfail.com.ar/
https://app.callonfail.com.ar/devices/cof-test
```

Update the marketing site from the VPS:

```bash
cd /opt/callonfail
git pull origin main
```

Content is bind-mounted; `git pull` is enough for HTML/CSS/assets. Recreate
Caddy only if `deploy/caddy/Caddyfile` or compose volumes changed:

```bash
docker compose up -d caddy
```

## TLS / ACME notes

Production `.env` must use the real app hostname (not `:80`):

```text
APP_DOMAIN=app.callonfail.com.ar
ACME_EMAIL=admin@callonfail.com.ar
```

`:80` is only for a first lab boot before DNS exists. Leaving it set while
`www` / apex are also in the Caddyfile makes HTTPS confusing and is not the
production layout.

Verify certificates from the VPS with correct SNI (do not curl bare `127.0.0.1`
over HTTPS without `--resolve`):

```bash
curl -Ik --resolve www.callonfail.com.ar:443:127.0.0.1 https://www.callonfail.com.ar/
curl -Ik --resolve app.callonfail.com.ar:443:127.0.0.1 https://app.callonfail.com.ar/health
docker compose logs caddy --tail 80 | grep -iE 'certificate obtained|acme|error'
```

If ACME is stuck after fixing DNS, clear Caddy cert state and retry:

```bash
docker compose stop caddy
docker volume ls | grep caddy
docker run --rm -v callonfail_caddy_data:/data alpine \
  sh -c 'rm -rf /data/caddy/certificates /data/caddy/acme /data/caddy/locks'
docker compose up -d caddy
docker compose logs -f caddy
```

Replace `callonfail_caddy_data` with the volume name from `docker volume ls`.

## VPS firewall

Open in AWS Lightsail networking and Ubuntu UFW:

```text
22/tcp    SSH
80/tcp    HTTP
443/tcp   HTTPS
8883/tcp  MQTT over TLS, later
```

Do not expose `1883/tcp` publicly for production.

Current Docker Compose publishes Mosquitto only on localhost:

```text
127.0.0.1:1883:1883
```

This is intentional until TLS/authentication are configured.

For a short lab test from an ESP32 outside the VPS, set this in `.env`:

```text
MQTT_BIND_ADDRESS=0.0.0.0
```

Unknown MQTT devices are ignored by default:

```text
MQTT_AUTO_PROVISION=false
```

For lab-only auto creation of unknown devices, set it to `true`. Production
should keep it disabled and create/provision devices from the web.

Then recreate services:

```bash
docker compose up -d --build
```

Open port `1883/tcp` only temporarily in AWS Lightsail and UFW:

```bash
sudo ufw allow 1883/tcp
```

Close it after the test:

```bash
sudo ufw delete allow 1883/tcp
```

## Initial server packages

```bash
sudo apt update
sudo apt upgrade -y
sudo apt install -y git curl ufw fail2ban ca-certificates
```

## Docker installation

```bash
curl -fsSL https://get.docker.com | sh
sudo usermod -aG docker ubuntu
```

Log out and log back in, then verify:

```bash
docker run hello-world
docker compose version
```

If Docker says permission denied:

```bash
sudo usermod -aG docker ubuntu
exit
```

Then reconnect over SSH.

## Ubuntu UFW setup

```bash
sudo ufw allow OpenSSH
sudo ufw allow 80/tcp
sudo ufw allow 443/tcp
sudo ufw allow 8883/tcp
sudo ufw enable
sudo ufw status
```

## Repository checkout

For branch testing:

```bash
sudo mkdir -p /opt
sudo chown ubuntu:ubuntu /opt
cd /opt
git clone -b cursor/backend-docker-setup-4c37 https://github.com/nmarcovecchio/cof.git callonfail
cd /opt/callonfail
```

After merge to `main`, use:

```bash
git clone https://github.com/nmarcovecchio/cof.git /opt/callonfail
cd /opt/callonfail
```

If the repo already exists:

```bash
cd /opt/callonfail
git pull
```

## Environment file

Create `.env` from the example:

```bash
cp .env.example .env
nano .env
```

For first boot without DNS/TLS:

```text
APP_DOMAIN=:80
```

After DNS points to the VPS:

```text
APP_DOMAIN=app.callonfail.com.ar
ACME_EMAIL=<admin-email>
```

UI timezone:

```text
APP_TIMEZONE=America/Argentina/Buenos_Aires
```

Dates are stored in UTC and rendered in the configured timezone with second
precision.

Admin login:

```text
SECRET_KEY=<long-random-secret>
ADMIN_USERNAME=admin
ADMIN_PASSWORD=<strong-password>
SESSION_COOKIE_SECURE=false
```

`/health` remains public for monitoring. Dashboard, device pages, config pages,
and command actions require login.

After HTTPS is confirmed, set:

```text
SESSION_COOKIE_SECURE=true
```

Always change:

```text
POSTGRES_PASSWORD=change-this-password
```

Do not commit `.env`.

## Docker operations

Start/build:

```bash
docker compose up -d --build
```

Status:

```bash
docker compose ps
```

Logs:

```bash
docker compose logs -f
docker compose logs -f web
docker compose logs -f mqtt-worker
docker compose logs -f mosquitto
```

Restart one service:

```bash
docker compose restart web
```

Stop:

```bash
docker compose down
```

## Health checks

From the VPS (use Host / `--resolve` once `APP_DOMAIN` is a real hostname):

```bash
curl -sS --resolve app.callonfail.com.ar:80:127.0.0.1 \
  http://app.callonfail.com.ar/health
curl -sk --resolve app.callonfail.com.ar:443:127.0.0.1 \
  https://app.callonfail.com.ar/health
curl -sk --resolve app.callonfail.com.ar:443:127.0.0.1 \
  https://app.callonfail.com.ar/api/status
```

From a browser after DNS/TLS:

```text
https://www.callonfail.com.ar/
https://app.callonfail.com.ar/
https://app.callonfail.com.ar/dashboard
https://app.callonfail.com.ar/devices/cof-test
```

Raw IP URLs are only useful for the first `:80` lab boot. In production they
will not serve the Flask app correctly.
## MQTT local test

Publish a test message inside the Mosquitto container:

```bash
docker compose exec mosquitto mosquitto_pub -h localhost -t devices/cof-test/telemetry -m '{"temperature":25.1}'
```

Watch the worker:

```bash
docker compose logs -f mqtt-worker
```

Expected result: the MQTT worker logs the incoming telemetry payload.

## MQTT ESP32 lab test

Preconditions:

- Firmware with MQTT support is running on the ESP32.
- ESP32 has Ethernet or WiFi internet.
- VPS `.env` has `MQTT_BIND_ADDRESS=0.0.0.0`.
- AWS Lightsail and UFW allow temporary `1883/tcp`.

From PlatformIO Monitor:

```text
mqtt <VPS_STATIC_IP> 1883 cof-test
mqtt-status
pub
```

Or, after DNS is configured:

```text
mqtt mqtt.callonfail.com.ar 1883 cof-test
mqtt-status
pub
```

Firmware lab default:

```text
MQTT host: mqtt.callonfail.com.ar
MQTT port: 1883
Device ID: cof-test
```

If no MQTT config is saved in the ESP32 preferences, firmware attempts to
connect to this endpoint automatically.

Watch the VPS:

```bash
docker compose logs -f mqtt-worker
```

Refresh:

```text
https://app.callonfail.com.ar/dashboard
```

## Current security posture

Current MVP state:

- Marketing site and Web/API run behind Caddy with Let's Encrypt.
- PostgreSQL is internal to Docker.
- Redis is internal to Docker.
- Mosquitto:
  - `1883` internal only (Docker network) for web + mqtt-worker
  - `8883` public with TLS + password auth + ACLs for devices

## MQTT TLS + auth setup

### 1) DNS

Ensure Cloudflare has:

```text
mqtt.callonfail.com.ar   A    <VPS_STATIC_IP>    DNS only
```

### 2) VPS firewall / Lightsail

Open:

```text
8883/tcp
```

Close public `1883/tcp` if it was opened for lab tests:

```bash
sudo ufw delete allow 1883/tcp || true
sudo ufw allow 8883/tcp
sudo ufw status
```

Also remove `1883` from the Lightsail networking panel if present.

### 3) Update repo and env

```bash
cd /opt/callonfail
git pull
nano .env
```

Add/update:

```text
MQTT_BACKEND_USERNAME=backend
MQTT_BACKEND_PASSWORD=<strong-backend-pass>
MQTT_DEVICE_USERNAME=cof-test
MQTT_DEVICE_PASSWORD=<strong-device-pass>
MQTT_TLS_DOMAIN=mqtt.callonfail.com.ar
SESSION_COOKIE_SECURE=true
```

Use the same device password in firmware (`COF_DEFAULT_MQTT_PASSWORD`) before building/OTA.

### 4) Generate Mosquitto users

```bash
export MQTT_BACKEND_PASSWORD='...'   # same as .env
export MQTT_DEVICE_PASSWORD='...'    # same as .env
./deploy/mosquitto/gen-passwd.sh
```

### 5) Issue MQTT certificate via Caddy

```bash
docker compose up -d caddy
curl -I https://mqtt.callonfail.com.ar/
./deploy/mosquitto/sync-certs-from-caddy.sh
```

### 6) Start hardened Mosquitto + backend

```bash
docker compose up -d --build
docker compose ps
docker compose logs -f mosquitto
docker compose logs -f mqtt-worker
```

### 7) Point the ESP32 to TLS MQTT

After OTA to firmware `>= 0.2.11`, devices previously using
`mqtt.callonfail.com.ar:1883` migrate automatically to `:8883` with the
default username/password compiled into firmware.

Manual Serial override:

```text
mqtt mqtt.callonfail.com.ar 8883 cof-test cof-test <device-password>
mqtt-status
```

### 8) Verify

From the VPS:

```bash
docker compose exec mosquitto mosquitto_pub \
  -h localhost -p 1883 \
  -u backend -P "$MQTT_BACKEND_PASSWORD" \
  -t devices/cof-test/command \
  -m '{"command":"status_report","device_id":"cof-test"}'
```

From dashboard: device should stay online and accept OTA/config commands.

## Next backend milestones

1. Keep DNS grey-cloud for Caddy ACME (or move to CF origin certs if proxying).
2. Set `SESSION_COOKIE_SECURE=true` once HTTPS is confirmed.
3. Add database migrations.
4. Rotate MQTT device passwords per device (not shared lab password).
5. Add OTA release management under `ota.callonfail.com.ar`.
6. Add backups and basic server monitoring.
