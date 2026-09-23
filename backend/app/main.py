import csv
import hashlib
import hmac
import io
import json
import math
import os
import re
import secrets
import uuid
from datetime import datetime, timedelta, timezone
from functools import wraps
from pathlib import Path
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

import redis
from flask import Flask, abort, flash, jsonify, redirect, render_template, request, session, url_for
from markupsafe import Markup
from sqlalchemy.orm.attributes import flag_modified
from sqlalchemy import text
from sqlalchemy.exc import IntegrityError
from werkzeug.middleware.proxy_fix import ProxyFix

from .alarm_ack import ACK_VIA, acknowledge_by_token, acknowledge_device_from_event, lookup_ack_link, open_device_alarms
from .alarm_log import friendly_step
from .alarms import (
    DEVICE_LIVE_SECONDS,
    MAX_CALL_TEXT_CHARS,
    SENSOR_ALIASES,
    build_call_text,
    configured_rules_view,
    dispatch_alarm,
    find_rule,
    fire_rule_alarm,
    latest_config_payload,
    open_alarm_event,
    resolve_contacts,
    rule_key,
    rule_sensor_name,
    spoken_number,
)
from .call_audio import has_retired_placeholder, prepare_call_audio, resolve_static_placeholders
from .contacts import normalize_contact, new_contact_id, sync_legacy_fields, tenant_contacts, tenant_telegram_chats
from .modem_queue import active_modem_jobs, enqueue_modem_job, pump_modem_queue
from .extensions import db
from .models import Device, DeviceConfig, Event, SensorWindow, Site, Telemetry, Tenant
from .mqtt_util import publish_mqtt, publish_mqtt_raw
from .notify import send_email, send_telegram
from .phones import is_e164_phone, join_values, normalize_phone, parse_telegram_chats
from .telemetry_series import (
    AUX_COLUMNS,
    _iso_utc,
    _parse_iso_epoch,
    _to_epoch,
    bucket_rows,
    bucket_seconds,
    extract_aux_values,
    extract_value,
    last_readings,
    series_for_range,
    windows_for_range,
)
from .tts import MAX_TEXT_CHARS

# Telemetry interval bounds, shared by the config form and by the POST handler.
#
# The upper bound is load-bearing. The firmware's publishing-liveness watchdog
# (kMqttSilenceReconnectMs = 90 s in firmware/src/main.cpp) forces a disconnect and
# reconnect after that much silence without a successful publish - and each
# reconnect republishes status + telemetry. With an interval above the watchdog
# the device reconnects every cycle, which is what produced the observed ~20 s
# telemetry cadence. So this cap must stay below that watchdog.
#
# Keep it comfortably below DEVICE_LIVE_SECONDS (600 s) too, or a healthy device
# renders "offline" for part of every cycle: telemetry is what refreshes
# last_seen_at.
TELEMETRY_INTERVAL_MIN_SECONDS = 10
TELEMETRY_INTERVAL_MAX_SECONDS = 300

# Telemetry history bounds. A 60 s cadence is ~1440 rows per device per day, so
# an unbounded range would both melt the query and ship a multi-megabyte chart
# payload to the browser. The window is capped and the chart is bucketed.
TELEMETRY_MAX_RANGE_DAYS = 30
TELEMETRY_CHART_MAX_ROWS = 50000
TELEMETRY_CSV_MAX_ROWS = 200000


def login_required(view):
    @wraps(view)
    def wrapped_view(**kwargs):
        if not session.get("authenticated"):
            return redirect(url_for("login", next=request.path))
        return view(**kwargs)

    return wrapped_view


def wants_json() -> bool:
    return request.args.get("format") == "json" or request.accept_mimetypes.best == "application/json"


def parse_telemetry_range(default_hours: int = 24):
    """Parse the from/to query args for the telemetry chart and CSV export.

    Returns ``(from_dt, to_dt, error)``. Times are naive UTC because
    ``Telemetry.received_at`` is a timestamptz that SQLAlchemy hands back aware.
    A bare date means midnight; an omitted ``to`` means "now".
    """
    def parse(raw):
        if not raw:
            return None
        candidate = raw.strip()
        # A UTC offset arrives as "+00:00" and a query string decodes "+" as a
        # space, so an unencoded timestamp reaches us as " 00:00". Accept that
        # rather than rejecting the range with a confusing format error.
        if candidate.endswith(" 00:00"):
            candidate = candidate[: -len(" 00:00")] + "+00:00"
        # JavaScript's Date.prototype.toISOString() - which is what the chart
        # sends - always emits a trailing "Z" and milliseconds. Python 3.10's
        # fromisoformat accepts neither, so normalise both before parsing:
        # otherwise every range the chart asks for comes back as HTTP 400 and
        # the graph renders empty.
        if candidate.endswith(("Z", "z")):
            candidate = candidate[:-1] + "+00:00"
        try:
            value = datetime.fromisoformat(candidate)
        except ValueError:
            return "invalid"
        if value.tzinfo is not None:
            value = value.astimezone(timezone.utc).replace(tzinfo=None)
        return value

    now = datetime.now(timezone.utc).replace(tzinfo=None)
    from_dt = parse(request.args.get("from"))
    to_dt = parse(request.args.get("to"))
    if from_dt == "invalid" or to_dt == "invalid":
        return None, None, "Formato de fecha invalido. Se espera ISO 8601 (ej. 2026-09-22T10:00)."
    if to_dt is None:
        to_dt = now
    if from_dt is None:
        from_dt = to_dt - timedelta(hours=default_hours)
    if from_dt > to_dt:
        return None, None, "El rango esta invertido: el inicio es posterior al fin."
    if to_dt - from_dt > timedelta(days=TELEMETRY_MAX_RANGE_DAYS):
        return None, None, f"El rango maximo es de {TELEMETRY_MAX_RANGE_DAYS} dias."
    return from_dt, to_dt, None


