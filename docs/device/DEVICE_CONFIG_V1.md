# Device configuration contract v1

This document defines the first CallOnFail device configuration contract.

The backend owns the desired configuration. The device stores the latest valid
configuration locally and reports what it applied.

## MQTT topics

For a device with ID `cof-000001`:

```text
devices/cof-000001/telemetry
devices/cof-000001/event
devices/cof-000001/status
devices/cof-000001/config/desired
devices/cof-000001/config/reported
devices/cof-000001/command
devices/cof-000001/ack
```

The `device_id` must match the backend device record created in `/devices/new`.
For lab testing, the seeded/default device is:

```text
cof-test
```

Recommended QoS:

```text
telemetry          QoS 0
event              QoS 1
status             QoS 1
config/desired     QoS 1, retained
config/reported    QoS 1
command            QoS 1
ack                QoS 1
```

## Desired config

Example payload published by the backend to:

```text
devices/cof-000001/config/desired
```

```json
{
  "schema_version": 1,
  "config_version": 1,
  "device_id": "cof-000001",
  "telemetry_interval_seconds": 60,
  "sensors": [
    {
      "id": "temp_1",
      "name": "Temperatura freezer",
      "type": "temperature",
      "enabled": true,
      "source": "ds18b20"
    },
    {
      "id": "temp_2",
      "name": "Temperatura ambiente",
      "type": "temperature",
      "enabled": true,
      "source": "sht31_temperature"
    },
    {
      "id": "humidity_1",
      "name": "Humedad ambiente",
      "type": "humidity",
      "enabled": true,
      "source": "sht31_humidity"
    },
    {
      "id": "mains_1",
      "name": "Red electrica",
      "type": "mains_voltage",
      "enabled": true,
      "source": "zmpt101b"
    },
    {
      "id": "water_1",
      "name": "Fuga de agua",
      "type": "water_leak",
      "enabled": true,
      "source": "input_1",
      "mode": "normally_open"
    }
  ],
  "outputs": [
    {
      "id": "output_1",
      "name": "Sirena",
      "type": "relay",
      "enabled": true
    },
    {
      "id": "output_2",
      "name": "Salida auxiliar",
      "type": "relay",
      "enabled": true
    }
  ],
  "calling": {
    "enabled": false,
    "max_attempts_per_alarm": 0,
    "notes": "Enable only for customers that want phone calls."
  },
  "audio": [
    {
      "id": "test_call",
      "enabled": false,
      "description": "Audio de prueba. El archivo debe validarse con una llamada real.",
      "url": "https://ota.callonfail.com.ar/audio/test_call.wav",
      "sha256": "",
      "modem_path": "C:/test_call.wav",
      "format": "wav_pcm_8000_mono_16bit"
    }
  ],
  "contacts": [
    {
      "id": "owner",
      "name": "Responsable",
      "phone": "+549XXXXXXXXXX"
    },
    {
      "id": "backup",
      "name": "Backup",
      "phone": "+549XXXXXXXXXX"
    }
  ],
  "rules": [
    {
      "id": "high_temperature",
      "enabled": true,
      "condition": {
        "sensor": "temp_1",
        "operator": ">",
        "value": 40,
        "duration_seconds": 60
      },
      "flow": "temperature_alarm"
    }
  ],
  "flows": [
    {
      "id": "temperature_alarm",
      "steps": [
        {
          "type": "call",
          "contact": "owner",
          "audio": "test_call"
        },
        {
          "type": "wait",
          "seconds": 120
        },
        {
          "type": "call",
          "contact": "backup",
          "audio": "test_call"
        },
        {
          "type": "output",
          "output": "output_1",
          "state": true,
          "seconds": 30
        }
      ]
    }
  ]
}
```

## Calling and audio model

Keep these concepts separate:

### Modem capability

Reported by firmware as telemetry/status:

```json
{
  "modem_ready": true,
  "audio_play_supported": true,
  "file_transfer_supported": true
}
```

This only means the modem supports the AT commands. It does **not** prove that
the customer wants calls enabled or that the audio was heard in a real call.

### Customer/device configuration

Controlled from the web:

```json
{
  "calling": {
    "enabled": false,
    "max_attempts_per_alarm": 0
  },
  "audio": [
    {
      "id": "test_call",
      "enabled": false,
      "url": "https://ota.callonfail.com.ar/audio/test_call.wav",
      "sha256": "",
      "modem_path": "C:/test_call.wav"
    }
  ]
}
```

**`max_attempts_per_alarm` is dead.** Only `calling.enabled` is read by the
firmware (`firmware/src/ota_config.cpp`, `doc["calling"]["enabled"]`). Nothing
reads `max_attempts_per_alarm`: retries are driven by the per-rule
escalate/delay settings on the server. The web form still shows a "Máx. intentos
por alarma" field for it and the JS still writes the value, so it looks
configurable, but changing it has no effect.

