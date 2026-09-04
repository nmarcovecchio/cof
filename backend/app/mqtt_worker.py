import json
import logging
import os
import signal
import sys
import time
from datetime import timedelta

import paho.mqtt.client as mqtt

from sqlalchemy.orm.attributes import flag_modified

from .extensions import db
from .main import create_app, event_display_severity
from .models import Device, DeviceConfig, Event, Site, Telemetry, Tenant, utcnow


LOG_LEVEL = os.environ.get("LOG_LEVEL", "INFO").upper()
MQTT_HOST = os.environ.get("MQTT_HOST", "mosquitto")
MQTT_PORT = int(os.environ.get("MQTT_PORT", "1883"))
MQTT_USERNAME = os.environ.get("MQTT_USERNAME", "")
MQTT_PASSWORD = os.environ.get("MQTT_PASSWORD", "")
MQTT_CLIENT_ID = os.environ.get("MQTT_WORKER_CLIENT_ID", "callonfail-backend-worker")
MQTT_AUTO_PROVISION = os.environ.get("MQTT_AUTO_PROVISION", "false").lower() == "true"

TOPICS = [
    ("devices/+/telemetry", 0),
    ("devices/+/event", 1),
    ("devices/+/status", 1),
    ("devices/+/config/reported", 1),
    ("devices/+/ack", 1),
]

logging.basicConfig(
    level=LOG_LEVEL,
    format="%(asctime)s %(levelname)s %(name)s %(message)s",
)
logger = logging.getLogger("callonfail.mqtt_worker")
running = True
flask_app = create_app()


def on_connect(client, userdata, flags, rc):
    if rc != 0:
        logger.error("MQTT connect failed rc=%s", rc)
        return

    logger.info("MQTT connected host=%s port=%s", MQTT_HOST, MQTT_PORT)
    for topic, qos in TOPICS:
        client.subscribe(topic, qos=qos)
        logger.info("MQTT subscribed topic=%s qos=%s", topic, qos)


def on_disconnect(client, userdata, rc):
    if rc != 0:
        logger.warning("MQTT unexpected disconnect rc=%s", rc)
    else:
        logger.info("MQTT disconnected")


def on_message(client, userdata, message):
    payload = message.payload.decode("utf-8", errors="replace")
    try:
        decoded = json.loads(payload)
    except json.JSONDecodeError:
        decoded = payload

    logger.info(
        "MQTT message topic=%s qos=%s retain=%s payload=%s",
        message.topic,
        message.qos,
        message.retain,
        decoded,
    )
    persist_message(message.topic, decoded)


