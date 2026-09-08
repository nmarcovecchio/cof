import logging
import math
import os
import secrets
import time
from datetime import datetime, timezone

from sqlalchemy.orm.attributes import flag_modified

from .alarm_ack import public_ack_url
from .alarm_log import append_alarm_step
from .contacts import pick_contacts, tenant_contacts, tenant_telegram_chats
from .extensions import db
from .models import Device, DeviceConfig, Event, utcnow
from .modem_queue import enqueue_modem_job, pump_modem_queue, schedule_alarm_rearm
from .notify import send_email, send_telegram, smtp_configured, telegram_configured
from .tts import MAX_TEXT_CHARS

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
_rearm_until: dict[str, float] = {}
_redis = None
_redis_failed = False
ACK_HINT = "Toca el enlace para confirmar y silenciar las alarmas de este equipo."


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


def _id_list(value) -> list[str] | None:
    if value is None:
        return None
    if isinstance(value, str):
        value = [value]
    if not isinstance(value, (list, tuple)):
        return None
    return [str(item).strip() for item in value if str(item).strip()]


def resolve_contacts(device: Device, config: dict | None = None, selection: dict | None = None) -> dict:
    del config  # Contacts live on the tenant. Device JSON is not an override.
    agenda = tenant_contacts(device.tenant)
    selection = selection or {}
    email_ids = _id_list(selection.get("email_contact_ids"))
    sms_ids = _id_list(selection.get("sms_contact_ids"))
    call_ids = _id_list(selection.get("call_contact_ids"))
    email_contacts = pick_contacts(agenda, email_ids, "email")
    sms_contacts = pick_contacts(agenda, sms_ids, "phone")
    call_contacts = pick_contacts(agenda, call_ids, "phone")
    emails = [item["email"] for item in email_contacts]
    sms_phones = [item["phone"] for item in sms_contacts]
    chats = tenant_telegram_chats(device.tenant)
    call_phones = [item["phone"] for item in call_contacts]
    return {
        "agenda": agenda,
        "emails": emails,
        "email": emails[0] if emails else "",
        "telegram_chat_ids": chats,
        "telegram_chat_id": chats[0] if chats else "",
        "phones": sms_phones or call_phones,
        "sms_phones": sms_phones,
        "call_phones": call_phones,
        "phone": (sms_phones or call_phones or [""])[0],
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


RULE_OPERATORS = {
    "gt": ">",
    "lt": "<",
    "gte": "≥",
    "lte": "≤",
    "eq": "=",
    "ne": "≠",
}

ACTION_LABELS = {
    "call": "llamada",
    "sms": "SMS",
    "email": "email",
    "telegram": "Telegram",
    "log_only": "solo log",
}


def configured_rules_view(device: Device) -> list[dict]:
    payload = latest_config_payload(device)
    sensors = {
        str(item.get("id")): item
        for item in (payload.get("sensors") or [])
        if isinstance(item, dict) and item.get("id")
    }
    agenda = {str(item.get("id")): item for item in tenant_contacts(device.tenant)}
    rows = []
    for rule in payload.get("rules") or []:
        if not isinstance(rule, dict):
            continue
        sensor_id = str(rule.get("sensor_id") or "")
        sensor = sensors.get(sensor_id) or {}
        sensor_name = (sensor.get("name") or "").strip() or sensor_id or "sensor"
        actions = [ACTION_LABELS.get(str(item), str(item)) for item in (rule.get("actions") or [])]
        who = []
        for key in ("call_contact_ids", "sms_contact_ids", "email_contact_ids"):
            for contact_id in rule.get(key) or []:
                contact = agenda.get(str(contact_id)) or {}
                name = (contact.get("name") or "").strip() or str(contact_id)
                if name not in who:
                    who.append(name)
        duration = _safe_seconds(rule.get("duration_seconds"))
        hysteresis = _safe_seconds(rule.get("hysteresis_seconds"))
        operator = RULE_OPERATORS.get(rule.get("operator"), rule.get("operator") or "?")
        title = (rule.get("description") or "").strip() or sensor_name
        rows.append(
            {
                "title": title,
                "condition": f"{sensor_name} {operator} {rule.get('threshold')}",
                "duration_label": "inmediato" if duration <= 0 else f"durante {duration}s",
                "actions": actions,
                "who": who,
                "escalate": bool(rule.get("escalate_calls", True)),
                "hysteresis": hysteresis,
                "clear": [ACTION_LABELS.get(str(item), str(item)) for item in (rule.get("clear_actions") or [])],
            }
        )
    return rows


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


def clear_alarm(event: Event, telemetry: dict | None = None, device: Device | None = None) -> None:
    event.cleared_at = utcnow()
    payload = dict(event.payload or {})
    payload["cleared_at"] = event.cleared_at.isoformat()
    if telemetry is not None:
        payload["cleared_value"] = sensor_value(telemetry, str(payload.get("sensor_id") or ""))
    event.payload = payload
    flag_modified(event, "payload")
    if device is not None:
        notify_alarm_cleared(device, event, telemetry)


def _sms_with_ack_link(spoken: str, ack_url: str) -> str:
    spoken = " ".join((spoken or "").split())
    if not ack_url:
        return spoken[:160]
    prefix = "Confirmar: "
    room = 160 - len(prefix) - len(ack_url) - 1
    body = spoken[: max(0, room)].rstrip()
    if body:
        return f"{body} {prefix}{ack_url}"[:160]
    return f"{prefix}{ack_url}"[:160]


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
    extra = extra or {}
    payload_cfg = config if config is not None else latest_config_payload(device)
    contacts = resolve_contacts(device, payload_cfg, extra)
    wanted = set(DEFAULT_MANUAL_ACTIONS if actions is None else actions)
    text = build_alarm_text(device, title, detail)
    spoken = " ".join(text.split())
    if len(spoken) > MAX_TEXT_CHARS:
        spoken = spoken[:MAX_TEXT_CHARS]

    escalate_calls = bool(extra.get("escalate_calls", True))
    clear_actions = list(extra.get("clear_actions") or [])
    escalate_delay = _safe_seconds(extra.get("escalate_delay_seconds"), 0)
    hysteresis = _safe_seconds(extra.get("hysteresis_seconds"), 0)
    sms_phones = contacts["sms_phones"]
    call_phones = contacts["call_phones"]
    channels = {
        "email": "email" in wanted and bool(contacts["emails"]),
        "telegram": "telegram" in wanted and bool(contacts["telegram_chat_ids"]),
        "sms": "sms" in wanted and bool(sms_phones),
        "call": "call" in wanted and contacts["calling_enabled"] and bool(call_phones),
    }
    results = {}
    ack_token = secrets.token_urlsafe(16)

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
            "ack_token": ack_token,
            "contacts": {
                "emails": contacts["emails"],
                "telegram_chat_ids": contacts["telegram_chat_ids"],
                "phones": sms_phones or call_phones,
                "sms_phones": sms_phones,
                "call_phones": call_phones,
                "phone": contacts["phone"],
                "calling_enabled": contacts["calling_enabled"],
            },
            "channels": channels,
            "results": results,
            "steps": [],
            "escalate_calls": escalate_calls,
            "escalate_delay_seconds": escalate_delay,
            "hysteresis_seconds": hysteresis,
            "clear_actions": clear_actions,
            **extra,
        },
    )
    db.session.add(event)
    db.session.flush()

    ack_url = public_ack_url(event.id, ack_token)
    notify_text = text + "\n\n" + ACK_HINT
    if ack_url:
        notify_text += f"\n{ack_url}"
    sms_text = _sms_with_ack_link(spoken, ack_url)

    if channels["email"]:
        if not contacts["smtp_ready"]:
            results["email"] = "skipped: SMTP no configurado"
            append_alarm_step(event, channel="email", to=contacts["emails"], status="skipped", detail="SMTP no configurado")
        else:
            try:
                send_email(
                    contacts["emails"],
                    f"[CallOnFail] Alarma #{event.id} - {device.name}",
                    notify_text,
                    extra_headers={
                        "Message-ID": f"<alarm-{event.id}-{ack_token}@callonfail.com.ar>",
                    },
                )
                results["email"] = f"sent:{len(contacts['emails'])}"
                append_alarm_step(event, channel="email", to=contacts["emails"], status="sent")
            except Exception as exc:
                logger.exception("Alarm email failed device=%s", device.device_uid)
                results["email"] = f"error: {exc}"
                append_alarm_step(event, channel="email", to=contacts["emails"], status="error", detail=str(exc))
    elif "email" in wanted:
        results["email"] = "skipped: sin email del cliente"
        append_alarm_step(event, channel="email", to=[], status="skipped", detail="sin email del cliente")

    if channels["telegram"]:
        if not contacts["telegram_ready"]:
            results["telegram"] = "skipped: TELEGRAM_BOT_TOKEN no configurado"
            append_alarm_step(event, channel="telegram", to=contacts["telegram_chat_ids"], status="skipped", detail="bot no configurado")
        else:
            try:
                send_telegram(contacts["telegram_chat_ids"], notify_text, button_url=ack_url)
                results["telegram"] = f"sent:{len(contacts['telegram_chat_ids'])}"
                append_alarm_step(event, channel="telegram", to=contacts["telegram_chat_ids"], status="sent")
            except Exception as exc:
                logger.exception("Alarm telegram failed device=%s", device.device_uid)
                results["telegram"] = f"error: {exc}"
                append_alarm_step(event, channel="telegram", to=contacts["telegram_chat_ids"], status="error", detail=str(exc))
    elif "telegram" in wanted:
        results["telegram"] = "skipped: sin chat de Telegram"
        append_alarm_step(event, channel="telegram", to=[], status="skipped", detail="sin chat de Telegram")

    if channels["sms"]:
        sms_states = []
        for index, phone in enumerate(sms_phones):
            job = enqueue_modem_job(
                device,
                "test_sms",
                {
                    "phone": phone,
                    "text": sms_text,
                    "alarm_event_id": event.id,
                    "phone_index": index,
                },
                source="alarm",
            )
            sms_states.append(job.status)
            append_alarm_step(
                event,
                channel="sms",
                to=phone,
                status=job.status,
                detail=job.result or "",
                command_id=job.command_id,
            )
        results["sms"] = _mqtt_note(contacts, ",".join(sms_states))
    elif "sms" in wanted:
        results["sms"] = "skipped: sin telefono del cliente"
        append_alarm_step(event, channel="sms", to=[], status="skipped", detail="sin telefono del cliente")

    if channels["call"]:
        job = enqueue_modem_job(
            device,
            "test_call",
            {
                "phone": call_phones[0],
                "text": spoken,
                "alarm_event_id": event.id,
                "phone_index": 0,
                "escalate_calls": escalate_calls,
            },
            source="alarm",
        )
        results["call"] = _mqtt_note(contacts, job.status)
        append_alarm_step(
            event,
            channel="call",
            to=call_phones[0],
            status=job.status,
            detail=job.result or "",
            command_id=job.command_id,
        )
        if escalate_calls and len(call_phones) > 1:
            results["call_cascade"] = f"escala {len(call_phones)} telefonos si no atienden (espera {escalate_delay}s)"
    elif "call" in wanted and not contacts["calling_enabled"]:
        results["call"] = "skipped: llamadas deshabilitadas en el dispositivo"
        append_alarm_step(event, channel="call", to=call_phones, status="skipped", detail="llamadas deshabilitadas")
    elif "call" in wanted:
        results["call"] = "skipped: sin telefono del cliente"
        append_alarm_step(event, channel="call", to=[], status="skipped", detail="sin telefono del cliente")

    pump_modem_queue(device)

    event.payload = {**(event.payload or {}), "results": results}
    flag_modified(event, "payload")
    if not channels["call"]:
        schedule_alarm_rearm(event)
    return event


