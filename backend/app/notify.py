import json
import os
import smtplib
import ssl
import urllib.error
import urllib.request
from email.message import EmailMessage


def smtp_configured() -> bool:
    return bool(os.environ.get("SMTP_HOST", "").strip() and os.environ.get("SMTP_FROM", "").strip())


def telegram_configured() -> bool:
    return bool(os.environ.get("TELEGRAM_BOT_TOKEN", "").strip())


def send_email(to_addr: str, subject: str, body: str) -> None:
    host = os.environ.get("SMTP_HOST", "").strip()
    port = int(os.environ.get("SMTP_PORT", "587"))
    user = os.environ.get("SMTP_USER", "").strip()
    password = os.environ.get("SMTP_PASSWORD", "").strip()
    mail_from = os.environ.get("SMTP_FROM", "").strip()
    starttls = os.environ.get("SMTP_STARTTLS", "true").lower() != "false"

    if not host or not mail_from:
        raise RuntimeError("SMTP_HOST / SMTP_FROM no estan configurados en el VPS")
    if not to_addr:
        raise RuntimeError("El cliente no tiene email de alerta")

    message = EmailMessage()
    message["From"] = mail_from
    message["To"] = to_addr
    message["Subject"] = subject
    message.set_content(body)

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


def send_telegram(chat_id: str, text: str) -> None:
    token = os.environ.get("TELEGRAM_BOT_TOKEN", "").strip()
    if not token:
        raise RuntimeError("TELEGRAM_BOT_TOKEN no esta configurado en el VPS")
    if not chat_id:
        raise RuntimeError("El cliente no tiene chat de Telegram")

    url = f"https://api.telegram.org/bot{token}/sendMessage"
    payload = json.dumps(
        {
            "chat_id": chat_id,
            "text": text,
            "disable_web_page_preview": True,
        }
    ).encode("utf-8")
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
