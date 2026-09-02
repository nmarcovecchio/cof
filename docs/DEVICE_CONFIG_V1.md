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
  "audio": [
    {
      "id": "test_call",
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

## Local vs cloud behavior

The cloud is the source of truth for desired configuration, but devices must
keep the latest valid config locally and continue critical alarm behavior when
offline.

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
