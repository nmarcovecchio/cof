from datetime import datetime, timezone

from sqlalchemy import UniqueConstraint

from .extensions import db


def utcnow():
    return datetime.now(timezone.utc)


class Tenant(db.Model):
    __tablename__ = "tenants"

    id = db.Column(db.Integer, primary_key=True)
    name = db.Column(db.String(160), nullable=False)
    slug = db.Column(db.String(80), nullable=False, unique=True, index=True)
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

    tenant = db.relationship("Tenant", back_populates="devices")
    site = db.relationship("Site", back_populates="devices")
    configs = db.relationship("DeviceConfig", back_populates="device", cascade="all, delete-orphan")
    telemetry = db.relationship("Telemetry", back_populates="device", cascade="all, delete-orphan")
    events = db.relationship("Event", back_populates="device", cascade="all, delete-orphan")


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

    device = db.relationship("Device", back_populates="telemetry")


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