If `calling.enabled` is false, the device must not place phone calls even if the
modem is ready. Alarm email, Telegram chat ID and phone number are stored on
the tenant (see `docs/ops/NOTIFICATIONS.md`). Device `notifications` in this JSON
is a leftover override only.

Audio assets should be dynamic. A customer may have no audio assets, one shared
test audio, or different audios per alarm flow.

**This `audio` array is dead in the current firmware.** No code reads it:
`firmware/src/ota_config.cpp` only reads `audio` from the **manifest**
(`checkManifest`), which is where the fallback asset is announced. The array is
still emitted by `default_device_config()` in `backend/app/main.py`, so it shows
up in every config payload, but changing it does nothing. The fallback is
provisioned from `ota/manifest.json` -> `ota/audio/cof_fallback.wav` ->
`C:/cof_fallback.wav`. Do not design against this array; use the manifest.

**Current implementation (0.2.64):** rules with a `call_text` get a
**pre-recorded** audio asset, synthesized once at config save and stored
content-addressed (`<sha256>.amr`) in `CALL_AUDIO_DIR`. The config carries a
`call_audio` block per rule with the stable URL and the modem path
`C:/a_<sha16>.amr`, and the device downloads it on config apply. The call then
plays that **local** file, so once synced the call no longer depends on the
network at alarm time. Two rules with the same text share one file.

**Caveat:** the download at config-apply time still uses `HTTPClient` and so
still needs lwIP (Ethernet/WiFi). A site with LAN wins - the audio is already on
the modem when the alarm fires, even if the link has since dropped. A site whose
only uplink is LTE still cannot fetch it, and keeps playing the generic fallback
until the modem's own HTTP path is implemented (see `docs/ops/BACKLOG.md` 8c).

`{valor}` is **retired**. Every placeholder is resolved at save time now: the
reading used to be the one value that did not exist until the alarm fired, which
forced a synthesis and a download during the alarm and made the offline fallback
say "un valor fuera de rango" - a phrase that is often false, since a rule can
fire on `menor que` or on a manual test with no reading at all. A number that
matters is written into the text and baked in like any other word; the exact
reading of each event travels by SMS and email. A rule saved before the change
that still carries `{valor}` is played without it and flagged in the config form.
`{umbral}` is known at save time and **is** baked into the audio. See
`docs/ops/NOTIFICATIONS.md` § "Texto de la llamada, por regla".

The `audio` array in this document still describes the modem **fallback** asset
(`C:/cof_fallback.wav`), the last resort when neither the pre-recorded nor the
downloaded AMR is available. The ESP32 cannot synthesize speech, so the fallback
is a frozen generic phrase.

Rule audio lives under its own `a_` namespace and is the only thing a config
sync will delete; the fallback and any unknown file are never touched. See
`syncRuleAudio()` in `firmware/src/ota_config.cpp`.

Note on the modem's own TTS (`AT+CTTS`): the A76XX audio application note says it
supports **Chinese and English only**, so it is not usable for Spanish call audio
and must not be designed around as a dynamic-audio path.

**Limits (measured, see `docs/voice/VOICE_SMS.md`):** this unit reports
`C:(4194304,1146880)` - 4.00 MiB total, 2.91 MiB free, which is 63% smaller than
the vendor manual's ~10.8 MiB example. Never size from that example. The free
space still holds ~199 assets of 10 s, so storage is not the constraint. The
constraint is ESP32 RAM during the call: `uploadAudioToModem()` buffers the whole
file in one `malloc`, capping a transfer at **180 KB** (`kMaxAudioBytes = 180000`,
`firmware/src/sms_voice.cpp`) - ~118 s of AMR-NB 12.2 kbps, against the ~53 s a
400-character `call_text` measures at. Voice quality cannot be raised within AMR-NB
(already at its 12.2 kbps top mode); only AMR-WB would widen the band, and it is
not documented as supported on this modem.

### Runtime status

Reported by firmware/backend after real operations:

```json
{
  "calling_enabled": false,
  "audio_assets": [
    {
      "id": "test_call",
      "modem_path": "C:/test_call.wav",
      "sync_status": "not_required",
      "last_sync_at": null,
      "last_play_result": null,
      "validated_by_real_call": false
    }
  ]
}
```

Recommended status values:

```text
not_required
pending
synced
sync_failed
play_command_ok
play_command_failed
validated_by_real_call
```

The product should show audio as "validated" only after a real call test proves
that the remote side heard the expected audio.

## Reported config

After validating and storing the desired config, the device publishes:

```text
devices/cof-000001/config/reported
```

