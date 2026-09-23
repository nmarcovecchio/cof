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

**Provisioning a fallback also needs lwIP.** The manifest that announces the asset
is fetched with the same `HTTPClient`, so a site with no Ethernet and no WiFi
cannot download the fallback either. Such a unit must be provisioned **while it
still has LAN** (at the bench, before installation); it cannot self-provision over
LTE. See `docs/ops/BACKLOG.md` 8c.

### How much audio fits, and where the limit really is

Measured on the lab unit (`AT+FSMEM`, firmware 0.2.62, see **Sondear modem**):
`C:(4194304,1146880)` - a **4.00 MiB** total, 1.09 MiB used, **2.91 MiB free**.
Note this is **63% smaller** than the ~10.8 MiB in the vendor manual's example, so
never size anything from that example. At AMR-NB 12.2 kbps the free space is
**~33 minutes** of speech, i.e. **~199** assets of 10 s or **~24-33** of the
~53 s a 400-character `call_text` measures at (a placeholder-heavy 89-character
sample encoded to 11.8 s). Storage is not the constraint, and the
design keeps hundreds of short assets available.

The constraint is **RAM on the ESP32 during the call**, because
`uploadAudioToModem()` buffers the entire file in one `malloc`. That single cap is
the real ceiling:

| Ceiling | Value | Why |
|---|---|---|
| Whole file in RAM | 180 KB | `kMaxAudioBytes = 180000` (`firmware/src/sms_voice.cpp`); a larger `Content-Length` is rejected with `TTS too large` before reading |
| Modem upload | 512-byte chunks | `AT+CFTRANRX` declares the full size, then the buffer is written in 512-byte chunks with a 1 ms delay |

180 KB of AMR-NB 12.2 kbps is **~118 s** of speech. The per-rule `call_text` caps
at 400 characters, which measures at ~0.13 s/char (~53 s for a placeholder-heavy
text), so there is roughly 2x headroom.

Neither ceiling is a modem `PCNT` limit: `PCNT` appears nowhere in the firmware,
and there is no `uint16` counter in this path. The cap is the ESP32 buffer.

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

**Implemented in the encoder (`_encode_amr_nb` in `backend/app/tts.py`):**

- `dynaudnorm=p=0.9:m=10` — loudness normalization. This is the change that is
  actually audible: the codec is fixed, but a quiet recording sounds much worse
  after AMR than a normalized one.
- `adelay=400:all=1` — 400 ms of leading silence. `AT+CCMXPLAY` starts as soon
  as the call is answered, so without this the greeting clips the first word.
  Verified with ffmpeg: a 1.000 s input becomes 1.400 s.

Both are applied to every asset, including the generic variants.

**Do not** use the modem's own TTS (`AT+CTTS`) as a higher-quality path: on the
A76XX it supports **Chinese and English only**, per its audio application note.
It cannot speak Spanish.

VPS `.env` must include:

```text
PUBLIC_BASE_URL=https://app.callonfail.com.ar
CALL_AUDIO_DIR=/opt/cof-audio
```

`CALL_AUDIO_DIR` is the permanent store of pre-recorded alarm audio. Unlike
`TTS_DIR` it is **never** pruned by age: an asset must still be there months
after the rule was saved.

MQTT command:

```json
{
  "command": "test_call",
  "device_id": "cof-test",
  "phone": "+549...",
  "text": "Alarma en Camara 1: DS18B20 paso el limite de -18 grados, valor actual -25 grados.",
  "audio_url": "https://app.callonfail.com.ar/audio/tmp/<32-hex>.amr",
  "audio_format": "amr_nb_8000",
  "call_audio": {
    "text_sha256": "<64-hex>",
    "url": "https://app.callonfail.com.ar/audio/asset/<64-hex>.amr",
    "modem_path": "C:/a_<16-hex>.amr",
    "dynamic": true
  }
}
```

`call_audio` is the pre-recorded asset for that rule, and it is **always
self-contained**: every placeholder is resolved at save time, so the device plays
the local file and downloads nothing at call time.

`dynamic` is part of the payload but is a fixed `false` now. It stays because the
device parses it and a unit on older firmware uses it to decide whether the local
file is complete; it no longer varies, because `{valor}` was retired. See
`NOTIFICATIONS.md` § "`{valor}` esta retirado".

Device downloads over **lwIP (Ethernet or WiFi)** - HTTPS, cert not verified - into
RAM, deletes any previous `C:/tts.amr` (`AT+FSDEL`), uploads with `AT+CFTRANRX`,
plays remote, then restores the previous modem audio path. LTE does not work here:
MQTT rides the modem's `AT+CIPOPEN` socket, which is not an lwIP interface, so
`HTTPClient` has no route. If this fails, the event message is specific
(`TTS HTTP 404`, `TTS too large`, `TTS no RAM`, `TTS no network`, etc.).

Rule audio files use their own namespace, `C:/a_<sha16>.amr`, and are the only
names the device will ever delete during a config sync. See
`syncRuleAudio()` in `firmware/src/ota_config.cpp`.

`syncRuleAudio()` reports one `call_audio` event per sync (not per file, so a
config with many rules cannot flood the log), with the counts of assets
downloaded, pruned and failed. It only publishes when something changed. This is
the only remote visibility into the audio sync on a unit with no serial access;
without it the whole sync printed to a port nobody can reach. The device page
renders it as `audio ok` / `audio con fallas` / `audio podado`.

Admin test-call **bypasses** `calling.enabled`. Alarm-driven calls must not.

The canned `C:/cof_fallback.wav` on the modem stays WAV; it is the last-resort
fallback and is announced by `manifest.json` instead of being compiled in. The
per-rule audios are AMR. `C:/cof_test.wav` is historical: `COF_MODEM_AUDIO_PATH`
no longer points at it.

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
