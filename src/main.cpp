#include <Arduino.h>
#include <ArduinoJson.h>
#include <DallasTemperature.h>
#include <ETH.h>
#include <HTTPClient.h>
#include <OneWire.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <U8g2lib.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include <esp_arduino_version.h>
#include <esp_task_wdt.h>

#include "cof_config.h"

constexpr uint8_t kEthPhyAddr = 1;
constexpr int kEthMdcPin = 23;
constexpr int kEthMdioPin = 18;
constexpr int kEthPowerPin = 16;
constexpr eth_clock_mode_t kEthClockMode = ETH_CLOCK_GPIO0_IN;
constexpr eth_phy_type_t kEthPhyType = ETH_PHY_LAN8720;

constexpr uint32_t kDisplayIntervalMs = 1000;
constexpr uint32_t kSensorIntervalMs = 3000;
constexpr uint32_t kModemIntervalMs = 30000;
constexpr uint32_t kMqttReconnectIntervalMs = 5000;
constexpr uint32_t kTelemetryPublishIntervalMs = 60000;
constexpr uint32_t kManifestInitialDelayMs = 15000;
constexpr uint32_t kManifestIntervalMs = 60UL * 60UL * 1000UL;
constexpr uint32_t kWatchdogTimeoutSeconds = 60;

HardwareSerial ModemSerial(2);
WiFiClient mqttNetworkClient;
PubSubClient mqttClient(mqttNetworkClient);
U8G2_SH1106_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);
Adafruit_SHT31 sht31;
OneWire oneWire(COF_PIN_ONEWIRE);
DallasTemperature ds18b20(&oneWire);
Preferences preferences;

struct RuntimeState {
  bool ethernetStarted = false;
  bool ethernetConnected = false;
  bool wifiConfigured = false;
  bool wifiConnected = false;
  bool mqttConfigured = false;
  bool mqttConnected = false;
  bool oledReady = false;
  bool sht31Ready = false;
  bool ds18b20Ready = false;
  bool modemReady = false;
  bool simReady = false;
  bool modemAudioPlaybackSupported = false;
  bool modemFileTransferSupported = false;
  bool callInProgress = false;
  bool otaInProgress = false;
  bool audioSyncInProgress = false;
  uint8_t pcfAddress = 0;
  bool pcfReady = false;
  float shtTemperature = NAN;
  float shtHumidity = NAN;
  float dsTemperature = NAN;
  int zmptRaw = 0;
  int signalQuality = -1;
  uint8_t oledAddress = COF_OLED_ADDRESS;
  String ipAddress = "-";
  String wifiSsid = "";
  String wifiIpAddress = "-";
  String mqttHost = COF_DEFAULT_MQTT_HOST;
  int mqttPort = COF_DEFAULT_MQTT_PORT;
  String mqttDeviceId = COF_DEFAULT_MQTT_DEVICE_ID;
  String mqttUsername = COF_DEFAULT_MQTT_USERNAME;
  String mqttPassword = COF_DEFAULT_MQTT_PASSWORD;
  int reportedConfigVersion = 0;
  String reportedConfigHash = "";
  String statusLine = "Booting";
  String modemAudioPath = COF_MODEM_AUDIO_PATH;
  String manifestFirmwareVersion = "";
  String manifestFirmwareUrl = "";
  String manifestAudioVersion = "";
  String manifestAudioUrl = "";
  String manifestPhoneNumber = COF_PHONE_NUMBER;
};

RuntimeState state;

uint32_t lastDisplayMs = 0;
uint32_t lastSensorMs = 0;
uint32_t lastModemMs = 0;
uint32_t lastMqttReconnectMs = 0;
uint32_t lastTelemetryPublishMs = 0;
uint32_t lastManifestMs = 0;
bool didInitialManifestCheck = false;
bool lastButtonPressed = false;
uint32_t buttonPressedAtMs = 0;
String serialCommandBuffer;
bool pendingConfigReport = false;
bool pendingConfigApplied = false;
int pendingConfigVersion = 0;
String pendingConfigHash = "";
String pendingConfigError = "";
bool pendingOtaCommand = false;
bool pendingCommandAck = false;
String pendingCommandId = "";
String pendingCommandName = "";
String pendingCommandStatus = "";
String pendingCommandMessage = "";

void setStatus(const String& line) {
  state.statusLine = line;
  Serial.println("[status] " + line);
}

bool networkConnected() {
  return state.ethernetConnected || state.wifiConnected;
}

void beginInternalWatchdog() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t config = {
    .timeout_ms = kWatchdogTimeoutSeconds * 1000,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
    .trigger_panic = true,
  };
  esp_task_wdt_init(&config);
#else
  esp_task_wdt_init(kWatchdogTimeoutSeconds, true);
#endif
  esp_task_wdt_add(nullptr);
  Serial.printf("[wdt] internal watchdog enabled: %lu seconds\n", kWatchdogTimeoutSeconds);
}

void feedWatchdog() {
  esp_task_wdt_reset();
}

void onNetworkEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      state.ethernetStarted = true;
      ETH.setHostname("callonfail");
      setStatus("ETH start");
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      setStatus("ETH cable OK");
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      state.ethernetConnected = true;
      state.ipAddress = ETH.localIP().toString();
      setStatus("ETH IP " + state.ipAddress);
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      state.ethernetConnected = false;
      state.ipAddress = "-";
      setStatus("ETH disconnected");
      break;
    case ARDUINO_EVENT_ETH_STOP:
      state.ethernetStarted = false;
      state.ethernetConnected = false;
      state.ipAddress = "-";
      setStatus("ETH stopped");
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      state.wifiConnected = true;
      state.wifiIpAddress = WiFi.localIP().toString();
      setStatus("WiFi IP " + state.wifiIpAddress);
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      state.wifiConnected = false;
      state.wifiIpAddress = "-";
      if (state.wifiConfigured) {
        setStatus("WiFi disconnected");
      }
      break;
    default:
      break;
  }
}

