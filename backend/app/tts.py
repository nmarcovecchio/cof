import os
import subprocess
import time
import uuid
from pathlib import Path

TTS_DIR = Path(os.environ.get("TTS_DIR", "/tmp/cof-tts"))
MAX_AGE_SECONDS = 15 * 60
MAX_TEXT_CHARS = 800
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


def synthesize_call_audio(text: str) -> tuple[Path, str]:
    cleanup_old_audio()
    spoken = " ".join((text or "").split())
    if not spoken:
        spoken = "CallOnFail prueba de llamada"
    if len(spoken) > MAX_TEXT_CHARS:
        spoken = spoken[:MAX_TEXT_CHARS]

    audio_id = uuid.uuid4().hex
    raw_path = TTS_DIR / f"{audio_id}-raw.wav"
    amr_path = TTS_DIR / f"{audio_id}.amr"

    speak = subprocess.run(
        [
            PIPER_BIN,
            "--model",
            PIPER_MODEL,
            "--output_file",
            str(raw_path),
            "--length_scale",
            "1.05",
            "--sentence_silence",
            "0.4",
        ],
        input=spoken.encode("utf-8"),
        capture_output=True,
        timeout=60,
        check=False,
    )
    if speak.returncode != 0 or not raw_path.exists():
        detail = (speak.stderr or speak.stdout).decode("utf-8", errors="replace").strip()
        raise RuntimeError(detail or "piper failed")

    convert = None
    last_error = "ffmpeg failed"
    for encoder in ("libopencore_amrnb", "amr_nb"):
        convert = subprocess.run(
            [
                "ffmpeg",
                "-y",
                "-i",
                str(raw_path),
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
            timeout=20,
            check=False,
        )
        if convert.returncode == 0 and amr_path.exists() and amr_path.stat().st_size > 12:
            break
        last_error = (convert.stderr or convert.stdout).decode("utf-8", errors="replace").strip() or last_error
        amr_path.unlink(missing_ok=True)

    raw_path.unlink(missing_ok=True)
    if convert is None or convert.returncode != 0 or not amr_path.exists():
        raise RuntimeError(last_error)

    header = amr_path.read_bytes()[:6]
    if header != b"#!AMR\n":
        amr_path.unlink(missing_ok=True)
        raise RuntimeError("ffmpeg did not produce AMR-NB")

    return amr_path, audio_id
