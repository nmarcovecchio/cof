import logging
import uuid
from datetime import timedelta

from sqlalchemy.orm.attributes import flag_modified

from .extensions import db
from .models import Device, DeviceModemJob, Event, utcnow
from .mqtt_util import publish_mqtt
from .tts import public_audio_url, synthesize_call_audio

logger = logging.getLogger("callonfail.modem_queue")

MAX_QUEUED_JOBS = 8
CALL_TIMEOUT = timedelta(minutes=4)
SMS_TIMEOUT = timedelta(minutes=2)

CALL_PROGRESS_PREFIXES = (
    "Downloading",
    "Preparing",
    "Dialing",
    "Locking",
    "Retrying",
    "Resetting",
    "Timeout, call already",
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
            continue
        in_flight = job
        break

    if in_flight is not None:
        return in_flight

    next_job = next((job for job in jobs if job.status == "queued"), None)
    if next_job is None:
        return None

    extra = dict(next_job.payload or {})
    extra.pop("source", None)
    extra.pop("alarm_event_id", None)
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
