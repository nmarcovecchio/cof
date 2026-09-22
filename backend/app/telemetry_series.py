"""Telemetry chart/export helpers.

Sensors are per-device configuration (``cfg["sensors"]``), each with a ``name``
(the alias the operator sees), a ``type``, a ``source`` and - since this module
was added - a ``payload_key`` telling us which field of the telemetry frame
carries the reading.

The alias is what gives a sensor a stable identity across a disconnect. A
sensor that goes away keeps its row in the config, so its line and its CSV
column stay in place - just empty for the period it was not reporting - and
when the hardware comes back the same series starts filling again. Nothing has
to be reconfigured and no historical range silently loses a column.

This lives in the payload rather than in the promoted columns on purpose: the
table only has temperature_1/2, humidity and water_leak, so a renamed or added
sensor has no column at all (see models.Telemetry).

The mains case is special: the firmware publishes ``mains_voltage`` as null
(``firmware/src/mqtt_io.cpp``) and the only real reading is the raw ADC value
``zmpt_raw``, which is what ``mains_1``'s payload_key points at. It is labelled
as uncalibrated when plotted.
"""

from __future__ import annotations

from datetime import datetime, timedelta

# Fallback payload keys by sensor type, for sensors with no payload_key (a
# config stored before the field existed whose id we do not recognise).
FALLBACK_KEYS_BY_TYPE = {
    "temperature": ("temperature_1", "temperature_2", "temp_1", "temp_2"),
    "humidity": ("humidity", "humidity_1"),
    "mains_voltage": ("zmpt_raw", "mains_voltage"),
}

# Types that are present in the config but make no sense as a chart line.
_NON_PLOT_TYPES = {"relay", "digital", "boolean", ""}

# Fixed palette so a series keeps its colour between reloads.
PALETTE = ("#0d6efd", "#dc3545", "#198754", "#fd7e14", "#6f42c1", "#0dcaf0")

# Extra payload columns exported to CSV alongside the configured sensors.
AUX_COLUMNS = (
    ("uptime_seconds", "uptime_s"),
    ("lte_signal", "lte_signal"),
    ("network_ethernet_ok", "ethernet_ok"),
    ("network_wifi_ok", "wifi_ok"),
    ("network_internet_ok", "internet_ok"),
)

# Chart bucket sizes, in seconds, by range width. Keeps the payload the browser
# has to render bounded (~2k points) no matter how long the range is.
_BUCKETS = (
    (timedelta(hours=6), 60),  # up to 6 h  -> 1 min
    (timedelta(days=2), 300),  # up to 2 d  -> 5 min
    (timedelta(days=10), 3600),  # up to 10 d -> 1 h
    (timedelta(days=40), 21600),  # up to 40 d -> 6 h
)


def _as_float(value):
    """Return a float, or None when the value is not usable as a number."""
    if value is None or isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        return float(value)
    if isinstance(value, str):
        try:
            return float(value.strip())
        except ValueError:
            return None
    return None


def payload_keys_for(sensor: dict) -> tuple:
    """Ordered payload keys a configured sensor's reading may live under.

    The explicit ``payload_key`` wins: it is what the operator sees and what
    survives a rename. Only when it is missing do we fall back to the type,
    which is inherently ambiguous between two probes of the same type - so it
    is a compatibility path, not the normal one.
    """
    key = str(sensor.get("payload_key") or "").strip()
    if key:
        return (key,)
    return FALLBACK_KEYS_BY_TYPE.get(str(sensor.get("type") or "").strip(), ())


def is_plottable(sensor: dict) -> bool:
    if str(sensor.get("type") or "").strip() in _NON_PLOT_TYPES:
        return False
    # Connectivity sensors are plain 1/0 flags; they are exported to CSV, not
    # drawn as a line.
    if str(sensor.get("source") or "").startswith("network_"):
        return False
    if not str(sensor.get("id") or "").strip():
        return False
    return bool(payload_keys_for(sensor))