def telemetry_rows_for(device, from_dt, to_dt, limit):
    """Rows for a device and range, oldest first, with a truncation flag."""
    query = (
        Telemetry.query.filter(
            Telemetry.device_id == device.id,
            Telemetry.received_at >= from_dt,
            Telemetry.received_at <= to_dt,
        )
        .order_by(Telemetry.received_at.asc())
        .limit(limit + 1)
    )
    rows = query.all()
    truncated = len(rows) > limit
    return rows[:limit], truncated


def device_sensor_windows(device):
    """Every sensor window for a device, in effect order."""
    return (
        SensorWindow.query.filter_by(device_id=device.id)
        .order_by(SensorWindow.starts_at.asc(), SensorWindow.id.asc())
        .all()
    )


def active_windows(device):
    return [w for w in device_sensor_windows(device) if w.ends_at is None]


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


def is_fresh(value, max_age_seconds: int = DEVICE_LIVE_SECONDS) -> bool:
    dt = parse_utc(value)
    if dt is None:
        return False
    return (datetime.now(timezone.utc) - dt).total_seconds() < max_age_seconds


def to_config_number(value):
    """Coerce a config value to float, or None when it is not a usable number.

    Mirrors alarms.to_number so that what the form accepts is what the alarm
    engine can actually evaluate. A blank threshold arrives from the template as
    JSON null (parseFloat("") is NaN and JSON.stringify turns NaN into null), and
    to_number(None) is None, which makes condition_holds return None and the rule
    is silently skipped forever - while the UI lists it as configured. Reject it
    at save time instead.
    """
    if isinstance(value, bool):
        return None
    if value is None or isinstance(value, str):
        return None
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    if math.isnan(number) or math.isinf(number):
        return None
    return number


def attach_call_audio(device, payload) -> None:
    """Pre-record the call text of every rule, in place, at save time.

    Runs before the config is stored so a synthesis failure aborts the save with
    a real error instead of leaving a rule whose call would play nothing. The
    result is a ``call_audio`` block per rule carrying the stable URL and the
    ``a_`` modem path the device must use.

    Sensor names are read from ``payload`` (the config being saved), NOT from
    ``rule_sensor_name()``: that one reads the last *persisted* config, so a
    sensor renamed in this very save would still be spoken with its old name.
    """
    # The name lives in the form/JSON being saved; this is the only place the
    # brand-new value is available.
    sensor_names = {
        str(sensor.get("id") or ""): str(sensor.get("name") or "")
        for sensor in payload.get("sensors") or []
        if isinstance(sensor, dict)
    }

    for rule in payload.get("rules") or []:
        if not isinstance(rule, dict):
            continue
        template = str(rule.get("call_text") or "")
        if not template.strip():
            rule.pop("call_audio", None)
            continue
        sensor_id = str(rule.get("sensor_id") or "")
        static = resolve_static_placeholders(
            template,
            {
                "equipo": device.name,
                "sitio": device.site.name if device.site else "",
                "cliente": device.tenant.name if device.tenant else "",
                "sensor": sensor_names.get(sensor_id) or sensor_id,
                "regla": rule.get("description") or sensor_id,
                "umbral": spoken_number(rule.get("threshold")),
            },
        )
        info = prepare_call_audio(static)
        if info is None:
            rule.pop("call_audio", None)
        else:
            rule["call_audio"] = info


def validate_config_payload(payload):
    """Return a Spanish error message for an unsaveable config, else None.

    These are the two ways a config could previously be saved and produce a
    device that never behaves as the operator expects. Both were silent.
    """
    if not isinstance(payload, dict):
        return "La configuracion debe ser un objeto JSON"

    interval = to_config_number(payload.get("telemetry_interval_seconds"))
    if interval is None:
        return "El intervalo de telemetria debe ser numerico"
    if interval < TELEMETRY_INTERVAL_MIN_SECONDS or interval > TELEMETRY_INTERVAL_MAX_SECONDS:
        return (
            f"El intervalo de telemetria debe estar entre {TELEMETRY_INTERVAL_MIN_SECONDS} y "
            f"{TELEMETRY_INTERVAL_MAX_SECONDS} segundos. Con un intervalo mayor el equipo "
            "reconecta en cada ciclo y la telemetria se desordena."
        )

    for index, rule in enumerate(payload.get("rules") or [], start=1):
        if not isinstance(rule, dict):
            continue
        if not rule.get("sensor_id"):
            continue
        if to_config_number(rule.get("threshold")) is None:
            sensor = str(rule.get("sensor_id"))
            return (
                f"La regla {index} (sensor {sensor}) no tiene un valor umbral valido. "
                "Sin umbral la regla nunca se evalua, asi que no se guardo."
            )
        # Reject the retired placeholder instead of silently dropping it. A text
        # that still says {valor} would be spoken without the reading - "El
        # sensor SHT31 detecto , respecto al 40" - and the operator would only
        # find out when the phone rang. Failing the save is the honest option;
        # the form shows this same message next to the field.
        text = str(rule.get("call_text") or "")
        if has_retired_placeholder(text):
            return (
                f"La regla {index} usa {{valor}}, que ya no existe: no se puede "
                "pregrabar y obligaria a sintetizar audio durante la alarma. "
                "Escribi el numero directamente en el texto (por ejemplo "
                "'supero los 40 grados') o usá {umbral}. El valor exacto de cada "
                "disparo llega por SMS y email."
            )
    return None


def device_is_live(device) -> bool:
    if device is None or getattr(device, "archived_at", None) is not None:
        return False
    # An explicit "offline" wins over the timestamp. The firmware publishes it as
    # a retained Last Will, and it is the only authoritative statement that the
    # device went away; trust it rather than waiting for last_seen_at to age out.
    if str(getattr(device, "status", "") or "").strip().lower() == "offline":
        return False
    return is_fresh(getattr(device, "last_seen_at", None))


