import imaplib
import json
import logging
import os
import re
import urllib.error
import urllib.request
from datetime import timedelta
from email import message_from_bytes
from email.header import decode_header
from email.utils import parseaddr

from sqlalchemy.orm.attributes import flag_modified

from .alarm_log import append_alarm_step
from .contacts import tenant_contacts
from .extensions import db
from .models import Device, Event, utcnow
from .modem_queue import cancel_alarm_jobs, pump_modem_queue
from .notify import send_email, send_telegram, smtp_configured, telegram_configured
from .phones import normalize_email, normalize_phone, phones_match

logger = logging.getLogger("callonfail.alarm_ack")

ACK_LINE = re.compile(r"^ok[.!]?$", re.IGNORECASE)
EVENT_IN_SUBJECT = re.compile(r"alarma\s*#\s*(\d+)", re.IGNORECASE)
TOKEN_IN_TEXT = re.compile(r"/alarms/(\d+)/ack/([A-Za-z0-9_\-]+)")

_telegram_offset = 0
_telegram_offset_loaded = False


def is_ack_text(text: str, *, email_body: bool = False) -> bool:
    if not text:
        return False
    for raw in str(text).replace("\r", "").split("\n"):
        line = raw.strip()
        if not line:
            continue
        if email_body:
            lowered = line.lower()
            if line.startswith(">") or lowered.startswith("on ") or lowered.startswith("el "):
                continue
            if lowered.startswith("from:") or lowered.startswith("de:"):
                continue
            if "wrote:" in lowered or "escribió:" in lowered or "escribio:" in lowered:
                continue
        return bool(ACK_LINE.match(line))
    return False


def public_ack_url(event_id: int, token: str) -> str:
    base = os.environ.get("PUBLIC_BASE_URL", "").rstrip("/")
    if not base or not token:
        return ""
    return f"{base}/alarms/{event_id}/ack/{token}"


def acknowledge_event(event: Event, *, channel: str, sender: str = "", notify: bool = True) -> bool:
    payload = dict(event.payload or {})
    if payload.get("acked"):
        return False

    payload["acked"] = True
    payload["acked_at"] = utcnow().isoformat()
    payload["acked_via"] = channel
    payload["acked_from"] = sender
    hysteresis = _safe_seconds(payload.get("hysteresis_seconds"), 0)
    payload["rearm_at"] = (utcnow() + timedelta(seconds=hysteresis)).isoformat() if hysteresis > 0 else None
    event.payload = payload
    flag_modified(event, "payload")

    cancelled = cancel_alarm_jobs(event.id)
    append_alarm_step(
        event,
        channel="ack",
        to=sender or channel,
        status="acked",
        detail=f"{channel} OK" + (f" ({cancelled} en cola cancelados)" if cancelled else ""),
    )
    if notify:
        _announce_stopped(event, channel, sender)
    return True


def acknowledge_by_token(event_id: int, token: str, *, channel: str = "link", sender: str = "") -> Event | None:
    event = Event.query.filter_by(id=event_id, type="alarm").first()
    if event is None:
        return None
    expected = str((event.payload or {}).get("ack_token") or "")
    if not expected or expected != token:
        return None
    acknowledge_event(event, channel=channel, sender=sender or "enlace")
    return event


def handle_inbound_sms(device: Device, payload: dict) -> None:
    sender = normalize_phone(str(payload.get("from") or payload.get("sender") or ""))
    text = str(payload.get("text") or payload.get("message") or "")
    if not is_ack_text(text):
        return
    event = _open_alarm_for(phone=sender, device=device)
    if event is None:
        return
    acknowledge_event(event, channel="sms", sender=sender or "sms")


