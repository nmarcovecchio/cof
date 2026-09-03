from .extensions import db
from .main import create_app
from .models import Device, Site, Tenant
from sqlalchemy import inspect, text


def ensure_schema_columns():
    inspector = inspect(db.engine)
    device_columns = {column["name"] for column in inspector.get_columns("devices")}

    statements = []
    if "hardware_profile" not in device_columns:
        statements.append("ALTER TABLE devices ADD COLUMN hardware_profile VARCHAR(80)")
    if "capabilities" not in device_columns:
        statements.append("ALTER TABLE devices ADD COLUMN capabilities JSONB")
    if "discovered" not in device_columns:
        statements.append("ALTER TABLE devices ADD COLUMN discovered JSONB")
    if "archived_at" not in device_columns:
        statements.append("ALTER TABLE devices ADD COLUMN archived_at TIMESTAMP WITH TIME ZONE")

    if not statements:
        return

    with db.engine.begin() as connection:
        for statement in statements:
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


def main():
    app = create_app()
    with app.app_context():
        db.create_all()
        ensure_schema_columns()
        ensure_seed_data()
        print("Database initialized")


if __name__ == "__main__":
    main()
