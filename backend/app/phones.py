import re


def normalize_phone(phone: str) -> str:
    return "".join(ch for ch in (phone or "").strip() if ch.isdigit() or ch == "+")


def is_e164_phone(phone: str) -> bool:
    return bool(re.fullmatch(r"\+[1-9]\d{7,14}", phone or ""))


def normalize_telegram_chat_id(value: str) -> str:
    return (value or "").strip().replace(" ", "")


def is_telegram_chat_id(value: str) -> bool:
    return bool(re.fullmatch(r"-?\d{5,20}", value or ""))


def normalize_email(value: str) -> str:
    return (value or "").strip()


def is_email(value: str) -> bool:
    return bool(re.fullmatch(r"[^@\s]+@[^@\s]+\.[^@\s]+", value or ""))
