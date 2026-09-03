#!/usr/bin/env bash
set -euo pipefail

# Generate Mosquitto password file from environment variables.
# Run from repo root on the VPS:
#   export MQTT_BACKEND_PASSWORD='...'
#   export MQTT_DEVICE_PASSWORD='...'
#   ./deploy/mosquitto/gen-passwd.sh

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
OUT_FILE="${ROOT_DIR}/deploy/mosquitto/passwd"

BACKEND_USER="${MQTT_BACKEND_USERNAME:-backend}"
BACKEND_PASS="${MQTT_BACKEND_PASSWORD:?Set MQTT_BACKEND_PASSWORD}"
DEVICE_USER="${MQTT_DEVICE_USERNAME:-cof-test}"
DEVICE_PASS="${MQTT_DEVICE_PASSWORD:?Set MQTT_DEVICE_PASSWORD}"

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

docker run --rm \
  -v "$TMP_DIR":/out \
  eclipse-mosquitto:2 \
  sh -c "touch /out/passwd && mosquitto_passwd -b -c /out/passwd '$BACKEND_USER' '$BACKEND_PASS' && mosquitto_passwd -b /out/passwd '$DEVICE_USER' '$DEVICE_PASS' && chmod 0644 /out/passwd"

cp "$TMP_DIR/passwd" "$OUT_FILE"
echo "Wrote $OUT_FILE"
echo "Users: $BACKEND_USER, $DEVICE_USER"
