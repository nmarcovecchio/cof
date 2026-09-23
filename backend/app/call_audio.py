"""Permanent call-audio store, addressed by the content of the spoken text.

Why this exists
---------------
The call used to download a freshly synthesized TTS file at dial time. On a
site whose only uplink is LTE that download cannot happen (HTTPClient needs an
lwIP interface, and MQTT over the modem's AT socket is not one), so the call
fell back to a single canned phrase shared by every rule.

Instead, the text of each rule is synthesized **once, when the config is
saved**, and stored under a filename derived from the text itself. The device
downloads it on config apply and plays it at call time with no network access.

Content addressing also deduplicates: two rules that say the same thing map to
the same ``text_sha256`` and therefore to a single AMR on the server and a
single file on the modem. Without it the same bytes would be copied once per
rule until the 4 MiB modem filesystem filled up.

Static vs dynamic
-----------------
``{valor}`` is the only placeholder that cannot be known at save time: it is
the reading at the moment the alarm fires. A text carrying it is stored as the
*generic* variant, with the reading replaced by a neutral phrase, so the call
can still be made offline. ``{umbral}`` is static - the threshold is set in the
form - so it *is* baked into the audio.
"""

import hashlib
import re
from pathlib import Path

from .models import AudioAsset
from .extensions import db
from .tts import AUDIO_STORE_DIR, AUDIO_EXT, synthesize_to_path

# The only placeholder whose value does not exist until the alarm fires.
DYNAMIC_PLACEHOLDER = "valor"

# What the generic variant says in place of the reading. It has to be a phrase
# that reads naturally over the phone and does not imply a number was spoken.
GENERIC_VALUE_WORDS = "un valor fuera de rango"

# Any {token}: used to detect whether a text still has unresolved placeholders.
_ANY_PLACEHOLDER = re.compile(r"\{[^{}]{0,40}\}")


def normalize_spoken(text: str) -> str:
    """Collapse whitespace so cosmetic edits do not create a new audio asset."""
    return " ".join((text or "").split())


def has_dynamic_placeholder(text: str) -> bool:
    return bool(
        re.search(r"\{\s*" + DYNAMIC_PLACEHOLDER + r"\s*\}", text or "", flags=re.IGNORECASE)
    )


def neutralize_dynamic(text: str) -> str:
    """Replace the runtime reading with a phrase that can be pre-recorded."""
    return re.sub(
        r"\{\s*" + DYNAMIC_PLACEHOLDER + r"\s*\}",
        GENERIC_VALUE_WORDS,
        text or "",
        flags=re.IGNORECASE,
    )


def drop_unknown_placeholders(text: str) -> str:
    """A placeholder we do not know would be read aloud as punctuation."""
    return _ANY_PLACEHOLDER.sub(" ", text or "")


def text_sha256(text: str) -> str:
    return hashlib.sha256(normalize_spoken(text).encode("utf-8")).hexdigest()


def asset_filename(text_sha: str) -> str:
    return f"{text_sha}.{AUDIO_EXT}"


def asset_url(text_sha: str) -> str:
    """Stable public URL. Derived from the hash, so it never changes."""
    import os

    base = os.environ.get("PUBLIC_BASE_URL", "").rstrip("/")
    if not base:
        from flask import request

        base = request.host_url.rstrip("/")
    return f"{base}/audio/asset/{asset_filename(text_sha)}"


def ensure_asset(text: str) -> AudioAsset | None:
    """Synthesize ``text`` into the store once, return its row.

    Returns ``None`` for an empty text: a rule with no call text has nothing to
    pre-record and must keep whatever the generic alarm path does.
    """
    spoken = normalize_spoken(text)
    if not spoken:
        return None

    dynamic = has_dynamic_placeholder(spoken)
    # The audio that actually gets stored never contains {valor}; the exact
    # reading is a runtime concern. Static placeholders are resolved by the
    # caller before we get here.
    stored_text = normalize_spoken(drop_unknown_placeholders(neutralize_dynamic(spoken)))
    text_sha = text_sha256(stored_text)

    existing = AudioAsset.query.filter_by(text_sha256=text_sha).first()
    if existing is not None:
        # Re-synthesize if the file vanished (volume wiped, manual delete).
        path = AUDIO_STORE_DIR / asset_filename(text_sha)
        if path.is_file():
            return existing
        synthesize_to_path(stored_text, path)
        existing.size_bytes = path.stat().st_size
        existing.amr_sha256 = hashlib.sha256(path.read_bytes()).hexdigest()
        db.session.flush()
        return existing

    path = AUDIO_STORE_DIR / asset_filename(text_sha)
    synthesize_to_path(stored_text, path)
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    asset = AudioAsset(
        text_sha256=text_sha,
        text=stored_text,
        amr_sha256=digest,
        size_bytes=path.stat().st_size,
        has_dynamic=dynamic,
    )
    db.session.add(asset)
    db.session.flush()
    return asset


def prepare_call_audio(text: str) -> dict | None:
    """Everything the config needs to describe one rule's call audio.

    Raises on synthesis failure so the save can surface the real error rather
    than shipping a rule whose call would be silent.
    """
    asset = ensure_asset(text)
    if asset is None:
        return None
    return {
        "text_sha256": asset.text_sha256,
        "url": asset_url(asset.text_sha256),
        "bytes": asset.size_bytes,
        "dynamic": asset.has_dynamic,
        # The name the device must use on the modem filesystem. The ``a_``
        # prefix is the device's garbage-collection namespace: it only ever
        # deletes files starting with it, so nothing else can be swept by
        # accident.
        "modem_path": f"C:/a_{asset.text_sha256[:16]}.{AUDIO_EXT}",
    }


def resolve_static_placeholders(text: str, values: dict) -> str:
    """Fill the placeholders that are known at save time.

    ``{valor}`` is deliberately left for the generic-variant pass; anything
    unknown is dropped so it is never read aloud.
    """
    resolved = normalize_spoken(text)
    for key, value in values.items():
        if key.lower() == DYNAMIC_PLACEHOLDER:
            continue
        resolved = re.sub(
            r"\{\s*" + re.escape(key) + r"\s*\}",
            str(value if value is not None else ""),
            resolved,
            flags=re.IGNORECASE,
        )
    return normalize_spoken(resolved)
