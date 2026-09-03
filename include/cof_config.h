#pragma once

// Firmware version shown on OLED and used by OTA comparison.
#define COF_FIRMWARE_VERSION "0.2.4"

// Raw GitHub manifest. After merging, keep this URL pointing at main.
#define COF_MANIFEST_URL "https://raw.githubusercontent.com/nmarcovecchio/cof/main/actual_version/manifest.json"

// Safety switch: set to 1 only after configuring COF_PHONE_NUMBER.
#define COF_ENABLE_CALLS 0
#define COF_PHONE_NUMBER "+549XXXXXXXXXX"

// WT32-ETH01 / WT32-S1 pin assignment.
#define COF_PIN_I2C_SDA 32
#define COF_PIN_I2C_SCL 33
#define COF_PIN_ONEWIRE 14
#define COF_PIN_ZMPT_ADC 36
#define COF_PIN_MODEM_RX 5
#define COF_PIN_MODEM_TX 17

// I2C defaults.
#define COF_OLED_ADDRESS 0x3C
#define COF_SHT31_ADDRESS 0x44

// Modem filesystem target for the audio played during the call.
#define COF_MODEM_AUDIO_PATH "C:/cof_test.wav"

// Default MQTT lab endpoint. Can be overridden from Serial with:
// mqtt HOST PORT DEVICE_ID [USER PASSWORD]
#define COF_DEFAULT_MQTT_HOST "mqtt.callonfail.com.ar"
#define COF_DEFAULT_MQTT_PORT 1883
#define COF_DEFAULT_MQTT_DEVICE_ID "cof-test"
#define COF_DEFAULT_MQTT_USERNAME ""
#define COF_DEFAULT_MQTT_PASSWORD ""