void beginEthernet() {
  WiFi.onEvent(onNetworkEvent);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ETH.begin(kEthPhyType, kEthPhyAddr, kEthMdcPin, kEthMdioPin, kEthPowerPin, kEthClockMode);
#else
  ETH.begin(kEthPhyAddr, kEthPowerPin, kEthMdcPin, kEthMdioPin, kEthPhyType, kEthClockMode);
#endif
}

void connectWiFi(const String& ssid, const String& password, bool saveCredentials) {
  if (ssid.length() == 0) {
    Serial.println("[wifi] missing SSID");
    return;
  }

  state.wifiConfigured = true;
  state.wifiSsid = ssid;
  if (saveCredentials) {
    preferences.putString("wifiSsid", ssid);
    preferences.putString("wifiPass", password);
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  setStatus("WiFi connecting");
  Serial.printf("[wifi] connecting to %s\n", ssid.c_str());
}

void beginSavedWiFi() {
  const String ssid = preferences.getString("wifiSsid", "");
  const String password = preferences.getString("wifiPass", "");
  state.wifiConfigured = ssid.length() > 0;
  state.wifiSsid = ssid;

  if (!state.wifiConfigured) {
    Serial.println("[wifi] no saved credentials");
    return;
  }

  connectWiFi(ssid, password, false);
}

void clearSavedWiFi() {
  preferences.remove("wifiSsid");
  preferences.remove("wifiPass");
  state.wifiConfigured = false;
  state.wifiConnected = false;
  state.wifiSsid = "";
  state.wifiIpAddress = "-";
  WiFi.disconnect(true, true);
  setStatus("WiFi cleared");
}

String mqttTopic(const String& suffix) {
  return "devices/" + state.mqttDeviceId + "/" + suffix;
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  String body;
  body.reserve(length);
  for (unsigned int i = 0; i < length; i++) {
    body += static_cast<char>(payload[i]);
  }

  Serial.printf("[mqtt] message topic=%s payload=%s\n", topic, body.c_str());
  setStatus("MQTT msg");

  const String topicString(topic);
  if (topicString == mqttTopic("config/desired")) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, body);
    pendingConfigVersion = 0;
    pendingConfigHash = "";
    pendingConfigApplied = false;
    pendingConfigError = "";

    if (error) {
      pendingConfigError = "invalid JSON";
    } else {
      const String targetDevice = doc["device_id"] | "";
      pendingConfigVersion = doc["config_version"] | 0;
      pendingConfigHash = doc["config_hash"] | "";

      if (targetDevice.length() > 0 && targetDevice != state.mqttDeviceId) {
        pendingConfigError = "device_id mismatch";
      } else if (pendingConfigVersion <= 0) {
        pendingConfigError = "missing config_version";
      } else {
        state.reportedConfigVersion = pendingConfigVersion;
        state.reportedConfigHash = pendingConfigHash;
        preferences.putInt("reportedCfgVersion", state.reportedConfigVersion);
        preferences.putString("reportedCfgHash", state.reportedConfigHash);
        pendingConfigApplied = true;
      }
    }

    pendingConfigReport = true;
  } else if (topicString == mqttTopic("command")) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, body);
    pendingCommandId = "";
    pendingCommandName = "";
    pendingCommandStatus = "rejected";
    pendingCommandMessage = "";

    if (error) {
      pendingCommandMessage = "invalid JSON";
    } else {
      pendingCommandId = doc["command_id"] | "";
      pendingCommandName = doc["command"] | "";
      const String targetDevice = doc["device_id"] | "";

      if (targetDevice.length() > 0 && targetDevice != state.mqttDeviceId) {
        pendingCommandMessage = "device_id mismatch";
      } else if (pendingCommandName == "ota_check") {
        pendingOtaCommand = true;
        pendingCommandStatus = "accepted";
        pendingCommandMessage = "OTA check scheduled";
      } else {
        pendingCommandMessage = "unsupported command";
      }
    }

    pendingCommandAck = true;
  }
}

void loadSavedMqttConfig() {
  state.mqttHost = preferences.getString("mqttHost", COF_DEFAULT_MQTT_HOST);
  state.mqttPort = preferences.getInt("mqttPort", COF_DEFAULT_MQTT_PORT);
  state.mqttDeviceId = preferences.getString("mqttDeviceId", COF_DEFAULT_MQTT_DEVICE_ID);
  state.mqttUsername = preferences.getString("mqttUser", COF_DEFAULT_MQTT_USERNAME);
  state.mqttPassword = preferences.getString("mqttPass", COF_DEFAULT_MQTT_PASSWORD);
  state.reportedConfigVersion = preferences.getInt("reportedCfgVersion", 0);
  state.reportedConfigHash = preferences.getString("reportedCfgHash", "");
  state.mqttConfigured = state.mqttHost.length() > 0 && state.mqttDeviceId.length() > 0;

  if (state.mqttConfigured) {
    mqttClient.setServer(state.mqttHost.c_str(), state.mqttPort);
    mqttClient.setCallback(onMqttMessage);
    mqttClient.setBufferSize(1024);
    Serial.printf("[mqtt] saved config host=%s port=%d device=%s\n",
                  state.mqttHost.c_str(),
                  state.mqttPort,
                  state.mqttDeviceId.c_str());
  } else {
    Serial.println("[mqtt] no saved config");
  }
}