def poll_telegram_acks() -> int:
    token = os.environ.get("TELEGRAM_BOT_TOKEN", "").strip()
    if not token:
        return 0
    offset = _telegram_update_offset()
    url = f"https://api.telegram.org/bot{token}/getUpdates?timeout=0&limit=20"
    if offset:
        url += f"&offset={offset}"
    try:
        with urllib.request.urlopen(url, timeout=8) as response:
            data = json.loads(response.read().decode("utf-8"))
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as exc:
        logger.warning("Telegram getUpdates failed: %s", exc)
        return 0
    if not data.get("ok"):
        return 0

    handled = 0
    last_id = offset
    for update in data.get("result") or []:
        last_id = max(last_id, int(update.get("update_id") or 0) + 1)
        msg = update.get("message") or update.get("channel_post") or {}
        text = str(msg.get("text") or "")
        chat = msg.get("chat") or {}
        chat_id = str(chat.get("id") or "").strip()
        if not chat_id or not is_ack_text(text):
            continue
        event = _open_alarm_for(chat_id=chat_id)
        if event is None:
            continue
        if acknowledge_event(event, channel="telegram", sender=chat_id):
            handled += 1
    _store_telegram_offset(last_id)
    return handled


def poll_imap_acks() -> int:
    host = os.environ.get("IMAP_HOST", "").strip()
    if not host:
        return 0
    user = (os.environ.get("IMAP_USER") or os.environ.get("SMTP_USER") or "").strip()
    password = (os.environ.get("IMAP_PASSWORD") or os.environ.get("SMTP_PASSWORD") or "").strip()
    if not user or not password:
        return 0
    port = int(os.environ.get("IMAP_PORT", "993"))
    handled = 0
    try:
        mailbox = imaplib.IMAP4_SSL(host, port, timeout=15)
        mailbox.login(user, password)
        mailbox.select("INBOX")
        typ, data = mailbox.search(None, "UNSEEN")
        if typ != "OK":
            mailbox.logout()
            return 0
        for raw_id in (data[0] or b"").split():
            typ, fetched = mailbox.fetch(raw_id, "(RFC822)")
            if typ != "OK" or not fetched or not fetched[0]:
                continue
            raw = fetched[0][1]
            if not isinstance(raw, (bytes, bytearray)):
                continue
            if _ack_from_email(message_from_bytes(raw)):
                handled += 1
        mailbox.logout()
    except Exception:
        logger.exception("IMAP ack poll failed")
    return handled


def _ack_from_email(message) -> bool:
    sender = normalize_email(parseaddr(message.get("From", ""))[1])
    subject = _decode_mime(message.get("Subject", ""))
    body = _email_text(message)
    if not is_ack_text(body, email_body=True):
        return False

    token_match = TOKEN_IN_TEXT.search(body) or TOKEN_IN_TEXT.search(subject)
    if token_match:
        event = acknowledge_by_token(int(token_match.group(1)), token_match.group(2), channel="email", sender=sender)
        return event is not None

    subject_match = EVENT_IN_SUBJECT.search(subject)
    if subject_match:
        event = Event.query.filter_by(id=int(subject_match.group(1)), type="alarm").first()
        if event is not None and _sender_matches(event, email=sender):
            return acknowledge_event(event, channel="email", sender=sender)

    refs = " ".join(filter(None, [message.get("In-Reply-To", ""), message.get("References", "")]))
    ref_match = re.search(r"alarm-(\d+)-", refs)
    if ref_match:
        event = Event.query.filter_by(id=int(ref_match.group(1)), type="alarm").first()
        if event is not None:
            return acknowledge_event(event, channel="email", sender=sender)

    event = _open_alarm_for(email=sender)
    if event is None:
        return False
    return acknowledge_event(event, channel="email", sender=sender)


def _open_alarm_for(*, phone: str = "", email: str = "", chat_id: str = "", device: Device | None = None) -> Event | None:
    query = Event.query.filter_by(type="alarm").filter(Event.cleared_at.is_(None)).order_by(Event.started_at.desc())
    if device is not None:
        query = query.filter_by(device_id=device.id)
    for event in query.limit(40).all():
        payload = event.payload or {}
        if payload.get("acked") or payload.get("source") == "manual":
            continue
        if phone and _sender_matches(event, phone=phone):
            return event
        if email and _sender_matches(event, email=email):
            return event
        if chat_id and _sender_matches(event, chat_id=chat_id):
            return event
    return None