```json
{
  "schema_version": 1,
  "device_id": "cof-000001",
  "config_version": 1,
  "applied": true,
  "config_hash": "sha256-of-canonical-config",
  "firmware": "0.1.4",
  "message": "config applied"
}
```

If config cannot be applied:

```json
{
  "schema_version": 1,
  "device_id": "cof-000001",
  "config_version": 1,
  "applied": false,
  "error": "unsupported sensor source input_3"
}
```

## Telemetry

The device publishes telemetry to:

```text
devices/cof-000001/telemetry
```

```json
{
  "device_id": "cof-000001",
  "firmware": "0.1.4",
  "uptime_seconds": 12345,
  "ip": "192.168.1.10",
  "ethernet": true,
  "wifi": true,
  "network": {
    "active": "ethernet",
    "ethernet": { "up": true, "ip": "192.168.1.10" },
    "wifi": {
      "configured": true,
      "up": true,
      "ssid": "RedDelSitio",
      "ip": "192.168.1.20",
      "rssi": -62
    },
    "lte": { "up": false, "ip": "-" }
  },
  "temperature_1": 24.8,
  "temperature_2": 25.1,
  "humidity": 52.0,
  "mains_voltage": 221.4,
  "water_leak": false,
  "input_1": false,
  "input_2": false,
  "output_1": false,
  "output_2": false,
  "modem_ready": true,
  "lte_signal": 18
}
```

## Hardware profile and discovery

Devices publish capabilities and discovered resources in `status` messages.
These payloads are retained and event-driven, not high-frequency telemetry.
Devices publish them:

- when MQTT connects,
- when Ethernet or WiFi gains or loses an IP,
- when hardware discovery changes,
- when the backend sends `status_report`.

Ethernet is the default route when both LAN links are up. WiFi is a backup
path. If Ethernet and WiFi are down, MQTT uses LTE data on the A7672.
Do **not** put WiFi passwords in retained `config/desired`; use the
`set_wifi` / `clear_wifi` commands.

Example:

```json
{
  "device_id": "cof-test",
  "status": "online",
  "firmware": "0.2.4",
  "ip": "192.168.1.10",
  "ethernet": true,
  "wifi": true,
  "network": {
    "active": "ethernet",
    "ethernet": { "up": true, "ip": "192.168.1.10" },
    "wifi": {
      "configured": true,
      "up": true,
      "ssid": "RedDelSitio",
      "ip": "192.168.1.20",
      "rssi": -62
    },
    "lte": { "up": false, "ip": "-" }
  },
  "hardware_profile": "cof-wt32-a7672-v1",
  "capabilities": {
    "ethernet": true,
    "wifi": true,
    "modem_a7672": true,
    "phone_calls": true,
    "audio_playback": true,
    "modem_file_transfer": true,
    "sht31": true,
    "ds18b20_bus": true,
    "max_ds18b20": 8,
    "mains_voltage": true,
    "pcf8574": true,
    "external_inputs": 2,
    "external_outputs": 2
  },
  "discovered": {
    "sht31": true,
    "pcf8574": true,
    "modem": true,
    "cellular": {
      "registered": true,
      "radio": "LTE",
      "model": "A7672SA-FASE",
      "ims": false,
      "csq": 31,
      "operator": "722310",
      "voice": {
        "path": "csfb",
        "last_ok": "csfb",
        "cs_attached": true,
        "radio": "LTE",
        "ims": false
      }
    },
    "ds18b20_count": 2,
    "ds18b20": [
      "28FF641D4C1603A1",
      "28FF882B2D1804B3"
    ]
  }
}
```

The backend stores this on the device record and uses it to guide dynamic
configuration. The UI should eventually let users map discovered physical
resources to logical sensors, for example:

```text
28FF641D4C1603A1 -> Freezer 1
28FF882B2D1804B3 -> Freezer 2
input_1          -> Fuga de agua
output_1         -> Sirena
```

## Archived devices

Devices should be archived instead of hard-deleted.

If an archived device continues publishing MQTT:

- the backend keeps the device archived,
- normal telemetry is not stored,
- an `archived_device_message` warning event is recorded,
- config and command actions are disabled until the device is restored.

This prevents a retired/deleted device from being recreated as active just
because it is still powered on and publishing.

To reuse an archived physical device for another customer, restore/reassign it:

```text
/devices?include_archived=1
-> Editar/reasignar
-> select new tenant/site
-> Guardar y restaurar
```

The same `device_id` is preserved, so the firmware does not need to change if it
continues using that MQTT identity.

## Events

The device publishes alarm/state changes to:

```text
devices/cof-000001/event
```

