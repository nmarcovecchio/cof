import re

_SPLIT = re.compile(r"[\n,;]+")


def split_values(value: str) -> list[str]:
    return [part.strip() for part in _SPLIT.split(value or "") if part.strip()]


def join_values(values: list[str]) -> str:
    return "\n".join(values)


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
    if "\n" in (value or "") or "\r" in (value or ""):
        return False
    return bool(re.fullmatch(r"[^@\s]+@[^@\s]+\.[^@\s]+", value or "", flags=re.IGNORECASE))


def parse_emails(value: str) -> tuple[list[str], list[str]]:
    valid: list[str] = []
    invalid: list[str] = []
    for raw in split_values(value):
        email = normalize_email(raw)
        if is_email(email):
            if email not in valid:
                valid.append(email)
        elif email:
            invalid.append(raw)
    return valid, invalid


def parse_phones(value: str) -> tuple[list[str], list[str]]:
    valid: list[str] = []
    invalid: list[str] = []
    for raw in split_values(value):
        phone = normalize_phone(raw)
        if is_e164_phone(phone):
            if phone not in valid:
                valid.append(phone)
        elif raw:
            invalid.append(raw)
    return valid, invalid


def phone_digits(phone: str) -> str:
    return "".join(ch for ch in (phone or "") if ch.isdigit())


def phones_match(incoming: str, stored) -> bool:
    left = phone_digits(incoming)
    if not left:
        return False
    items = stored if isinstance(stored, (list, tuple)) else [stored]
    for item in items:
        right = phone_digits(str(item or ""))
        if not right:
            continue
        if left == right:
            return True
        if len(left) >= 8 and len(right) >= 8 and (left.endswith(right[-8:]) or right.endswith(left[-8:])):
            return True
    return False


def parse_telegram_chats(value: str) -> tuple[list[str], list[str]]:
    valid: list[str] = []
    invalid: list[str] = []
    for raw in split_values(value):
        chat_id = normalize_telegram_chat_id(raw)
        if is_telegram_chat_id(chat_id):
            if chat_id not in valid:
                valid.append(chat_id)
        elif raw:
            invalid.append(raw)
    return valid, invalid
