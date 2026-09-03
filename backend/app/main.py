import hashlib
import hmac
import json
import os
import re
import secrets
import uuid
from datetime import datetime, timezone
from functools import wraps
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

import paho.mqtt.publish as mqtt_publish
import redis
from flask import Flask, abort, jsonify, redirect, render_template, request, session, url_for
from markupsafe import Markup
from sqlalchemy import text
from sqlalchemy.exc import IntegrityError

from .extensions import db
from .models import Device, DeviceConfig, Event, Site, Telemetry, Tenant


def login_required(view):
    @wraps(view)
    def wrapped_view(**kwargs):
        if not session.get("authenticated"):
            return redirect(url_for("login", next=request.path))
        return view(**kwargs)

    return wrapped_view


def wants_json() -> bool:
    return request.args.get("format") == "json" or request.accept_mimetypes.best == "application/json"


def slugify(value: str) -> str:
    slug = "".join(ch.lower() if ch.isalnum() else "-" for ch in value.strip())
    return "-".join(part for part in slug.split("-") if part)


def safe_next_url(value: str | None) -> str | None:
    if not value or not value.startswith("/") or value.startswith("//"):
        return None
    return value


def get_csrf_token() -> str:
    token = session.get("_csrf_token")
    if not token:
        token = secrets.token_urlsafe(32)
        session["_csrf_token"] = token
    return token


