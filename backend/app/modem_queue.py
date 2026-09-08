import logging
import uuid
from datetime import datetime, timedelta, timezone

from sqlalchemy.orm.attributes import flag_modified

from .alarm_log import append_alarm_step, call_outcome, update_alarm_step
from .extensions import db
from .models import Device, DeviceModemJob, Event, utcnow
from .mqtt_util import publish_mqtt
from .tts import public_audio_url, synthesize_call_audio

logger = logging.getLogger("callonfail.modem_queue")

MAX_QUEUED_JOBS = 16
CALL_TIMEOUT = timedelta(minutes=4)
SMS_TIMEOUT = timedelta(minutes=2)

# Progress MQTT from firmware (publishTestCallProgress). The job finishes on
# the later Call done / Call no answer / … result, not on these.
CALL_PROGRESS_PREFIXES = (
    "Downloading",
    "Preparing",
    "Dialing",
    "Locking",
    "Retrying",
    "Resetting",
    "Timeout, call already",
    "Timeout, call still",
    "Ringing",
    "Playing audio",
    "Audio finished",
    "Remote hangup",
    "CSFB in progress",
    "Modem:",
)

MODEM_COMMANDS = {"test_call", "test_sms"}


def strip_firmware_suffix(message: str) -> str:
    text = (message or "").strip()
    if " [" in text and text.endswith("]"):
        text = text.rsplit(" [", 1)[0].strip()
    return text


def is_final_call_event(message: str) -> bool:
    text = strip_firmware_suffix(message)
    if not text:
        return False
    if any(text.startswith(prefix) for prefix in CALL_PROGRESS_PREFIXES):
        return False
    return True


def is_final_sms_event(message: str) -> bool:
    return bool(strip_firmware_suffix(message))


def active_modem_jobs(device: Device) -> list[DeviceModemJob]:
    return (
        DeviceModemJob.query.filter(
            DeviceModemJob.device_id == device.id,
            DeviceModemJob.status.in_(("queued", "sent")),
        )
        .order_by(DeviceModemJob.id.asc())
        .all()
    )


def cancel_alarm_jobs(event_id: int, reason: str = "acked") -> int:
    cancelled = 0
    jobs = DeviceModemJob.query.filter_by(status="queued").all()
    now = utcnow()
    for job in jobs:
        payload = job.payload or {}
        if payload.get("alarm_event_id") != event_id:
            continue
        if payload.get("ack_notice") or payload.get("clear_notice"):
            continue
        job.status = "cancelled"
        job.result = "acked"
        job.finished_at = now
        cancelled += 1
        event = Event.query.filter_by(id=event_id).first()
        if event is not None:
            append_alarm_step(
                event,
                channel="call" if job.command == "test_call" else "sms",
                to=payload.get("phone"),
                status="cancelled",
                detail="cancelado por OK" if reason == "acked" else "cancelado: alarma normalizada",
                command_id=job.command_id,
            )
    return cancelled


def schedule_alarm_rearm(event: Event) -> None:
    payload = dict(event.payload or {})
    if payload.get("acked") or event.cleared_at is not None:
        return
    try:
        delay = max(0, min(86400, int(payload.get("hysteresis_seconds") or 0)))
    except (TypeError, ValueError):
        delay = 0
    if delay <= 0:
        return
    payload["rearm_at"] = (utcnow() + timedelta(seconds=delay)).isoformat()
    event.payload = payload
    flag_modified(event, "payload")
    append_alarm_step(event, channel="cycle", to=[], status="rearm", detail=f"se reintenta en {delay}s si sigue mal")


def pump_due_modem_jobs() -> int:
    device_ids = {
        job.device_id
        for job in DeviceModemJob.query.filter(DeviceModemJob.status.in_(("queued", "sent"))).all()
    }
    pumped = 0
    for device_id in device_ids:
        device = Device.query.get(device_id)
        if device is None:
            continue
        if pump_modem_queue(device) is not None:
            pumped += 1
    return pumped


