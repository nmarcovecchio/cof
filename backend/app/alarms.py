import logging
import math
import os
import time
import uuid
from datetime import datetime, timezone

from sqlalchemy.orm.attributes import flag_modified

from .extensions import db
from .models import Device, DeviceConfig, Event, utcnow
from .mqtt_util import publish_mqtt
from .notify import send_email, send_telegram, smtp_configured, telegram_configured
from .phones import parse_emails, parse_phones, parse_telegram_chats
from .tts import MAX_TEXT_CHARS, public_audio_url, synthesize_call_audio

logger = logging.getLogger("callonfail.alarms")

SENSOR_ALIASES = {
    "temp_1": ("temp_1", "temperature_1"),
    "temp_2": ("temp_2", "temperature_2"),
    "humidity_1": ("humidity_1", "humidity"),
    "mains_1": ("mains_1", "mains_voltage"),
    "water_1": ("water_1", "water_leak", "input_1"),
}

DEFAULT_MANUAL_ACTIONS = ("email", "telegram", "sms", "call")
DEVICE_LIVE_SECONDS = 180

_rule_since: dict[str, float] = {}
_redis = None
_redis_failed = False


def latest_config_payload(device: Device) -> dict:
    config = DeviceConfig.query.filter_by(device_id=device.id).order_by(DeviceConfig.version.desc()).first()
    payload = config.desired_payload if config and isinstance(config.desired_payload, dict) else {}
    return payload


def calling_enabled(device: Device, config: dict | None = None) -> bool:
    payload = config if config is not None else latest_config_payload(device)
    calling = payload.get("calling") or {}
    return bool(calling.get("enabled"))


def device_recently_seen(device: Device) -> bool:
    seen = getattr(device, "last_seen_at", None)
    if seen is None:
        return False
    if seen.tzinfo is None:
        seen = seen.replace(tzinfo=timezone.utc)
    return (datetime.now(timezone.utc) - seen).total_seconds() < DEVICE_LIVE_SECONDS


def resolve_contacts(device: Device, config: dict | None = None) -> dict:
    del config  # Contacts live on the tenant. Device JSON is not an override.
    tenant = device.tenant
    emails, _ = parse_emails(getattr(tenant, "notify_email", "") or "")
    chats, _ = parse_telegram_chats(getattr(tenant, "telegram_chat_id", "") or "")
    phones, _ = parse_phones(getattr(tenant, "phone", "") or "")
    return {
        "emails": emails,
        "email": emails[0] if emails else "",
        "telegram_chat_ids": chats,
        "telegram_chat_id": chats[0] if chats else "",
        "phones": phones,
        "phone": phones[0] if phones else "",
        "calling_enabled": calling_enabled(device),
        "smtp_ready": smtp_configured(),
        "telegram_ready": telegram_configured(),
        "device_live": device_recently_seen(device),
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
        number = float(value)
    except (TypeError, ValueError):
        return None
    if math.isnan(number) or math.isinf(number):
        return None
    return number


def condition_holds(rule: dict, telemetry: dict) -> bool | None:
    raw = sensor_value(telemetry, str(rule.get("sensor_id") or ""))
    if raw is None:
        return None
    left = to_number(raw)
    right = to_number(rule.get("threshold"))
    operator = str(rule.get("operator") or "gt")
    if left is None or right is None:
        return None
    if operator == "gt":
        return left > right
    if operator == "lt":
        return left < right
    if operator == "gte":
        return left >= right
    if operator == "lte":
        return left <= right
    if operator == "eq":
        return abs(left - right) <= 1e-6
    if operator == "ne":
        return abs(left - right) > 1e-6
    return None


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
    global _redis, _redis_failed
    if _redis_failed:
        return None
    if _redis is not None:
        return _redis
    url = os.environ.get("REDIS_URL")
    if not url:
        _redis_failed = True
        return None
    try:
        import redis

        _redis = redis.Redis.from_url(url, socket_connect_timeout=2, decode_responses=True)
        return _redis
    except Exception:
        logger.exception("Redis unavailable for alarm debounce")
        _redis_failed = True
        return None


def _condition_since(device_id: int, key: str, now_ts: float, active: bool) -> float | None:
    mem_key = f"{device_id}:{key}"
    redis_key = f"cof:rule:{device_id}:{key}"
    client = _redis_client()

    if not active:
        _rule_since.pop(mem_key, None)
        if client is not None:
            try:
                client.delete(redis_key)
            except Exception:
                logger.exception("Redis rule clear failed")
        return None

    if client is not None:
        try:
            existing = client.get(redis_key)
            if existing:
                started = float(existing)
                _rule_since[mem_key] = started
                return started
            client.set(redis_key, str(now_ts))
            _rule_since[mem_key] = now_ts
            return now_ts
        except Exception:
            logger.exception("Redis rule state failed")

    if mem_key in _rule_since:
        return _rule_since[mem_key]
    _rule_since[mem_key] = now_ts
    return now_ts


def open_alarm_event(device_id: int, key: str) -> Event | None:
    events = (
        Event.query.filter_by(device_id=device_id, type="alarm")
        .filter(Event.cleared_at.is_(None))
        .order_by(Event.started_at.desc())
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
    flag_modified(event, "payload")


def _mqtt_note(contacts: dict, sent: str) -> str:
    if contacts.get("device_live"):
        return sent
    return f"{sent} (equipo no visto en 3 min; SMS/llamada se pueden perder)"


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
        "email": "email" in wanted and bool(contacts["emails"]),
        "telegram": "telegram" in wanted and bool(contacts["telegram_chat_ids"]),
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
            "contacts": {
                "emails": contacts["emails"],
                "telegram_chat_ids": contacts["telegram_chat_ids"],
                "phones": contacts["phones"],
                "phone": contacts["phone"],
                "calling_enabled": contacts["calling_enabled"],
            },
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
                send_email(contacts["emails"], f"[CallOnFail] {title} - {device.name}", text)
                results["email"] = f"sent:{len(contacts['emails'])}"
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
                send_telegram(contacts["telegram_chat_ids"], text)
                results["telegram"] = f"sent:{len(contacts['telegram_chat_ids'])}"
            except Exception as exc:
                logger.exception("Alarm telegram failed device=%s", device.device_uid)
                results["telegram"] = f"error: {exc}"
    elif "telegram" in wanted:
        results["telegram"] = "skipped: sin chat de Telegram"

    if channels["sms"]:
        results["sms"] = _mqtt_note(
            contacts,
            _publish_device_command(
                device,
                "test_sms",
                {"phone": contacts["phone"], "text": spoken[:160]},
            ),
        )
    elif "sms" in wanted:
        results["sms"] = "skipped: sin telefono del cliente"

    if channels["call"]:
        results["call"] = _mqtt_note(contacts, _publish_alarm_call(device, contacts["phone"], spoken))
        if len(contacts["phones"]) > 1:
            results["call_cascade"] = "pending: hoy llama al primero; la cascada X->Y->Z todavia no esta"
    elif "call" in wanted and not contacts["calling_enabled"]:
        results["call"] = "skipped: llamadas deshabilitadas en el dispositivo"
    elif "call" in wanted:
        results["call"] = "skipped: sin telefono del cliente"

    event.payload = {**(event.payload or {}), "results": results}
    flag_modified(event, "payload")
    if source == "manual":
        event.cleared_at = utcnow()
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


def _safe_duration(rule: dict) -> int:
    try:
        value = int(rule.get("duration_seconds") or 0)
    except (TypeError, ValueError):
        return 0
    return max(0, value)


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
        duration = _safe_duration(rule)
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
