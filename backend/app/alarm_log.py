from sqlalchemy.orm.attributes import flag_modified

from .models import Event, utcnow

CALL_ANSWERED_PREFIXES = ("Call done",)
CALL_ESCALATE_PREFIXES = (
    "Call no answer",
    "Call rejected",
    "Call dial",
    "Call no carrier",
    "Call not connected",
    "timeout",
)


def append_alarm_step(event: Event, *, channel: str, to, status: str, detail: str = "", command_id: str | None = None) -> dict:
    payload = dict(event.payload or {})
    steps = list(payload.get("steps") or [])
    targets = to if isinstance(to, list) else [to] if to else []
    step = {
        "at": utcnow().isoformat(),
        "channel": channel,
        "to": [str(item) for item in targets if item],
        "status": status,
        "detail": detail,
        "command_id": command_id,
    }
    steps.append(step)
    payload["steps"] = steps
    event.payload = payload
    flag_modified(event, "payload")
    return step


def update_alarm_step(event: Event, command_id: str | None, status: str, detail: str = "") -> dict | None:
    if not command_id:
        return None
    payload = dict(event.payload or {})
    steps = list(payload.get("steps") or [])
    for step in reversed(steps):
        if step.get("command_id") == command_id:
            step["status"] = status
            step["detail"] = detail
            step["finished_at"] = utcnow().isoformat()
            payload["steps"] = steps
            event.payload = payload
            flag_modified(event, "payload")
            return step
    return None


def call_outcome(result: str) -> str:
    text = (result or "").strip()
    if any(text.startswith(prefix) for prefix in CALL_ANSWERED_PREFIXES):
        return "answered"
    if any(text.lower().startswith(prefix.lower()) for prefix in CALL_ESCALATE_PREFIXES):
        return "no_answer"
    return "error"


def friendly_step(step: dict) -> str:
    channel = step.get("channel") or ""
    status = step.get("status") or ""
    targets = ", ".join(step.get("to") or [])
    detail = (step.get("detail") or "").strip()

    if channel == "email":
        if status.startswith("error") or status.startswith("skipped"):
            return f"No se pudo enviar email a {targets or 'nadie'}: {detail or status}"
        return f"Enviamos email a {targets or 'los destinatarios del cliente'}"
    if channel == "telegram":
        if status.startswith("error") or status.startswith("skipped"):
            return f"No se pudo avisar por Telegram: {detail or status}"
        return f"Avisamos al grupo de Telegram {targets}".strip()
    if channel == "sms":
        if status == "queued":
            return f"SMS en cola para {targets}"
        if status == "sent":
            return f"Enviando SMS a {targets}"
        if status.startswith("error") or status.startswith("skipped") or status == "failed":
            return f"SMS a {targets} falló: {detail or status}"
        if (detail or "").startswith("SMS sent"):
            return f"Enviamos SMS a {targets}"
        return f"SMS a {targets}: {detail or status}"
    if channel == "call":
        outcome = call_outcome(detail or status)
        if status == "queued":
            return f"Llamada en cola a {targets}"
        if status == "sent":
            return f"Llamando a {targets}"
        if outcome == "answered":
            return f"Atendió {targets}"
        if outcome == "no_answer":
            return f"No atendió {targets}" + (f" ({detail})" if detail and not detail.startswith("Call no answer") else "")
        return f"Llamada a {targets}: {detail or status}"
    if channel == "clear":
        if status.startswith("error"):
            return f"No se pudo avisar la normalización: {detail or status}"
        via = targets or "los canales configurados"
        return f"La alarma se normalizó. Avisamos por {via}"
    if channel == "ack":
        if status == "notified":
            return f"Avisamos que se detuvo el escalamiento por {targets or 'los canales configurados'}"
        return f"Se detuvo el escalamiento ({detail or 'OK'})"
    if status == "cancelled":
        return f"Cancelamos {channel} a {targets}: {detail or status}"
    return detail or f"{channel} {status}"