def persist_message(topic, payload):
    parts = topic.split("/")
    if len(parts) < 3 or parts[0] != "devices":
        logger.warning("Ignoring unsupported topic=%s", topic)
        return

    device_uid = parts[1]
    message_type = "/".join(parts[2:])

    if not isinstance(payload, dict):
        payload = {"raw": payload}

    with flask_app.app_context():
        try:
            device = get_or_create_device(device_uid)
            if device is None:
                logger.warning("Ignoring unknown device because auto-provision is disabled device=%s topic=%s", device_uid, topic)
                return

            device.last_seen_at = utcnow()

            if device.archived_at is not None:
                recent_warning = (
                    Event.query.filter_by(device_id=device.id, type="archived_device_message")
                    .filter(Event.started_at >= utcnow() - timedelta(hours=1))
                    .first()
                )
                if recent_warning is None:
                    db.session.add(
                        Event(
                            device_id=device.id,
                            type="archived_device_message",
                            severity="warning",
                            message=f"Archived device still publishing {message_type}",
                            payload={"topic": topic, "payload": payload},
                        )
                    )
                db.session.commit()
                logger.warning("Archived device still publishing device=%s topic=%s", device_uid, topic)
                return

            device.status = "online"
            device.firmware_version = payload.get("firmware") or payload.get("firmware_version") or device.firmware_version
            device.ip_address = payload.get("ip") or payload.get("ip_address") or device.ip_address

            if message_type == "telemetry":
                if isinstance(payload.get("cellular"), dict):
                    discovered = dict(device.discovered or {})
                    current = dict(discovered.get("cellular") or {})
                    current.update(payload["cellular"])
                    current["received_at"] = utcnow().isoformat()
                    discovered["cellular"] = current
                    device.discovered = discovered
                    flag_modified(device, "discovered")
                db.session.add(
                    Telemetry(
                        device_id=device.id,
                        payload=payload,
                        firmware_version=device.firmware_version,
                        mains_voltage=to_float(payload.get("mains_voltage")),
                        temperature_1=to_float(payload.get("temperature_1") or payload.get("temp_1")),
                        temperature_2=to_float(payload.get("temperature_2") or payload.get("temp_2")),
                        humidity=to_float(payload.get("humidity")),
                        water_leak=to_bool_or_none(payload.get("water_leak")),
                    )
                )
            elif message_type == "event":
                event_type = str(payload.get("type", "event"))
                message = payload.get("message")
                db.session.add(
                    Event(
                        device_id=device.id,
                        type=event_type,
                        severity=event_display_severity(
                            event_type,
                            message,
                            str(payload.get("severity", "info")),
                        ),
                        message=message,
                        payload=payload,
                    )
                )
            elif message_type == "status":
                device.status = str(payload.get("status", "online"))
                device.hardware_profile = payload.get("hardware_profile") or device.hardware_profile
                if isinstance(payload.get("capabilities"), dict):
                    device.capabilities = payload["capabilities"]
                if isinstance(payload.get("discovered"), dict):
                    discovered = dict(payload["discovered"])
                    cell = dict(discovered.get("cellular") or {})
                    if cell:
                        cell["received_at"] = utcnow().isoformat()
                        discovered["cellular"] = cell
                    device.discovered = discovered
                    flag_modified(device, "discovered")
            elif message_type == "config/reported":
                version = to_int(payload.get("config_version"))
                if version is not None:
                    applied = bool(payload.get("applied"))
                    if applied:
                        device.reported_config_version = version
                    config = DeviceConfig.query.filter_by(device_id=device.id, version=version).first()
                    if config is not None:
                        config.reported_payload = payload
                        config.status = "applied" if applied else "rejected"
                        config.applied_at = utcnow() if applied else None
                    db.session.add(
                        Event(
                            device_id=device.id,
                            type="config_applied" if applied else "config_rejected",
                            severity="info" if applied else "warning",
                            message=(
                                f"config v{version} applied"
                                if applied
                                else f"config v{version} rejected: {payload.get('error', 'unknown')}"
                            ),
                            payload=payload,
                        )
                    )
            elif message_type == "ack":
                command = payload.get("command", "command")
                status = payload.get("status", "unknown")
                db.session.add(
                    Event(
                        device_id=device.id,
                        type="command_ack",
                        severity="info" if status == "accepted" else "warning",
                        message=f"{command}: {status}",
                        payload=payload,
                    )
                )

            db.session.commit()
        except Exception:
            db.session.rollback()
            logger.exception("Failed to persist MQTT message topic=%s", topic)
        finally:
            db.session.remove()


def get_or_create_device(device_uid):
    device = Device.query.filter_by(device_uid=device_uid).first()
    if device is not None:
        return device

    if not MQTT_AUTO_PROVISION:
        return None

    tenant = Tenant.query.filter_by(slug="demo").first()
    if tenant is None:
        tenant = Tenant(name="Demo CallOnFail", slug="demo")
        db.session.add(tenant)
        db.session.flush()

    site = Site.query.filter_by(tenant_id=tenant.id, name="Banco de pruebas").first()
    if site is None:
        site = Site(tenant_id=tenant.id, name="Banco de pruebas")
        db.session.add(site)
        db.session.flush()

    device = Device(
        tenant_id=tenant.id,
        site_id=site.id,
        device_uid=device_uid,
        name=f"Dispositivo {device_uid}",
        status="online",
    )
    db.session.add(device)
    db.session.flush()
    return device


def to_float(value):
    if value is None:
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def to_int(value):
    if value is None:
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def to_bool_or_none(value):
    if value is None:
        return None
    if isinstance(value, bool):
        return value
    if isinstance(value, str):
        return value.lower() in {"1", "true", "yes", "on"}
    return bool(value)


def handle_signal(signum, frame):
    global running
    logger.info("Stopping MQTT worker signal=%s", signum)
    running = False


def main():
    signal.signal(signal.SIGTERM, handle_signal)
    signal.signal(signal.SIGINT, handle_signal)

    client = mqtt.Client(client_id=MQTT_CLIENT_ID)
    if MQTT_USERNAME:
        client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)

    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message

    while running:
        try:
            logger.info("Connecting to MQTT host=%s port=%s", MQTT_HOST, MQTT_PORT)
            client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
            client.loop_start()
            while running:
                time.sleep(1)
            client.loop_stop()
            client.disconnect()
        except Exception:
            logger.exception("MQTT worker error, retrying")
            time.sleep(5)

    return 0


if __name__ == "__main__":
    sys.exit(main())
