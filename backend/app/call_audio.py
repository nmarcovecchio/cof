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

Every placeholder is static now
-------------------------------
This used to carry a "dynamic" mode for ``{valor}``, the reading at the moment
the alarm fires. It was removed on purpose:

- The exact reading forced a synthesis + download **during the alarm**, which is
  the worst possible moment to depend on the VPS, the site's uplink and the
  modem's disk.
- Its offline fallback said "un valor fuera de rango", and that phrase is often
  simply false: a rule can fire on ``menor que``, or on a manual test with no
  reading at all. So the call did not degrade into silence - it degraded into a
  wrong sentence, which is worse.
- The number mixed with a real one: ``{umbral}`` *was* baked in, so the audio
  could say a true threshold and a fabricated reading in the same breath.

The operator now writes the number into the text when the number matters ("el
sensor {sensor} supero los 40 grados"), and it bakes in like any other static
word. The exact reading of each event travels by SMS and email, which need no
synthesis and no download at all.

So a stored asset is always self-contained, and the call needs no network.
"""

import hashlib
import re
from pathlib import Path

from .models import AudioAsset
from .extensions import db
from .tts import AUDIO_STORE_DIR, AUDIO_EXT, synthesize_to_path

# Any {token}: used to detect whether a text still has unresolved placeholders.
_ANY_PLACEHOLDER = re.compile(r"\{[^{}]{0,40}\}")

# Placeholder that used to be resolved at call time. Only referenced to detect
# and reject it in already-saved rules and in the form: it is no longer part of
# the supported set, because it cannot be pre-recorded. See the module docstring.
RETIRED_PLACEHOLDER = "valor"


def has_retired_placeholder(text: str) -> bool:
    """True when a text still uses ``{valor}``.

    Used to warn about rules saved before the placeholder was retired, and to
    reject a save that would silently drop it.
    """
    return bool(
        re.search(r"\{\s*" + RETIRED_PLACEHOLDER + r"\s*\}", text or "", flags=re.IGNORECASE)
    )


def normalize_spoken(text: str) -> str:
    """Collapse whitespace so cosmetic edits do not create a new audio asset."""
    return " ".join((text or "").split())


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

    ``text`` must already have every placeholder resolved by the caller; a
    leftover ``{...}`` is dropped rather than spoken.
    """
    spoken = normalize_spoken(text)
    if not spoken:
        return None

    stored_text = normalize_spoken(drop_unknown_placeholders(spoken))
    if not stored_text:
        return None
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
        # Always False now. The column stays because older rows carry the value
        # and the device contract still has the field; see `dynamic` below.
        has_dynamic=False,
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
        # Kept in the payload as a fixed False. It is part of the config contract
        # the device parses, and a device on older firmware uses it to decide
        # whether to prefer the local file. It no longer varies: every asset is
        # self-contained. Removable only alongside a firmware change that stops
        # reading it.
        "dynamic": False,
        # The name the device must use on the modem filesystem. The ``a_``
        # prefix is the device's garbage-collection namespace: it only ever
        # deletes files starting with it, so nothing else can be swept by
        # accident.
        "modem_path": f"C:/a_{asset.text_sha256[:16]}.{AUDIO_EXT}",
    }


def resolve_static_placeholders(text: str, values: dict) -> str:
    """Fill every placeholder that is known at save time.

    All of them are, now: what the template does not resolve is dropped, so a
    leftover ``{valor}`` never reaches the synthesizer and is never read aloud
    as punctuation. ``has_retired_placeholder`` is what surfaces it to the
    operator instead of letting it vanish quietly.
    """
    resolved = normalize_spoken(text)
    for key, value in values.items():
        if key.lower() == RETIRED_PLACEHOLDER:
            continue
        resolved = re.sub(
            r"\{\s*" + re.escape(key) + r"\s*\}",
            str(value if value is not None else ""),
            resolved,
            flags=re.IGNORECASE,
        )
    return normalize_spoken(resolved)
