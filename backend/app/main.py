import hashlib
import json
import os
from datetime import datetime, timezone

import paho.mqtt.publish as mqtt_publish
import redis
from flask import Flask, jsonify, redirect, render_template, request, url_for
from sqlalchemy import text

from .extensions import db
from .models import Device, DeviceConfig, Event, Telemetry, Tenant


def create_app() -> Flask:
    app = Flask(__name__)
    app.config["SQLALCHEMY_DATABASE_URI"] = os.environ.get("DATABASE_URL")
    app.config["SQLALCHEMY_TRACK_MODIFICATIONS"] = False
    db.init_app(app)

    @app.get("/")
    def index():
        return redirect(url_for("dashboard"))

    @app.get("/dashboard")
    def dashboard():
        tenants = Tenant.query.order_by(Tenant.name).all()
        devices = Device.query.order_by(Device.created_at.desc()).all()
        recent_events = Event.query.order_by(Event.started_at.desc()).limit(10).all()
        recent_telemetry = Telemetry.query.order_by(Telemetry.received_at.desc()).limit(10).all()
        return render_template(
            "dashboard.html",
            tenants=tenants,
            devices=devices,
            recent_events=recent_events,
            recent_telemetry=recent_telemetry,
        )

    @app.get("/devices")
    def devices():
        rows = Device.query.order_by(Device.created_at.desc()).all()
        return jsonify([serialize_device(device) for device in rows])

    @app.get("/devices/<device_uid>")
    def device_detail(device_uid):
        device = Device.query.filter_by(device_uid=device_uid).first_or_404()
        recent_telemetry = (
            Telemetry.query.filter_by(device_id=device.id).order_by(Telemetry.received_at.desc()).limit(20).all()
        )
        recent_events = Event.query.filter_by(device_id=device.id).order_by(Event.started_at.desc()).limit(20).all()
        configs = DeviceConfig.query.filter_by(device_id=device.id).order_by(DeviceConfig.version.desc()).limit(5).all()
        return render_template(
            "device_detail.html",
            device=device,
            recent_telemetry=recent_telemetry,
            recent_events=recent_events,
            configs=configs,
        )

    @app.route("/devices/<device_uid>/config", methods=["GET", "POST"])
    def device_config(device_uid):
        device = Device.query.filter_by(device_uid=device_uid).first_or_404()
        latest_config = DeviceConfig.query.filter_by(device_id=device.id).order_by(DeviceConfig.version.desc()).first()

        if request.method == "POST":
            raw_payload = request.form.get("payload", "")
            try:
                payload = json.loads(raw_payload)
            except json.JSONDecodeError as exc:
                return render_config_form(device, raw_payload, error=f"JSON invalido: {exc}")

            next_version = (latest_config.version + 1) if latest_config else 1
            payload["schema_version"] = payload.get("schema_version", 1)
            payload["device_id"] = device.device_uid
            requested_version = int(payload.get("config_version") or next_version)
            if latest_config and requested_version <= latest_config.version:
                requested_version = next_version
            payload["config_version"] = requested_version

            canonical = json.dumps(payload, sort_keys=True, separators=(",", ":"))
            config_hash = hashlib.sha256(canonical.encode("utf-8")).hexdigest()
            payload["config_hash"] = config_hash

            config = DeviceConfig(
                device_id=device.id,
                version=payload["config_version"],
                status="desired",
                config_hash=config_hash,
                desired_payload=payload,
            )
            db.session.add(config)
            device.desired_config_version = payload["config_version"]
            db.session.commit()

            publish_config_desired(device, payload)
            return redirect(url_for("device_detail", device_uid=device.device_uid))

        payload = latest_config.desired_payload if latest_config else default_device_config(device)
        return render_config_form(device, json.dumps(payload, indent=2, ensure_ascii=False))

    @app.get("/health")
    def health():
        return jsonify(
            {
                "ok": True,
                "service": "callonfail-backend",
                "time": datetime.now(timezone.utc).isoformat(),
            }
        )

    @app.get("/api/status")
    def status():
        return jsonify(
            {
                "ok": True,
                "database": check_database(),
                "redis": check_redis(),
                "time": datetime.now(timezone.utc).isoformat(),
            }
        )

    return app


def check_database() -> dict:
    try:
        db.session.execute(text("select 1"))
        return {"ok": True}
    except Exception as exc:  # pragma: no cover - visible in health endpoint
        return {"ok": False, "error": str(exc)}


def check_redis() -> dict:
    redis_url = os.environ.get("REDIS_URL")
    if not redis_url:
        return {"ok": False, "error": "REDIS_URL is not configured"}

    try:
        client = redis.Redis.from_url(redis_url, socket_connect_timeout=3)
        client.ping()
        return {"ok": True}
    except Exception as exc:  # pragma: no cover - visible in health endpoint
        return {"ok": False, "error": str(exc)}


def publish_config_desired(device: Device, payload: dict):
    topic = f"devices/{device.device_uid}/config/desired"
    mqtt_host = os.environ.get("MQTT_HOST", "mosquitto")
    mqtt_port = int(os.environ.get("MQTT_PORT", "1883"))
    auth = None
    username = os.environ.get("MQTT_USERNAME", "")
    if username:
        auth = {"username": username, "password": os.environ.get("MQTT_PASSWORD", "")}

    mqtt_publish.single(
        topic,
        payload=json.dumps(payload, separators=(",", ":")),
        qos=1,
        retain=True,
        hostname=mqtt_host,
        port=mqtt_port,
        auth=auth,
    )


def default_device_config(device: Device) -> dict:
    return {
        "schema_version": 1,
        "config_version": (device.desired_config_version or 0) + 1,
        "device_id": device.device_uid,
        "telemetry_interval_seconds": 60,
        "sensors": [
            {"id": "temp_1", "name": "DS18B20", "type": "temperature", "enabled": True, "source": "ds18b20"},
            {
                "id": "temp_2",
                "name": "SHT31 temperatura",
                "type": "temperature",
                "enabled": True,
                "source": "sht31_temperature",
            },
            {"id": "humidity_1", "name": "SHT31 humedad", "type": "humidity", "enabled": True, "source": "sht31_humidity"},
            {"id": "mains_1", "name": "Red electrica", "type": "mains_voltage", "enabled": True, "source": "zmpt101b"},
        ],
        "outputs": [
            {"id": "output_1", "name": "Salida 1", "type": "relay", "enabled": True},
            {"id": "output_2", "name": "Salida 2", "type": "relay", "enabled": True},
        ],
        "audio": [
            {
                "id": "test_call",
                "url": "https://raw.githubusercontent.com/nmarcovecchio/cof/main/actual_version/audio/cof_test.wav",
                "sha256": "",
                "modem_path": "C:/cof_test.wav",
                "format": "wav_pcm_8000_mono_16bit",
            }
        ],
        "rules": [],
        "flows": [],
    }


def serialize_device(device: Device) -> dict:
    return {
        "device_uid": device.device_uid,
        "name": device.name,
        "tenant": device.tenant.name if device.tenant else None,
        "site": device.site.name if device.site else None,
        "status": device.status,
        "firmware_version": device.firmware_version,
        "last_seen_at": device.last_seen_at.isoformat() if device.last_seen_at else None,
        "desired_config_version": device.desired_config_version,
        "reported_config_version": device.reported_config_version,
    }


def render_config_form(device, payload: str, error: str | None = None) -> str:
    return render_template("config_form.html", device=device, payload=payload, error=error)


app = create_app()