def create_app() -> Flask:
    app = Flask(__name__)
    app.config["SQLALCHEMY_DATABASE_URI"] = os.environ.get("DATABASE_URL")
    app.config["SQLALCHEMY_TRACK_MODIFICATIONS"] = False
    app.config["APP_TIMEZONE"] = os.environ.get("APP_TIMEZONE", "America/Argentina/Buenos_Aires")
    app.config["SECRET_KEY"] = os.environ.get("SECRET_KEY", "change-this-secret-key")
    app.config["SESSION_COOKIE_HTTPONLY"] = True
    app.config["SESSION_COOKIE_SAMESITE"] = "Lax"
    app.config["SESSION_COOKIE_SECURE"] = os.environ.get("SESSION_COOKIE_SECURE", "false").lower() == "true"
    db.init_app(app)

    @app.template_filter("datetime_local")
    def datetime_local(value):
        if value is None:
            return "-"

        if value.tzinfo is None:
            value = value.replace(tzinfo=timezone.utc)

        try:
            target_tz = ZoneInfo(app.config["APP_TIMEZONE"])
        except ZoneInfoNotFoundError:
            target_tz = ZoneInfo("UTC")

        return value.astimezone(target_tz).strftime("%Y-%m-%d %H:%M:%S %Z")

    @app.template_filter("json_pretty")
    def json_pretty(value):
        return json.dumps(value or {}, indent=2, ensure_ascii=False, sort_keys=True)

    @app.before_request
    def csrf_protect():
        if request.method != "POST":
            return

        expected = session.get("_csrf_token")
        submitted = request.form.get("_csrf_token", "")
        if not expected or not hmac.compare_digest(expected, submitted):
            abort(400)

    @app.context_processor
    def inject_csrf():
        def csrf_field():
            return Markup(f'<input type="hidden" name="_csrf_token" value="{get_csrf_token()}">')

        return {"csrf_field": csrf_field}

    @app.get("/")
    def index():
        return redirect(url_for("dashboard"))

    @app.route("/login", methods=["GET", "POST"])
    def login():
        error = None
        if request.method == "POST":
            username = request.form.get("username", "")
            password = request.form.get("password", "")
            expected_username = os.environ.get("ADMIN_USERNAME", "admin")
            expected_password = os.environ.get("ADMIN_PASSWORD", "change-me")

            if hmac.compare_digest(username, expected_username) and hmac.compare_digest(password, expected_password):
                session["authenticated"] = True
                session["username"] = username
                return redirect(safe_next_url(request.args.get("next")) or url_for("dashboard"))

            error = "Usuario o password invalidos"

        return render_template("login.html", error=error)

    @app.post("/logout")
    def logout():
        session.clear()
        return redirect(url_for("login"))

    @app.get("/dashboard")
    @login_required
    def dashboard():
        tenants = Tenant.query.order_by(Tenant.name).all()
        devices = Device.query.filter(Device.archived_at.is_(None)).order_by(Device.created_at.desc()).all()
        recent_events = Event.query.order_by(Event.started_at.desc()).limit(10).all()
        recent_telemetry = Telemetry.query.order_by(Telemetry.received_at.desc()).limit(10).all()
        return render_template(
            "dashboard.html",
            tenants=tenants,
            devices=devices,
            recent_events=recent_events,
            recent_telemetry=recent_telemetry,
        )

    @app.route("/tenants")
    @login_required
    def tenants():
        rows = Tenant.query.order_by(Tenant.name).all()
        return render_template("tenants.html", tenants=rows)

    @app.route("/tenants/new", methods=["GET", "POST"])
    @login_required
    def tenant_new():
        error = None
        if request.method == "POST":
            name = request.form.get("name", "").strip()
            slug = request.form.get("slug", "").strip() or slugify(name)
            if not name or not slug:
                error = "Nombre y slug son requeridos"
            else:
                db.session.add(Tenant(name=name, slug=slug))
                try:
                    db.session.commit()
                    return redirect(url_for("tenants"))
                except IntegrityError:
                    db.session.rollback()
                    error = "Ya existe un cliente con ese slug"
        return render_template("tenant_form.html", error=error)

    @app.post("/tenants/<int:tenant_id>/delete")
    @login_required
    def tenant_delete(tenant_id):
        tenant = Tenant.query.get_or_404(tenant_id)
        db.session.delete(tenant)
        db.session.commit()
        return redirect(url_for("tenants"))

    @app.route("/sites/new", methods=["GET", "POST"])
    @login_required
    def site_new():
        tenants_rows = Tenant.query.order_by(Tenant.name).all()
        error = None
        if request.method == "POST":
            name = request.form.get("name", "").strip()
            tenant_id = request.form.get("tenant_id", type=int)
            if not name or not tenant_id:
                error = "Cliente y nombre de sitio son requeridos"
            else:
                db.session.add(Site(name=name, tenant_id=tenant_id))
                try:
                    db.session.commit()
                    return redirect(url_for("tenants"))
                except IntegrityError:
                    db.session.rollback()
                    error = "Ya existe un sitio con ese nombre para el cliente"
        return render_template("site_form.html", tenants=tenants_rows, error=error)

    @app.post("/sites/<int:site_id>/delete")
    @login_required
    def site_delete(site_id):
        site = Site.query.get_or_404(site_id)
        Device.query.filter_by(site_id=site.id).update({"site_id": None})
        db.session.delete(site)
        db.session.commit()
        return redirect(url_for("tenants"))

    @app.get("/devices")
    @login_required
    def devices():
        include_archived = request.args.get("include_archived") == "1"
        query = Device.query
        if include_archived:
            query = query.filter(Device.archived_at.isnot(None))
        else:
            query = query.filter(Device.archived_at.is_(None))
        rows = query.order_by(Device.created_at.desc()).all()
        if wants_json():
            return jsonify([serialize_device(device) for device in rows])
        return render_template("devices.html", devices=rows, include_archived=include_archived)

    @app.route("/devices/new", methods=["GET", "POST"])
    @login_required
    def device_new():
        tenants_rows = Tenant.query.order_by(Tenant.name).all()
        sites = Site.query.order_by(Site.name).all()
        error = None

        if request.method == "POST":
            device_uid = request.form.get("device_uid", "").strip()
            name = request.form.get("name", "").strip()
            tenant_id = request.form.get("tenant_id", type=int)
            site_id = request.form.get("site_id", type=int)
            site_id = site_id or None

            if not device_uid or not name or not tenant_id:
                error = "Device ID, nombre y cliente son requeridos"
            else:
                if site_id is not None:
                    site = Site.query.get(site_id)
                    if site is None or site.tenant_id != tenant_id:
                        error = "El sitio seleccionado no pertenece al cliente"

            if error is None:
                db.session.add(Device(device_uid=device_uid, name=name, tenant_id=tenant_id, site_id=site_id, status="new"))
                try:
                    db.session.commit()
                    return redirect(url_for("device_detail", device_uid=device_uid))
                except IntegrityError:
                    db.session.rollback()
                    error = "Ya existe un dispositivo con ese Device ID"

        return render_template("device_form.html", tenants=tenants_rows, sites=sites, error=error)

    @app.route("/devices/<device_uid>/edit", methods=["GET", "POST"])
    @login_required
    def device_edit(device_uid):
        device = Device.query.filter_by(device_uid=device_uid).first_or_404()
        tenants_rows = Tenant.query.order_by(Tenant.name).all()
        sites = Site.query.order_by(Site.name).all()
        error = None

        if request.method == "POST":
            name = request.form.get("name", "").strip()
            tenant_id = request.form.get("tenant_id", type=int)
            site_id = request.form.get("site_id", type=int) or None
            restore = request.form.get("restore") == "1"

            if not name or not tenant_id:
                error = "Nombre y cliente son requeridos"
            else:
                if site_id is not None:
                    site = Site.query.get(site_id)
                    if site is None or site.tenant_id != tenant_id:
                        error = "El sitio seleccionado no pertenece al cliente"

            if error is None:
                device.name = name
                device.tenant_id = tenant_id
                device.site_id = site_id
                if restore:
                    device.archived_at = None
                    if device.status == "archived":
                        device.status = "new"
                db.session.commit()
                return redirect(url_for("device_detail", device_uid=device.device_uid))

        return render_template("device_edit.html", device=device, tenants=tenants_rows, sites=sites, error=error)

    @app.post("/devices/<device_uid>/delete")
    @login_required
    def device_delete(device_uid):
        device = Device.query.filter_by(device_uid=device_uid).first_or_404()
        device.archived_at = datetime.now(timezone.utc)
        device.status = "archived"
        try:
            publish_mqtt_raw(f"devices/{device.device_uid}/config/desired", payload="", qos=1, retain=True)
        except Exception as exc:
            db.session.add(
                Event(
                    device_id=device.id,
                    type="mqtt_retained_clear_failed",
                    severity="warning",
                    message="Failed to clear retained config/desired",
                    payload={"error": str(exc)},
                )
            )
        db.session.commit()
        return redirect(url_for("devices"))

    @app.post("/devices/<device_uid>/restore")
    @login_required
    def device_restore(device_uid):
        device = Device.query.filter_by(device_uid=device_uid).first_or_404()
        device.archived_at = None
        if device.status == "archived":
            device.status = "new"
        db.session.commit()
        return redirect(url_for("device_detail", device_uid=device.device_uid))

    @app.get("/devices/<device_uid>")
    @login_required
    def device_detail(device_uid):
        device = Device.query.filter_by(device_uid=device_uid).first_or_404()
        recent_telemetry = (
            Telemetry.query.filter_by(device_id=device.id).order_by(Telemetry.received_at.desc()).limit(20).all()
        )
        recent_events = Event.query.filter_by(device_id=device.id).order_by(Event.started_at.desc()).limit(40).all()
        configs = DeviceConfig.query.filter_by(device_id=device.id).order_by(DeviceConfig.version.desc()).limit(5).all()
        return render_template(
            "device_detail.html",
            device=device,
            recent_telemetry=recent_telemetry,
            recent_events=recent_events,
            configs=configs,
            test_phone=first_contact_phone(configs),
        )

    @app.route("/devices/<device_uid>/config", methods=["GET", "POST"])
    @login_required
    def device_config(device_uid):
        device = Device.query.filter_by(device_uid=device_uid).first_or_404()
        if device.archived_at is not None:
            return redirect(url_for("device_detail", device_uid=device.device_uid))
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
            try:
                requested_version = int(payload.get("config_version") or next_version)
            except (TypeError, ValueError):
                return render_config_form(device, raw_payload, error="config_version debe ser numerico")
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

            try:
                publish_config_desired(device, payload)
            except Exception as exc:
                config.status = "publish_failed"
                db.session.commit()
                return render_config_form(device, raw_payload, error=f"Config guardada pero no publicada por MQTT: {exc}")

            return redirect(url_for("device_detail", device_uid=device.device_uid))

        payload = latest_config.desired_payload if latest_config else default_device_config(device)
        return render_config_form(device, json.dumps(payload, indent=2, ensure_ascii=False))

    @app.post("/devices/<device_uid>/commands/ota-check")
    @login_required
    def device_command_ota_check(device_uid):
        return send_device_command(device_uid, "ota_check", "OTA check command sent")

    @app.post("/devices/<device_uid>/commands/status-report")
    @login_required
    def device_command_status_report(device_uid):
        return send_device_command(device_uid, "status_report", "Status report command sent")

    @app.post("/devices/<device_uid>/commands/test-call")
    @login_required
    def device_command_test_call(device_uid):
        phone = normalize_phone(request.form.get("phone", ""))
        if not is_e164_phone(phone):
            return send_device_command(
                device_uid,
                "test_call",
                "Test call rejected: invalid phone",
                extra={"phone": phone},
                publish=False,
            )
        return send_device_command(
            device_uid,
            "test_call",
            f"Test call command sent to {phone}",
            extra={"phone": phone},
        )

    @app.post("/devices/<device_uid>/commands/test-sms")
    @login_required
    def device_command_test_sms(device_uid):
        phone = normalize_phone(request.form.get("phone", ""))
        text = (request.form.get("text") or "").strip() or "CallOnFail prueba SMS"
        if len(text) > 160:
            text = text[:160]
        if not is_e164_phone(phone):
            return send_device_command(
                device_uid,
                "test_sms",
                "Test SMS rejected: invalid phone",
                extra={"phone": phone, "text": text},
                publish=False,
            )
        return send_device_command(
            device_uid,
            "test_sms",
            f"Test SMS command sent to {phone}",
            extra={"phone": phone, "text": text},
        )

    def send_device_command(device_uid, command, message, extra=None, publish=True):
        device = Device.query.filter_by(device_uid=device_uid).first_or_404()
        if device.archived_at is not None:
            return redirect(url_for("device_detail", device_uid=device.device_uid))
        command_id = str(uuid.uuid4())
        payload = {
            "command_id": command_id,
            "command": command,
            "device_id": device.device_uid,
            "created_at": datetime.now(timezone.utc).isoformat(),
        }
        if extra:
            payload.update(extra)
        event_type = "command_sent"
        severity = "info"
        if publish:
            try:
                publish_mqtt(f"devices/{device.device_uid}/command", payload, qos=1, retain=False)
            except Exception as exc:
                payload["publish_error"] = str(exc)
                event_type = "command_failed"
                severity = "warning"
        else:
            event_type = "command_failed"
            severity = "warning"
        db.session.add(
            Event(
                device_id=device.id,
                type=event_type,
                severity=severity,
                message=message,
                payload=payload,
            )
        )
        db.session.commit()
        return redirect(url_for("device_detail", device_uid=device.device_uid))

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
    publish_mqtt(f"devices/{device.device_uid}/config/desired", payload, qos=1, retain=True)