void saveMqttConfig(const String& host, int port, const String& deviceId, const String& username, const String& password) {
  state.mqttHost = host;
  state.mqttPort = port;
  state.mqttDeviceId = deviceId;
  state.mqttUsername = username;
  state.mqttPassword = password;
  state.mqttConfigured = true;

  preferences.putString("mqttHost", host);
  preferences.putInt("mqttPort", port);
  preferences.putString("mqttDeviceId", deviceId);
  preferences.putString("mqttUser", username);
  preferences.putString("mqttPass", password);

  mqttClient.setServer(state.mqttHost.c_str(), state.mqttPort);
  mqttClient.setCallback(onMqttMessage);
  mqttClient.setBufferSize(1024);
  setStatus("MQTT saved");
}

void clearMqttConfig() {
  preferences.remove("mqttHost");
  preferences.remove("mqttPort");
  preferences.remove("mqttDeviceId");
  preferences.remove("mqttUser");
  preferences.remove("mqttPass");
  state.mqttConnected = false;
  state.mqttHost = COF_DEFAULT_MQTT_HOST;
  state.mqttPort = COF_DEFAULT_MQTT_PORT;
  state.mqttDeviceId = COF_DEFAULT_MQTT_DEVICE_ID;
  state.mqttUsername = COF_DEFAULT_MQTT_USERNAME;
  state.mqttPassword = COF_DEFAULT_MQTT_PASSWORD;
  state.mqttConfigured = state.mqttHost.length() > 0 && state.mqttDeviceId.length() > 0;
  mqttClient.disconnect();
  setStatus("MQTT defaults");
}

String currentIpAddress() {
  if (state.ethernetConnected) {
    return state.ipAddress;
  }
  if (state.wifiConnected) {
    return state.wifiIpAddress;
  }
  return "-";
}

bool publishMqttJson(const String& suffix, JsonDocument& doc, bool retained = false, uint8_t qos = 0) {
  if (!state.mqttConnected) {
    return false;
  }

  char payload[768];
  const size_t length = serializeJson(doc, payload, sizeof(payload));
  const String topic = mqttTopic(suffix);
  const bool ok = mqttClient.publish(topic.c_str(), reinterpret_cast<const uint8_t*>(payload), length, retained);
  Serial.printf("[mqtt] publish topic=%s ok=%s payload=%s\n", topic.c_str(), ok ? "yes" : "no", payload);
  (void)qos;
  return ok;
}

void publishDeviceStatus(const char* status, bool retained = true) {
  JsonDocument doc;
  doc["device_id"] = state.mqttDeviceId;
  doc["status"] = status;
  doc["firmware"] = COF_FIRMWARE_VERSION;
  doc["ip"] = currentIpAddress();
  doc["ethernet"] = state.ethernetConnected;
  doc["wifi"] = state.wifiConnected;
  doc["modem_ready"] = state.modemReady;
  doc["sim_ready"] = state.simReady;
  doc["lte_signal"] = state.signalQuality;
  doc["reported_config_version"] = state.reportedConfigVersion;
  publishMqttJson("status", doc, retained, 1);
}

void publishConfigReported() {
  JsonDocument doc;
  doc["schema_version"] = 1;
  doc["device_id"] = state.mqttDeviceId;
  doc["config_version"] = pendingConfigVersion;
  doc["applied"] = pendingConfigApplied;
  doc["config_hash"] = pendingConfigHash;
  doc["firmware"] = COF_FIRMWARE_VERSION;
  if (pendingConfigApplied) {
    doc["message"] = "config applied";
  } else {
    doc["error"] = pendingConfigError;
  }
  publishMqttJson("config/reported", doc, false, 1);
}

void publishCommandAck() {
  JsonDocument doc;
  doc["device_id"] = state.mqttDeviceId;
  doc["command_id"] = pendingCommandId;
  doc["command"] = pendingCommandName;
  doc["status"] = pendingCommandStatus;
  doc["message"] = pendingCommandMessage;
  doc["firmware"] = COF_FIRMWARE_VERSION;
  publishMqttJson("ack", doc, false, 1);
}

void publishTelemetryNow() {
  if (!state.mqttConnected) {
    Serial.println("[mqtt] telemetry skipped, not connected");
    return;
  }

  JsonDocument doc;
  doc["device_id"] = state.mqttDeviceId;
  doc["firmware"] = COF_FIRMWARE_VERSION;
  doc["uptime_seconds"] = millis() / 1000;
  doc["ip"] = currentIpAddress();
  doc["ethernet"] = state.ethernetConnected;
  doc["wifi"] = state.wifiConnected;
  if (isnan(state.dsTemperature)) {
    doc["temperature_1"] = nullptr;
  } else {
    doc["temperature_1"] = state.dsTemperature;
  }
  if (isnan(state.shtTemperature)) {
    doc["temperature_2"] = nullptr;
  } else {
    doc["temperature_2"] = state.shtTemperature;
  }
  if (isnan(state.shtHumidity)) {
    doc["humidity"] = nullptr;
  } else {
    doc["humidity"] = state.shtHumidity;
  }
  doc["mains_voltage"] = nullptr;
  doc["zmpt_raw"] = state.zmptRaw;
  doc["modem_ready"] = state.modemReady;
  doc["sim_ready"] = state.simReady;
  doc["lte_signal"] = state.signalQuality;
  doc["input_1"] = lastButtonPressed;
  doc["output_1"] = false;
  doc["output_2"] = false;
  publishMqttJson("telemetry", doc, false, 0);
}