def cellular_is_current(cell, device=None) -> bool:
    # Threshold must track DEVICE_LIVE_SECONDS: cellular.received_at is stamped
    # only when a status message carries a cellular dict, and status is published
    # at most every kCellularStatusIntervalMs (5 min) in the firmware and not at
    # all when MQTT rides LTE. A shorter window would call fresh data stale.
    if not device_is_live(device):
        return False
    if not isinstance(cell, dict) or not cell:
        return False
    received = cell.get("received_at")
    if received:
        return is_fresh(received, DEVICE_LIVE_SECONDS)
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
        if not isinstance(value, (str, datetime)):
            return "-"

        if isinstance(value, str):
            try:
                value = datetime.fromisoformat(value.replace("Z", "+00:00"))
            except ValueError:
                return value

        try:
            if value.tzinfo is None:
                value = value.replace(tzinfo=timezone.utc)
            try:
                target_tz = ZoneInfo(app.config["APP_TIMEZONE"])
            except ZoneInfoNotFoundError:
                target_tz = ZoneInfo("UTC")
            return value.astimezone(target_tz).strftime("%Y-%m-%d %H:%M:%S %Z")
        except Exception:
            return "-"

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

    @app.template_filter("tenant_contacts")
    def tenant_contacts_filter(tenant):
        return tenant_contacts(tenant)

    @app.template_filter("tenant_telegram")
    def tenant_telegram_filter(tenant):
        return tenant_telegram_chats(tenant)

    @app.before_request
    def csrf_protect():
        if request.method != "POST":
            return
        path = request.path or ""
        if "/ack/" in path or path.startswith("/a/"):
            return

        expected = session.get("_csrf_token")
        submitted = request.form.get("_csrf_token", "")
        if not expected or not hmac.compare_digest(expected, submitted):
            abort(400)

    @app.context_processor
    def inject_csrf():
        def csrf_field():
            return Markup(f'<input type="hidden" name="_csrf_token" value="{get_csrf_token()}">')

        # Also exposed as a value (not just a rendered input) so fetch() calls
        # can send the token in the body, which is the only place csrf_protect
        # looks for it.
        return {"csrf_field": csrf_field, "csrf_token": get_csrf_token}

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
            alarm_states=alarm_states_map(devices),
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

    @app.get("/a/<int:event_id>/<token>")
    @app.get("/alarms/<int:event_id>/ack/<token>")
    def alarm_ack_public(event_id, token):
        event, status = lookup_ack_link(event_id, token)
        if status == "invalid" or event is None:
            return render_template("alarm_ack.html", state="invalid", event=None), 404
        if status == "expired":
            return render_template(
                "alarm_ack.html",
                state="expired",
                event=event,
                device_name=event.device.name if event.device else "",
            )
        return render_template(
            "alarm_ack.html",
            state="confirm",
            event=event,
            open_count=len(open_device_alarms(event)),
            device_name=event.device.name if event.device else "",
        )

    @app.post("/a/<int:event_id>/<token>")
    @app.post("/alarms/<int:event_id>/ack/<token>")
    def alarm_ack_confirm(event_id, token):
        event, status = lookup_ack_link(event_id, token)
        if status == "expired":
            return render_template(
                "alarm_ack.html",
                state="expired",
                event=event,
                device_name=event.device.name if event and event.device else "",
            )
        event = acknowledge_by_token(event_id, token, channel="link", sender="")
        if event is None:
            return render_template("alarm_ack.html", state="invalid", event=None), 404
        db.session.commit()
        return render_template(
            "alarm_ack.html",
            state="done",
            event=event,
            open_count=0,
            device_name=event.device.name if event.device else "",
        )

    @app.post("/alarms/<int:event_id>/silence")
    @login_required
    def alarm_silence(event_id):
        event = Event.query.filter_by(id=event_id, type="alarm").first_or_404()
        changed = acknowledge_device_from_event(event, channel="web", sender=session.get("username") or "web")
        db.session.commit()
        if changed:
            flash(f"Se silenciaron {len(changed)} alarma(s) de este equipo. Revisá el detalle.", "success")
        else:
            flash("No habia alarmas abiertas para silenciar", "warning")
        return redirect(url_for("alarm_detail", event_id=event.id))

    @app.post("/devices/<device_uid>/alarms/silence")
    @login_required
    def device_silence_alarms(device_uid):
        device = Device.query.filter_by(device_uid=device_uid).first_or_404()
        open_events = (
            Event.query.filter_by(device_id=device.id, type="alarm")
            .filter(Event.cleared_at.is_(None))
            .order_by(Event.started_at.desc())
            .all()
        )
        if not open_events:
            flash("No habia alarmas abiertas para silenciar", "warning")
            return redirect(url_for("device_detail", device_uid=device.device_uid))
        changed = acknowledge_device_from_event(open_events[0], channel="web", sender=session.get("username") or "web")
        db.session.commit()
        if changed:
            flash(f"Se silenciaron {len(changed)} alarma(s) de este equipo.", "success")
        else:
            flash("No habia alarmas abiertas para silenciar", "warning")
        return redirect(url_for("device_detail", device_uid=device.device_uid))

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
        return render_template(
            "tenant_form.html",
            tenant=None,
            error=error,
            contacts=[],
            telegram_chat_id="",
        )

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
        return render_template(
            "tenant_form.html",
            tenant=tenant,
            error=error,
            contacts=tenant_contacts(tenant),
            telegram_chat_id=join_values(tenant_telegram_chats(tenant)),
        )

    @app.post("/tenants/<int:tenant_id>/test-email")
    @login_required
    def tenant_test_email(tenant_id):
        tenant = Tenant.query.get_or_404(tenant_id)
        emails = [item["email"] for item in tenant_contacts(tenant) if item.get("email")]
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
        chats = tenant_telegram_chats(tenant)
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
        return render_template(
            "devices.html",
            devices=rows,
            include_archived=include_archived,
            alarm_states=alarm_states_map(rows),
        )

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
        recent_events = Event.query.filter_by(device_id=device.id).order_by(Event.started_at.desc()).limit(15).all()
        configs = DeviceConfig.query.filter_by(device_id=device.id).order_by(DeviceConfig.version.desc()).limit(5).all()
        recent_alarms = []
        for event in (
            Event.query.filter_by(device_id=device.id, type="alarm")
            .order_by(Event.started_at.desc())
            .limit(8)
            .all()
        ):
            try:
                recent_alarms.append(alarm_view(event))
            except Exception:
                app.logger.exception("alarm_view failed device=%s event=%s", device.device_uid, event.id)
        try:
            rules = configured_rules_view(device)
        except Exception:
            app.logger.exception("configured_rules_view failed device=%s", device.device_uid)
            rules = []
        modem_trace_event = next(
            (
                event
                for event in (
                    Event.query.filter_by(device_id=device.id)
                    .order_by(Event.started_at.desc())
                    .limit(80)
                    .all()
                )
                if isinstance(event.payload, dict) and event.payload.get("modem_log")
            ),
            None,
        )
        alarm_state = None
        try:
            alarm_state = alarm_states_map([device]).get(device.id)
        except Exception:
            app.logger.exception("alarm_states_map failed device=%s", device.device_uid)
        return render_template(
            "device_detail.html",
            device=device,
            recent_events=recent_events,
            recent_alarms=recent_alarms,
            configured_rules=rules,
            configs=configs,
            test_phone=last_used_test_phone(device, configs),
            modem_trace_event=modem_trace_event,
            contacts=resolve_contacts(device, latest_config_payload(device)),
            modem_jobs=active_modem_jobs(device),
            alarm_state=alarm_state,
            telemetry_series=series_for_range(
                device_sensor_windows(device),
                datetime.now(timezone.utc).replace(tzinfo=None) - timedelta(hours=24),
                datetime.now(timezone.utc).replace(tzinfo=None),
            ),
            last_readings=last_readings(
                device.id,
                device_sensor_windows(device),
                datetime.now(timezone.utc).replace(tzinfo=None),
            ),
            telemetry_max_days=TELEMETRY_MAX_RANGE_DAYS,
        )

    @app.get("/devices/<device_uid>/telemetry.json")
    @login_required
    def device_telemetry_json(device_uid):
        device = Device.query.filter_by(device_uid=device_uid).first_or_404()
        from_dt, to_dt, error = parse_telemetry_range()
        if error:
            return jsonify({"error": error}), 400
        series = series_for_range(device_sensor_windows(device), from_dt, to_dt)
        rows, truncated = telemetry_rows_for(device, from_dt, to_dt, TELEMETRY_CHART_MAX_ROWS)
        resolution = bucket_seconds(from_dt, to_dt)
        points, total = bucket_rows(rows, series, resolution)
        return jsonify(
            {
                "device_uid": device.device_uid,
                "from": _iso_utc(from_dt),
                "to": _iso_utc(to_dt),
                "bucket_seconds": resolution,
                "series": series,
                "points": points,
                "total_samples": total,
                "truncated": truncated,
            }
        )

    @app.get("/devices/<device_uid>/telemetry.csv")
    @login_required
    def device_telemetry_csv(device_uid):
        device = Device.query.filter_by(device_uid=device_uid).first_or_404()
        from_dt, to_dt, error = parse_telemetry_range()
        if error:
            return error, 400, {"Content-Type": "text/plain; charset=utf-8"}
        series = series_for_range(device_sensor_windows(device), from_dt, to_dt)
        rows, truncated = telemetry_rows_for(device, from_dt, to_dt, TELEMETRY_CSV_MAX_ROWS)
        if truncated:
            return (
                f"El rango tiene mas de {TELEMETRY_CSV_MAX_ROWS} muestras. "
                "Acota el rango para poder exportarlo.",
                413,
                {"Content-Type": "text/plain; charset=utf-8"},
            )

        buffer = io.StringIO()
        writer = csv.writer(buffer)
        header = ["fecha_hora", "device_id"]
        header.extend(item["label"] + (f" ({item['unit']})" if item["unit"] else "") for item in series)
        header.extend(label for _, label in AUX_COLUMNS)
        writer.writerow(header)
        # Each column must honour its own window: without this a reassigned
        # sensor exports the same raw sample under the old alias and the new
        # one, so the CSV would disagree with the chart (which does filter).
        spans = {
            item["id"]: (
                _parse_iso_epoch(item.get("starts_at")),
                _parse_iso_epoch(item.get("ends_at")),
            )
            for item in series
        }
        for row in rows:
            payload = row.payload if isinstance(row.payload, dict) else {}
            line = [row.received_at.isoformat() if row.received_at else "", device.device_uid]
            at = _to_epoch(row.received_at)
            for item in series:
                starts, ends = spans[item["id"]]
                if at is None or (starts is not None and at < starts) or (ends is not None and at >= ends):
                    line.append(None)
                else:
                    line.append(extract_value(payload, item))
            line.extend(extract_aux_values(payload).values())
            writer.writerow(line)

        filename = f"telemetria-{device.device_uid}-{from_dt:%Y%m%d}-{to_dt:%Y%m%d}.csv"
        response = app.response_class(
            buffer.getvalue().encode("utf-8-sig"),
            mimetype="text/csv",
            headers={"Content-Disposition": f'attachment; filename="{filename}"'},
        )
        return response

    @app.post("/devices/<device_uid>/sensors/<int:window_id>/close")
    @login_required
    def device_sensor_close(device_uid, window_id):
        """Stop recording a sensor. The history stays, it just stops growing."""
        device = Device.query.filter_by(device_uid=device_uid).first_or_404()
        window = SensorWindow.query.filter_by(id=window_id, device_id=device.id).first_or_404()
        if window.ends_at is None:
            window.ends_at = datetime.now(timezone.utc)
            window.closed_reason = "retired"
            db.session.commit()
        return redirect(url_for("device_config", device_uid=device.device_uid))

    @app.post("/devices/<device_uid>/sensors/<int:window_id>/reassign")
    @login_required
    def device_sensor_reassign(device_uid, window_id):
        """Move a sensor to a new role: close its window, open one with a new alias.

        The probe keeps reading the same payload field, so closing the old
        window is what stops the two aliases from both claiming the new samples.
        """
        device = Device.query.filter_by(device_uid=device_uid).first_or_404()
        window = SensorWindow.query.filter_by(id=window_id, device_id=device.id).first_or_404()
        alias = (request.form.get("alias") or "").strip()
        if not alias:
            return redirect(url_for("device_config", device_uid=device.device_uid))
        now = datetime.now(timezone.utc)
        if window.ends_at is None:
            window.ends_at = now
            window.closed_reason = "reassigned"
        db.session.add(
            SensorWindow(
                device_id=device.id,
                sensor_id=window.sensor_id,
                alias=alias,
                payload_key=window.payload_key,
                sensor_type=window.sensor_type,
                starts_at=now,
            )
        )
        db.session.commit()
        return redirect(url_for("device_config", device_uid=device.device_uid))

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

            validation_error = validate_config_payload(payload)
            if validation_error:
                return render_config_form(device, raw_payload, error=validation_error)

            # Pre-record the call text before persisting. A TTS failure has to
            # stop the save here; a config saved with a rule whose audio is
            # missing would dial and play silence.
            try:
                attach_call_audio(device, payload)
            except Exception as exc:
                return render_config_form(
                    device,
                    raw_payload,
                    error=f"No se pudo preparar el audio de la llamada: {exc}",
                )

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
        device = Device.query.filter_by(device_uid=device_uid).first_or_404()
        if not device_is_live(device):
            flash(
                "Sin MQTT: el OTA no llega al equipo. Enchufá Ethernet. "
                "Si el WiFi tiene clave mala, pulsá Olvidar y después OTA.",
                "warning",
            )
            return redirect(url_for("device_detail", device_uid=device.device_uid))
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

    @app.post("/devices/<device_uid>/commands/modem-probe")
    @login_required
    def device_command_modem_probe(device_uid):
        return send_device_command(
            device_uid,
            "modem_probe",
            "Modem probe command sent. Results arrive as modem_probe events.",
        )

    @app.post("/devices/<device_uid>/commands/set-wifi")
    @login_required
    def device_command_set_wifi(device_uid):
        ssid = (request.form.get("ssid") or "").strip()
        password = request.form.get("password") or ""
        if not ssid:
            flash("SSID requerido", "warning")
            return redirect(url_for("device_detail", device_uid=device_uid))
        if len(ssid) > 32:
            flash("SSID demasiado largo", "warning")
            return redirect(url_for("device_detail", device_uid=device_uid))
        if len(password) > 64:
            flash("Password demasiado largo", "warning")
            return redirect(url_for("device_detail", device_uid=device_uid))
        flash(f"WiFi enviado: {ssid}", "success")
        return send_device_command(
            device_uid,
            "set_wifi",
            f"WiFi command sent for {ssid}",
            extra={"ssid": ssid, "password": password},
        )

    @app.post("/devices/<device_uid>/commands/clear-wifi")
    @login_required
    def device_command_clear_wifi(device_uid):
        device = Device.query.filter_by(device_uid=device_uid).first_or_404()
        discovered = dict(device.discovered or {})
        network = dict(discovered.get("network") or {})
        network["wifi"] = {
            "configured": False,
            "up": False,
            "ssid": "",
            "ip": "-",
            "rssi": 0,
        }
        if network.get("active") == "wifi":
            eth = network.get("ethernet") or {}
            lte = network.get("lte") or {}
            if eth.get("up"):
                network["active"] = "ethernet"
            elif lte.get("up"):
                network["active"] = "lte"
            else:
                network["active"] = "none"
        discovered["network"] = network
        device.discovered = discovered
        flag_modified(device, "discovered")
        db.session.commit()
        flash("Pedido de olvidar WiFi enviado", "success")
        return send_device_command(device_uid, "clear_wifi", "WiFi clear command sent")

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

    @app.get("/audio/tmp/<audio_id>.mp3")
    def tts_audio_mp3(audio_id):
        """Browser-playable preview of a temporary TTS file.

        Browsers cannot decode AMR, so the Listen button asks for this instead.
        Transcodes on demand from the AMR next to it.
        """
        from .tts import transcode_to_mp3

        if not re.fullmatch(r"[a-f0-9]{32}", audio_id or ""):
            abort(404)
        from pathlib import Path

        amr_path = Path(os.environ.get("TTS_DIR", "/tmp/cof-tts")) / f"{audio_id}.amr"
        mp3_path = amr_path.with_suffix(".mp3")
        if not mp3_path.is_file():
            if not amr_path.is_file():
                abort(404)
            try:
                transcode_to_mp3(amr_path)
            except Exception:
                abort(404)
        return _serve_tts_audio(audio_id, ".mp3", "audio/mpeg")

    @app.get("/audio/asset/<path:filename>")
    def call_audio_asset(filename):
        """Serve a stored call-audio asset.

        Public on purpose (no login): the device fetches it without a session.
        The name is a content hash, so it is not guessable in a way that leaks
        anything, and caching is safe forever because the content is immutable.
        """
        from .call_audio import AUDIO_STORE_DIR

        name = os.path.basename(filename or "")
        if not re.fullmatch(r"[a-f0-9]{64}\.amr", name):
            abort(404)
        path = AUDIO_STORE_DIR / name
        if not path.is_file():
            abort(404)
        data = path.read_bytes()
        resp = app.response_class(data, mimetype="audio/amr")
        resp.headers["Content-Length"] = str(len(data))
        resp.headers["Cache-Control"] = "public, max-age=31536000, immutable"
        resp.headers["Content-Encoding"] = "identity"
        return resp

    @app.get("/audio/asset-preview/<text_sha>.mp3")
    def call_audio_asset_preview(text_sha):
        """Browser-playable MP3 of a stored asset, for the Listen button.

        Transcodes from the stored AMR on first request and caches it next to
        it. Kept off the `.amr` URL the device downloads so the device's file is
        never replaced by an MP3.
        """
        from .call_audio import AUDIO_STORE_DIR, asset_filename
        from .tts import PREVIEW_EXT, transcode_to_mp3

        if not re.fullmatch(r"[a-f0-9]{64}", text_sha or ""):
            abort(404)
        amr_path = AUDIO_STORE_DIR / asset_filename(text_sha)
        if not amr_path.is_file():
            abort(404)
        mp3_path = amr_path.with_suffix(f".{PREVIEW_EXT}")
        if not mp3_path.is_file():
            try:
                transcode_to_mp3(amr_path)
            except Exception:
                abort(404)
        data = mp3_path.read_bytes()
        resp = app.response_class(data, mimetype="audio/mpeg")
        resp.headers["Content-Length"] = str(len(data))
        # Private: this is behind a login, unlike the device-facing asset.
        resp.headers["Cache-Control"] = "private, max-age=31536000"
        resp.headers["Content-Encoding"] = "identity"
        return resp

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

    @app.post("/devices/<device_uid>/alarms/call-audio-preview")
    @login_required
    def device_call_audio_preview(device_uid):
        """Synthesize a rule's call text so the operator can hear it.

        Reuses an existing asset when the text is already stored (instant, and
        what the call will really sound like). Otherwise synthesizes an
        ephemeral copy that the 15-minute sweeper deletes - previewing must not
        litter the permanent store with audio nobody saved.

        Returns a browser-playable URL. Browsers cannot decode AMR (the device's
        format), so the audio is transcoded to MP3 for playback; the bytes are
        decoded from the real AMR, so the preview cannot drift from the call.
        """
        from .call_audio import (
            AUDIO_STORE_DIR,
            asset_filename,
            drop_unknown_placeholders,
            normalize_spoken,
            text_sha256,
        )
        from .models import AudioAsset
        from .tts import (
            PREVIEW_EXT,
            public_preview_url,
            synthesize_call_audio,
            transcode_to_mp3,
        )

        device = Device.query.filter_by(device_uid=device_uid).first_or_404()
        body = request.get_json(silent=True) or request.form
        template = str(body.get("text") or "")
        if not template.strip():
            return jsonify({"error": "Escribí el texto de la llamada primero."}), 400
        # Same reason the save refuses it: a {valor} here would be previewed as a
        # sentence with a hole in it, and the operator would think it is fine.
        if has_retired_placeholder(template):
            return jsonify({
                "error": (
                    "{valor} ya no existe: no se puede pregrabar. Escribí el número "
                    "directamente en el texto, o usá {umbral}."
                )
            }), 400

        # The form sends the sensor name it is about to save; falling back to the
        # persisted config only when the browser did not provide one keeps the
        # preview identical to what attach_call_audio() will record on save.
        sensor_id = str(body.get("sensor_id") or "")
        sensor_name = str(body.get("sensor_name") or "").strip() or (
            rule_sensor_name(device, sensor_id) or sensor_id
        )
        static = resolve_static_placeholders(
            template,
            {
                "equipo": device.name,
                "sitio": device.site.name if device.site else "",
                "cliente": device.tenant.name if device.tenant else "",
                "sensor": sensor_name,
                "regla": str(body.get("description") or ""),
                "umbral": spoken_number(body.get("threshold")),
            },
        )
        stored_text = normalize_spoken(drop_unknown_placeholders(static))[:MAX_CALL_TEXT_CHARS]
        if not stored_text:
            return jsonify({"error": "El texto quedó vacío después de resolver las variables."}), 400

        sha = text_sha256(stored_text)
        existing = AudioAsset.query.filter_by(text_sha256=sha).first()
        stored_amr = AUDIO_STORE_DIR / asset_filename(sha)
        if existing is not None and stored_amr.is_file():
            # Already saved: play the exact file the call will use. A stable
            # asset URL is fine, the browser transcodes it on demand.
            return jsonify({
                "url": f"/audio/asset-preview/{sha}.{PREVIEW_EXT}",
                "cached": True,
            })

        try:
            amr_path, audio_id = synthesize_call_audio(stored_text)
            transcode_to_mp3(amr_path)
        except Exception as exc:
            return jsonify({"error": f"No se pudo sintetizar: {exc}"}), 500
        return jsonify({"url": public_preview_url(audio_id), "cached": False})

    @app.post("/devices/<device_uid>/alarms/trigger")
    @login_required
    def device_trigger_alarm(device_uid):
        device = Device.query.filter_by(device_uid=device_uid).first_or_404()
        if device.archived_at is not None:
            return redirect(url_for("device_detail", device_uid=device.device_uid))
        config = latest_config_payload(device)
        rule_id = (request.form.get("rule_id") or "").strip()
        if not rule_id:
            event = dispatch_alarm(
                device,
                source="manual",
                title="Alarma de prueba",
                detail="Disparada desde la web (agenda completa)",
            )
        else:
            rule = find_rule(config, rule_id)
            if rule is None:
                flash("Elegí una regla publicada para probarla.", "warning")
                return redirect(url_for("device_detail", device_uid=device.device_uid))
            open_event = open_alarm_event(device.id, rule_key(rule))
            if open_event is not None:
                flash("Esa regla ya tiene una alarma abierta. Silenciala antes de probar de nuevo.", "warning")
                return redirect(url_for("device_detail", device_uid=device.device_uid))
            title = (rule.get("description") or "").strip() or "Alarma de prueba"
            event = fire_rule_alarm(
                device,
                rule,
                source="manual",
                title=title,
                detail="Disparada desde la web",
                config=config,
                hold_until_ack=True,
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
                payload=redact_command_secrets(payload),
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
    if not name or not slug:
        return "Nombre y slug son requeridos"

    chats, bad_chats = parse_telegram_chats(request.form.get("telegram_chat_id", ""))
    if bad_chats:
        return "Telegram chat ID invalido: " + ", ".join(bad_chats)

    names = request.form.getlist("contact_name")
    phones = request.form.getlist("contact_phone")
    emails = request.form.getlist("contact_email")
    ids = request.form.getlist("contact_id")
    count = max(len(names), len(phones), len(emails), len(ids))
    contacts = []
    for index in range(count):
        raw = {
            "id": ids[index] if index < len(ids) else "",
            "name": names[index] if index < len(names) else "",
            "phone": phones[index] if index < len(phones) else "",
            "email": emails[index] if index < len(emails) else "",
            "telegram_chat_id": "",
        }
        if not any(str(raw.get(key) or "").strip() for key in ("name", "phone", "email")):
            continue
        if not raw["id"]:
            raw["id"] = new_contact_id()
        contact = normalize_contact(raw)
        if contact is None:
            label = raw["name"] or raw["phone"] or raw["email"] or f"fila {index + 1}"
            return f"Contacto invalido ({label}). Telefono +549... o email valido."
        contacts.append(contact)

    tenant.name = name
    tenant.slug = slug
    sync_legacy_fields(tenant, contacts)
    tenant.telegram_chat_id = join_values(chats) or None
    return None


def _step_tone(step: dict) -> str:
    channel = str(step.get("channel") or "")
    status = str(step.get("status") or "")
    detail = str(step.get("detail") or "")
    if channel == "clear":
        return "clear" if not status.startswith("error") else "bad"
    if status in {"acked", "notified", "rearm", "cycle_done"} or status == "answered" or detail.startswith("Call done") or detail.startswith("SMS sent"):
        return "ok"
    if status == "sent" and channel != "call":
        return "ok"
    if status in {"queued"} or (status == "sent" and channel == "call"):
        return "wait"
    if status in {"no_answer", "error", "failed", "skipped", "exhausted", "cancelled"}:
        return "bad"
    return "wait"


def alarm_view(event: Event) -> dict:
    payload = event.payload if isinstance(event.payload, dict) else {}
    steps = []
    for step in payload.get("steps") or []:
        if not isinstance(step, dict):
            continue
        steps.append({**step, "label": friendly_step(step), "tone": _step_tone(step)})
    return {
        "event": event,
        "open": event.cleared_at is None,
        "title": payload.get("title") or "Alarma",
        "detail": payload.get("detail") or event.message or "",
        "steps": steps,
        "last": steps[-1]["label"] if steps else (event.message or "Sin actividad"),
        "escalate": bool(payload.get("escalate_calls", True)),
        "escalate_delay": payload.get("escalate_delay_seconds") or 0,
        "hysteresis": payload.get("hysteresis_seconds") or 0,
        "acked": bool(payload.get("acked")),
        "acked_label": (
            f"{payload.get('acked_from') or 'Alguien'} silenció desde {ACK_VIA.get(payload.get('acked_via'), 'el enlace de confirmación')}"
            if payload.get("acked")
            else ""
        ),
        "can_silence": event.cleared_at is None or bool(open_device_alarms(event)),
        "clear_actions": payload.get("clear_actions") or [],
    }


def open_alarm_events_for_devices(device_ids: list[int]) -> dict[int, list[Event]]:
    if not device_ids:
        return {}
    rows = (
        Event.query.filter(Event.device_id.in_(device_ids), Event.type == "alarm", Event.cleared_at.is_(None))
        .order_by(Event.started_at.desc())
        .all()
    )
    grouped: dict[int, list[Event]] = {}
    for event in rows:
        grouped.setdefault(event.device_id, []).append(event)
    return grouped


def alarm_state_from_events(events: list[Event]) -> dict | None:
    if not events:
        return None
    views = [alarm_view(event) for event in events]
    active = [item for item in views if not item["acked"]]
    acked = [item for item in views if item["acked"]]
    if active:
        tone, label = "danger", "alarma"
    else:
        tone, label = "warning", "reconocida"
    return {
        "tone": tone,
        "label": label,
        "count": len(views),
        "alarms": active + acked,
        "can_silence": bool(active),
    }


def alarm_states_map(devices) -> dict[int, dict]:
    grouped = open_alarm_events_for_devices([device.id for device in devices])
    return {device_id: alarm_state_from_events(events) for device_id, events in grouped.items()}


def publish_config_desired(device: Device, payload: dict):
    publish_mqtt(f"devices/{device.device_uid}/config/desired", payload, qos=1, retain=True)


# Connectivity sensors offered to every device. The firmware publishes them as
# 1 = ok / 0 = down, so an alarm rule is `operator: lt, threshold: 1`.
NETWORK_SENSORS = [
    {
        "id": "net_ethernet",
        "name": "Ethernet conectado (1 = si)",
        "type": "connectivity",
        "enabled": True,
        "source": "network_ethernet",
    },
    {
        "id": "net_wifi",
        "name": "WiFi conectado (1 = si)",
        "type": "connectivity",
        "enabled": True,
        "source": "network_wifi",
    },
    {
        "id": "net_internet",
        "name": "Internet disponible (1 = si)",
        "type": "connectivity",
        "enabled": True,
        "source": "network_internet",
    },
]


def ensure_network_sensors(cfg: dict) -> dict:
    """Make the connectivity sensors available without editing every config.

    Older devices have a stored config that predates these sensors. Injecting
    them here (rather than forcing a migration) means the operator immediately
    sees them in the rules dropdown, and because the config form rebuilds the
    payload from the rendered rows, they are persisted on the next save.
    """
    sensors = cfg.setdefault("sensors", [])
    present = {str(sensor.get("source") or "") for sensor in sensors if isinstance(sensor, dict)}
    for spec in NETWORK_SENSORS:
        if spec["source"] not in present:
            sensors.append(dict(spec))
    return cfg


# Sensors that shipped before the config carried a payload_key, mapped by id.
# The key is what the firmware actually publishes (see mqtt_io.cpp), which is
# NOT the "source": that one is a driver name ("sht31_humidity" is published as
# "humidity"). Without this backfill a stored config cannot say where to read
# its own sensor from, and the chart falls back to guessing by type - which is
# how the second temperature probe ends up rendering the first probe's value.
LEGACY_PAYLOAD_KEYS = {
    "temp_1": "temperature_1",
    "temp_2": "temperature_2",
    "humidity_1": "humidity",
    "mains_1": "zmpt_raw",
}


def ensure_payload_keys(cfg: dict) -> dict:
    """Back-fill the payload_key on sensors from configs stored before it existed.

    Done on read rather than as a DB migration: the operator's next save
    persists it, and the network sensors (whose source already equals their
    payload key) get one too so the field is uniform.
    """
    for sensor in cfg.get("sensors") or []:
        if not isinstance(sensor, dict):
            continue
        if sensor.get("payload_key"):
            continue
        sensor_id = str(sensor.get("id") or "")
        source = str(sensor.get("source") or "")
        if sensor_id in LEGACY_PAYLOAD_KEYS:
            sensor["payload_key"] = LEGACY_PAYLOAD_KEYS[sensor_id]
        elif source:
            # Connectivity and any sensor added later: the source IS the key.
            sensor["payload_key"] = source
    return cfg


def default_device_config(device: Device) -> dict:
    return {
        "schema_version": 1,
        "config_version": (device.desired_config_version or 0) + 1,
        "device_id": device.device_uid,
        "telemetry_interval_seconds": 60,
        "sensors": [
            {"id": "temp_1", "name": "DS18B20", "type": "temperature", "enabled": True, "source": "ds18b20", "payload_key": "temperature_1"},
            {
                "id": "temp_2",
                "name": "SHT31 temperatura",
                "type": "temperature",
                "enabled": True,
                "source": "sht31_temperature",
                "payload_key": "temperature_2",
            },
            {"id": "humidity_1", "name": "SHT31 humedad", "type": "humidity", "enabled": True, "source": "sht31_humidity", "payload_key": "humidity"},
            {"id": "mains_1", "name": "Red electrica", "type": "mains_voltage", "enabled": True, "source": "zmpt101b", "payload_key": "zmpt_raw"},
            *[dict(spec) for spec in NETWORK_SENSORS],
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
                "description": "Audio de respaldo de la llamada de alarma. El equipo lo baja al modem y lo reproduce SOLO cuando no puede bajar el TTS del servidor (sitio sin Ethernet ni WiFi). El archivo debe validarse con una llamada real.",
                "url": "https://app.callonfail.com.ar/ota/audio/cof_fallback.wav",
                "sha256": "",
                "modem_path": "C:/cof_fallback.wav",
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
    for contact in tenant_contacts(getattr(device, "tenant", None)):
        if contact.get("phone"):
            return contact["phone"]
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


def redact_command_secrets(payload: dict) -> dict:
    stored = dict(payload or {})
    if "password" in stored:
        stored["password"] = bool(stored.get("password"))
    return stored


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
    if isinstance(cfg, dict):
        cfg = ensure_payload_keys(ensure_network_sensors(cfg))
    # Warn about rules whose sensor has never carried a value. The concrete case is
    # mains_voltage: the firmware publishes it as null (only zmpt_raw is a real
    # reading), so a mains rule is accepted by the form, listed in the UI, and can
    # never fire. condition_holds returns None for a missing sensor and
    # evaluate_device_rules silently skips the rule, so nothing else surfaces it.
    #
    # The mapped PCF8574 input is the deliberate exception: input_1 is polled but
    # only forwarded to telemetry when water_leak is true, so absence of data is
    # normal there and flagging it would be a false positive.
    observed = set()
    latest = (
        Telemetry.query.filter_by(device_id=device.id)
        .order_by(Telemetry.received_at.desc())
        .first()
        if device is not None and getattr(device, "id", None) is not None
        else None
    )
    if latest is not None and isinstance(latest.payload, dict):
        observed = {str(key) for key, value in latest.payload.items() if value is not None}
    inert_sensors = []
    rules = cfg.get("rules") if isinstance(cfg, dict) else None
    sensors = {str(item.get("id")): item for item in (cfg.get("sensors") or []) if isinstance(item, dict)} if isinstance(cfg, dict) else {}
    seen_sensor_ids = []
    for rule in rules or []:
        if not isinstance(rule, dict):
            continue
        sensor_id = str(rule.get("sensor_id") or "")
        if not sensor_id or sensor_id in seen_sensor_ids:
            continue
        seen_sensor_ids.append(sensor_id)
        if sensor_id == "input_1":
            continue
        sensor = sensors.get(sensor_id) or {}
        if str(sensor.get("type")) == "mains_voltage" or sensor_id == "mains_1":
            inert_sensors.append((sensor_id, sensor.get("name") or "Red electrica", "el firmware todavia no calcula la tension (solo publica la lectura cruda del ADC)"))
            continue
        keys = SENSOR_ALIASES.get(sensor_id, (sensor_id,))
        if observed and not any(key in observed for key in keys):
            inert_sensors.append((sensor_id, sensor.get("name") or sensor_id, "nunca reporto un valor"))
    # Rules saved before {valor} was retired. They are not broken - build_call_text
    # drops the placeholder and the call still plays a pre-recorded file - but the
    # sentence it says has a hole exactly where the reading used to be, and only
    # the operator can decide what to put there now. Surfaced on the form rather
    # than silently rewritten: guessing a number would be worse than the hole.
    retired_placeholder_rules = [
        (index, str(rule.get("description") or rule.get("sensor_id") or ""))
        for index, rule in enumerate(rules or [], start=1)
        if isinstance(rule, dict) and has_retired_placeholder(str(rule.get("call_text") or ""))
    ]
    return render_template(
        "config_form.html",
        device=device,
        payload=payload,
        cfg=cfg,
        error=error,
        contacts=tenant_contacts(device.tenant),
        telegram_chats=tenant_telegram_chats(device.tenant),
        telemetry_min=TELEMETRY_INTERVAL_MIN_SECONDS,
        telemetry_max=TELEMETRY_INTERVAL_MAX_SECONDS,
        live_seconds=DEVICE_LIVE_SECONDS,
        inert_sensors=inert_sensors,
        sensor_windows=device_sensor_windows(device),
        call_text_max=MAX_CALL_TEXT_CHARS,
        retired_placeholder_rules=retired_placeholder_rules,
    )


app = create_app()
