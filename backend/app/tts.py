import os
import subprocess
import time
import uuid
from pathlib import Path

TTS_DIR = Path(os.environ.get("TTS_DIR", "/tmp/cof-tts"))
# Permanent store for the audio of alarm rules. Unlike TTS_DIR this is never
# pruned by age: an asset has to still be there months after the rule was
# saved, because the call plays it without synthesizing anything.
AUDIO_STORE_DIR = Path(os.environ.get("CALL_AUDIO_DIR", "/opt/cof-audio"))
MAX_AGE_SECONDS = 15 * 60
MAX_TEXT_CHARS = 800
# Leading silence. AT+CCMXPLAY starts as soon as the call is answered, so
# without this the greeting clips the first word. 400 ms is roughly the gap a
# person leaves before speaking.
LEAD_SILENCE_MS = 400
PIPER_BIN = os.environ.get("PIPER_BIN", "/opt/piper/piper")
PIPER_MODEL = os.environ.get(
    "PIPER_MODEL",
    "/opt/piper-voices/es_AR-daniela-high.onnx",
)
AUDIO_FORMAT = "amr_nb_8000"
AUDIO_EXT = "amr"


def cleanup_old_audio() -> None:
    TTS_DIR.mkdir(parents=True, exist_ok=True)
    now = time.time()
    for path in TTS_DIR.glob("*"):
        if path.suffix.lower() not in {".wav", ".amr"}:
            continue
        try:
            if now - path.stat().st_mtime > MAX_AGE_SECONDS:
                path.unlink(missing_ok=True)
        except OSError:
            pass


def public_audio_url(audio_id: str) -> str:
    base = os.environ.get("PUBLIC_BASE_URL", "").rstrip("/")
    if not base:
        from flask import request

        base = request.host_url.rstrip("/")
    return f"{base}/audio/tmp/{audio_id}.{AUDIO_EXT}"


def _encode_amr_nb(raw_path: Path, amr_path: Path) -> None:
    """Encode a raw WAV to AMR-NB with the quality tweaks applied.

    Loudness normalization is what actually improves perceived quality over a
    phone line - the codec is fixed at 12.2 kbps - and the leading silence stops
    the greeting from being clipped. Both are applied here so every asset gets
    them, including the generic variants.
    """
    filters = []
    if LEAD_SILENCE_MS > 0:
        filters.append(f"adelay={LEAD_SILENCE_MS}:all=1")
    # peak: only attenuate, never boost, so a quiet source is not amplified into
    # clipping; dynaudnorm evens out the level within the phrase.
    filters.append("dynaudnorm=p=0.9:m=10")
    filter_chain = ",".join(filters)

    convert = None
    last_error = "ffmpeg failed"
    for encoder in ("libopencore_amrnb", "amr_nb"):
        convert = subprocess.run(
            [
                "ffmpeg",
                "-y",
                "-i",
                str(raw_path),
                "-af",
                filter_chain,
                "-ac",
                "1",
                "-ar",
                "8000",
                "-c:a",
                encoder,
                "-b:a",
                "12200",
                str(amr_path),
            ],
            capture_output=True,
            timeout=30,
            check=False,
        )
        if convert.returncode == 0 and amr_path.exists() and amr_path.stat().st_size > 12:
            break
        last_error = (convert.stderr or convert.stdout).decode("utf-8", errors="replace").strip() or last_error
        amr_path.unlink(missing_ok=True)

    if convert is None or convert.returncode != 0 or not amr_path.exists():
        raise RuntimeError(last_error)

    header = amr_path.read_bytes()[:6]
    if header != b"#!AMR\n":
        amr_path.unlink(missing_ok=True)
        raise RuntimeError("ffmpeg did not produce AMR-NB")


def synthesize_to_path(text: str, amr_path: Path) -> None:
    """Synthesize ``text`` into exactly ``amr_path``.

    Used by the permanent store, where the caller already decided the filename
    from the text hash. The ephemeral path (``synthesize_call_audio``) keeps its
    own uuid naming because it is served once and deleted.
    """
    spoken = " ".join((text or "").split())
    if not spoken:
        spoken = "CallOnFail prueba de llamada"
    if len(spoken) > MAX_TEXT_CHARS:
        spoken = spoken[:MAX_TEXT_CHARS]

    amr_path.parent.mkdir(parents=True, exist_ok=True)
    raw_path = amr_path.with_suffix(".raw.wav")
    try:
        speak = subprocess.run(
            [
                PIPER_BIN,
                "--model",
                PIPER_MODEL,
                "--output_file",
                str(raw_path),
                "--length_scale",
                os.environ.get("TTS_LENGTH_SCALE", "1.30"),
                "--sentence_silence",
                os.environ.get("TTS_SENTENCE_SILENCE", "0.55"),
            ],
            input=spoken.encode("utf-8"),
            capture_output=True,
            timeout=60,
            check=False,
        )
        if speak.returncode != 0 or not raw_path.exists():
            detail = (speak.stderr or speak.stdout).decode("utf-8", errors="replace").strip()
            raise RuntimeError(detail or "piper failed")
        _encode_amr_nb(raw_path, amr_path)
    finally:
        raw_path.unlink(missing_ok=True)


def synthesize_call_audio(text: str) -> tuple[Path, str]:
    cleanup_old_audio()
    audio_id = uuid.uuid4().hex
    amr_path = TTS_DIR / f"{audio_id}.amr"
    synthesize_to_path(text, amr_path)
    return amr_path, audio_id