def series_for_device(cfg: dict) -> list[dict]:
    """Chartable series for a device, derived from its own sensor config.

    Every configured sensor yields a series whether or not it is currently
    reporting, so a disconnected probe keeps its place (and its CSV column)
    instead of vanishing from a historical range.
    """
    sensors = cfg.get("sensors") if isinstance(cfg, dict) else None
    series = []
    for sensor in sensors or []:
        if not isinstance(sensor, dict) or not is_plottable(sensor):
            continue
        sensor_type = str(sensor.get("type") or "").strip()
        sensor_id = str(sensor.get("id") or "").strip()
        if sensor_type == "mains_voltage":
            unit = "ADC"
            axis = "mains"
            label = sensor.get("name") or "Red electrica"
            note = "ADC crudo, sin calibrar"
        elif sensor_type == "humidity":
            unit = "%"
            axis = "climate"
            label = sensor.get("name") or sensor_id
            note = ""
        elif sensor_type == "temperature":
            unit = "\u00b0C"
            axis = "climate"
            label = sensor.get("name") or sensor_id
            note = ""
        else:
            unit = ""
            axis = "climate"
            label = sensor.get("name") or sensor_id
            note = ""
        series.append(
            {
                "id": sensor_id,
                "label": label,
                "unit": unit,
                "axis": axis,
                "note": note,
                "type": sensor_type,
                "keys": list(payload_keys_for(sensor)),
                "has_data": False,  # filled in by bucket_rows
            }
        )
    for index, item in enumerate(series):
        item["color"] = PALETTE[index % len(PALETTE)]
    return series


def extract_value(payload: dict, series: dict):
    """Numeric value for one series, or None to leave a gap in the chart."""
    if not isinstance(payload, dict):
        return None
    for key in series.get("keys") or ():
        value = _as_float(payload.get(key))
        if value is not None:
            return value
    return None


def extract_aux_values(payload: dict) -> dict:
    """The fixed extra columns written to CSV (uptime, LTE, connectivity)."""
    payload = payload if isinstance(payload, dict) else {}
    return {label: payload.get(key) for key, label in AUX_COLUMNS}


def bucket_seconds(from_dt: datetime, to_dt: datetime) -> int:
    """Recommended aggregation bucket for a range, in seconds."""
    span = to_dt - from_dt
    for limit, seconds in _BUCKETS:
        if span <= limit:
            return seconds
    return _BUCKETS[-1][1]


def bucket_rows(rows, series: list[dict], resolution: int):
    """Average the samples into fixed-size buckets for charting.

    Returns ``(points, total_samples)`` where each point is
    ``{"t": iso8601, "v": {series_id: value | None}}``. A bucket with no usable
    value for a series stays None, so the chart shows a gap instead of a zero.

    Each series is also flagged ``has_data`` in place: a sensor that reported
    nothing across the whole range is disconnected, not merely sparse, and the
    UI says so instead of drawing an invisible flat line.
    """
    resolution = max(int(resolution or 60), 1)
    total = len(rows)
    if not rows:
        return [], 0

    buckets: dict[int, dict[str, list[float]]] = {}
    for row in rows:
        received_at = row.received_at
        if received_at is None:
            continue
        epoch = int(received_at.timestamp())
        slot = epoch - (epoch % resolution)
        accumulator = buckets.setdefault(slot, {})
        payload = row.payload if isinstance(row.payload, dict) else {}
        for item in series:
            value = extract_value(payload, item)
            if value is None:
                continue
            accumulator.setdefault(item["id"], []).append(value)

    points = []
    for slot in sorted(buckets):
        accumulator = buckets[slot]
        values = {}
        for item in series:
            samples = accumulator.get(item["id"]) or []
            values[item["id"]] = round(sum(samples) / len(samples), 2) if samples else None
        points.append(
            {
                "t": datetime.utcfromtimestamp(slot).replace(tzinfo=None).isoformat() + "Z",
                "v": values,
            }
        )

    # Flag which sensors actually reported anywhere in the range.
    for item in series:
        item["has_data"] = any(
            point["v"].get(item["id"]) is not None for point in points
        )
    return points, total
