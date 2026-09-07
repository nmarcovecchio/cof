import json
import os
import smtplib
import ssl
import urllib.error
import urllib.request
from email.message import EmailMessage

TELEGRAM_MAX_CHARS = 4096


def smtp_configured() -> bool:
    return bool(os.environ.get("SMTP_HOST", "").strip() and os.environ.get("SMTP_FROM", "").strip())


def telegram_configured() -> bool:
    return bool(os.environ.get("TELEGRAM_BOT_TOKEN", "").strip())


def _as_list(value) -> list[str]:
    if value is None:
        return []
    if isinstance(value, (list, tuple)):
        return [str(item).strip() for item in value if str(item).strip()]
    item = str(value).strip()
    return [item] if item else []


def send_email(to_addrs, subject: str, body: str, extra_headers: dict | None = None) -> None:
    host = os.environ.get("SMTP_HOST", "").strip()
    port = int(os.environ.get("SMTP_PORT", "587"))
    user = os.environ.get("SMTP_USER", "").strip()
    password = os.environ.get("SMTP_PASSWORD", "").strip()
    mail_from = os.environ.get("SMTP_FROM", "").strip()
    starttls = os.environ.get("SMTP_STARTTLS", "true").lower() != "false"
    recipients = _as_list(to_addrs)

    if not host or not mail_from:
        raise RuntimeError("SMTP_HOST / SMTP_FROM no estan configurados en el VPS")
    if not recipients:
        raise RuntimeError("El cliente no tiene email de alerta")

    message = EmailMessage()
    message["From"] = mail_from
    message["To"] = ", ".join(recipients)
    message["Subject"] = " ".join((subject or "").split())
    message.set_content(body or "")
    for key, value in (extra_headers or {}).items():
        if key and value:
            message[key] = str(value)

    if starttls:
        with smtplib.SMTP(host, port, timeout=20) as smtp:
            smtp.ehlo()
            smtp.starttls(context=ssl.create_default_context())
            smtp.ehlo()
            if user:
                smtp.login(user, password)
            smtp.send_message(message)
        return

    with smtplib.SMTP_SSL(host, port, timeout=20, context=ssl.create_default_context()) as smtp:
        if user:
            smtp.login(user, password)
        smtp.send_message(message)


def send_telegram(chat_ids, text: str, button_url: str = "", button_text: str = "Confirmar y silenciar") -> None:
    token = os.environ.get("TELEGRAM_BOT_TOKEN", "").strip()
    if not token:
        raise RuntimeError("TELEGRAM_BOT_TOKEN no esta configurado en el VPS")
    targets = _as_list(chat_ids)
    if not targets:
        raise RuntimeError("El cliente no tiene chat de Telegram")

    body = text or ""
    if len(body) > TELEGRAM_MAX_CHARS:
        body = body[: TELEGRAM_MAX_CHARS - 1] + "…"

    errors = []
    for chat_id in targets:
        try:
            _send_telegram_one(token, chat_id, body, button_url=button_url, button_text=button_text)
        except Exception as exc:
            errors.append(f"{chat_id}: {exc}")
    if errors:
        raise RuntimeError("; ".join(errors))


def _send_telegram_one(token: str, chat_id: str, text: str, button_url: str = "", button_text: str = "Confirmar y silenciar") -> None:
    url = f"https://api.telegram.org/bot{token}/sendMessage"
    message = {
        "chat_id": chat_id,
        "text": text,
        "disable_web_page_preview": True,
    }
    if button_url.startswith("https://") and button_text:
        message["reply_markup"] = {
            "inline_keyboard": [[{"text": button_text, "url": button_url}]],
        }
    payload = json.dumps(message).encode("utf-8")
    request = urllib.request.Request(
        url,
        data=payload,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=15) as response:
            data = json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"Telegram HTTP {exc.code}: {detail}") from exc
    except urllib.error.URLError as exc:
        raise RuntimeError(f"Telegram no responde: {exc.reason}") from exc

    if not data.get("ok"):
        raise RuntimeError(str(data.get("description") or "Telegram send failed"))
