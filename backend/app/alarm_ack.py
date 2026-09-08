import imaplib
import logging
import os
import re
import secrets
from datetime import timedelta
from email import message_from_bytes
from email.header import decode_header
from email.utils import parseaddr

from sqlalchemy.orm.attributes import flag_modified

from .alarm_log import append_alarm_step
from .contacts import tenant_contacts, tenant_telegram_chats
from .extensions import db
from .models import Device, Event, utcnow
from .modem_queue import cancel_alarm_jobs
from .notify import send_telegram, telegram_configured
from .phones import normalize_email, normalize_phone, phones_match

logger = logging.getLogger("callonfail.alarm_ack")

ACK_WORD = re.compile(r"\bok\b", re.IGNORECASE)
EVENT_IN_SUBJECT = re.compile(r"alarma\s*#\s*(\d+)", re.IGNORECASE)
TOKEN_IN_TEXT = re.compile(r"/(?:alarms/(\d+)/ack|a/(\d+))/([A-Za-z0-9_\-]+)")

ACK_VIA = {
    "email": "el enlace del email",
    "email_reply": "un email con OK",
    "sms": "el enlace del SMS",
    "sms_reply": "un SMS con OK",
    "telegram": "Telegram",
    "link": "el enlace de confirmación",
    "web": "la web",
}


def display_actor(name: str, addr: str = "") -> str:
    name = (name or "").strip()
    addr = str(addr or "").strip()
    if name and addr and name != addr:
        return f"{name} ({addr})"
    return name or addr


def actor_name_for(device, *, email: str = "", phone: str = "") -> str:
    if device is None:
        return ""
    email_n = normalize_email(email or "")
    phone_n = normalize_phone(phone or "")
    for contact in tenant_contacts(device.tenant):
        if email_n and normalize_email(contact.get("email") or "") == email_n:
            return str(contact.get("name") or "").strip()
        if phone_n and phones_match(phone_n, contact.get("phone") or ""):
            return str(contact.get("name") or "").strip()
    return ""


def issue_ack_link(event: Event, *, channel: str, to: str = "", name: str = "") -> str:
    token = secrets.token_urlsafe(16)
    payload = dict(event.payload or {})
    links = [item for item in (payload.get("ack_links") or []) if isinstance(item, dict)]
    links.append({"token": token, "channel": channel, "name": name, "to": to})
    payload["ack_links"] = links
    event.payload = payload
    flag_modified(event, "payload")
    return public_ack_url(event.id, token)


def ack_claim_for_token(event: Event, token: str) -> dict:
    payload = event.payload or {}
    for item in payload.get("ack_links") or []:
        if isinstance(item, dict) and item.get("token") == token:
            return item
    return {"channel": "link", "name": "", "to": ""}


def _token_matches(event: Event, token: str) -> bool:
    if not token:
        return False
    payload = event.payload or {}
    if token == str(payload.get("ack_token") or ""):
        return True
    return any(isinstance(item, dict) and item.get("token") == token for item in (payload.get("ack_links") or []))


def is_ack_text(text: str, *, email_body: bool = False) -> bool:
    if not text:
        return False
    body = str(text)
    if email_body:
        kept = []
        for raw in body.replace("\r", "").split("\n"):
            line = raw.strip()
            if not line:
                continue
            lowered = line.lower()
            if line.startswith(">") or lowered.startswith("on ") or lowered.startswith("el "):
                continue
            if lowered.startswith("from:") or lowered.startswith("de:"):
                continue
            if "wrote:" in lowered or "escribió:" in lowered or "escribio:" in lowered:
                continue
            kept.append(line)
        body = "\n".join(kept)
    return bool(ACK_WORD.search(body))


def public_ack_url(event_id: int, token: str) -> str:
    base = os.environ.get("PUBLIC_BASE_URL", "").rstrip("/")
    if not base or not token:
        return ""
    return f"{base}/a/{event_id}/{token}"


def _ids_from_ack_match(match) -> tuple[int, str] | None:
    event_id = match.group(1) or match.group(2)
    token = match.group(3) if match.lastindex and match.lastindex >= 3 else None
    if not event_id or not token:
        return None
    return int(event_id), token


def acknowledge_event(event: Event, *, channel: str, sender: str = "", notify: bool = True) -> bool:
    payload = dict(event.payload or {})
    if event.cleared_at is not None or payload.get("acked"):
        return False

    payload["acked"] = True
    payload["acked_at"] = utcnow().isoformat()
    payload["acked_via"] = channel
    payload["acked_from"] = sender
    hysteresis = _safe_seconds(payload.get("hysteresis_seconds"), 0)
    payload["rearm_at"] = (utcnow() + timedelta(seconds=hysteresis)).isoformat() if hysteresis > 0 else None
    event.payload = payload
    flag_modified(event, "payload")

    if payload.get("source") == "manual":
        event.cleared_at = utcnow()
        payload["cleared_at"] = event.cleared_at.isoformat()
        payload["clear_reason"] = "acked_manual"
        event.payload = payload
        flag_modified(event, "payload")
    cancelled = cancel_alarm_jobs(event.id, reason="acked")
    who = sender or ACK_VIA.get(channel, channel)
    via = ACK_VIA.get(channel, channel)
    detail = f"desde {via}"
    if cancelled:
        detail = f"{detail} ({cancelled} en cola cancelados)"
    append_alarm_step(
        event,
        channel="ack",
        to=who,
        status="acked",
        detail=detail,
    )
    if notify:
        _announce_stopped(event, channel, sender)
    return True


