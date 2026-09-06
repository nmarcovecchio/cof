import logging
import os
import time
import uuid
from datetime import datetime, timezone

from .extensions import db
from .models import Device, DeviceConfig, Event, utcnow
from .mqtt_util import publish_mqtt
from .notify import send_email, send_telegram, smtp_configured, telegram_configured
from .phones import is_e164_phone, is_email, is_telegram_chat_id, normalize_email, normalize_phone, normalize_telegram_chat_id
from .tts import MAX_TEXT_CHARS, public_audio_url, synthesize_call_audio

logger = logging.getLogger("callonfail.alarms")

SENSOR_ALIASES = {
    "temp_1": ("temp_1", "temperature_1"),
    "temp_2": ("temp_2", "temperature_2"),
    "humidity_1": ("humidity_1", "humidity"),
    "mains_1": ("mains_1", "mains_voltage"),
    "water_1": ("water_1", "water_leak", "input_1"),
}

OPERATORS = {
    "gt": lambda left, right: left > right,
    "lt": lambda left, right: left < right,
    "gte": lambda left, right: left >= right,
    "lte": lambda left, right: left <= right,
    "eq": lambda left, right: left == right,
    "ne": lambda left, right: left != right,
}

DEFAULT_MANUAL_ACTIONS = ("email", "telegram", "sms", "call")


def latest_config_payload(device: Device) -> dict:
    config = DeviceConfig.query.filter_by(device_id=device.id).order_by(DeviceConfig.version.desc()).first()
    payload = config.desired_payload if config and isinstance(config.desired_payload, dict) else {}
    return payload


def calling_enabled(device: Device, config: dict | None = None) -> bool:
    payload = config if config is not None else latest_config_payload(device)
    calling = payload.get("calling") or {}
    return bool(calling.get("enabled"))


def resolve_contacts(device: Device, config: dict | None = None) -> dict:
    payload = config if config is not None else latest_config_payload(device)
    tenant = device.tenant
    notifications = payload.get("notifications") or {}
    calling = payload.get("calling") or {}
    contacts = payload.get("contacts") or []

    email = normalize_email(getattr(tenant, "notify_email", "") or "")
    override_email = normalize_email(str(notifications.get("email") or ""))
    if is_email(override_email):
        email = override_email

    chat_id = normalize_telegram_chat_id(getattr(tenant, "telegram_chat_id", "") or "")
    override_chat = normalize_telegram_chat_id(str(notifications.get("telegram_chat_id") or ""))
    if is_telegram_chat_id(override_chat):
        chat_id = override_chat

    phone = normalize_phone(getattr(tenant, "phone", "") or "")
    override_phone = normalize_phone(str(calling.get("phone") or ""))
    if not is_e164_phone(override_phone) and contacts and isinstance(contacts[0], dict):
        override_phone = normalize_phone(str(contacts[0].get("phone") or ""))
    if is_e164_phone(override_phone):
        phone = override_phone

    return {
        "email": email if is_email(email) else "",
        "telegram_chat_id": chat_id if is_telegram_chat_id(chat_id) else "",
        "phone": phone if is_e164_phone(phone) else "",
        "calling_enabled": calling_enabled(device, payload),
        "smtp_ready": smtp_configured(),
        "telegram_ready": telegram_configured(),
    }


def build_alarm_text(device: Device, title: str, detail: str) -> str:
    tenant_name = device.tenant.name if device.tenant else "-"
    site_name = device.site.name if device.site else "-"
    lines = [
        title,
        f"Cliente: {tenant_name}",
        f"Sitio: {site_name}",
        f"Equipo: {device.name} ({device.device_uid})",
    ]
    if detail:
        lines.append(detail)
    return "\n".join(lines)


def rule_key(rule: dict) -> str:
    if rule.get("id"):
        return str(rule["id"])
    return "|".join(
        [
            str(rule.get("sensor_id") or ""),
            str(rule.get("operator") or ""),
            str(rule.get("threshold") if rule.get("threshold") is not None else ""),
        ]
    )


def sensor_value(payload: dict, sensor_id: str):
    keys = SENSOR_ALIASES.get(sensor_id, (sensor_id,))
    for key in keys:
        if key in payload and payload.get(key) is not None:
            return payload.get(key)
    return None