def publish_mqtt(topic: str, payload: dict, qos: int = 1, retain: bool = False):
    publish_mqtt_raw(topic, json.dumps(payload, separators=(",", ":")), qos=qos, retain=retain)


def publish_mqtt_raw(topic: str, payload: str, qos: int = 1, retain: bool = False):
    mqtt_host = os.environ.get("MQTT_HOST", "mosquitto")
    mqtt_port = int(os.environ.get("MQTT_PORT", "1883"))
    auth = None
    username = os.environ.get("MQTT_USERNAME", "")
    if username:
        auth = {"username": username, "password": os.environ.get("MQTT_PASSWORD", "")}

    mqtt_publish.single(
        topic,
        payload=payload,
        qos=qos,
        retain=retain,
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
        "calling": {
            "enabled": False,
            "max_attempts_per_alarm": 0,
            "notes": "Enable only for customers that want phone calls.",
        },
        "audio": [
            {
                "id": "test_call",
                "enabled": False,
                "description": "Audio de prueba. El archivo debe validarse con una llamada real.",
                "url": "https://raw.githubusercontent.com/nmarcovecchio/cof/main/actual_version/audio/cof_test.wav",
                "sha256": "",
                "modem_path": "C:/cof_test.wav",
                "format": "wav_pcm_8000_mono_16bit",
            }
        ],
        "notifications": {
            "email": "",
            "telegram_chat_id": "",
        },
        "rules": [],
        "flows": [],
    }