def _job_ready(job: DeviceModemJob, now) -> bool:
    raw = (job.payload or {}).get("not_before")
    if not raw:
        return True
    ready_at = _parse_utc(raw)
    return ready_at is None or ready_at <= now


def _parse_utc(value):
    if isinstance(value, datetime):
        dt = value
    else:
        try:
            dt = datetime.fromisoformat(str(value).replace("Z", "+00:00"))
        except ValueError:
            return None
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return dt


def _safe_delay(value) -> int:
    try:
        return max(0, min(3600, int(value)))
    except (TypeError, ValueError):
        return 0


def enqueue_modem_job(device: Device, command: str, extra: dict, source: str = "alarm") -> DeviceModemJob:
    if command not in MODEM_COMMANDS:
        raise ValueError(f"unsupported modem command {command}")

    queued = DeviceModemJob.query.filter_by(device_id=device.id, status="queued").count()
    sent = DeviceModemJob.query.filter_by(device_id=device.id, status="sent").count()
    if queued + sent >= MAX_QUEUED_JOBS:
        job = DeviceModemJob(
            device_id=device.id,
            command=command,
            command_id=str(uuid.uuid4()),
            status="failed",
            payload={"source": source, **(extra or {})},
            result="queue full",
            finished_at=utcnow(),
        )
        db.session.add(job)
        db.session.flush()
        return job

    job = DeviceModemJob(
        device_id=device.id,
        command=command,
        command_id=str(uuid.uuid4()),
        status="queued",
        payload={"source": source, **(extra or {})},
    )
    db.session.add(job)
    db.session.flush()
    return job


def pump_modem_queue(device: Device) -> DeviceModemJob | None:
    jobs = (
        DeviceModemJob.query.filter(
            DeviceModemJob.device_id == device.id,
            DeviceModemJob.status.in_(("queued", "sent")),
        )
        .order_by(DeviceModemJob.id.asc())
        .with_for_update()
        .all()
    )
    now = utcnow()
    in_flight = None
    for job in jobs:
        if job.status != "sent":
            continue
        timeout = CALL_TIMEOUT if job.command == "test_call" else SMS_TIMEOUT
        if job.sent_at and now - job.sent_at > timeout:
            job.status = "failed"
            job.result = "timeout waiting device result"
            job.finished_at = now
            _note_alarm_job(job, job.result)
            _maybe_escalate_call(device, job)
            continue
        in_flight = job
        break

    if in_flight is not None:
        return in_flight

    next_job = next((job for job in jobs if job.status == "queued"), None)
    if next_job is None:
        return None
    if not _job_ready(next_job, now):
        return None

    extra = dict(next_job.payload or {})
    extra.pop("source", None)
    extra.pop("alarm_event_id", None)
    extra.pop("phone_index", None)
    extra.pop("escalate_calls", None)
    extra.pop("clear_notice", None)
    extra.pop("ack_notice", None)
    extra.pop("not_before", None)
    mqtt_payload = {
        "command_id": next_job.command_id,
        "command": next_job.command,
        "device_id": device.device_uid,
        "created_at": now.isoformat(),
        **extra,
    }
    if next_job.command == "test_call":
        text = str(extra.get("text") or "CallOnFail alarma")
        try:
            _path, audio_id = synthesize_call_audio(text)
            mqtt_payload["audio_url"] = public_audio_url(audio_id)
            mqtt_payload["audio_format"] = "amr_nb_8000"
        except Exception as exc:
            logger.exception("Queued TTS failed device=%s", device.device_uid)
            next_job.status = "failed"
            next_job.result = f"TTS failed ({exc})"
            next_job.finished_at = now
            _note_alarm_job(next_job, next_job.result)
            db.session.flush()
            return pump_modem_queue(device)

    try:
        publish_mqtt(f"devices/{device.device_uid}/command", mqtt_payload, qos=1, retain=False)
    except Exception as exc:
        logger.exception("Queued command publish failed device=%s", device.device_uid)
        next_job.status = "failed"
        next_job.result = f"mqtt error: {exc}"
        next_job.finished_at = now
        _note_alarm_job(next_job, next_job.result)
        db.session.flush()
        return pump_modem_queue(device)

    next_job.status = "sent"
    next_job.sent_at = now
    _note_alarm_job(next_job, "sent")
    return next_job


