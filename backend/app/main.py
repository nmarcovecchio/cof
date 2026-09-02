import os
from html import escape
from datetime import datetime, timezone

import redis
from flask import Flask, jsonify
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
        return (
            "<html><body>"
            "<h1>CallOnFail backend</h1>"
            "<p>Backend is running.</p>"
            "<p>Health: <a href=\"/health\">/health</a></p>"
            "<p>Status: <a href=\"/api/status\">/api/status</a></p>"
            "<p>Dashboard: <a href=\"/dashboard\">/dashboard</a></p>"
            "</body></html>"
        )

    @app.get("/dashboard")
    def dashboard():
        tenants = Tenant.query.order_by(Tenant.name).all()
        devices = Device.query.order_by(Device.created_at.desc()).all()
        recent_events = Event.query.order_by(Event.started_at.desc()).limit(10).all()
        recent_telemetry = Telemetry.query.order_by(Telemetry.received_at.desc()).limit(10).all()
        return render_dashboard(tenants, devices, recent_events, recent_telemetry)

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
        return render_device_detail(device, recent_telemetry, recent_events, configs)

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


def render_dashboard(tenants, devices, recent_events, recent_telemetry) -> str:
    device_rows = "".join(
        "<tr>"
        f"<td><a href=\"/devices/{escape(device.device_uid)}\">{escape(device.device_uid)}</a></td>"
        f"<td>{escape(device.name)}</td>"
        f"<td>{escape(device.status)}</td>"
        f"<td>{escape(device.firmware_version or '-')}</td>"
        f"<td>{device.last_seen_at or '-'}</td>"
        "</tr>"
        for device in devices
    )
    event_rows = "".join(
        "<tr>"
        f"<td>{event.started_at}</td>"
        f"<td>{escape(event.device.device_uid)}</td>"
        f"<td>{escape(event.severity)}</td>"
        f"<td>{escape(event.type)}</td>"
        f"<td>{escape(event.message or '')}</td>"
        "</tr>"
        for event in recent_events
    )
    telemetry_rows = "".join(
        "<tr>"
        f"<td>{row.received_at}</td>"
        f"<td>{escape(row.device.device_uid)}</td>"
        f"<td>{row.temperature_1 if row.temperature_1 is not None else '-'}</td>"
        f"<td>{row.humidity if row.humidity is not None else '-'}</td>"
        f"<td>{row.mains_voltage if row.mains_voltage is not None else '-'}</td>"
        "</tr>"
        for row in recent_telemetry
    )

    return render_page(
        "Dashboard",
        f"""
        <div class="cards">
          <div><strong>Clientes</strong><br>{len(tenants)}</div>
          <div><strong>Dispositivos</strong><br>{len(devices)}</div>
          <div><strong>Eventos recientes</strong><br>{len(recent_events)}</div>
        </div>
        <h2>Dispositivos</h2>
        <table>
          <thead><tr><th>ID</th><th>Nombre</th><th>Estado</th><th>FW</th><th>Ultima conexion</th></tr></thead>
          <tbody>{device_rows or '<tr><td colspan="5">Sin dispositivos todavia</td></tr>'}</tbody>
        </table>
        <h2>Telemetria reciente</h2>
        <table>
          <thead><tr><th>Fecha</th><th>Dispositivo</th><th>Temp</th><th>Hum</th><th>VAC</th></tr></thead>
          <tbody>{telemetry_rows or '<tr><td colspan="5">Sin telemetria todavia</td></tr>'}</tbody>
        </table>
        <h2>Eventos recientes</h2>
        <table>
          <thead><tr><th>Fecha</th><th>Dispositivo</th><th>Severidad</th><th>Tipo</th><th>Mensaje</th></tr></thead>
          <tbody>{event_rows or '<tr><td colspan="5">Sin eventos todavia</td></tr>'}</tbody>
        </table>
        """,
    )


def render_device_detail(device, recent_telemetry, recent_events, configs) -> str:
    telemetry_rows = "".join(
        "<tr>"
        f"<td>{row.received_at}</td>"
        f"<td>{row.temperature_1 if row.temperature_1 is not None else '-'}</td>"
        f"<td>{row.temperature_2 if row.temperature_2 is not None else '-'}</td>"
        f"<td>{row.humidity if row.humidity is not None else '-'}</td>"
        f"<td>{row.mains_voltage if row.mains_voltage is not None else '-'}</td>"
        "</tr>"
        for row in recent_telemetry
    )
    event_rows = "".join(
        "<tr>"
        f"<td>{event.started_at}</td>"
        f"<td>{escape(event.severity)}</td>"
        f"<td>{escape(event.type)}</td>"
        f"<td>{escape(event.message or '')}</td>"
        "</tr>"
        for event in recent_events
    )
    config_rows = "".join(
        "<tr>"
        f"<td>{config.version}</td>"
        f"<td>{escape(config.status)}</td>"
        f"<td>{escape(config.config_hash or '-')}</td>"
        f"<td>{config.applied_at or '-'}</td>"
        "</tr>"
        for config in configs
    )
    return render_page(
        device.device_uid,
        f"""
        <p><a href="/dashboard">Volver</a></p>
        <h2>{escape(device.name)}</h2>
        <ul>
          <li>Tenant: {escape(device.tenant.name)}</li>
          <li>Estado: {escape(device.status)}</li>
          <li>Firmware: {escape(device.firmware_version or '-')}</li>
          <li>Ultima conexion: {device.last_seen_at or '-'}</li>
          <li>Config deseada/reportada: {device.desired_config_version or '-'} / {device.reported_config_version or '-'}</li>
        </ul>
        <h2>Telemetria</h2>
        <table>
          <thead><tr><th>Fecha</th><th>Temp 1</th><th>Temp 2</th><th>Hum</th><th>VAC</th></tr></thead>
          <tbody>{telemetry_rows or '<tr><td colspan="5">Sin telemetria</td></tr>'}</tbody>
        </table>
        <h2>Eventos</h2>
        <table>
          <thead><tr><th>Fecha</th><th>Severidad</th><th>Tipo</th><th>Mensaje</th></tr></thead>
          <tbody>{event_rows or '<tr><td colspan="4">Sin eventos</td></tr>'}</tbody>
        </table>
        <h2>Configuraciones</h2>
        <table>
          <thead><tr><th>Version</th><th>Estado</th><th>Hash</th><th>Aplicada</th></tr></thead>
          <tbody>{config_rows or '<tr><td colspan="4">Sin configuraciones</td></tr>'}</tbody>
        </table>
        """,
    )


def render_page(title: str, body: str) -> str:
    safe_title = escape(title)
    return f"""
    <!doctype html>
    <html lang="es">
      <head>
        <meta charset="utf-8">
        <meta name="viewport" content="width=device-width, initial-scale=1">
        <title>{safe_title} - CallOnFail</title>
        <style>
          body {{ font-family: Arial, sans-serif; margin: 24px; color: #17202a; }}
          a {{ color: #0b6efd; }}
          table {{ border-collapse: collapse; width: 100%; margin-bottom: 28px; }}
          th, td {{ border: 1px solid #d0d7de; padding: 8px; text-align: left; }}
          th {{ background: #f6f8fa; }}
          .cards {{ display: flex; gap: 16px; margin: 16px 0 28px; }}
          .cards div {{ border: 1px solid #d0d7de; border-radius: 6px; padding: 16px; min-width: 140px; }}
        </style>
      </head>
      <body>
        <h1>{safe_title}</h1>
        {body}
      </body>
    </html>
    """


app = create_app()