void connectMqttIfNeeded() {
  if (!state.mqttConfigured || !networkConnected()) {
    return;
  }

  if (mqttClient.connected()) {
    state.mqttConnected = true;
    mqttClient.loop();
    return;
  }

  state.mqttConnected = false;
  const uint32_t now = millis();
  if (now - lastMqttReconnectMs < kMqttReconnectIntervalMs) {
    return;
  }
  lastMqttReconnectMs = now;

  mqttClient.setServer(state.mqttHost.c_str(), state.mqttPort);
  mqttClient.setCallback(onMqttMessage);

  const String clientId = state.mqttDeviceId + "-" + String(static_cast<uint32_t>(ESP.getEfuseMac()), HEX);
  const String willTopic = mqttTopic("status");
  const String willPayload = "{\"status\":\"offline\",\"device_id\":\"" + state.mqttDeviceId + "\"}";
  const char* username = state.mqttUsername.length() > 0 ? state.mqttUsername.c_str() : nullptr;
  const char* password = state.mqttUsername.length() > 0 ? state.mqttPassword.c_str() : nullptr;

  Serial.printf("[mqtt] connecting host=%s port=%d device=%s\n",
                state.mqttHost.c_str(),
                state.mqttPort,
                state.mqttDeviceId.c_str());

  const bool ok = mqttClient.connect(
      clientId.c_str(),
      username,
      password,
      willTopic.c_str(),
      1,
      true,
      willPayload.c_str());

  if (!ok) {
    Serial.printf("[mqtt] connect failed state=%d\n", mqttClient.state());
    setStatus("MQTT fail");
    return;
  }

  state.mqttConnected = true;
  mqttClient.subscribe(mqttTopic("config/desired").c_str(), 1);
  mqttClient.subscribe(mqttTopic("command").c_str(), 1);
  publishDeviceStatus("online", true);
  publishTelemetryNow();
  setStatus("MQTT OK");
}

