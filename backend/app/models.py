from datetime import datetime, timezone

from sqlalchemy import Index, UniqueConstraint

from .extensions import db


def utcnow():
    return datetime.now(timezone.utc)


class Tenant(db.Model):
    __tablename__ = "tenants"

    id = db.Column(db.Integer, primary_key=True)
    name = db.Column(db.String(160), nullable=False)
    slug = db.Column(db.String(80), nullable=False, unique=True, index=True)
    notify_email = db.Column(db.Text, nullable=True)
    telegram_chat_id = db.Column(db.String(255), nullable=True)
    phone = db.Column(db.String(512), nullable=True)
    contacts = db.Column(db.JSON, nullable=True)
    created_at = db.Column(db.DateTime(timezone=True), nullable=False, default=utcnow)

    sites = db.relationship("Site", back_populates="tenant", cascade="all, delete-orphan")
    devices = db.relationship("Device", back_populates="tenant", cascade="all, delete-orphan")


class Site(db.Model):
    __tablename__ = "sites"

    id = db.Column(db.Integer, primary_key=True)
    tenant_id = db.Column(db.Integer, db.ForeignKey("tenants.id"), nullable=False, index=True)
    name = db.Column(db.String(160), nullable=False)
    created_at = db.Column(db.DateTime(timezone=True), nullable=False, default=utcnow)

    tenant = db.relationship("Tenant", back_populates="sites")
    devices = db.relationship("Device", back_populates="site")

    __table_args__ = (UniqueConstraint("tenant_id", "name", name="uq_sites_tenant_name"),)


class Device(db.Model):
    __tablename__ = "devices"

    id = db.Column(db.Integer, primary_key=True)
    tenant_id = db.Column(db.Integer, db.ForeignKey("tenants.id"), nullable=False, index=True)
    site_id = db.Column(db.Integer, db.ForeignKey("sites.id"), nullable=True, index=True)
    device_uid = db.Column(db.String(80), nullable=False, unique=True, index=True)
    name = db.Column(db.String(160), nullable=False)
    status = db.Column(db.String(40), nullable=False, default="new")
    hardware_profile = db.Column(db.String(80), nullable=True)
    capabilities = db.Column(db.JSON, nullable=True)
    discovered = db.Column(db.JSON, nullable=True)
    firmware_version = db.Column(db.String(40), nullable=True)
    ip_address = db.Column(db.String(80), nullable=True)
    last_seen_at = db.Column(db.DateTime(timezone=True), nullable=True)
    desired_config_version = db.Column(db.Integer, nullable=True)
    reported_config_version = db.Column(db.Integer, nullable=True)
    created_at = db.Column(db.DateTime(timezone=True), nullable=False, default=utcnow)
    updated_at = db.Column(db.DateTime(timezone=True), nullable=False, default=utcnow, onupdate=utcnow)
    archived_at = db.Column(db.DateTime(timezone=True), nullable=True)

    tenant = db.relationship("Tenant", back_populates="devices")
    site = db.relationship("Site", back_populates="devices")
    configs = db.relationship("DeviceConfig", back_populates="device", cascade="all, delete-orphan")
    telemetry = db.relationship("Telemetry", back_populates="device", cascade="all, delete-orphan")
    events = db.relationship("Event", back_populates="device", cascade="all, delete-orphan")
    modem_jobs = db.relationship("DeviceModemJob", back_populates="device", cascade="all, delete-orphan")
    sensor_windows = db.relationship("SensorWindow", back_populates="device", cascade="all, delete-orphan")


class DeviceConfig(db.Model):
    __tablename__ = "device_configs"

    id = db.Column(db.Integer, primary_key=True)
    device_id = db.Column(db.Integer, db.ForeignKey("devices.id"), nullable=False, index=True)
    version = db.Column(db.Integer, nullable=False)
    status = db.Column(db.String(40), nullable=False, default="desired")
    config_hash = db.Column(db.String(128), nullable=True)
    desired_payload = db.Column(db.JSON, nullable=False)
    reported_payload = db.Column(db.JSON, nullable=True)
    created_at = db.Column(db.DateTime(timezone=True), nullable=False, default=utcnow)
    applied_at = db.Column(db.DateTime(timezone=True), nullable=True)

    device = db.relationship("Device", back_populates="configs")

    __table_args__ = (UniqueConstraint("device_id", "version", name="uq_device_configs_device_version"),)


