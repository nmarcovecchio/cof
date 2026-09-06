import hashlib
import hmac
import json
import os
import re
import secrets
import uuid
from datetime import datetime, timezone
from functools import wraps
from pathlib import Path
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

import redis
from flask import Flask, abort, flash, jsonify, redirect, render_template, request, session, url_for
from markupsafe import Markup
from sqlalchemy import text
from sqlalchemy.exc import IntegrityError
from werkzeug.middleware.proxy_fix import ProxyFix

from .alarm_log import friendly_step
from .alarms import dispatch_alarm, latest_config_payload, resolve_contacts
from .modem_queue import active_modem_jobs, enqueue_modem_job, pump_modem_queue
from .extensions import db
from .models import Device, DeviceConfig, Event, Site, Telemetry, Tenant
from .mqtt_util import publish_mqtt, publish_mqtt_raw
from .notify import send_email, send_telegram
from .phones import (
    is_e164_phone,
    join_values,
    normalize_phone,
    parse_emails,
    parse_phones,
    parse_telegram_chats,
)
from .tts import MAX_TEXT_CHARS


def login_required(view):
    @wraps(view)
    def wrapped_view(**kwargs):
        if not session.get("authenticated"):
            return redirect(url_for("login", next=request.path))
        return view(**kwargs)

    return wrapped_view


def wants_json() -> bool:
    return request.args.get("format") == "json" or request.accept_mimetypes.best == "application/json"


def cellular_signal(csq):
    try:
        value = int(csq)
    except (TypeError, ValueError):
        return {"tone": "secondary", "label": "Sin dato", "bars": 0, "csq": None}
    if value < 0 or value == 99:
        return {"tone": "secondary", "label": "Sin dato", "bars": 0, "csq": value}
    if value >= 20:
        return {"tone": "success", "label": "Excelente", "bars": 4, "csq": value}
    if value >= 15:
        return {"tone": "success", "label": "Buena", "bars": 3, "csq": value}
    if value >= 10:
        return {"tone": "warning", "label": "Aceptable", "bars": 2, "csq": value}
    return {"tone": "danger", "label": "Debil", "bars": 1, "csq": value}


def parse_utc(value):
    if value is None or value == "":
        return None
    if isinstance(value, datetime):
        dt = value
    elif isinstance(value, str):
        try:
            dt = datetime.fromisoformat(value.replace("Z", "+00:00"))
        except ValueError:
            return None
    else:
        return None
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return dt.astimezone(timezone.utc)


def is_fresh(value, max_age_seconds: int = 180) -> bool:
    dt = parse_utc(value)
    if dt is None:
        return False
    return (datetime.now(timezone.utc) - dt).total_seconds() < max_age_seconds


def device_is_live(device) -> bool:
    if device is None or getattr(device, "archived_at", None) is not None:
        return False
    return is_fresh(getattr(device, "last_seen_at", None), 180)


def cellular_is_current(cell, device=None) -> bool:
    if not device_is_live(device):
        return False
    if not isinstance(cell, dict) or not cell:
        return False
    received = cell.get("received_at")
    if received:
        return is_fresh(received, 12 * 60)
    return True


def cellular_lte_ok(cell):
    if not isinstance(cell, dict):
        return False
    radio = str(cell.get("radio") or "").upper()
    return "LTE" in radio and bool(cell.get("registered")) and cell.get("cereg") in (1, 5, 9, 10)


