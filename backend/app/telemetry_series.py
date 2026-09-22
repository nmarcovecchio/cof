"""Telemetry chart/export helpers.

The series a chart shows come from ``SensorWindow`` rows, not from the device
config. A window is a sensor's *alias* over a period: it says the probe wired
to ``sensor_id`` was called "Camera A" from January to September, and "Camera B"
after that, reading ``temperature_1`` the whole time.

That indirection is what makes reassigning a sensor honest. The reading keeps
arriving on the same payload field, so without the window the chart would draw
one continuous line whose meaning silently changed halfway. With it, a range
that spans the hand-over returns two series - one per alias - each owning only
its own stretch, and a retired sensor keeps its column in a historical export.

This lives in the payload rather than in the promoted columns on purpose: the
table only has temperature_1/2, humidity and water_leak, so a renamed or added
sensor has no column at all (see models.Telemetry).

The mains case is special: the firmware publishes ``mains_voltage`` as null
(``firmware/src/mqtt_io.cpp``) and the only real reading is the raw ADC value
``zmpt_raw``, which is what a ``mains_1`` window points at. It is labelled as
uncalibrated when plotted.
"""

from __future__ import annotations

from datetime import datetime, timedelta, timezone

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

# A sensor window in the chart is a line whose X positions are real timestamps,
# so Chart.js draws a straight segment between two consecutive points whatever
# the time between them. Emitting only the buckets that carry samples therefore
# hides an outage: the line bridges the gap as if the device had kept reporting,
# because no null ever reaches the renderer to trip `spanGaps: false`.
#
# Filling the empty buckets fixes that, but filling *every* one would cut the
# line on any single missed reading (a 60 s cadence with a 300 s bucket leaves
# gaps all the time). So a gap only becomes a visible break once it lasts
# _GAP_MIN_EMPTY_BUCKETS buckets, and shorter gaps stay bridged.
#
# Note this bounds the resolution of the feature: an outage shorter than the
# bucket is invisible by construction. Over 30 days the bucket is 6 h, so a two
# hour outage cannot be represented at that zoom regardless of this threshold.
# 3 is a starting value; a device configured at the 300 s maximum cadence can
# leave a stray empty bucket from clock skew alone, and one or two of those
# should not read as an outage.
_GAP_MIN_EMPTY_BUCKETS = 3


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


def is_plottable(window) -> bool:
    sensor_type = str(getattr(window, "sensor_type", "") or "").strip()
    if sensor_type in _NON_PLOT_TYPES:
        return False
    # Connectivity sensors are plain 1/0 flags; they are exported to CSV, not
    # drawn as a line.
    if str(getattr(window, "payload_key", "") or "").startswith("network_"):
        return False
    return bool(str(getattr(window, "payload_key", "") or "").strip())


def _label_and_axis(window) -> tuple[str, str, str, str]:
    """(label, unit, axis, note) for a window, by sensor type."""
    sensor_type = str(getattr(window, "sensor_type", "") or "").strip()
    alias = str(getattr(window, "alias", "") or getattr(window, "sensor_id", "") or "")
    if sensor_type == "mains_voltage":
        return alias or "Red electrica", "ADC", "mains", "ADC crudo, sin calibrar"
    if sensor_type == "humidity":
        return alias, "%", "climate", ""
    if sensor_type == "temperature":
        return alias, "\u00b0C", "climate", ""
    return alias, "", "climate", ""


def _iso_utc(value) -> str | None:
    """Serialise a datetime as an ISO-8601 UTC instant ending in ``Z``.

    Postgres hands back timezone-aware datetimes while SQLite (and some fixture
    paths) give naive ones. ``value.isoformat() + "Z"`` is wrong for the aware
    case: it produces ``...+00:00Z``, which is not parseable, and the failure
    was silent - the window filter then saw a null bound and stopped applying
    entirely. Normalise to UTC first and only then append the suffix.
    """
    if value is None:
        return None
    if isinstance(value, datetime):
        if value.tzinfo is None:
            # Stored timestamps are UTC by convention; treat naive as UTC rather
            # than letting the local timezone shift the instant.
            value = value.replace(tzinfo=timezone.utc)
        value = value.astimezone(timezone.utc)
        return value.replace(tzinfo=None).isoformat() + "Z"
    return str(value)


def _parse_iso_epoch(value):
    """Epoch seconds for an ISO string (as serialised into a series), or None."""
    if not value:
        return None
    text = str(value).strip()
    # Accept a trailing "Z" the same way parse_telemetry_range does.
    if text.endswith(("Z", "z")):
        text = text[:-1] + "+00:00"
    try:
        parsed = datetime.fromisoformat(text)
    except ValueError:
        return None
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    return int(parsed.timestamp())


def _to_epoch(value):
    """Epoch seconds from a datetime, an ISO string or an epoch, or None.

    Naive datetimes are read as UTC. Using ``datetime.timestamp()`` directly
    would interpret them in the server's local timezone, so every window
    comparison would be off by the UTC offset (3 h for Argentina).
    """
    if value is None:
        return None
    if isinstance(value, datetime):
        if value.tzinfo is None:
            value = value.replace(tzinfo=timezone.utc)
        return int(value.timestamp())
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        return int(value)
    return _parse_iso_epoch(value)


def window_covers(window, from_dt, to_dt) -> bool:
    """Whether a sensor window overlaps the requested range at all.

    Both bounds are compared as epochs so the caller can pass either datetimes
    or the ISO strings carried in a serialised series.
    """
    starts = _to_epoch(getattr(window, "starts_at", None))
    ends = _to_epoch(getattr(window, "ends_at", None))
    range_start = _to_epoch(from_dt)
    range_end = _to_epoch(to_dt)
    if starts is None or range_start is None or range_end is None:
        return False
    if starts > range_end:
        return False
    if ends is not None and ends < range_start:
        return False
    return True


