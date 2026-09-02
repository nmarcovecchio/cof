import json
import logging
import os
import signal
import sys
import time

import paho.mqtt.client as mqtt


LOG_LEVEL = os.environ.get("LOG_LEVEL", "INFO").upper()
MQTT_HOST = os.environ.get("MQTT_HOST", "mosquitto")
MQTT_PORT = int(os.environ.get("MQTT_PORT", "1883"))
MQTT_USERNAME = os.environ.get("MQTT_USERNAME", "")
MQTT_PASSWORD = os.environ.get("MQTT_PASSWORD", "")
MQTT_CLIENT_ID = os.environ.get("MQTT_WORKER_CLIENT_ID", "callonfail-backend-worker")

TOPICS = [
    ("devices/+/telemetry", 0),
    ("devices/+/event", 1),
    ("devices/+/status", 1),
    ("devices/+/config/reported", 1),
    ("devices/+/ack", 1),
]

logging.basicConfig(
    level=LOG_LEVEL,
    format="%(asctime)s %(levelname)s %(name)s %(message)s",
)
logger = logging.getLogger("callonfail.mqtt_worker")
running = True


def on_connect(client, userdata, flags, rc):
    if rc != 0:
        logger.error("MQTT connect failed rc=%s", rc)
        return

    logger.info("MQTT connected host=%s port=%s", MQTT_HOST, MQTT_PORT)
    for topic, qos in TOPICS:
        client.subscribe(topic, qos=qos)
        logger.info("MQTT subscribed topic=%s qos=%s", topic, qos)


def on_disconnect(client, userdata, rc):
    if rc != 0:
        logger.warning("MQTT unexpected disconnect rc=%s", rc)
    else:
        logger.info("MQTT disconnected")


def on_message(client, userdata, message):
    payload = message.payload.decode("utf-8", errors="replace")
    try:
        decoded = json.loads(payload)
    except json.JSONDecodeError:
        decoded = payload

    logger.info(
        "MQTT message topic=%s qos=%s retain=%s payload=%s",
        message.topic,
        message.qos,
        message.retain,
        decoded,
    )


def handle_signal(signum, frame):
    global running
    logger.info("Stopping MQTT worker signal=%s", signum)
    running = False


def main():
    signal.signal(signal.SIGTERM, handle_signal)
    signal.signal(signal.SIGINT, handle_signal)

    client = mqtt.Client(client_id=MQTT_CLIENT_ID)
    if MQTT_USERNAME:
        client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)

    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message

    while running:
        try:
            logger.info("Connecting to MQTT host=%s port=%s", MQTT_HOST, MQTT_PORT)
            client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
            client.loop_start()
            while running:
                time.sleep(1)
            client.loop_stop()
            client.disconnect()
        except Exception:
            logger.exception("MQTT worker error, retrying")
            time.sleep(5)

    return 0


if __name__ == "__main__":
    sys.exit(main())