def event_display_severity(event_type, message, severity="info"):
    text = (message or "").strip()
    kind = (event_type or "").strip()
    if kind == "test_call" and text.startswith("Call done"):
        return "info"
    if kind == "test_sms" and text.startswith("SMS sent"):
        return "info"
    return severity or "info"


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
    app.wsgi_app = ProxyFix(app.wsgi_app, x_for=1, x_proto=1, x_host=1)
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
        if value is None or value == "":
            return "-"

        if isinstance(value, str):
            try:
                value = datetime.fromisoformat(value.replace("Z", "+00:00"))
            except ValueError:
                return value

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

    @app.template_filter("event_severity")
    def event_severity(event):
        return event_display_severity(
            getattr(event, "type", ""),
            getattr(event, "message", ""),
            getattr(event, "severity", "info"),
        )

    @app.template_filter("cellular_signal")
    def cellular_signal_filter(csq):
        return cellular_signal(csq)

    @app.template_filter("cellular_lte_ok")
    def cellular_lte_ok_filter(cell):
        return cellular_lte_ok(cell)

    @app.template_filter("device_live")
    def device_live_filter(device):
        return device_is_live(device)

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

    @app.get("/alarms")
    @login_required
    def alarms():
        device_uid = (request.args.get("device") or "").strip()
        query = Event.query.filter_by(type="alarm").order_by(Event.started_at.desc())
        device = None
        if device_uid:
            device = Device.query.filter_by(device_uid=device_uid).first_or_404()
            query = query.filter_by(device_id=device.id)
        rows = query.limit(80).all()
        return render_template("alarms.html", alarms=[alarm_view(event) for event in rows], device=device)

    @app.get("/alarms/<int:event_id>")
    @login_required
    def alarm_detail(event_id):
        event = Event.query.filter_by(id=event_id, type="alarm").first_or_404()
        return render_template("alarm_detail.html", alarm=alarm_view(event))

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
            tenant = Tenant()
            error = apply_tenant_form(tenant)
            if error is None:
                db.session.add(tenant)
                try:
                    db.session.commit()
                    flash("Cliente creado", "success")
                    return redirect(url_for("tenant_edit", tenant_id=tenant.id))
                except IntegrityError:
                    db.session.rollback()
                    error = "Ya existe un cliente con ese slug"
        return render_template("tenant_form.html", tenant=None, error=error)

    @app.route("/tenants/<int:tenant_id>/edit", methods=["GET", "POST"])
    @login_required
    def tenant_edit(tenant_id):
        tenant = Tenant.query.get_or_404(tenant_id)
        error = None
        if request.method == "POST":
            error = apply_tenant_form(tenant)
            if error is None:
                try:
                    db.session.commit()
                    flash("Cliente actualizado", "success")
                    return redirect(url_for("tenant_edit", tenant_id=tenant.id))
                except IntegrityError:
                    db.session.rollback()
                    error = "Ya existe un cliente con ese slug"
        return render_template("tenant_form.html", tenant=tenant, error=error)

    @app.post("/tenants/<int:tenant_id>/test-email")
    @login_required
    def tenant_test_email(tenant_id):
        tenant = Tenant.query.get_or_404(tenant_id)
        emails, _ = parse_emails(tenant.notify_email or "")
        if not emails:
            flash("Configura un email valido en el cliente", "danger")
            return redirect(url_for("tenant_edit", tenant_id=tenant.id))
        try:
            send_email(
                emails,
                "[CallOnFail] Prueba de email",
                f"Prueba de alerta para {tenant.name}.\nSi recibis esto, SMTP esta bien.",
            )
            flash("Email de prueba enviado a " + ", ".join(emails), "success")
        except Exception as exc:
            flash(f"No se pudo enviar el email: {exc}", "danger")
        return redirect(url_for("tenant_edit", tenant_id=tenant.id))

    @app.post("/tenants/<int:tenant_id>/test-telegram")
    @login_required
    def tenant_test_telegram(tenant_id):
        tenant = Tenant.query.get_or_404(tenant_id)
        chats, _ = parse_telegram_chats(tenant.telegram_chat_id or "")
        if not chats:
            flash("Configura el chat ID de Telegram del cliente", "danger")
            return redirect(url_for("tenant_edit", tenant_id=tenant.id))
        try:
            send_telegram(chats, f"CallOnFail prueba de Telegram para {tenant.name}.")
            flash("Mensaje de prueba enviado a Telegram", "success")
        except Exception as exc:
            flash(f"No se pudo enviar a Telegram: {exc}", "danger")
        return redirect(url_for("tenant_edit", tenant_id=tenant.id))

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
        recent_events = Event.query.filter_by(device_id=device.id).order_by(Event.started_at.desc()).limit(15).all()
        configs = DeviceConfig.query.filter_by(device_id=device.id).order_by(DeviceConfig.version.desc()).limit(5).all()
        modem_trace_event = next(
            (
                event
                for event in recent_events
                if isinstance(event.payload, dict) and event.payload.get("modem_log")
            ),
            None,
        )
        return render_template(
            "device_detail.html",
            device=device,
            recent_telemetry=recent_telemetry,
            recent_events=recent_events,
            configs=configs,
            test_phone=last_used_test_phone(device, configs),
            modem_trace_event=modem_trace_event,
            contacts=resolve_contacts(device, latest_config_payload(device)),
            modem_jobs=active_modem_jobs(device),
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

    @app.get("/ota/firmware.bin")
    def ota_firmware_bin():
        path = Path(os.environ.get("OTA_DIR", "/opt/cof-ota")) / "firmware.bin"
        if not path.is_file():
            abort(404)
        data = path.read_bytes()
        resp = app.response_class(data, mimetype="application/octet-stream")
        resp.headers["Content-Length"] = str(len(data))
        resp.headers["Cache-Control"] = "no-store"
        return resp

    @app.post("/devices/<device_uid>/commands/status-report")
    @login_required
    def device_command_status_report(device_uid):
        return send_device_command(device_uid, "status_report", "Status report command sent")

    @app.post("/devices/<device_uid>/commands/test-call")
    @login_required
    def device_command_test_call(device_uid):
        phone = normalize_phone(request.form.get("phone", ""))
        text = (request.form.get("text") or "").strip() or "CallOnFail prueba de llamada"
        if len(text) > MAX_TEXT_CHARS:
            text = text[:MAX_TEXT_CHARS]
        if not is_e164_phone(phone):
            return send_device_command(
                device_uid,
                "test_call",
                "Test call rejected: invalid phone",
                extra={"phone": phone, "text": text},
                publish=False,
            )
        return send_device_command(
            device_uid,
            "test_call",
            f"Test call queued to {phone}",
            extra={"phone": phone, "text": text},
        )

    @app.get("/audio/tmp/<audio_id>.amr")
    def tts_audio_amr(audio_id):
        return _serve_tts_audio(audio_id, ".amr", "audio/amr")

    @app.get("/audio/tmp/<audio_id>.wav")
    def tts_audio_wav(audio_id):
        return _serve_tts_audio(audio_id, ".wav", "audio/wav")

    def _serve_tts_audio(audio_id, suffix, mimetype):
        if not re.fullmatch(r"[a-f0-9]{32}", audio_id or ""):
            abort(404)
        from pathlib import Path

        audio_path = Path(os.environ.get("TTS_DIR", "/tmp/cof-tts")) / f"{audio_id}{suffix}"
        if not audio_path.is_file():
            abort(404)
        data = audio_path.read_bytes()
        resp = app.response_class(data, mimetype=mimetype)
        resp.headers["Content-Length"] = str(len(data))
        resp.headers["Cache-Control"] = "no-store"
        resp.headers["Content-Encoding"] = "identity"
        return resp

    @app.post("/devices/<device_uid>/alarms/trigger")
    @login_required
    def device_trigger_alarm(device_uid):
        device = Device.query.filter_by(device_uid=device_uid).first_or_404()
        if device.archived_at is not None:
            return redirect(url_for("device_detail", device_uid=device.device_uid))
        event = dispatch_alarm(
            device,
            source="manual",
            title="Alarma de prueba",
            detail="Disparada desde la web",
        )
        db.session.commit()
        results = (event.payload or {}).get("results") or {}
        sent = [name for name, value in results.items() if str(value).startswith("sent") or str(value).startswith("queued")]
        if sent:
            flash("Alarma disparada: " + ", ".join(f"{name}={results[name]}" for name in sent), "success")
        else:
            flash("Alarma registrada. Revisa eventos: " + ", ".join(f"{k}={v}" for k, v in results.items()), "warning")
        return redirect(url_for("device_detail", device_uid=device.device_uid))

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
            f"Test SMS queued to {phone}",
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
            phone = normalize_phone(str(extra.get("phone") or ""))
            if is_e164_phone(phone):
                session["test_phone"] = phone
        event_type = "command_sent"
        severity = "info"
        if publish and command in {"test_call", "test_sms"}:
            job = enqueue_modem_job(device, command, extra or {}, source="test")
            started = pump_modem_queue(device)
            payload["command_id"] = job.command_id
            payload["queue_status"] = job.status
            if started is not None and started.id == job.id:
                message = message.replace("queued", "sent")
                payload["queue_status"] = started.status
            elif job.status == "queued":
                message = message if "queued" in message else message.replace("sent", "queued")
            elif job.status == "failed":
                event_type = "command_failed"
                severity = "warning"
                message = f"{command} failed: {job.result}"
        elif publish:
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


def apply_tenant_form(tenant: Tenant) -> str | None:
    name = request.form.get("name", "").strip()
    slug = request.form.get("slug", "").strip() or slugify(name)
    emails, bad_emails = parse_emails(request.form.get("notify_email", ""))
    chats, bad_chats = parse_telegram_chats(request.form.get("telegram_chat_id", ""))
    phones, bad_phones = parse_phones(request.form.get("phone", ""))

    if not name or not slug:
        return "Nombre y slug son requeridos"
    if bad_emails:
        return "Email invalido: " + ", ".join(bad_emails)
    if bad_chats:
        return "Telegram chat ID invalido: " + ", ".join(bad_chats)
    if bad_phones:
        return "Telefono invalido. Usa +54911... uno por linea. Error: " + ", ".join(bad_phones)

    tenant.name = name
    tenant.slug = slug
    tenant.notify_email = join_values(emails) or None
    tenant.telegram_chat_id = join_values(chats) or None
    tenant.phone = join_values(phones) or None
    return None


def _step_tone(step: dict) -> str:
    channel = step.get("channel") or ""
    status = step.get("status") or ""
    detail = step.get("detail") or ""
    if channel == "clear":
        return "clear" if not status.startswith("error") else "bad"
    if status == "answered" or detail.startswith("Call done") or detail.startswith("SMS sent"):
        return "ok"
    if status == "sent" and channel != "call":
        return "ok"
    if status in {"queued"} or (status == "sent" and channel == "call"):
        return "wait"
    if status in {"no_answer", "error", "failed", "skipped", "exhausted"}:
        return "bad"
    return "wait"


def alarm_view(event: Event) -> dict:
    payload = event.payload or {}
    steps = []
    for step in payload.get("steps") or []:
        steps.append({**step, "label": friendly_step(step), "tone": _step_tone(step)})
    return {
        "event": event,
        "open": event.cleared_at is None and (payload.get("source") != "manual"),
        "title": payload.get("title") or "Alarma",
        "detail": payload.get("detail") or event.message or "",
        "steps": steps,
        "last": steps[-1]["label"] if steps else (event.message or "Sin actividad"),
        "escalate": bool(payload.get("escalate_calls", True)),
        "clear_actions": payload.get("clear_actions") or [],
    }


def publish_config_desired(device: Device, payload: dict):
    publish_mqtt(f"devices/{device.device_uid}/config/desired", payload, qos=1, retain=True)


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


def last_used_test_phone(device, configs) -> str:
    tenant_phones, _ = parse_phones(getattr(getattr(device, "tenant", None), "phone", "") or "")
    if tenant_phones:
        return tenant_phones[0]
    session_phone = normalize_phone(str(session.get("test_phone") or ""))
    if is_e164_phone(session_phone):
        return session_phone
    recent = (
        Event.query.filter_by(device_id=device.id, type="command_sent")
        .order_by(Event.started_at.desc())
        .limit(20)
        .all()
    )
    for event in recent:
        payload = event.payload or {}
        phone = normalize_phone(str(payload.get("phone") or ""))
        if is_e164_phone(phone):
            return phone
    return first_contact_phone(configs)


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