def windows_for_range(windows, from_dt, to_dt) -> list:
    """The sensor windows that were in effect at some point inside the range.

    This is what makes a reassignment honest: asking for a range that spans the
    hand-over returns both the old alias and the new one, each owning its own
    stretch, instead of one series carrying two different meanings.
    """
    selected = [w for w in windows if is_plottable(w) and window_covers(w, from_dt, to_dt)]
    selected.sort(key=lambda w: (_to_epoch(getattr(w, "starts_at", None)) or 0, getattr(w, "id", 0) or 0))
    return selected


def series_for_range(windows, from_dt, to_dt) -> list[dict]:
    """Chartable series for a device, from the sensor windows active in a range.

    Every selected window becomes its own series whether or not it is currently
    reporting, so a sensor that was disconnected - or retired - keeps its place
    (and its CSV column) instead of vanishing from a historical range.
    """
    series = []
    for window in windows_for_range(windows, from_dt, to_dt):
        label, unit, axis, note = _label_and_axis(window)
        series.append(
            {
                # Two windows can share a sensor_id (before/after a
                # reassignment), so the series is keyed by the window id, which
                # is unique.
                "id": f"w{window.id}",
                "sensor_id": str(getattr(window, "sensor_id", "") or ""),
                "label": label,
                "unit": unit,
                "axis": axis,
                "note": note,
                "type": str(getattr(window, "sensor_type", "") or ""),
                "keys": [str(getattr(window, "payload_key", "") or "")],
                "starts_at": _iso_utc(getattr(window, "starts_at", None)),
                "ends_at": _iso_utc(getattr(window, "ends_at", None)),
                "closed": getattr(window, "ends_at", None) is not None,
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


def _empty_point(slot: int, series: list[dict]) -> dict:
    """A bucket with no reading for any series."""
    return {"t": _iso_utc(datetime.fromtimestamp(slot, tz=timezone.utc)), "v": {item["id"]: None for item in series}}


def _fill_gaps(points: list[dict], series: list[dict], resolution: int) -> list[dict]:
    """Insert null buckets so a long silence breaks the line instead of bridging it.

    Only the space *between* the first and last point is filled: the chart should
    not claim the device was up (or down) before it ever reported, nor after it
    stopped. That also means a range whose whole span is empty stays empty.

    Gaps shorter than ``_GAP_MIN_EMPTY_BUCKETS`` buckets are left alone, so the
    line stays continuous across a single missed reading and the cut only means
    something when it appears.
    """
    if len(points) < 2:
        return points

    resolution = max(int(resolution or 60), 1)
    out: list[dict] = []
    for previous, current in zip(points, points[1:]):
        out.append(previous)
        previous_epoch = _parse_iso_epoch(previous.get("t"))
        current_epoch = _parse_iso_epoch(current.get("t"))
        if previous_epoch is None or current_epoch is None:
            continue
        missing = (current_epoch - previous_epoch) // resolution - 1
        if missing < _GAP_MIN_EMPTY_BUCKETS:
            # Too short to mean an outage; leave it bridged.
            continue
        for step in range(1, missing + 1):
            out.append(_empty_point(previous_epoch + step * resolution, series))
    out.append(points[-1])
    return out


def bucket_rows(rows, series: list[dict], resolution: int):
    """Average the samples into fixed-size buckets for charting.

    Returns ``(points, total_samples)`` where each point is
    ``{"t": iso8601, "v": {series_id: value | None}}``. A bucket with no usable
    value for a series stays None, so the chart shows a gap instead of a zero.

    A sample only counts towards a series whose window covers that instant.
    Without this an alias would keep plotting readings taken outside its own
    validity - so after moving a probe from camera A to camera B, A would still
    show B's temperatures.

    Each series is also flagged ``has_data`` in place: a sensor that reported
    nothing across the whole range is disconnected or retired, not merely
    sparse, and the UI says so instead of drawing an invisible flat line.
    """
    resolution = max(int(resolution or 60), 1)
    total = len(rows)
    if not rows:
        return [], 0

    # Per-series epoch bounds, so the inner loop stays cheap.
    spans = {}
    for item in series:
        spans[item["id"]] = (
            _parse_iso_epoch(item.get("starts_at")),
            _parse_iso_epoch(item.get("ends_at")),
        )

    buckets: dict[int, dict[str, list[float]]] = {}
    for row in rows:
        received_at = row.received_at
        if received_at is None:
            continue
        # Go through the same UTC normalisation as the window bounds: calling
        # timestamp() on a naive datetime would read it in the server's local
        # timezone and shift every comparison by the UTC offset, which let a
        # reassigned alias absorb its predecessor's samples.
        at = _to_epoch(received_at)
        if at is None:
            continue
        slot = at - (at % resolution)
        accumulator = buckets.setdefault(slot, {})
        payload = row.payload if isinstance(row.payload, dict) else {}
        for item in series:
            starts, ends = spans[item["id"]]
            # Half-open window [starts, ends): at a reassignment A.ends_at equals
            # B.starts_at, so an inclusive end would count the hand-over sample
            # in both aliases (and twice in the CSV).
            if starts is not None and at < starts:
                continue
            if ends is not None and at >= ends:
                continue
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
        points.append({"t": _iso_utc(datetime.fromtimestamp(slot, tz=timezone.utc)), "v": values})

    points = _fill_gaps(points, series, resolution)

    # Flag which sensors actually reported anywhere in the range.
    for item in series:
        item["has_data"] = any(
            point["v"].get(item["id"]) is not None for point in points
        )
    return points, total