def lookup_ack_link(event_id: int, token: str) -> tuple[Event | None, str]:
    event = Event.query.filter_by(id=event_id, type="alarm").first()
    if event is None:
        return None, "invalid"
    if not _token_matches(event, token):
        return None, "invalid"
    if event.cleared_at is not None:
        return event, "expired"
    return event, "open"


def event_for_ack_token(event_id: int, token: str) -> Event | None:
    event, status = lookup_ack_link(event_id, token)
    if status != "open":
        return None
    return event


def open_device_alarms(event: Event) -> list[Event]:
    return (
        Event.query.filter_by(device_id=event.device_id, type="alarm")
        .filter(Event.cleared_at.is_(None))
        .order_by(Event.started_at.desc())
        .all()
    )


def acknowledge_device_from_event(event: Event, *, channel: str, sender: str = "") -> list[Event]:
    targets = open_device_alarms(event)
    changed = []
    for item in targets:
        if acknowledge_event(item, channel=channel, sender=sender, notify=False):
            changed.append(item)
    if changed:
        _announce_stopped(changed[0], channel, sender, silenced=len(changed))
    return changed


def acknowledge_by_token(event_id: int, token: str, *, channel: str = "link", sender: str = "") -> Event | None:
    event = event_for_ack_token(event_id, token)
    if event is None:
        return None
    claim = ack_claim_for_token(event, token)
    channel = str(claim.get("channel") or channel or "link")
    sender = (claim.get("name") or sender or claim.get("to") or "").strip()
    if not sender:
        sender = "Alguien del grupo Telegram" if channel == "telegram" else "Alguien"
    acknowledge_device_from_event(event, channel=channel, sender=sender)
    return event


def handle_inbound_sms(device: Device, payload: dict) -> None:
    sender = normalize_phone(str(payload.get("from") or payload.get("sender") or ""))
    text = str(payload.get("text") or payload.get("message") or "")
    if not is_ack_text(text):
        return
    event = _open_alarm_for(phone=sender, device=device)
    if event is None:
        return
    acknowledge_device_from_event(
        event,
        channel="sms_reply",
        sender=actor_name_for(device, phone=sender) or sender or "sms",
    )


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
        parsed = _ids_from_ack_match(token_match)
        if parsed is None:
            return False
        event_id, token = parsed
        event = acknowledge_by_token(event_id, token, channel="email", sender=sender)
        return event is not None

    subject_match = EVENT_IN_SUBJECT.search(subject)
    if subject_match:
        event = Event.query.filter_by(id=int(subject_match.group(1)), type="alarm").first()
        if event is not None and _sender_matches(event, email=sender):
            name = actor_name_for(event.device, email=sender) or sender
            return bool(acknowledge_device_from_event(event, channel="email_reply", sender=name))

    refs = " ".join(filter(None, [message.get("In-Reply-To", ""), message.get("References", "")]))
    ref_match = re.search(r"alarm-(\d+)-", refs)
    if ref_match:
        event = Event.query.filter_by(id=int(ref_match.group(1)), type="alarm").first()
        if event is not None:
            name = actor_name_for(event.device, email=sender) or sender
            return bool(acknowledge_device_from_event(event, channel="email_reply", sender=name))

    event = _open_alarm_for(email=sender)
    if event is None:
        return False
    name = actor_name_for(event.device, email=sender) or sender
    return bool(acknowledge_device_from_event(event, channel="email_reply", sender=name))


def _open_alarm_for(*, phone: str = "", email: str = "", chat_id: str = "", device: Device | None = None) -> Event | None:
    query = Event.query.filter_by(type="alarm").filter(Event.cleared_at.is_(None)).order_by(Event.started_at.desc())
    if device is not None:
        query = query.filter_by(device_id=device.id)
    for event in query.limit(40).all():
        payload = event.payload or {}
        if payload.get("acked"):
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
    if chat_id and str(chat_id).strip() in {str(item).strip() for item in tenant_telegram_chats(device.tenant)}:
        return True
    for contact in tenant_contacts(device.tenant):
        if phone and phones_match(phone, contact.get("phone") or ""):
            return True
        if email and normalize_email(contact.get("email") or "").lower() == normalize_email(email).lower():
            return True
    return False


def _announce_stopped(event: Event, channel: str, sender: str, silenced: int = 1) -> None:
    device = Device.query.get(event.device_id)
    if device is None:
        return
    from .alarms import build_alarm_text

    chats = tenant_telegram_chats(device.tenant) or ((event.payload or {}).get("contacts") or {}).get("telegram_chat_ids") or []
    text = build_alarm_text(
        device,
        "Escalamiento detenido",
        f"Recibimos OK por {channel}"
        + (f" de {sender}" if sender else "")
        + f". Se silenciaron {silenced} alarma(s) de {device.name}. Entra a la web para ver que paso.",
    )
    if chats and telegram_configured():
        try:
            send_telegram(chats, text)
            append_alarm_step(event, channel="ack", to=chats, status="notified", detail="aviso al grupo de Telegram")
        except Exception:
            logger.exception("Ack telegram notice failed")
            append_alarm_step(event, channel="ack", to=chats, status="error", detail="no se pudo avisar al grupo")
    else:
        append_alarm_step(event, channel="ack", to=[], status="notified", detail="queda en la web; sin grupo de Telegram")


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