def to_number(value):
    if isinstance(value, bool):
        return 1.0 if value else 0.0
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def condition_holds(rule: dict, telemetry: dict) -> bool | None:
    raw = sensor_value(telemetry, str(rule.get("sensor_id") or ""))
    if raw is None:
        return None
    left = to_number(raw)
    right = to_number(rule.get("threshold"))
    compare = OPERATORS.get(str(rule.get("operator") or "gt"))
    if left is None or right is None or compare is None:
        return None
    return compare(left, right)


def format_rule_detail(rule: dict, telemetry: dict) -> str:
    description = (rule.get("description") or "").strip()
    sensor_id = rule.get("sensor_id") or "?"
    operator = rule.get("operator") or "?"
    threshold = rule.get("threshold")
    value = sensor_value(telemetry, str(sensor_id))
    core = f"{sensor_id} {operator} {threshold} (valor {value})"
    if description:
        return f"{description}: {core}"
    return core


def _redis_client():
    url = os.environ.get("REDIS_URL")
    if not url:
        return None
    try:
        import redis

        return redis.Redis.from_url(url, socket_connect_timeout=2, decode_responses=True)
    except Exception:
        logger.exception("Redis unavailable for alarm debounce")
        return None


def _condition_since(device_id: int, key: str, now_ts: float, active: bool) -> float | None:
    client = _redis_client()
    redis_key = f"cof:rule:{device_id}:{key}"
    if client is None:
        return now_ts if active else None
    try:
        if not active:
            client.delete(redis_key)
            return None
        existing = client.get(redis_key)
        if existing:
            return float(existing)
        client.set(redis_key, str(now_ts))
        return now_ts
    except Exception:
        logger.exception("Redis rule state failed")
        return now_ts if active else None


def open_alarm_event(device_id: int, key: str) -> Event | None:
    events = (
        Event.query.filter_by(device_id=device_id, type="alarm")
        .filter(Event.cleared_at.is_(None))
        .order_by(Event.started_at.desc())
        .limit(20)
        .all()
    )
    for event in events:
        payload = event.payload or {}
        if payload.get("rule_key") == key:
            return event
    return None


def clear_alarm(event: Event, telemetry: dict | None = None) -> None:
    event.cleared_at = utcnow()
    payload = dict(event.payload or {})
    payload["cleared_at"] = event.cleared_at.isoformat()
    if telemetry is not None:
        payload["cleared_value"] = sensor_value(telemetry, str(payload.get("sensor_id") or ""))
    event.payload = payload


