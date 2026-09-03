# CallOnFail VPS configuration

This file is the shared source of truth for VPS setup and operations. Update it
whenever the server, deployment, DNS, MQTT, TLS, backups, or Docker layout
changes.

## Current goal

Run the CallOnFail MVP backend on a single VPS using Docker Compose:

- Flask + Gunicorn web/API service.
- PostgreSQL database.
- Redis.
- Mosquitto MQTT broker.
- MQTT worker.
- Caddy reverse proxy.

## VPS baseline

Recommended baseline:

- Ubuntu 24.04 LTS.
- 2 vCPU.
- 2 GB RAM minimum, 4 GB RAM preferred.
- 60 GB SSD minimum.
- Static public IP.

## DNS plan

Cloudflare manages `callonfail.com.ar`.

Recommended DNS records:

```text
app.callonfail.com.ar    A    <VPS_STATIC_IP>
api.callonfail.com.ar    A    <VPS_STATIC_IP>
mqtt.callonfail.com.ar   A    <VPS_STATIC_IP>
ota.callonfail.com.ar    A    <VPS_STATIC_IP>
```

Cloudflare proxy mode:

```text
app.callonfail.com.ar    Proxied or DNS only
api.callonfail.com.ar    Proxied or DNS only
mqtt.callonfail.com.ar   DNS only
ota.callonfail.com.ar    Proxied or DNS only
```

MQTT must be `DNS only` because the normal Cloudflare proxy does not proxy raw
MQTT on port 8883.

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
```

`/health` remains public for monitoring. Dashboard, device pages, config pages,
and command actions require login.

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

From the VPS:

```bash
curl http://localhost/health
curl http://localhost/api/status
```

From a browser during first HTTP test:

```text
http://<VPS_STATIC_IP>/
```

After DNS/TLS:

```text
https://app.callonfail.com.ar/
```

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
http://<VPS_STATIC_IP>/dashboard
```

## Current security posture

Current MVP state:

- Web/API can run behind Caddy.
- PostgreSQL is internal to Docker.
- Redis is internal to Docker.
- Mosquitto is internal/localhost only.
- MQTT authentication/TLS for external devices is not configured yet.

Before connecting deployed devices over the internet, add:

- MQTT username/password or certificates per device.
- MQTT TLS on `8883`.
- ACLs so each device can only access its own topics.
- Backup automation for PostgreSQL.
- Basic server monitoring.

## Next backend milestones

1. Merge backend scaffold to `main`.
2. Configure DNS for `app.callonfail.com.ar`.
3. Enable HTTPS via Caddy.
4. Add database migrations.
5. Add users/tenants/devices tables.
6. Add device provisioning.
7. Add MQTT auth/ACLs.
8. Add telemetry persistence.
9. Add dashboard for device status.
10. Add desired/reported config over MQTT.
11. Add OTA release management.
12. Add backups.
