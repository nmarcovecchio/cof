from .extensions import db
from .main import create_app
from .models import Device, Site, Tenant


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
        ensure_seed_data()
        print("Database initialized")


if __name__ == "__main__":
    main()