def notify_alarm_cleared(device: Device, event: Event, telemetry: dict | None = None) -> None:
    payload = event.payload or {}
    wanted = [item for item in (payload.get("clear_actions") or []) if item in {"email", "telegram", "sms"}]
    if not wanted:
        return
    stored = payload.get("contacts") or {}
    contacts = resolve_contacts(device, selection=payload)
    emails = stored.get("emails") or contacts["emails"]
    chats = stored.get("telegram_chat_ids") or contacts["telegram_chat_ids"]
    phones = stored.get("sms_phones") or stored.get("phones") or contacts["sms_phones"]
    detail = payload.get("detail") or event.message or "alarma"
    value = ""
    if telemetry is not None:
        value = str(sensor_value(telemetry, str(payload.get("sensor_id") or "")) or "")
    text = build_alarm_text(
        device,
        "Alarma normalizada",
        f"{detail}" + (f" (ahora {value})" if value else ""),
    )
    via = []
    errors = []
    if "email" in wanted and emails and smtp_configured():
        try:
            send_email(emails, f"[CallOnFail] Normalizada - {device.name}", text)
            via.append("email")
        except Exception as exc:
            logger.exception("Clear email failed device=%s", device.device_uid)
            errors.append(str(exc))
    if "telegram" in wanted and chats and telegram_configured():
        try:
            send_telegram(chats, text)
            via.append("telegram")
        except Exception as exc:
            logger.exception("Clear telegram failed device=%s", device.device_uid)
            errors.append(str(exc))
    if "sms" in wanted and phones:
        spoken = " ".join(text.split())[:160]
        for index, phone in enumerate(phones):
            job = enqueue_modem_job(
                device,
                "test_sms",
                {
                    "phone": phone,
                    "text": spoken,
                    "alarm_event_id": event.id,
                    "phone_index": index,
                    "clear_notice": True,
                },
                source="alarm_clear",
            )
            append_alarm_step(event, channel="sms", to=phone, status=job.status, detail="normalizacion", command_id=job.command_id)
        via.append("sms")
        pump_modem_queue(device)
    status = "error" if errors and not via else "sent"
    append_alarm_step(
        event,
        channel="clear",
        to=via or wanted,
        status=status,
        detail="; ".join(errors),
    )


