# Voice and SMS — validated lab (2026-09-04)

This is the working reference for outgoing **test calls** and **test SMS**
on the current lab device. Follow this before changing radio, audio, or Claro
settings.

Validated on:

```text
Device:     cof-test
Hardware:   WT32-ETH01 + A7672
SIM:        Claro Argentina (operator 722310)
Firmware:   0.2.42 (inbound SMS for OK ack, plus command_id on call/SMS result)
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

From `firmware/include/cof_config.h`:

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

1. Download TTS AMR (test call only).
2. `AT+CFUN=4` → wait → `AT+CFUN=1` (RF bounce).
3. Wait until `CREG` / radio online / `CPAS` idle / CSQ valid.
4. One `ATD<E.164>;` (semicolon required for voice).
5. Wait for connect URCs (ignore early CSFB `NO CARRIER` for ~40 s).
6. `AT+CCMXPLAY="C:/tts.amr",1,0` (remote path = into the call).
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

Claro CSFB event table, traces and what we can mark:
[docs/operators/claro-ar.md](operators/claro-ar.md).

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

## Test call audio (AMR-NB)

The text field on the device page is used for **SMS and spoken call audio**.
SMS still caps at 160 characters. Call TTS accepts up to **800 characters**
(the per-rule alarm call text caps at 400; see `docs/ops/NOTIFICATIONS.md`).

Backend (`backend/app/tts.py`):

1. Piper `es_AR-daniela-high` (español argentino; baked into the web image).
2. `ffmpeg` → **AMR-NB 8 kHz, 12.2 kbps** (`#!AMR\n`). About 1.5 KB/s vs 16 KB/s WAV.
3. File lives in `/tmp/cof-tts/<id>.amr` for 15 minutes.
4. Public URL: `https://app.callonfail.com.ar/audio/tmp/<id>.amr`

ESP32 RAM still buffers the whole file (`kMaxAudioBytes` 180 KB). AMR at 12.2 kbps
fits ~**2 minutes** of speech; 800 characters is typically under a minute.

### The audio download needs lwIP, so it needs Ethernet or WiFi

`uploadAudioToModem()` fetches the AMR with `WiFiClientSecure` + `HTTPClient`
**before dialing**. On a site whose only path is LTE, MQTT rides the modem's
`AT+CIPOPEN` socket, which is not a lwIP interface, so there is no route for
`HTTPClient` and the download always fails.

Since **0.2.61** an alarm call does not die there: it falls back to the canned
asset on the modem (`C:/cof_fallback.wav`, announced by `ota/manifest.json`) and
dials anyway, reporting `TTS unavailable, using fallback`. Without that asset the
call returns the download error and **no call is placed**. The fallback is a
frozen generic phrase: the firmware has no TTS, so it cannot speak the site name
or the measured value. See `docs/ops/NOTIFICATIONS.md`.

### How much audio fits, and where the limit really is

The modem's C: is the roomy part: the A76XX manual documents a total of ~11 MB
(`AT+FSMEM` -> `+FSMEM: C:(11348480,2201600)` in the vendor example). At AMR-NB
12.2 kbps that is **hundreds** of short assets. Storage was never the constraint.

The constraint is **RAM on the ESP32 during the call**, because
`uploadAudioToModem()` buffers the entire file in one `malloc`. Two ceilings:

| Ceiling | Value | Why |
|---|---|---|
| Download | 240 KB | `PCNT` advances in 512-byte blocks, so the transfer stops at 480 blocks |
| Modem upload | ~10 s of speech per burst | `PCNT` is a `uint16`, max 65,535; sent in 15 KB chunks |

240 KB of AMR-NB 12.2 kbps is ~161 s of speech. The per-rule `call_text` caps at
400 characters, which is typically 60-80 s, so there is roughly 2x headroom.

### Voice quality: what can actually move

AMR-NB is a narrowband codec by definition, and the backend already encodes at
its highest mode (12.2 kbps, `backend/app/tts.py`) from Piper's
`es_AR-daniela-high`, the high-quality model. So:

- **Raising the bitrate does nothing.** 12.2 kbps is the top AMR-NB mode.
- **AMR-WB** (50-7000 Hz instead of 300-3400) is the only real bandwidth
  improvement. The modem's audio application note lists AMR and 8 kHz/16-bit WAV
  for remote playback and does **not** document AMR-WB; the carrier also has to
  negotiate it. Untested, and worth a bench check before designing around it.
- **What is cheap and helps:** prosody (`TTS_LENGTH_SCALE`,
  `TTS_SENTENCE_SILENCE`), loudness normalization before encoding (AMR punishes
  quiet input), and trying another Piper voice of the same language.

**Do not** use the modem's own TTS (`AT+CTTS`) as a higher-quality path: on the
A76XX it supports **Chinese and English only**, per its audio application note.
It cannot speak Spanish.

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
  "audio_url": "https://app.callonfail.com.ar/audio/tmp/<32-hex>.amr",
  "audio_format": "amr_nb_8000"
}
```

Device downloads over Ethernet (HTTPS, cert not verified) into RAM, deletes any
previous `C:/tts.amr` (`AT+FSDEL`), uploads with `AT+CFTRANRX`, plays remote,
then restores the previous modem audio path. If this fails, the event message
is specific (`TTS HTTP 404`, `TTS too large`, `TTS no RAM`, etc.).

Admin test-call **bypasses** `calling.enabled`. Alarm-driven calls must not.

The canned `cof_test.wav` on the modem stays WAV; only spoken test-call audio
is AMR. The alarm call fallback (`C:/cof_fallback.wav`) is WAV too, and is
announced by `manifest.json` instead of being compiled in.

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
docker compose up -d caddy
```

Firmware OTA reads the GitHub manifest, then downloads
`https://app.callonfail.com.ar/ota/firmware.bin` from this VPS (same host as TTS).
Do that deploy **before** pressing OTA on a 0.2.30 device.

Device must already be on firmware `>= 0.2.31` (OTA) to download AMR `audio_url`.
If ACK is `unsupported`, OTA first.

## Still pending (do not mix with this win)

1. MQTT TLS + per-device passwords — only with Serial access to the ESP32.

Alarmas por regla, email, Telegram y **Disparar esta alarma** estan en
`docs/ops/NOTIFICATIONS.md`. El telefono/email/chat van en el cliente.