class Telemetry(db.Model):
    __tablename__ = "telemetry"

    id = db.Column(db.Integer, primary_key=True)
    device_id = db.Column(db.Integer, db.ForeignKey("devices.id"), nullable=False, index=True)
    received_at = db.Column(db.DateTime(timezone=True), nullable=False, default=utcnow, index=True)
    payload = db.Column(db.JSON, nullable=False)
    firmware_version = db.Column(db.String(40), nullable=True)
    mains_voltage = db.Column(db.Float, nullable=True)
    temperature_1 = db.Column(db.Float, nullable=True)
    temperature_2 = db.Column(db.Float, nullable=True)
    humidity = db.Column(db.Float, nullable=True)
    water_leak = db.Column(db.Boolean, nullable=True)

    # Composite index for the date-range queries behind the telemetry charts and
    # CSV export, which always filter device_id + received_at together. The two
    # single-column indexes are not enough for a range on one device.
    __table_args__ = (db.Index("ix_telemetry_device_received", "device_id", "received_at"),)

    device = db.relationship("Device", back_populates="telemetry")


class SensorWindow(db.Model):
    """A sensor's alias for a period of time - who it was and where it read from.

    A sensor has a physical identity (``sensor_id``, i.e. the pin it is wired
    to) that outlives its role. The same probe can monitor camera A, then be
    moved to camera B; the reading keeps arriving on the same payload field, so
    the alias is the only thing that says what it means right now.

    ``ends_at`` null means the window is open (currently recording). Closing it
    freezes the history instead of losing it: the closed window keeps its alias
    over its own stretch, and a new window can then take over the same
    ``payload_key`` with a different name without the two being confused.

    This is deliberately a table rather than a field inside
    ``DeviceConfig.desired_payload``. That payload is pruned to the last few
    versions when the device page renders it, so a window stored there would
    vanish and take a historical alias with it.
    """

    __tablename__ = "sensor_windows"

    id = db.Column(db.Integer, primary_key=True)
    device_id = db.Column(db.Integer, db.ForeignKey("devices.id"), nullable=False, index=True)
    sensor_id = db.Column(db.String(80), nullable=False)
    alias = db.Column(db.String(160), nullable=False)
    payload_key = db.Column(db.String(80), nullable=False)
    sensor_type = db.Column(db.String(40), nullable=True)
    starts_at = db.Column(db.DateTime(timezone=True), nullable=False, default=utcnow)
    ends_at = db.Column(db.DateTime(timezone=True), nullable=True)
    closed_reason = db.Column(db.String(40), nullable=True)
    created_at = db.Column(db.DateTime(timezone=True), nullable=False, default=utcnow)

    device = db.relationship("Device", back_populates="sensor_windows")

    __table_args__ = (
        # Every range query filters by device and then tests for overlap.
        Index("ix_sensor_windows_device_span", "device_id", "starts_at", "ends_at"),
    )


class DeviceModemJob(db.Model):
    __tablename__ = "device_modem_jobs"

    id = db.Column(db.Integer, primary_key=True)
    device_id = db.Column(db.Integer, db.ForeignKey("devices.id"), nullable=False, index=True)
    command = db.Column(db.String(40), nullable=False)
    command_id = db.Column(db.String(64), nullable=False, unique=True, index=True)
    status = db.Column(db.String(20), nullable=False, default="queued", index=True)
    payload = db.Column(db.JSON, nullable=False, default=dict)
    result = db.Column(db.String(240), nullable=True)
    created_at = db.Column(db.DateTime(timezone=True), nullable=False, default=utcnow, index=True)
    sent_at = db.Column(db.DateTime(timezone=True), nullable=True)
    finished_at = db.Column(db.DateTime(timezone=True), nullable=True)

    device = db.relationship("Device", back_populates="modem_jobs")


class AudioAsset(db.Model):
    """One spoken call text, synthesized once and reused forever.

    Content-addressed by ``text_sha256`` (the normalized spoken text), so two
    rules - on the same device or on different ones - that say the same thing
    share a single AMR file on disk and a single download to the modem. Without
    this the device would re-download a byte-identical file per rule.

    Every stored text is self-contained: all placeholders are resolved by the
    caller before synthesis, so the call never needs the network.
    """

    __tablename__ = "audio_assets"

    id = db.Column(db.Integer, primary_key=True)
    text_sha256 = db.Column(db.String(64), nullable=False, unique=True, index=True)
    text = db.Column(db.Text, nullable=False)
    amr_sha256 = db.Column(db.String(64), nullable=False)
    size_bytes = db.Column(db.Integer, nullable=False, default=0)
    created_at = db.Column(db.DateTime(timezone=True), nullable=False, default=utcnow)


class Event(db.Model):
    __tablename__ = "events"

    id = db.Column(db.Integer, primary_key=True)
    device_id = db.Column(db.Integer, db.ForeignKey("devices.id"), nullable=False, index=True)
    type = db.Column(db.String(80), nullable=False, index=True)
    severity = db.Column(db.String(40), nullable=False, default="info")
    message = db.Column(db.String(240), nullable=True)
    payload = db.Column(db.JSON, nullable=False, default=dict)
    started_at = db.Column(db.DateTime(timezone=True), nullable=False, default=utcnow, index=True)
    cleared_at = db.Column(db.DateTime(timezone=True), nullable=True)

    device = db.relationship("Device", back_populates="events")