def _safe_duration(rule: dict) -> int:
    return _safe_seconds(rule.get("duration_seconds"), 0)


def _safe_seconds(value, default: int = 0) -> int:
    try:
        return max(0, min(86400, int(value)))
    except (TypeError, ValueError):
        return default


def _rearm_key(device_id: int, key: str) -> tuple[str, str]:
    return f"{device_id}:{key}", f"cof:rearm:{device_id}:{key}"


def set_rearm(device_id: int, key: str, until_ts: float | None) -> None:
    mem_key, redis_key = _rearm_key(device_id, key)
    client = _redis_client()
    if until_ts is None:
        _rearm_until.pop(mem_key, None)
        if client is not None:
            try:
                client.delete(redis_key)
            except Exception:
                logger.exception("Redis rearm clear failed")
        return
    _rearm_until[mem_key] = until_ts
    if client is not None:
        try:
            ttl = max(1, int(until_ts - time.time()) + 5)
            client.set(redis_key, str(until_ts), ex=ttl)
        except Exception:
            logger.exception("Redis rearm set failed")


def get_rearm(device_id: int, key: str) -> float | None:
    mem_key, redis_key = _rearm_key(device_id, key)
    client = _redis_client()
    if client is not None:
        try:
            existing = client.get(redis_key)
            if existing:
                value = float(existing)
                _rearm_until[mem_key] = value
                return value
        except Exception:
            logger.exception("Redis rearm get failed")
    return _rearm_until.get(mem_key)