def normalize_phone(phone: str) -> str:
    return "".join(ch for ch in (phone or "").strip() if ch.isdigit() or ch == "+")


def is_e164_phone(phone: str) -> bool:
    return bool(re.fullmatch(r"\+[1-9]\d{7,14}", phone or ""))


def first_contact_phone(configs) -> str:
    if not configs:
        return ""
    payload = configs[0].desired_payload or {}
    contacts = payload.get("contacts") or []
    if contacts and isinstance(contacts[0], dict):
        phone = normalize_phone(str(contacts[0].get("phone") or ""))
        if is_e164_phone(phone):
            return phone
    calling = payload.get("calling") or {}
    phone = normalize_phone(str(calling.get("phone") or ""))
    return phone if is_e164_phone(phone) else ""


def serialize_device(device: Device) -> dict:
    return {
        "device_uid": device.device_uid,
        "name": device.name,
        "tenant": device.tenant.name if device.tenant else None,
        "site": device.site.name if device.site else None,
        "status": device.status,
        "archived_at": device.archived_at.isoformat() if device.archived_at else None,
        "hardware_profile": device.hardware_profile,
        "capabilities": device.capabilities,
        "discovered": device.discovered,
        "firmware_version": device.firmware_version,
        "last_seen_at": device.last_seen_at.isoformat() if device.last_seen_at else None,
        "desired_config_version": device.desired_config_version,
        "reported_config_version": device.reported_config_version,
    }


def render_config_form(device, payload: str, error: str | None = None) -> str:
    try:
        cfg = json.loads(payload) if isinstance(payload, str) else payload
    except Exception:
        cfg = {}
    return render_template("config_form.html", device=device, payload=payload, cfg=cfg, error=error)


app = create_app()
