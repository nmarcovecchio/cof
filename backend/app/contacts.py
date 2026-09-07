import uuid

from .phones import is_e164_phone, is_email, is_telegram_chat_id, normalize_email, normalize_phone, normalize_telegram_chat_id


def new_contact_id() -> str:
    return "c_" + uuid.uuid4().hex[:10]


def normalize_contact(raw: dict) -> dict | None:
    if not isinstance(raw, dict):
        return None
    name = str(raw.get("name") or "").strip()
    phone = normalize_phone(str(raw.get("phone") or ""))
    email = normalize_email(str(raw.get("email") or ""))
    chat = normalize_telegram_chat_id(str(raw.get("telegram_chat_id") or ""))
    if phone and not is_e164_phone(phone):
        return None
    if email and not is_email(email):
        return None
    if chat and not is_telegram_chat_id(chat):
        return None
    if not name and not phone and not email and not chat:
        return None
    if not name:
        name = phone or email or chat
    return {
        "id": str(raw.get("id") or new_contact_id()),
        "name": name,
        "phone": phone if is_e164_phone(phone) else "",
        "email": email if is_email(email) else "",
        "telegram_chat_id": chat if is_telegram_chat_id(chat) else "",
    }


def normalize_contact_list(raw) -> list[dict]:
    contacts = []
    seen = set()
    if not isinstance(raw, list):
        return contacts
    for item in raw:
        contact = normalize_contact(item)
        if contact is None or contact["id"] in seen:
            continue
        seen.add(contact["id"])
        contacts.append(contact)
    return contacts


def contacts_from_legacy(tenant) -> list[dict]:
    from .phones import parse_emails, parse_phones

    emails, _ = parse_emails(getattr(tenant, "notify_email", "") or "")
    phones, _ = parse_phones(getattr(tenant, "phone", "") or "")
    contacts = []
    count = max(len(emails), len(phones), 1 if emails or phones else 0)
    for index in range(count):
        contacts.append(
            {
                "id": new_contact_id(),
                "name": f"Contacto {index + 1}",
                "phone": phones[index] if index < len(phones) else "",
                "email": emails[index] if index < len(emails) else "",
                "telegram_chat_id": "",
            }
        )
    return [item for item in (normalize_contact(c) for c in contacts) if item]


def tenant_contacts(tenant) -> list[dict]:
    if tenant is None:
        return []
    stored = normalize_contact_list(getattr(tenant, "contacts", None))
    if stored:
        return stored
    return contacts_from_legacy(tenant)


def sync_legacy_fields(tenant, contacts: list[dict]) -> None:
    tenant.contacts = contacts
    tenant.notify_email = "\n".join(item["email"] for item in contacts if item.get("email")) or None
    tenant.phone = "\n".join(item["phone"] for item in contacts if item.get("phone")) or None


def tenant_telegram_chats(tenant) -> list[str]:
    from .phones import parse_telegram_chats

    chats, _ = parse_telegram_chats(getattr(tenant, "telegram_chat_id", "") or "")
    if chats:
        return chats
    found = []
    for item in normalize_contact_list(getattr(tenant, "contacts", None)):
        chat = item.get("telegram_chat_id") or ""
        if chat and chat not in found:
            found.append(chat)
    return found


def contacts_by_id(contacts: list[dict]) -> dict[str, dict]:
    return {item["id"]: item for item in contacts}


def pick_contacts(contacts: list[dict], ids: list[str] | None, field: str | None = None) -> list[dict]:
    by_id = contacts_by_id(contacts)
    if ids is None:
        picked = list(contacts)
    else:
        picked = [by_id[item] for item in ids if item in by_id]
    if field:
        picked = [item for item in picked if item.get(field)]
    return picked