def _parse_iso(value):
    if not value:
        return None
    try:
        dt = datetime.fromisoformat(str(value).replace("Z", "+00:00"))
    except ValueError:
        return None
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return dt


def evaluate_device_rules(device: Device, telemetry: dict) -> list[Event]:
    config = latest_config_payload(device)
    rules = config.get("rules") or []
    if not rules:
        return []

    fired: list[Event] = []
    now_ts = time.time()
    now = datetime.now(timezone.utc)
    for rule in rules:
        if not isinstance(rule, dict):
            continue
        key = rule_key(rule)
        holds = condition_holds(rule, telemetry)
        hysteresis = _safe_seconds(rule.get("hysteresis_seconds"), 0)
        if holds is None:
            continue
        if not holds:
            _condition_since(device.id, key, now_ts, False)
            open_event = open_alarm_event(device.id, key)
            if open_event is not None:
                from .modem_queue import cancel_alarm_jobs

                cancel_alarm_jobs(open_event.id, reason="cleared")
                clear_alarm(open_event, telemetry, device)
                if hysteresis > 0:
                    set_rearm(device.id, key, now_ts + hysteresis)
            continue

        open_event = open_alarm_event(device.id, key)
        if open_event is not None:
            payload = open_event.payload or {}
            rearm_at = _parse_iso(payload.get("rearm_at"))
            if rearm_at is not None and now >= rearm_at:
                open_event.cleared_at = utcnow()
                recycled = dict(payload)
                recycled["cleared_at"] = open_event.cleared_at.isoformat()
                recycled["recycle_reason"] = "hysteresis"
                open_event.payload = recycled
                flag_modified(open_event, "payload")
            else:
                continue

        rearm = get_rearm(device.id, key)
        if rearm is not None and now_ts < rearm:
            continue

        since = _condition_since(device.id, key, now_ts, True)
        duration = _safe_duration(rule)
        if since is None or (now_ts - since) < duration:
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
                "escalate_calls": bool(rule.get("escalate_calls", True)),
                "escalate_delay_seconds": _safe_seconds(rule.get("escalate_delay_seconds"), 0),
                "hysteresis_seconds": hysteresis,
                "email_contact_ids": _id_list(rule.get("email_contact_ids")),
                "sms_contact_ids": _id_list(rule.get("sms_contact_ids")),
                "call_contact_ids": _id_list(rule.get("call_contact_ids")),
                "clear_actions": [item for item in (rule.get("clear_actions") or ["email", "telegram"]) if item in {"email", "telegram", "sms"}],
            },
            config=config,
        )
        set_rearm(device.id, key, None)
        fired.append(event)
    return fired
