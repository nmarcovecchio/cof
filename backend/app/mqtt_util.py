import json
import os

import paho.mqtt.publish as mqtt_publish


def publish_mqtt(topic: str, payload: dict, qos: int = 1, retain: bool = False):
    publish_mqtt_raw(topic, json.dumps(payload, separators=(",", ":")), qos=qos, retain=retain)


def publish_mqtt_raw(topic: str, payload: str, qos: int = 1, retain: bool = False):
    mqtt_host = os.environ.get("MQTT_HOST", "mosquitto")
    mqtt_port = int(os.environ.get("MQTT_PORT", "1883"))
    auth = None
    username = os.environ.get("MQTT_USERNAME", "")
    if username:
        auth = {"username": username, "password": os.environ.get("MQTT_PASSWORD", "")}

    mqtt_publish.single(
        topic,
        payload=payload,
        qos=qos,
        retain=retain,
        hostname=mqtt_host,
        port=mqtt_port,
        auth=auth,
    )
