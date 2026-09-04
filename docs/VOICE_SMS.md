# Voice and SMS — validated lab (2026-09-04)

This is the working reference for outgoing **test calls** and **test SMS**
on the current lab device. Follow this before changing radio, audio, or Claro
settings.

Validated on:

```text
Device:     cof-test
Hardware:   WT32-ETH01 + A7672
SIM:        Claro Argentina (operator 722310)
Firmware:   0.2.30
MQTT:       mqtt.callonfail.com.ar:1883 (anonymous, no TLS)
Web:        https://app.callonfail.com.ar/devices/cof-test
```

Both **Probar SMS** and **Probar llamada** succeeded from the web on this setup.

## What not to change without a new live test

- Do not default MQTT to port `8883` / TLS until the broker actually serves TLS.
- Do not dial CSFB on a stale LTE attach as the first `ATD`.
- Do not rely on `AT+CTTS` (on-modem TTS). Many A7672 builds return `ERROR`.
- Do not treat `device.status == online` in the DB as live. The UI uses
  `last_seen_at` (fresh if younger than 3 minutes).
- Keep `COF_ENABLE_CALLS` at `0` in firmware. Admin test-call/SMS bypass it.
  Automatic alarm calls still require `calling.enabled=true` in device config.

## Claro / modem config (compiled in firmware)

From `include/cof_config.h`:

```text
APN       internet.claro.com.ar
APN user  clarogprs
APN pass  clarogprs777
SMSC      +5491115030500
```

Idle radio stays automatic (`AT+CNMP=2`) so LTE is used for SMS and signal.
Voice-centric LTE: `AT+CEMODE=1`, `AT+CEVDP=3`, `AT+CAVIMS=1`.

VoLTE (`CIREG` / IMS registered) was **not** the working path on this SIM.
The working voice path is **CS after a radio bounce**, then one dial.

## Voice call sequence (must keep)

On LTE without IMS, the first `ATD` usually returns `NO CARRIER`. That is not
a “retry that sometimes works”: the CS domain was not ready. Firmware 0.2.27+
does this **before** the first dial:

1. Download TTS WAV (test call only).
2. `AT+CFUN=4` → wait → `AT+CFUN=1` (RF bounce).
3. Wait until `CREG` / radio online / `CPAS` idle / CSQ valid.
4. One `ATD<E.164>;` (semicolon required for voice).
5. Wait for connect URCs (ignore early CSFB `NO CARRIER` for ~40 s).
6. `AT+CCMXPLAY="C:/tts.wav",1,0` (remote path = into the call).
7. Hang up, restore `AT+CNMP=2` and packet/SMS services.

If that still fails, fallback is lock GSM (`AT+CNMP=13`), dial once, then
restore LTE.

Progress events on the device page (in order on a good call):

```text
Downloading TTS audio
Preparing CS radio
Dialing, waiting for voice
Call done [...]
```

`Call done` means the remote side **answered** (`+CLCC` state 0). CSFB
`LTE->GSM` or `+COLP` alone is not success: Claro often emits those while the
phone is still ringing. Unanswered calls must show `Call no answer` or
`Call ringing timeout`, not `Call done`.

## Test SMS

Web publishes:

```json
{
  "command": "test_sms",
  "device_id": "cof-test",
  "phone": "+549...",
  "text": "CallOnFail prueba de llamada"
}
```

Firmware waits until the SMS stack is ready (`CMGF` / `CPMS`), keeps the SIM
SMSC, then `AT+CMGS`. Result event `SMS sent` is success.

After a failed CSFB call, firmware restores LTE + SMS bearer so SMS still
works. Do not poll `AT` during an active CSFB in a way that aborts the call.

## Test call audio (WAV first; AMR later)

The text field on the device page is used for **SMS and spoken call audio**.

Backend (`backend/app/tts.py`):

1. `espeak-ng -v es` (robotic Spanish; good enough for lab).
2. `ffmpeg` → **8 kHz, 16-bit, mono WAV** (same format the modem already plays).
3. File lives in `/tmp/cof-tts/<id>.wav` for 15 minutes.
4. Public URL: `https://app.callonfail.com.ar/audio/tmp/<id>.wav`

VPS `.env` must include:

```text
PUBLIC_BASE_URL=https://app.callonfail.com.ar
```

MQTT command:

```json
{
  "command": "test_call",
  "device_id": "cof-test",
  "phone": "+549...",
  "text": "CallOnFail prueba de llamada",
  "audio_url": "https://app.callonfail.com.ar/audio/tmp/<32-hex>.wav",
  "audio_format": "wav_pcm_8000_mono_16bit"
}
```

Device downloads over Ethernet (HTTPS, cert not verified) into RAM, deletes any
previous `C:/tts.wav` (`AT+FSDEL`), uploads with `AT+CFTRANRX`, plays remote,
then restores the previous modem audio path. If this fails, the event message
is specific (`TTS HTTP 404`, `TTS modem prompt fail`, etc.), not a generic
`TTS audio fail`.

Admin test-call **bypasses** `calling.enabled`. Alarm-driven calls must not.

AMR is a later optimization (smaller UART transfer). Do not switch until a
real call plays an AMR file on this same A7672.

## Web / MQTT ops

- Commands: `devices/<id>/command` QoS 1, not retained.
- ACK: `devices/<id>/ack`.
- Call/SMS result: `devices/<id>/event` types `test_call` / `test_sms`.
- Cellular card is **not live** unless `last_seen_at` is < 3 minutes old.
  Stale LTE/CSQ is shown as last report, not “Conectado”.
- Firmware OTA still only on web **OTA**, Serial `o`, or long button press.
- MQTT silence: no successful publish for 3 min → reconnect; 6 min →
  `ESP.restart()`. A WT32-ETH01 LAN8720 PHY hang may still need a **power
  cycle** (PHY reset is not wired to a GPIO).

## After deploy

```bash
cd /opt/callonfail
git pull
docker compose up -d --build web
```

Device must already be on firmware `>= 0.2.28` (OTA) to download `audio_url`.
If ACK is `unsupported`, OTA first.

## Still pending (do not mix with this win)

1. Alarm rule evaluation in `mqtt_worker`.
2. Email (Flask SMTP) and Telegram bot notifications.
3. MQTT TLS + per-device passwords — only with Serial access to the ESP32.
4. AMR TTS instead of WAV.