def _sender_matches(event: Event, *, phone: str = "", email: str = "", chat_id: str = "") -> bool:
    contacts = (event.payload or {}).get("contacts") or {}
    if phone and phones_match(phone, contacts.get("sms_phones") or contacts.get("phones") or []):
        return True
    if email:
        wanted = {normalize_email(item).lower() for item in (contacts.get("emails") or []) if item}
        if normalize_email(email).lower() in wanted:
            return True
    if chat_id:
        wanted = {str(item).strip() for item in (contacts.get("telegram_chat_ids") or []) if item}
        if str(chat_id).strip() in wanted:
            return True
    device = Device.query.get(event.device_id)
    if device is None:
        return False
    for contact in tenant_contacts(device.tenant):
        if phone and phones_match(phone, contact.get("phone") or ""):
            return True
        if email and normalize_email(contact.get("email") or "").lower() == normalize_email(email).lower():
            return True
        if chat_id and str(contact.get("telegram_chat_id") or "").strip() == str(chat_id).strip():
            return True
    return False


def _announce_stopped(event: Event, channel: str, sender: str) -> None:
    device = Device.query.get(event.device_id)
    if device is None:
        return
    from .alarms import build_alarm_text
    from .modem_queue import enqueue_modem_job

    contacts = (event.payload or {}).get("contacts") or {}
    text = build_alarm_text(
        device,
        "Escalamiento detenido",
        f"Recibimos OK por {channel}" + (f" de {sender}" if sender else "") + ". No se llaman mas contactos.",
    )
    emails = contacts.get("emails") or []
    chats = contacts.get("telegram_chat_ids") or []
    phones = contacts.get("sms_phones") or contacts.get("phones") or []
    via = []
    if emails and smtp_configured():
        try:
            send_email(emails, f"[CallOnFail] Escalamiento detenido - {device.name}", text)
            via.append("email")
        except Exception:
            logger.exception("Ack email notice failed")
    if chats and telegram_configured():
        try:
            send_telegram(chats, text)
            via.append("telegram")
        except Exception:
            logger.exception("Ack telegram notice failed")
    if phones:
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
                    "ack_notice": True,
                },
                source="alarm_ack",
            )
            append_alarm_step(event, channel="sms", to=phone, status=job.status, detail="aviso detencion", command_id=job.command_id)
        via.append("sms")
        pump_modem_queue(device)
    append_alarm_step(event, channel="ack", to=via, status="notified", detail="se aviso que se detuvo el escalamiento")


def _safe_seconds(value, default: int = 0) -> int:
    try:
        return max(0, min(86400, int(value)))
    except (TypeError, ValueError):
        return default


def _decode_mime(value: str) -> str:
    parts = []
    for text, enc in decode_header(value or ""):
        if isinstance(text, bytes):
            parts.append(text.decode(enc or "utf-8", errors="replace"))
        else:
            parts.append(text)
    return "".join(parts)


def _email_text(message) -> str:
    if message.is_multipart():
        for part in message.walk():
            if part.get_content_type() == "text/plain" and "attachment" not in str(part.get("Content-Disposition") or ""):
                payload = part.get_payload(decode=True) or b""
                charset = part.get_content_charset() or "utf-8"
                return payload.decode(charset, errors="replace")
        return ""
    payload = message.get_payload(decode=True) or b""
    charset = message.get_content_charset() or "utf-8"
    if isinstance(payload, bytes):
        return payload.decode(charset, errors="replace")
    return str(payload or "")


def _telegram_update_offset() -> int:
    global _telegram_offset, _telegram_offset_loaded
    if _telegram_offset_loaded:
        return _telegram_offset
    _telegram_offset_loaded = True
    url = os.environ.get("REDIS_URL")
    if not url:
        return _telegram_offset
    try:
        import redis

        value = redis.Redis.from_url(url, socket_connect_timeout=2, decode_responses=True).get("cof:telegram:offset")
        if value:
            _telegram_offset = int(value)
    except Exception:
        logger.exception("Telegram offset read failed")
    return _telegram_offset


def _store_telegram_offset(value: int) -> None:
    global _telegram_offset
    if value <= _telegram_offset:
        return
    _telegram_offset = value
    url = os.environ.get("REDIS_URL")
    if not url:
        return
    try:
        import redis

        redis.Redis.from_url(url, socket_connect_timeout=2, decode_responses=True).set("cof:telegram:offset", str(value))
    except Exception:
        logger.exception("Telegram offset write failed")