def dispatch_alarm(
    device: Device,
    *,
    source: str,
    title: str,
    detail: str = "",
    actions: list[str] | tuple[str, ...] | None = None,
    extra: dict | None = None,
    config: dict | None = None,
) -> Event:
    payload_cfg = config if config is not None else latest_config_payload(device)
    contacts = resolve_contacts(device, payload_cfg)
    wanted = set(DEFAULT_MANUAL_ACTIONS if actions is None else actions)
    text = build_alarm_text(device, title, detail)
    spoken = " ".join(text.split())
    if len(spoken) > MAX_TEXT_CHARS:
        spoken = spoken[:MAX_TEXT_CHARS]

    channels = {
        "email": "email" in wanted and bool(contacts["email"]),
        "telegram": "telegram" in wanted and bool(contacts["telegram_chat_id"]),
        "sms": "sms" in wanted and bool(contacts["phone"]),
        "call": "call" in wanted and contacts["calling_enabled"] and bool(contacts["phone"]),
    }
    results = {}

    event = Event(
        device_id=device.id,
        type="alarm",
        severity="error",
        message=text.replace("\n", " ")[:240],
        payload={
            "source": source,
            "title": title,
            "detail": detail,
            "text": text,
            "contacts": {key: contacts[key] for key in ("email", "telegram_chat_id", "phone", "calling_enabled")},
            "channels": channels,
            "results": results,
            **(extra or {}),
        },
    )
    db.session.add(event)
    db.session.flush()

    if channels["email"]:
        if not contacts["smtp_ready"]:
            results["email"] = "skipped: SMTP no configurado"
        else:
            try:
                send_email(contacts["email"], f"[CallOnFail] {title} - {device.name}", text)
                results["email"] = "sent"
            except Exception as exc:
                logger.exception("Alarm email failed device=%s", device.device_uid)
                results["email"] = f"error: {exc}"
    elif "email" in wanted:
        results["email"] = "skipped: sin email del cliente"

    if channels["telegram"]:
        if not contacts["telegram_ready"]:
            results["telegram"] = "skipped: TELEGRAM_BOT_TOKEN no configurado"
        else:
            try:
                send_telegram(contacts["telegram_chat_id"], text)
                results["telegram"] = "sent"
            except Exception as exc:
                logger.exception("Alarm telegram failed device=%s", device.device_uid)
                results["telegram"] = f"error: {exc}"
    elif "telegram" in wanted:
        results["telegram"] = "skipped: sin chat de Telegram"

    if channels["sms"]:
        results["sms"] = _publish_device_command(
            device,
            "test_sms",
            {"phone": contacts["phone"], "text": spoken[:160]},
        )
    elif "sms" in wanted:
        results["sms"] = "skipped: sin telefono del cliente"

    if channels["call"]:
        results["call"] = _publish_alarm_call(device, contacts["phone"], spoken)
    elif "call" in wanted and not contacts["calling_enabled"]:
        results["call"] = "skipped: llamadas deshabilitadas en el dispositivo"
    elif "call" in wanted:
        results["call"] = "skipped: sin telefono del cliente"

    event.payload = {**(event.payload or {}), "results": results}
    return event


def _publish_device_command(device: Device, command: str, extra: dict) -> str:
    payload = {
        "command_id": str(uuid.uuid4()),
        "command": command,
        "device_id": device.device_uid,
        "created_at": datetime.now(timezone.utc).isoformat(),
        **extra,
    }
    try:
        publish_mqtt(f"devices/{device.device_uid}/command", payload, qos=1, retain=False)
        return "sent"
    except Exception as exc:
        logger.exception("Alarm command failed device=%s command=%s", device.device_uid, command)
        return f"error: {exc}"


def _publish_alarm_call(device: Device, phone: str, text: str) -> str:
    try:
        _path, audio_id = synthesize_call_audio(text)
        extra = {
            "phone": phone,
            "text": text,
            "audio_url": public_audio_url(audio_id),
            "audio_format": "amr_nb_8000",
        }
    except Exception as exc:
        logger.exception("Alarm TTS failed device=%s", device.device_uid)
        return f"error: TTS failed ({exc})"
    return _publish_device_command(device, "test_call", extra)


def evaluate_device_rules(device: Device, telemetry: dict) -> list[Event]:
    config = latest_config_payload(device)
    rules = config.get("rules") or []
    if not rules:
        return []

    fired: list[Event] = []
    now_ts = time.time()
    for rule in rules:
        if not isinstance(rule, dict):
            continue
        key = rule_key(rule)
        holds = condition_holds(rule, telemetry)
        if holds is None:
            continue
        if not holds:
            _condition_since(device.id, key, now_ts, False)
            open_event = open_alarm_event(device.id, key)
            if open_event is not None:
                clear_alarm(open_event, telemetry)
            continue

        since = _condition_since(device.id, key, now_ts, True)
        duration = int(rule.get("duration_seconds") or 0)
        if since is None or (now_ts - since) < duration:
            continue
        if open_alarm_event(device.id, key) is not None:
            continue

        selected = list(rule.get("actions") or [])
        actions = [item for item in selected if item != "log_only"]
        if not actions and "log_only" not in selected:
            actions = ["email", "telegram"]
        detail = format_rule_detail(rule, telemetry)
        event = dispatch_alarm(
            device,
            source="rule",
            title="Alarma CallOnFail",
            detail=detail,
            actions=actions,
            extra={
                "rule_key": key,
                "sensor_id": rule.get("sensor_id"),
                "operator": rule.get("operator"),
                "threshold": rule.get("threshold"),
                "value": sensor_value(telemetry, str(rule.get("sensor_id") or "")),
            },
            config=config,
        )
        fired.append(event)
    return fired