bool httpGetString(const String& url, String& out, uint32_t timeoutMs = 15000) {
  if (!networkConnected()) {
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(timeoutMs);

  if (!http.begin(client, url)) {
    return false;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[http] GET %s failed: %d\n", url.c_str(), code);
    http.end();
    return false;
  }

  out = http.getString();
  http.end();
  return true;
}

int compareVersions(const String& a, const String& b) {
  int ai = 0;
  int bi = 0;
  while (ai < a.length() || bi < b.length()) {
    long av = 0;
    long bv = 0;
    while (ai < a.length() && a[ai] != '.') {
      if (isDigit(a[ai])) {
        av = av * 10 + (a[ai] - '0');
      }
      ai++;
    }
    while (bi < b.length() && b[bi] != '.') {
      if (isDigit(b[bi])) {
        bv = bv * 10 + (b[bi] - '0');
      }
      bi++;
    }
    if (av != bv) {
      return av > bv ? 1 : -1;
    }
    ai++;
    bi++;
  }
  return 0;
}

void flushModemInput() {
  while (ModemSerial.available()) {
    ModemSerial.read();
  }
}

String readModemUntil(uint32_t timeoutMs, const String& token = "") {
  String response;
  const uint32_t startedAt = millis();
  while (millis() - startedAt < timeoutMs) {
    feedWatchdog();
    while (ModemSerial.available()) {
      const char c = static_cast<char>(ModemSerial.read());
      response += c;
      if (token.length() > 0 && response.indexOf(token) >= 0) {
        return response;
      }
    }
    delay(10);
  }
  return response;
}

bool sendAT(const String& command, const String& expected = "OK", uint32_t timeoutMs = 2000, String* responseOut = nullptr) {
  flushModemInput();
  Serial.println("[modem] >> " + command);
  ModemSerial.print(command);
  ModemSerial.print("\r\n");
  String response = readModemUntil(timeoutMs, expected);
  response.trim();
  Serial.println("[modem] << " + response);

  if (responseOut != nullptr) {
    *responseOut = response;
  }
  return expected.length() == 0 || response.indexOf(expected) >= 0;
}

bool i2cDevicePresent(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

void scanI2cBus() {
  Serial.printf("[i2c] scan SDA=%d SCL=%d\n", COF_PIN_I2C_SDA, COF_PIN_I2C_SCL);
  uint8_t found = 0;
  for (uint8_t address = 1; address < 127; address++) {
    if (i2cDevicePresent(address)) {
      Serial.printf("[i2c] found device at 0x%02X\n", address);
      found++;
    }
  }
  if (found == 0) {
    Serial.println("[i2c] no devices found");
  }
}

void initDisplay() {
  scanI2cBus();

  if (i2cDevicePresent(0x3C)) {
    state.oledAddress = 0x3C;
  } else if (i2cDevicePresent(0x3D)) {
    state.oledAddress = 0x3D;
  } else {
    state.oledReady = false;
    setStatus("OLED no I2C");
    return;
  }

  display.setI2CAddress(state.oledAddress << 1);
  display.begin();
  state.oledReady = true;
  setStatus("OLED 0x" + String(state.oledAddress, HEX) + " OK");
}

bool modemWaitForPrompt(uint32_t timeoutMs) {
  const String response = readModemUntil(timeoutMs, ">");
  Serial.println("[modem] << " + response);
  return response.indexOf(">") >= 0;
}

void detectPcf8574() {
  const uint8_t ranges[][2] = {
    {0x20, 0x27},
    {0x38, 0x3F},
  };

  for (const auto& range : ranges) {
    for (uint8_t address = range[0]; address <= range[1]; address++) {
      Wire.beginTransmission(address);
      if (Wire.endTransmission() == 0) {
        state.pcfReady = true;
        state.pcfAddress = address;
        Wire.beginTransmission(address);
        Wire.write(0xFF);
        Wire.endTransmission();
        Serial.printf("[i2c] PCF8574 found at 0x%02X\n", address);
        return;
      }
    }
  }

  state.pcfReady = false;
  Serial.println("[i2c] PCF8574 not found");
}

bool readTestButton() {
  if (!state.pcfReady) {
    return false;
  }

  Wire.requestFrom(state.pcfAddress, static_cast<uint8_t>(1));
  if (Wire.available() != 1) {
    return false;
  }

  const uint8_t value = Wire.read();
  return (value & 0x01) == 0;
}

void readSensors() {
  if (state.sht31Ready) {
    state.shtTemperature = sht31.readTemperature();
    state.shtHumidity = sht31.readHumidity();
  }

  if (state.ds18b20Ready) {
    ds18b20.requestTemperatures();
    const float temp = ds18b20.getTempCByIndex(0);
    if (temp > -100.0F && temp < 125.0F) {
      state.dsTemperature = temp;
    }
  }

  state.zmptRaw = analogRead(COF_PIN_ZMPT_ADC);
}

void drawDisplay() {
  if (!state.oledReady) {
    return;
  }

  char line[32];
  display.clearBuffer();
  display.setFont(u8g2_font_5x8_tf);

  display.drawStr(0, 8, "CallOnFail");
  snprintf(line, sizeof(line), "FW %s", COF_FIRMWARE_VERSION);
  display.drawStr(72, 8, line);

  if (state.ethernetConnected) {
    snprintf(line, sizeof(line), "ETH %s", state.ipAddress.c_str());
  } else if (state.wifiConnected) {
    snprintf(line, sizeof(line), "WIFI %s", state.wifiIpAddress.c_str());
  } else {
    snprintf(line, sizeof(line), "NET NO");
  }
  display.drawStr(0, 19, line);

  if (state.sht31Ready && !isnan(state.shtTemperature) && !isnan(state.shtHumidity)) {
    snprintf(line, sizeof(line), "SHT %.1fC %.0f%%", state.shtTemperature, state.shtHumidity);
  } else {
    snprintf(line, sizeof(line), "SHT --");
  }
  display.drawStr(0, 30, line);

  if (state.ds18b20Ready && !isnan(state.dsTemperature)) {
    snprintf(line, sizeof(line), "DS18 %.1fC", state.dsTemperature);
  } else {
    snprintf(line, sizeof(line), "DS18 --");
  }
  display.drawStr(0, 41, line);

  snprintf(line, sizeof(line), "ADC %d MQTT %s", state.zmptRaw, state.mqttConnected ? "OK" : "--");
  display.drawStr(0, 52, line);

  String footer = state.statusLine;
  if (footer.length() > 21) {
    footer = footer.substring(0, 21);
  }
  display.drawStr(0, 63, footer.c_str());

  display.sendBuffer();
}

bool initModem() {
  ModemSerial.begin(115200, SERIAL_8N1, COF_PIN_MODEM_RX, COF_PIN_MODEM_TX);
  delay(300);

  bool gotAt = false;
  for (int attempt = 0; attempt < 5; attempt++) {
    if (sendAT("AT", "OK", 1000)) {
      gotAt = true;
      break;
    }
    delay(500);
  }

  if (!gotAt) {
    state.modemReady = false;
    state.simReady = false;
    setStatus("Modem no AT");
    return false;
  }

  state.modemReady = true;
  sendAT("ATE0", "OK", 1000);
  sendAT("ATI", "OK", 2000);

  String response;
  if (sendAT("AT+CPIN?", "OK", 2000, &response)) {
    state.simReady = response.indexOf("READY") >= 0;
  }

  if (sendAT("AT+CSQ", "OK", 2000, &response)) {
    const int marker = response.indexOf("+CSQ:");
    if (marker >= 0) {
      state.signalQuality = response.substring(marker + 5).toInt();
    }
  }

  state.modemAudioPlaybackSupported = sendAT("AT+CCMXPLAY=?", "OK", 3000);
  state.modemFileTransferSupported = sendAT("AT+CFTRANRX=?", "OK", 3000);

  setStatus(state.simReady ? "Modem/SIM OK" : "Modem OK no SIM");
  return true;
}

void pollModem() {
  if (!state.modemReady) {
    initModem();
    return;
  }

  String response;
  if (sendAT("AT+CSQ", "OK", 2000, &response)) {
    const int marker = response.indexOf("+CSQ:");
    if (marker >= 0) {
      state.signalQuality = response.substring(marker + 5).toInt();
    }
  }
}

bool uploadAudioToModem(const String& url, const String& modemPath, const String& audioVersion) {
  if (!networkConnected() || !state.modemReady || !state.modemFileTransferSupported) {
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(30000);

  if (!http.begin(client, url)) {
    return false;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[audio] download failed: %d\n", code);
    http.end();
    return false;
  }

  const int size = http.getSize();
  if (size <= 0) {
    Serial.println("[audio] missing content length");
    http.end();
    return false;
  }

  state.audioSyncInProgress = true;
  setStatus("Audio to modem");

  flushModemInput();
  ModemSerial.printf("AT+CFTRANRX=\"%s\",%d\r\n", modemPath.c_str(), size);
  if (!modemWaitForPrompt(10000)) {
    http.end();
    state.audioSyncInProgress = false;
    setStatus("Audio prompt fail");
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buffer[512];
  int remaining = size;
  while (remaining > 0) {
    feedWatchdog();
    const size_t chunk = min(static_cast<int>(sizeof(buffer)), remaining);
    const int bytesRead = stream->readBytes(buffer, chunk);
    if (bytesRead <= 0) {
      http.end();
      state.audioSyncInProgress = false;
      setStatus("Audio read fail");
      return false;
    }
    ModemSerial.write(buffer, bytesRead);
    remaining -= bytesRead;
    delay(1);
  }

  const String response = readModemUntil(20000, "OK");
  http.end();
  state.audioSyncInProgress = false;

  if (response.indexOf("OK") >= 0) {
    preferences.putString("audioVersion", audioVersion);
    setStatus("Audio synced");
    return true;
  }

  setStatus("Audio upload fail");
  return false;
}

bool performOta(const String& url, const String& newVersion) {
  if (!networkConnected()) {
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(30000);

  if (!http.begin(client, url)) {
    return false;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[ota] download failed: %d\n", code);
    http.end();
    return false;
  }

  const int size = http.getSize();
  if (size <= 0) {
    http.end();
    return false;
  }

  state.otaInProgress = true;
  setStatus("OTA updating");

  if (!Update.begin(size)) {
    Serial.printf("[ota] Update.begin failed: %s\n", Update.errorString());
    http.end();
    state.otaInProgress = false;
    return false;
  }

  WiFiClient& stream = http.getStream();
  const size_t written = Update.writeStream(stream);
  if (written != static_cast<size_t>(size)) {
    Serial.printf("[ota] incomplete write: %u/%d\n", static_cast<unsigned>(written), size);
    Update.abort();
    http.end();
    state.otaInProgress = false;
    return false;
  }

  if (!Update.end() || !Update.isFinished()) {
    Serial.printf("[ota] Update.end failed: %s\n", Update.errorString());
    http.end();
    state.otaInProgress = false;
    return false;
  }

  preferences.putString("lastOtaVersion", newVersion);
  setStatus("OTA rebooting");
  http.end();
  delay(1000);
  ESP.restart();
  return true;
}

void checkManifest(bool allowFirmwareUpdate) {
  String payload;
  setStatus("Check manifest");
  if (!httpGetString(COF_MANIFEST_URL, payload)) {
    setStatus("Manifest fail");
    return;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    Serial.printf("[manifest] json error: %s\n", error.c_str());
    setStatus("Manifest JSON err");
    return;
  }

  state.manifestFirmwareVersion = doc["firmware"]["version"] | "";
  state.manifestFirmwareUrl = doc["firmware"]["url"] | "";
  state.manifestAudioVersion = doc["audio"]["version"] | "";
  state.manifestAudioUrl = doc["audio"]["url"] | "";
  state.modemAudioPath = doc["audio"]["modem_path"] | COF_MODEM_AUDIO_PATH;
  state.manifestPhoneNumber = doc["config"]["phone_number"] | COF_PHONE_NUMBER;

  if (allowFirmwareUpdate && state.manifestFirmwareVersion.length() > 0 &&
      state.manifestFirmwareUrl.length() > 0 &&
      compareVersions(state.manifestFirmwareVersion, COF_FIRMWARE_VERSION) > 0) {
    performOta(state.manifestFirmwareUrl, state.manifestFirmwareVersion);
    return;
  }

  const String currentAudioVersion = preferences.getString("audioVersion", "");
  if (state.manifestAudioVersion.length() > 0 && state.manifestAudioUrl.length() > 0 &&
      state.manifestAudioVersion != currentAudioVersion) {
    uploadAudioToModem(state.manifestAudioUrl, state.modemAudioPath, state.manifestAudioVersion);
    return;
  }

  setStatus("Manifest OK");
}

void placeCallAndPlayAudio() {
  if (!COF_ENABLE_CALLS) {
    setStatus("Calls disabled");
    Serial.println("[call] Set COF_ENABLE_CALLS to 1 and COF_PHONE_NUMBER before testing calls.");
    return;
  }

  if (!state.modemReady || !state.simReady) {
    setStatus("No modem/SIM");
    return;
  }

  String phone = state.manifestPhoneNumber;
  phone.trim();
  if (phone.length() < 8 || phone.indexOf("X") >= 0) {
    setStatus("No phone cfg");
    return;
  }

  state.callInProgress = true;
  setStatus("Calling");
  if (!sendAT("ATD" + phone + ";", "OK", 10000)) {
    state.callInProgress = false;
    setStatus("Call failed");
    return;
  }

  delay(8000);
  setStatus("Playing audio");
  sendAT("AT+CCMXPLAY=\"" + state.modemAudioPath + "\",1,0", "OK", 5000);
  readModemUntil(45000, "+AUDIOSTATE: audio play stop");
  sendAT("ATH", "OK", 5000);

  state.callInProgress = false;
  setStatus("Call done");
}

void handleButton() {
  const bool pressed = readTestButton();
  const uint32_t now = millis();

  if (pressed && !lastButtonPressed) {
    buttonPressedAtMs = now;
    setStatus("Button pressed");
  }

  if (!pressed && lastButtonPressed) {
    const uint32_t heldMs = now - buttonPressedAtMs;
    if (heldMs > 3000) {
      checkManifest(true);
    } else {
      placeCallAndPlayAudio();
    }
  }

  lastButtonPressed = pressed;
}

void printSerialHelp() {
  Serial.println();
  Serial.println("CallOnFail serial commands:");
  Serial.println("  h          help");
  Serial.println("  s          print status");
  Serial.println("  i          scan I2C bus");
  Serial.println("  t          read sensors now");
  Serial.println("  m          re-init modem");
  Serial.println("  o          force manifest/OTA check");
  Serial.println("  a          force manifest/audio sync check");
  Serial.println("  c          place test call if calls are enabled");
  Serial.println("  r          restart ESP32");
  Serial.println("  wifi SSID PASSWORD  save and connect WiFi");
  Serial.println("  wifi-clear          forget saved WiFi");
  Serial.println("  wifi-status         print WiFi status");
  Serial.println("  mqtt HOST PORT DEVICE_ID [USER PASSWORD]");
  Serial.println("  mqtt-clear          forget saved MQTT config");
  Serial.println("  mqtt-status         print MQTT status");
  Serial.println("  pub                 publish telemetry now");
  Serial.println("  AT...      send raw AT command to modem");
  Serial.println();
}

void printRuntimeStatus() {
  Serial.println();
  Serial.println("CallOnFail status");
  Serial.println("-----------------");
  Serial.println("Firmware: " COF_FIRMWARE_VERSION);
  Serial.printf("OLED: %s", state.oledReady ? "OK" : "NO");
  if (state.oledReady) {
    Serial.printf(" 0x%02X", state.oledAddress);
  }
  Serial.println();
  Serial.printf("Ethernet: %s IP=%s\n", state.ethernetConnected ? "OK" : "NO", state.ipAddress.c_str());
  Serial.printf("WiFi: %s configured=%s SSID=%s IP=%s\n",
                state.wifiConnected ? "OK" : "NO",
                state.wifiConfigured ? "YES" : "NO",
                state.wifiSsid.c_str(),
                state.wifiIpAddress.c_str());
  Serial.printf("MQTT: %s configured=%s host=%s port=%d device=%s\n",
                state.mqttConnected ? "OK" : "NO",
                state.mqttConfigured ? "YES" : "NO",
                state.mqttHost.c_str(),
                state.mqttPort,
                state.mqttDeviceId.c_str());
  Serial.printf("Reported config: version=%d hash=%s\n",
                state.reportedConfigVersion,
                state.reportedConfigHash.c_str());
  Serial.printf("SHT31: %s temp=%.2f humidity=%.2f\n",
                state.sht31Ready ? "OK" : "NO", state.shtTemperature, state.shtHumidity);
  Serial.printf("DS18B20: %s temp=%.2f\n", state.ds18b20Ready ? "OK" : "NO", state.dsTemperature);
  Serial.printf("ZMPT ADC raw: %d\n", state.zmptRaw);
  Serial.printf("PCF8574: %s", state.pcfReady ? "OK" : "NO");
  if (state.pcfReady) {
    Serial.printf(" 0x%02X", state.pcfAddress);
  }
  Serial.println();
  Serial.printf("Modem: %s SIM=%s CSQ=%d audio=%s transfer=%s\n",
                state.modemReady ? "OK" : "NO",
                state.simReady ? "OK" : "NO",
                state.signalQuality,
                state.modemAudioPlaybackSupported ? "OK" : "NO",
                state.modemFileTransferSupported ? "OK" : "NO");
  Serial.printf("Manifest firmware: %s\n", state.manifestFirmwareVersion.c_str());
  Serial.printf("Manifest audio: %s\n", state.manifestAudioVersion.c_str());
  Serial.printf("Status: %s\n", state.statusLine.c_str());
  Serial.println();
}

void handleSerialCommand(const String& command) {
  if (command.length() == 0) {
    return;
  }

  if (command.startsWith("AT") || command.startsWith("at")) {
    sendAT(command, "", 5000);
    return;
  }

  if (command.startsWith("wifi ")) {
    String args = command.substring(5);
    args.trim();
    const int separator = args.indexOf(' ');
    if (separator <= 0) {
      Serial.println("[wifi] usage: wifi SSID PASSWORD");
      return;
    }
    String ssid = args.substring(0, separator);
    String password = args.substring(separator + 1);
    password.trim();
    connectWiFi(ssid, password, true);
    return;
  }

  if (command.equalsIgnoreCase("wifi-clear")) {
    clearSavedWiFi();
    return;
  }

  if (command.equalsIgnoreCase("wifi-status")) {
    Serial.printf("[wifi] configured=%s connected=%s SSID=%s IP=%s RSSI=%d\n",
                  state.wifiConfigured ? "yes" : "no",
                  state.wifiConnected ? "yes" : "no",
                  state.wifiSsid.c_str(),
                  state.wifiIpAddress.c_str(),
                  state.wifiConnected ? WiFi.RSSI() : 0);
    return;
  }

  if (command.startsWith("mqtt ")) {
    String args = command.substring(5);
    args.trim();

    String tokens[5];
    int count = 0;
    while (args.length() > 0 && count < 5) {
      const int separator = args.indexOf(' ');
      if (separator < 0) {
        tokens[count++] = args;
        break;
      }
      tokens[count++] = args.substring(0, separator);
      args = args.substring(separator + 1);
      args.trim();
    }

    if (count != 3 && count != 5) {
      Serial.println("[mqtt] usage: mqtt HOST PORT DEVICE_ID [USER PASSWORD]");
      return;
    }

    const int port = tokens[1].toInt();
    if (port <= 0 || port > 65535) {
      Serial.println("[mqtt] invalid port");
      return;
    }

    saveMqttConfig(tokens[0], port, tokens[2], count == 5 ? tokens[3] : "", count == 5 ? tokens[4] : "");
    connectMqttIfNeeded();
    return;
  }

  if (command.equalsIgnoreCase("mqtt-clear")) {
    clearMqttConfig();
    return;
  }

  if (command.equalsIgnoreCase("mqtt-status")) {
    Serial.printf("[mqtt] configured=%s connected=%s host=%s port=%d device=%s state=%d\n",
                  state.mqttConfigured ? "yes" : "no",
                  state.mqttConnected ? "yes" : "no",
                  state.mqttHost.c_str(),
                  state.mqttPort,
                  state.mqttDeviceId.c_str(),
                  mqttClient.state());
    return;
  }

  if (command.equalsIgnoreCase("pub")) {
    publishTelemetryNow();
    return;
  }

  if (command.length() != 1) {
    Serial.println("[serial] unknown command. Type h for help.");
    return;
  }

  switch (command[0]) {
    case 'h':
    case 'H':
      printSerialHelp();
      break;
    case 's':
    case 'S':
      printRuntimeStatus();
      break;
    case 'i':
    case 'I':
      scanI2cBus();
      break;
    case 't':
    case 'T':
      readSensors();
      drawDisplay();
      printRuntimeStatus();
      break;
    case 'm':
    case 'M':
      initModem();
      break;
    case 'o':
    case 'O':
      checkManifest(true);
      break;
    case 'a':
    case 'A':
      checkManifest(false);
      break;
    case 'c':
    case 'C':
      placeCallAndPlayAudio();
      break;
    case 'r':
    case 'R':
      Serial.println("[serial] restarting ESP32");
      delay(200);
      ESP.restart();
      break;
    default:
      Serial.println("[serial] unknown command. Type h for help.");
      break;
  }
}

void handleSerialInput() {
  while (Serial.available()) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      serialCommandBuffer.trim();
      handleSerialCommand(serialCommandBuffer);
      serialCommandBuffer = "";
      continue;
    }
    if (serialCommandBuffer.length() < 120) {
      serialCommandBuffer += c;
    } else {
      serialCommandBuffer = "";
      Serial.println("[serial] command too long, buffer cleared");
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  beginInternalWatchdog();
  Serial.println();
  Serial.println("CallOnFail boot");
  Serial.println("Firmware " COF_FIRMWARE_VERSION);
  printSerialHelp();

  preferences.begin("cof", false);
  loadSavedMqttConfig();
  pinMode(COF_PIN_ZMPT_ADC, INPUT);

  Wire.begin(COF_PIN_I2C_SDA, COF_PIN_I2C_SCL);
  Wire.setClock(100000);
  initDisplay();
  drawDisplay();

  state.sht31Ready = sht31.begin(COF_SHT31_ADDRESS);
  Serial.println(state.sht31Ready ? "[i2c] SHT31 OK" : "[i2c] SHT31 not found");

  ds18b20.begin();
  state.ds18b20Ready = ds18b20.getDeviceCount() > 0;
  Serial.printf("[onewire] DS18B20 count: %d\n", ds18b20.getDeviceCount());

  detectPcf8574();
  beginEthernet();
  beginSavedWiFi();
  initModem();
  readSensors();
  drawDisplay();
}

void loop() {
  const uint32_t now = millis();
  feedWatchdog();
  handleSerialInput();

  if (now - lastSensorMs >= kSensorIntervalMs) {
    lastSensorMs = now;
    readSensors();
  }

  if (now - lastDisplayMs >= kDisplayIntervalMs) {
    lastDisplayMs = now;
    drawDisplay();
  }

  connectMqttIfNeeded();

  if (state.mqttConnected && pendingConfigReport) {
    publishConfigReported();
    pendingConfigReport = false;
  }

  if (state.mqttConnected && pendingCommandAck) {
    publishCommandAck();
    pendingCommandAck = false;
  }

  if (pendingOtaCommand && !state.callInProgress && !state.otaInProgress && !state.audioSyncInProgress) {
    pendingOtaCommand = false;
    setStatus("OTA command");
    checkManifest(true);
  }

  if (state.mqttConnected && now - lastTelemetryPublishMs >= kTelemetryPublishIntervalMs) {
    lastTelemetryPublishMs = now;
    publishTelemetryNow();
  }

  if (now - lastModemMs >= kModemIntervalMs && !state.callInProgress && !state.audioSyncInProgress) {
    lastModemMs = now;
    pollModem();
  }

  if (state.pcfReady && !state.callInProgress && !state.otaInProgress && !state.audioSyncInProgress) {
    handleButton();
  }

  if (networkConnected() && !state.callInProgress && !state.otaInProgress && !state.audioSyncInProgress) {
    if (!didInitialManifestCheck && now > kManifestInitialDelayMs) {
      didInitialManifestCheck = true;
      lastManifestMs = now;
      checkManifest(true);
    } else if (didInitialManifestCheck && now - lastManifestMs >= kManifestIntervalMs) {
      lastManifestMs = now;
      checkManifest(true);
    }
  }

  delay(20);
}