```json
{
  "device_id": "cof-000001",
  "type": "temperature_high",
  "severity": "alarm",
  "message": "Temperatura freezer mayor a 40 C",
  "sensor": "temp_1",
  "value": 42.5,
  "threshold": 40
}
```

## Commands

The backend can publish commands to:

```text
devices/cof-000001/command
```

### Force OTA check

Firmware updates should be explicit commands, not automatic background updates.
The device can still poll the manifest for auxiliary metadata, but firmware
upgrade should happen through this command, Serial `o`, or a physical
maintenance action.

```json
{
  "command_id": "uuid",
  "command": "ota_check",
  "device_id": "cof-000001",
  "created_at": "2026-09-03T00:00:00Z"
}
```

### Force status/capabilities report

```json
{
  "command_id": "uuid",
  "command": "status_report",
  "device_id": "cof-000001",
  "created_at": "2026-09-03T00:00:00Z"
}
```

The device responds with an ACK and publishes a fresh retained `status` message
including `hardware_profile`, `capabilities`, `discovered`, and `network`.

### Set WiFi credentials

Install with Ethernet first, then publish this from the device page. QoS 1, **not
retained**. The firmware stores SSID/password in NVS (same keys as Serial
`wifi`) and associates in STA mode. The ACK and the backend Event store the
SSID only; `password` is stored as a boolean.

```json
{
  "command_id": "uuid",
  "command": "set_wifi",
  "device_id": "cof-000001",
  "ssid": "RedDelSitio",
  "password": "...",
  "created_at": "2026-09-12T00:00:00Z"
}
```

### Clear saved WiFi

```json
{
  "command_id": "uuid",
  "command": "clear_wifi",
  "device_id": "cof-000001",
  "created_at": "2026-09-12T00:00:00Z"
}
```

The device forgets NVS credentials and disconnects WiFi. Ethernet is unchanged.

### Test call with audio

An explicit operator action from the device page. The backend synthesizes the
form text to AMR-NB 8 kHz (`docs/voice/VOICE_SMS.md`) and the device
downloads `audio_url` into `C:/tts.amr` before dialing.

This command may place a call even if `calling.enabled` is false. Automatic
alarm calls still require `calling.enabled=true`.

```json
{
  "command_id": "uuid",
  "command": "test_call",
  "device_id": "cof-000001",
  "phone": "+5491112345678",
  "text": "CallOnFail prueba de llamada",
  "audio_url": "https://app.callonfail.com.ar/audio/tmp/<32-hex>.amr",
  "audio_format": "amr_nb_8000",
  "created_at": "2026-09-04T00:00:00Z"
}
```

The device ACKs immediately, downloads TTS audio when `audio_url` is present,
prepares CS radio if VoLTE is not registered, then publishes an `event` of type
`test_call`. A result that starts with `Call done` is success, even when
radio/IMS/CEER details are appended. `discovered.cellular.voice.path` is
inferred live from IMS, radio and `CREG`: `volte`, `csfb`, `gsm`, `lte_data`,
or `none`. A new IMSI/operator clears the last learned path so another SIM is
re-diagnosed.

### Test SMS

```json
{
  "command_id": "uuid",
  "command": "test_sms",
  "device_id": "cof-000001",
  "phone": "+5491112345678",
  "text": "CallOnFail prueba SMS",
  "created_at": "2026-09-03T00:00:00Z"
}
```

The device ACKs immediately, sends the SMS with `AT+CMGF=1` / `AT+CMGS`,
then publishes an `event` of type `test_sms` with the result.

The device responds to:

```text
devices/cof-000001/ack
```

```json
{
  "device_id": "cof-000001",
  "command_id": "uuid",
  "command": "ota_check",
  "status": "accepted",
  "message": "OTA check scheduled",
  "firmware": "0.2.3"
}
```

## Local vs cloud behavior

The cloud is the source of truth for desired configuration, but devices must
keep the latest valid config locally and continue critical alarm behavior when
offline. WiFi credentials are an exception: they live only on the device NVS
and are set with `set_wifi` / `clear_wifi` (or Serial), never in retained
`config/desired`.

The backend should show:

```text
desired config version
reported config version
applied/pending/error
last config report time
```

## MVP web test flow

1. Open the device page:

   ```text
   http://<VPS_STATIC_IP>/devices/cof-test
   ```

2. Click `Editar/publicar configuracion`.
3. Save the JSON.
4. The backend stores a new `device_configs` row.
5. The backend publishes the payload retained to:

   ```text
   devices/cof-test/config/desired
   ```

6. Firmware receives the config, stores `config_version` and `config_hash`, then publishes:

   ```text
   devices/cof-test/config/reported
   ```

7. The MQTT worker stores the report and updates the device's reported config
   version.
8. Refresh the device page and verify:

   ```text
   Config deseada/reportada: <same version> / <same version>
   ```
