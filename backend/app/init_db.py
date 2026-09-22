from .extensions import db
from .main import create_app
from .models import Device, Site, Tenant, utcnow
from sqlalchemy import inspect, text


def ensure_schema_columns():
    inspector = inspect(db.engine)
    device_columns = {column["name"] for column in inspector.get_columns("devices")}
    tenant_columns = {column["name"] for column in inspector.get_columns("tenants")}

    statements = []
    if "hardware_profile" not in device_columns:
        statements.append("ALTER TABLE devices ADD COLUMN hardware_profile VARCHAR(80)")
    if "capabilities" not in device_columns:
        statements.append("ALTER TABLE devices ADD COLUMN capabilities JSONB")
    if "discovered" not in device_columns:
        statements.append("ALTER TABLE devices ADD COLUMN discovered JSONB")
    if "archived_at" not in device_columns:
        statements.append("ALTER TABLE devices ADD COLUMN archived_at TIMESTAMP WITH TIME ZONE")
    if "notify_email" not in tenant_columns:
        statements.append("ALTER TABLE tenants ADD COLUMN notify_email TEXT")
    if "telegram_chat_id" not in tenant_columns:
        statements.append("ALTER TABLE tenants ADD COLUMN telegram_chat_id VARCHAR(255)")
    if "phone" not in tenant_columns:
        statements.append("ALTER TABLE tenants ADD COLUMN phone VARCHAR(512)")
    if "contacts" not in tenant_columns:
        statements.append("ALTER TABLE tenants ADD COLUMN contacts JSONB")

    widen = [
        "ALTER TABLE tenants ALTER COLUMN notify_email TYPE TEXT",
        "ALTER TABLE tenants ALTER COLUMN telegram_chat_id TYPE TEXT",
        "ALTER TABLE tenants ALTER COLUMN phone TYPE VARCHAR(512)",
    ]

    # Composite index for the telemetry date-range queries. db.create_all() does
    # not touch an existing table, so a table created before the index existed
    # never gets it; IF NOT EXISTS keeps this idempotent on every boot.
    indexes = [
        "CREATE INDEX IF NOT EXISTS ix_telemetry_device_received ON telemetry (device_id, received_at)",
    ]

    with db.engine.begin() as connection:
        for statement in statements:
            connection.execute(text(statement))
        for statement in widen:
            connection.execute(text(statement))
        for statement in indexes:
            connection.execute(text(statement))


def ensure_seed_data():
    tenant = Tenant.query.filter_by(slug="demo").first()
    if tenant is None:
        tenant = Tenant(name="Demo CallOnFail", slug="demo")
        db.session.add(tenant)
        db.session.flush()

    site = Site.query.filter_by(tenant_id=tenant.id, name="Banco de pruebas").first()
    if site is None:
        site = Site(tenant_id=tenant.id, name="Banco de pruebas")
        db.session.add(site)
        db.session.flush()

    device = Device.query.filter_by(device_uid="cof-test").first()
    if device is None:
        device = Device(
            tenant_id=tenant.id,
            site_id=site.id,
            device_uid="cof-test",
            name="Dispositivo de prueba",
            status="new",
        )
        db.session.add(device)

    db.session.commit()


def ensure_sensor_windows():
    """Give every device with a config one open window per configured sensor.

    Devices predate the table, so without this backfill their chart would be
    empty. Each window starts at the device's first stored sample (so the whole
    existing history is attributed to it) and is left open - the operator closes
    or reassigns it from the panel.

    Idempotent: a device that already has any window is skipped, so re-running
    this on every boot never duplicates or reopens anything.
    """
    from .models import Device, DeviceConfig, SensorWindow, Telemetry

    query = DeviceConfig.query.order_by(DeviceConfig.device_id, DeviceConfig.version.desc())
    latest_by_device: dict[int, DeviceConfig] = {}
    for config in query:
        latest_by_device.setdefault(config.device_id, config)

    for device_id, config in latest_by_device.items():
        if SensorWindow.query.filter_by(device_id=device_id).first() is not None:
            continue
        payload = config.desired_payload if isinstance(config.desired_payload, dict) else {}
        sensors = payload.get("sensors") or []
        if not sensors:
            continue
        first = (
            Telemetry.query.filter_by(device_id=device_id)
            .order_by(Telemetry.received_at.asc())
            .first()
        )
        starts_at = first.received_at if first and first.received_at else utcnow()
        for sensor in sensors:
            if not isinstance(sensor, dict):
                continue
            sensor_id = str(sensor.get("id") or "").strip()
            payload_key = str(sensor.get("payload_key") or sensor.get("source") or "").strip()
            if not sensor_id or not payload_key:
                continue
            db.session.add(
                SensorWindow(
                    device_id=device_id,
                    sensor_id=sensor_id,
                    alias=str(sensor.get("name") or sensor_id),
                    payload_key=payload_key,
                    sensor_type=str(sensor.get("type") or ""),
                    starts_at=starts_at,
                )
            )
    db.session.commit()


def main():
    app = create_app()
    with app.app_context():
        db.create_all()
        ensure_schema_columns()
        ensure_seed_data()
        ensure_sensor_windows()
        print("Database initialized")


if __name__ == "__main__":
    main()
