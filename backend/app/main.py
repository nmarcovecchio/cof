import os
from datetime import datetime, timezone

import psycopg
import redis
from flask import Flask, jsonify


def create_app() -> Flask:
    app = Flask(__name__)

    @app.get("/")
    def index():
        return (
            "<html><body>"
            "<h1>CallOnFail backend</h1>"
            "<p>Backend is running.</p>"
            "<p>Health: <a href=\"/health\">/health</a></p>"
            "<p>Status: <a href=\"/api/status\">/api/status</a></p>"
            "</body></html>"
        )

    @app.get("/health")
    def health():
        return jsonify(
            {
                "ok": True,
                "service": "callonfail-backend",
                "time": datetime.now(timezone.utc).isoformat(),
            }
        )

    @app.get("/api/status")
    def status():
        return jsonify(
            {
                "ok": True,
                "database": check_database(),
                "redis": check_redis(),
                "time": datetime.now(timezone.utc).isoformat(),
            }
        )

    return app


def check_database() -> dict:
    database_url = os.environ.get("DATABASE_URL")
    if not database_url:
        return {"ok": False, "error": "DATABASE_URL is not configured"}

    try:
        with psycopg.connect(database_url, connect_timeout=3) as conn:
            with conn.cursor() as cur:
                cur.execute("select 1")
                cur.fetchone()
        return {"ok": True}
    except Exception as exc:  # pragma: no cover - visible in health endpoint
        return {"ok": False, "error": str(exc)}


def check_redis() -> dict:
    redis_url = os.environ.get("REDIS_URL")
    if not redis_url:
        return {"ok": False, "error": "REDIS_URL is not configured"}

    try:
        client = redis.Redis.from_url(redis_url, socket_connect_timeout=3)
        client.ping()
        return {"ok": True}
    except Exception as exc:  # pragma: no cover - visible in health endpoint
        return {"ok": False, "error": str(exc)}


app = create_app()
