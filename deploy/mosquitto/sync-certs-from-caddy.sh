#!/usr/bin/env bash
set -euo pipefail

# Copy Let's Encrypt certs issued by Caddy for mqtt.callonfail.com.ar
# into deploy/mosquitto/certs for the Mosquitto TLS listener.
#
# Prerequisites:
#   1. DNS mqtt.callonfail.com.ar -> VPS (DNS only / grey cloud)
#   2. Caddyfile includes mqtt.callonfail.com.ar
#   3. docker compose up -d caddy  (so the cert exists)
#
# Run from repo root on the VPS:
#   ./deploy/mosquitto/sync-certs-from-caddy.sh

DOMAIN="${MQTT_TLS_DOMAIN:-mqtt.callonfail.com.ar}"
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
CERT_DIR="${ROOT_DIR}/deploy/mosquitto/certs"
COMPOSE_PROJECT="${COMPOSE_PROJECT_NAME:-$(basename "$ROOT_DIR")}"

mkdir -p "$CERT_DIR"

# Prefer explicit volume name; fall back to compose project prefix.
VOLUME_NAME="${CADDY_DATA_VOLUME:-}"
if [[ -z "$VOLUME_NAME" ]]; then
  if docker volume inspect "${COMPOSE_PROJECT}_caddy_data" >/dev/null 2>&1; then
    VOLUME_NAME="${COMPOSE_PROJECT}_caddy_data"
  elif docker volume inspect caddy_data >/dev/null 2>&1; then
    VOLUME_NAME="caddy_data"
  else
    echo "Could not find caddy_data volume. Set CADDY_DATA_VOLUME=..." >&2
    docker volume ls
    exit 1
  fi
fi

echo "Using volume: $VOLUME_NAME"

docker run --rm \
  -v "$VOLUME_NAME":/caddy-data:ro \
  -v "$CERT_DIR":/out \
  alpine:3.20 \
  sh -c "
    set -e
    CRT=\$(find /caddy-data -type f -name '${DOMAIN}.crt' | head -n 1)
    KEY=\$(find /caddy-data -type f -name '${DOMAIN}.key' | head -n 1)
    if [ -z \"\$CRT\" ] || [ -z \"\$KEY\" ]; then
      echo 'Certificate for ${DOMAIN} not found in Caddy data yet.' >&2
      echo 'Make sure Caddy has served https://${DOMAIN} at least once.' >&2
      find /caddy-data -type f -name '*.crt' | head
      exit 1
    fi
    cp \"\$CRT\" /out/server.crt
    cp \"\$KEY\" /out/server.key
    # For Mosquitto cafile when using a public LE leaf, reuse the leaf bundle.
    # Caddy .crt usually includes the intermediates needed for clients.
    cp \"\$CRT\" /out/ca.crt
    chmod 0644 /out/server.crt /out/ca.crt
    chmod 0600 /out/server.key
    echo 'Synced certs:'
    ls -la /out
  "

echo "Done. Restart mosquitto after syncing:"
echo "  docker compose up -d mosquitto"