def complete_modem_job(device: Device, event_type: str, message: str, command_id: str | None = None) -> DeviceModemJob | None:
    if event_type == "test_call" and not is_final_call_event(message):
        return None
    if event_type == "test_sms" and not is_final_sms_event(message):
        return None

    job = None
    if command_id:
        job = DeviceModemJob.query.filter_by(device_id=device.id, command_id=command_id).first()
    if job is None:
        job = (
            DeviceModemJob.query.filter_by(device_id=device.id, command=event_type, status="sent")
            .order_by(DeviceModemJob.sent_at.asc())
            .first()
        )
    if job is None:
        return None

    job.status = "done"
    job.result = strip_firmware_suffix(message)[:240]
    job.finished_at = utcnow()
    _note_alarm_job(job, job.result)
    _maybe_escalate_call(device, job)
    return job


def _note_alarm_job(job: DeviceModemJob, result: str) -> None:
    payload = job.payload or {}
    event_id = payload.get("alarm_event_id")
    if not event_id:
        return
    event = Event.query.filter_by(id=event_id).first()
    if event is None:
        return
    event_payload = dict(event.payload or {})
    results = dict(event_payload.get("results") or {})
    key = "call" if job.command == "test_call" else "sms"
    results[key] = result
    event_payload["results"] = results
    event.payload = event_payload
    flag_modified(event, "payload")
    if not update_alarm_step(event, job.command_id, job.status if job.status != "done" else _step_status(job.command, result), result):
        append_alarm_step(
            event,
            channel=key,
            to=payload.get("phone"),
            status=_step_status(job.command, result) if job.status == "done" else job.status,
            detail=result,
            command_id=job.command_id,
        )


def _step_status(command: str, result: str) -> str:
    if command == "test_call":
        return call_outcome(result)
    if (result or "").startswith("SMS sent"):
        return "sent"
    if result in {"queued", "sent"}:
        return result
    return "error" if result else "sent"


def _maybe_escalate_call(device: Device, job: DeviceModemJob) -> None:
    if job.command != "test_call":
        return
    payload = job.payload or {}
    event_id = payload.get("alarm_event_id")
    event = Event.query.filter_by(id=event_id).first() if event_id else None
    if event is None:
        return
    event_payload = event.payload or {}
    if event.cleared_at is not None:
        return
    if event_payload.get("acked"):
        append_alarm_step(event, channel="ack", to=[], status="acked", detail="escalamiento ya detenido")
        return
    phones = (event_payload.get("contacts") or {}).get("call_phones") or (event_payload.get("contacts") or {}).get("phones") or []
    next_index = int(payload.get("phone_index") or 0) + 1
    if not payload.get("escalate_calls", True) or next_index >= len(phones):
        append_alarm_step(event, channel="call", to=[], status="cycle_done", detail="fin de ciclo de llamadas")
        schedule_alarm_rearm(event)
        return
    next_phone = phones[next_index]
    extra = {
        "phone": next_phone,
        "text": payload.get("text") or event_payload.get("text") or "CallOnFail alarma",
        "alarm_event_id": event.id,
        "phone_index": next_index,
        "escalate_calls": True,
    }
    delay = _safe_delay(event_payload.get("escalate_delay_seconds"))
    if delay > 0:
        extra["not_before"] = (utcnow() + timedelta(seconds=delay)).isoformat()
    next_job = enqueue_modem_job(device, "test_call", extra, source="alarm")
    append_alarm_step(
        event,
        channel="call",
        to=next_phone,
        status=next_job.status,
        detail=next_job.result or "siguiente contacto",
        command_id=next_job.command_id,
    )
