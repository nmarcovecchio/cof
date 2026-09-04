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
WiFiClient mqttPlainClient;
WiFiClientSecure mqttTlsClient;
PubSubClient mqttClient;
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
  bool networkRegistered = false;
  bool modemAudioPlaybackSupported = false;
  bool modemFileTransferSupported = false;
  int cregStat = -1;
  int ceregStat = -1;
  int cgregStat = -1;
  String operatorName = "";
  String smsc = "";
  String apn = COF_MODEM_APN;
  String modemModel = "";
  String radioInfo = "";
  String radioMode = "";
  int cnmp = -1;
  int imsVoice = -1;
  int imsReg = -1;
  bool forcedGsmForCall = false;
  bool skipGsmVoice = false;
  String imsi = "";
  String voiceIdentity = "";
  String predictedVoicePath = "unknown";
  String observedVoicePath = "";
  String radioAtDial = "";
  String radioAtConnect = "";
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
  uint32_t telemetryIntervalMs = kTelemetryPublishIntervalMs;
  bool callingEnabled = false;
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
bool pendingStatusReportCommand = false;
bool pendingTestCallCommand = false;
String pendingTestCallPhone = "";
bool pendingTestSmsCommand = false;
String pendingTestSmsPhone = "";
String pendingTestSmsText = "";
bool pendingCommandAck = false;
String pendingCommandId = "";
String pendingCommandName = "";
String pendingCommandStatus = "";
String pendingCommandMessage = "";

bool csAttached();
void onMqttMessage(char* topic, byte* payload, unsigned int length);

void setStatus(const String& line) {
  state.statusLine = line;
  Serial.println("[status] " + line);
}

bool mqttUsesTls() {
  return state.mqttPort == 8883 || state.mqttPort == 8884;
}

void configureMqttClientTransport() {
  if (mqttUsesTls()) {
    mqttTlsClient.setInsecure();
    mqttClient.setClient(mqttTlsClient);
  } else {
    mqttClient.setClient(mqttPlainClient);
  }
  mqttClient.setServer(state.mqttHost.c_str(), state.mqttPort);
  mqttClient.setCallback(onMqttMessage);
  mqttClient.setBufferSize(4096);
}

bool applyDesiredConfig(JsonDocument& doc) {
  pendingConfigError = "";

  int telemetrySeconds = doc["telemetry_interval_seconds"] | 60;
  if (telemetrySeconds < 10 || telemetrySeconds > 3600) {
    pendingConfigError = "telemetry_interval_seconds out of range";
    return false;
  }

  bool callingEnabled = false;
  if (doc["calling"].is<JsonObject>() || doc["calling"].is<JsonObjectConst>()) {
    callingEnabled = doc["calling"]["enabled"] | false;
  } else if (!doc["calling"].isNull()) {
    pendingConfigError = "calling must be object";
    return false;
  }

  if (!preferences.putInt("cfgVer", pendingConfigVersion)) {
    pendingConfigError = "failed to store config_version";
    return false;
  }
  if (!preferences.putString("cfgHash", pendingConfigHash)) {
    pendingConfigError = "failed to store config_hash";
    return false;
  }
  if (!preferences.putInt("telemetrySec", telemetrySeconds)) {
    pendingConfigError = "failed to store telemetry interval";
    return false;
  }
  if (!preferences.putBool("callEn", callingEnabled)) {
    pendingConfigError = "failed to store calling flag";
    return false;
  }

  state.reportedConfigVersion = pendingConfigVersion;
  state.reportedConfigHash = pendingConfigHash;
  state.telemetryIntervalMs = static_cast<uint32_t>(telemetrySeconds) * 1000UL;
  state.callingEnabled = callingEnabled;

  Serial.printf("[config] applied v%d hash=%s telemetry=%ds calling=%s\n",
                pendingConfigVersion,
                pendingConfigHash.c_str(),
                telemetrySeconds,
                callingEnabled ? "on" : "off");
  return true;
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
      } else if (pendingConfigVersion == state.reportedConfigVersion &&
                 pendingConfigHash.length() > 0 &&
                 pendingConfigHash == state.reportedConfigHash) {
        Serial.printf("[config] skip already applied v%d\n", pendingConfigVersion);
      } else if (!applyDesiredConfig(doc)) {
        if (pendingConfigError.length() == 0) {
          pendingConfigError = "config apply failed";
        }
        pendingConfigReport = true;
      } else {
        pendingConfigApplied = true;
        pendingConfigReport = true;
        setStatus("Config v" + String(pendingConfigVersion));
      }
    }

    if (pendingConfigError.length() > 0) {
      pendingConfigReport = true;
    }
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
      } else if (pendingCommandName == "status_report") {
        pendingStatusReportCommand = true;
        pendingCommandStatus = "accepted";
        pendingCommandMessage = "Status report scheduled";
      } else if (pendingCommandName == "test_call") {
        pendingTestCallCommand = true;
        pendingTestCallPhone = doc["phone"] | "";
        pendingTestCallPhone.trim();
        pendingCommandStatus = "accepted";
        pendingCommandMessage = "Test call scheduled";
      } else if (pendingCommandName == "test_sms") {
        pendingTestSmsCommand = true;
        pendingTestSmsPhone = doc["phone"] | "";
        pendingTestSmsPhone.trim();
        pendingTestSmsText = doc["text"] | "CallOnFail prueba SMS";
        pendingTestSmsText.trim();
        pendingCommandStatus = "accepted";
        pendingCommandMessage = "Test SMS scheduled";
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
  state.reportedConfigVersion = preferences.getInt("cfgVer", preferences.getInt("reportedCfgVersion", 0));
  state.reportedConfigHash = preferences.getString("cfgHash", preferences.getString("reportedCfgHash", ""));
  const int telemetrySeconds = preferences.getInt("telemetrySec", 60);
  state.telemetryIntervalMs = static_cast<uint32_t>(constrain(telemetrySeconds, 10, 3600)) * 1000UL;
  state.callingEnabled = preferences.getBool("callEn", preferences.getBool("callingEnabled", COF_ENABLE_CALLS != 0));
  state.skipGsmVoice = preferences.getBool("skipGsm", false);
  state.observedVoicePath = preferences.getString("voiceOk", "");
  state.voiceIdentity = preferences.getString("voiceId", "");
  state.mqttConfigured = state.mqttHost.length() > 0 && state.mqttDeviceId.length() > 0;

  if (state.mqttConfigured) {
    // Migrate previous plaintext lab endpoint to TLS + auth defaults.
    if (state.mqttHost == "mqtt.callonfail.com.ar" && state.mqttPort == 1883) {
      state.mqttPort = COF_DEFAULT_MQTT_PORT;
      if (state.mqttUsername.length() == 0) {
        state.mqttUsername = COF_DEFAULT_MQTT_USERNAME;
      }
      if (state.mqttPassword.length() == 0) {
        state.mqttPassword = COF_DEFAULT_MQTT_PASSWORD;
      }
      preferences.putInt("mqttPort", state.mqttPort);
      preferences.putString("mqttUser", state.mqttUsername);
      preferences.putString("mqttPass", state.mqttPassword);
      Serial.println("[mqtt] migrated lab endpoint to TLS :8883");
    }

    configureMqttClientTransport();
    Serial.printf("[mqtt] saved config host=%s port=%d device=%s user=%s tls=%s\n",
                  state.mqttHost.c_str(),
                  state.mqttPort,
                  state.mqttDeviceId.c_str(),
                  state.mqttUsername.c_str(),
                  mqttUsesTls() ? "yes" : "no");
    Serial.printf("[config] reported=v%d telemetry=%lus calling=%s\n",
                  state.reportedConfigVersion,
                  static_cast<unsigned long>(state.telemetryIntervalMs / 1000UL),
                  state.callingEnabled ? "on" : "off");
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

  configureMqttClientTransport();
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

  char payload[3072];
  const String topic = mqttTopic(suffix);
  const size_t requiredLength = measureJson(doc);
  if (requiredLength >= sizeof(payload)) {
    Serial.printf("[mqtt] payload too large topic=%s required=%u max=%u\n",
                  topic.c_str(),
                  static_cast<unsigned>(requiredLength),
                  static_cast<unsigned>(sizeof(payload) - 1));
    return false;
  }

  const size_t length = serializeJson(doc, payload, sizeof(payload));
  const bool ok = mqttClient.publish(topic.c_str(), reinterpret_cast<const uint8_t*>(payload), length, retained);
  Serial.printf("[mqtt] publish topic=%s ok=%s payload=%s\n", topic.c_str(), ok ? "yes" : "no", payload);
  (void)qos;
  return ok;
}

String ds18b20AddressToString(const DeviceAddress address) {
  char buffer[17];
  for (uint8_t i = 0; i < 8; i++) {
    snprintf(&buffer[i * 2], 3, "%02X", address[i]);
  }
  buffer[16] = '\0';
  return String(buffer);
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

  doc["hardware_profile"] = "cof-wt32-a7672-v1";
  JsonObject capabilities = doc["capabilities"].to<JsonObject>();
  capabilities["ethernet"] = true;
  capabilities["wifi"] = true;
  capabilities["modem_a7672"] = true;
  capabilities["phone_calls"] = true;
  capabilities["sms"] = true;
  capabilities["audio_playback"] = state.modemAudioPlaybackSupported;
  capabilities["modem_file_transfer"] = state.modemFileTransferSupported;
  capabilities["sht31"] = true;
  capabilities["ds18b20_bus"] = true;
  capabilities["max_ds18b20"] = 8;
  capabilities["mains_voltage"] = true;
  capabilities["pcf8574"] = true;
  capabilities["external_inputs"] = 2;
  capabilities["external_outputs"] = 2;

  JsonObject discovered = doc["discovered"].to<JsonObject>();
  discovered["sht31"] = state.sht31Ready;
  discovered["pcf8574"] = state.pcfReady;
  discovered["modem"] = state.modemReady;
  JsonObject cellular = discovered["cellular"].to<JsonObject>();
  cellular["registered"] = state.networkRegistered;
  cellular["creg"] = state.cregStat;
  cellular["cereg"] = state.ceregStat;
  cellular["cgreg"] = state.cgregStat;
  cellular["csq"] = state.signalQuality;
  cellular["operator"] = state.operatorName;
  cellular["apn"] = state.apn;
  cellular["smsc"] = state.smsc;
  cellular["model"] = state.modemModel;
  cellular["radio"] = state.radioMode;
  cellular["cpsi"] = state.radioInfo;
  cellular["cnmp"] = state.cnmp;
  cellular["ims"] = state.imsReg == 1;
  cellular["ims_reg"] = state.imsReg;
  cellular["ims_voice"] = state.imsVoice;
  cellular["imsi"] = state.imsi;
  JsonObject voice = cellular["voice"].to<JsonObject>();
  voice["path"] = state.predictedVoicePath;
  voice["last_ok"] = state.observedVoicePath;
  voice["cs_attached"] = csAttached();
  voice["radio"] = state.radioMode;
  voice["ims"] = state.imsReg == 1;
  cellular["voice_path"] = state.predictedVoicePath;
  cellular["gsm_usable"] = !state.skipGsmVoice;
  discovered["ds18b20_count"] = ds18b20.getDeviceCount();
  JsonArray ds18b20Addresses = discovered["ds18b20"].to<JsonArray>();
  for (int i = 0; i < ds18b20.getDeviceCount(); i++) {
    DeviceAddress address;
    if (ds18b20.getAddress(address, i)) {
      ds18b20Addresses.add(ds18b20AddressToString(address));
    }
  }

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

void publishDeviceEvent(const char* type, const char* severity, const String& message) {
  JsonDocument doc;
  doc["device_id"] = state.mqttDeviceId;
  doc["firmware"] = COF_FIRMWARE_VERSION;
  doc["type"] = type;
  doc["severity"] = severity;
  doc["message"] = message;
  publishMqttJson("event", doc, false, 1);
}

void waitWithWatchdog(uint32_t ms) {
  const uint32_t startedAt = millis();
  while (millis() - startedAt < ms) {
    feedWatchdog();
    if (state.mqttConnected) {
      mqttClient.loop();
    }
    delay(50);
  }
}

bool phoneLooksValid(const String& phone) {
  return phone.length() >= 8 && phone.indexOf("X") < 0;
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

  configureMqttClientTransport();

  const String clientId = state.mqttDeviceId + "-" + String(static_cast<uint32_t>(ESP.getEfuseMac()), HEX);
  const String willTopic = mqttTopic("status");
  const String willPayload = "{\"status\":\"offline\",\"device_id\":\"" + state.mqttDeviceId + "\"}";
  const char* username = state.mqttUsername.length() > 0 ? state.mqttUsername.c_str() : nullptr;
  const char* password = state.mqttUsername.length() > 0 ? state.mqttPassword.c_str() : nullptr;

  Serial.printf("[mqtt] connecting host=%s port=%d device=%s user=%s tls=%s\n",
                state.mqttHost.c_str(),
                state.mqttPort,
                state.mqttDeviceId.c_str(),
                state.mqttUsername.c_str(),
                mqttUsesTls() ? "yes" : "no");

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
    if (state.mqttConnected) {
      mqttClient.loop();
    }
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

int parseCommaStat(const String& response, const char* tag) {
  const int tagAt = response.indexOf(tag);
  if (tagAt < 0) {
    return -1;
  }
  const int comma = response.indexOf(',', tagAt);
  if (comma < 0) {
    return -1;
  }
  return response.substring(comma + 1).toInt();
}

String extractQuoted(const String& response) {
  const int start = response.indexOf('"');
  if (start < 0) {
    return "";
  }
  const int end = response.indexOf('"', start + 1);
  if (end < 0) {
    return "";
  }
  return response.substring(start + 1, end);
}

String firstNonEmptyAtLine(const String& response) {
  int start = 0;
  while (start < static_cast<int>(response.length())) {
    int end = response.indexOf('\n', start);
    if (end < 0) {
      end = response.length();
    }
    String line = response.substring(start, end);
    line.replace("\r", "");
    line.trim();
    start = end + 1;
    if (line.length() == 0 || line == "OK" || line == "ERROR" || line.startsWith("AT")) {
      continue;
    }
    return line;
  }
  return "";
}

String extractAtTagValue(const String& response, const char* tag) {
  const int idx = response.indexOf(tag);
  if (idx < 0) {
    return "";
  }
  int start = idx + static_cast<int>(strlen(tag));
  while (start < static_cast<int>(response.length()) &&
         (response[start] == ' ' || response[start] == ':')) {
    start++;
  }
  int end = start;
  while (end < static_cast<int>(response.length()) &&
         response[end] != '\r' && response[end] != '\n') {
    end++;
  }
  String value = response.substring(start, end);
  value.trim();
  return value;
}

bool radioIsGsm();
bool imsVoiceReady();
void persistSkipGsm(bool skip);

bool networkStatRegistered(int stat) {
  return stat == 1 || stat == 5 || stat == 9 || stat == 10;
}

bool csAttached() {
  return networkStatRegistered(state.cregStat);
}

String inferVoicePath() {
  if (imsVoiceReady()) {
    return "volte";
  }
  if (radioIsGsm() && csAttached()) {
    return "gsm";
  }
  if (state.radioMode.indexOf("LTE") >= 0) {
    if (state.cregStat == 9 || state.cregStat == 10) {
      return "csfb_not_preferred";
    }
    if (csAttached()) {
      return "csfb";
    }
    if (networkStatRegistered(state.ceregStat)) {
      return "lte_data";
    }
  }
  if (!state.networkRegistered) {
    return "none";
  }
  return "unknown";
}

String observeVoicePath(const String& radioDial, const String& radioConnect) {
  if (imsVoiceReady() && radioConnect.indexOf("LTE") >= 0) {
    return "volte";
  }
  if (radioDial.indexOf("LTE") >= 0 && radioConnect.indexOf("GSM") >= 0) {
    return "csfb";
  }
  if (radioDial.indexOf("GSM") >= 0 && radioConnect.indexOf("GSM") >= 0) {
    return "gsm";
  }
  if (radioDial.indexOf("LTE") >= 0 && radioConnect.indexOf("LTE") >= 0) {
    return imsVoiceReady() ? "volte" : "csfb";
  }
  return inferVoicePath();
}

void persistObservedVoicePath(const String& path) {
  state.observedVoicePath = path;
  preferences.putString("voiceOk", path);
}

void noteSubscriberIdentity() {
  String identity = state.imsi;
  if (state.operatorName.length() > 0) {
    if (identity.length() > 0) {
      identity += "|";
    }
    identity += state.operatorName;
  }
  if (identity.length() < 3) {
    return;
  }
  if (state.voiceIdentity == identity) {
    return;
  }
  if (state.voiceIdentity.length() > 0) {
    Serial.println("[modem] SIM/operator changed, relearning voice path");
    persistSkipGsm(false);
    persistObservedVoicePath("");
  }
  state.voiceIdentity = identity;
  preferences.putString("voiceId", identity);
}

void refreshCellularStatus() {
  String response;
  if (sendAT("AT+CREG?", "OK", 2000, &response)) {
    state.cregStat = parseCommaStat(response, "+CREG:");
  }
  if (sendAT("AT+CEREG?", "OK", 2000, &response)) {
    state.ceregStat = parseCommaStat(response, "+CEREG:");
  }
  if (sendAT("AT+CGREG?", "OK", 2000, &response)) {
    state.cgregStat = parseCommaStat(response, "+CGREG:");
  }
  if (sendAT("AT+COPS?", "OK", 3000, &response)) {
    const String name = extractQuoted(response);
    if (name.length() > 0) {
      state.operatorName = name;
    }
  }
  if (sendAT("AT+CSCA?", "OK", 3000, &response)) {
    const String smsc = extractQuoted(response);
    if (smsc.length() > 0) {
      state.smsc = smsc;
    }
  }
  if (sendAT("AT+CSQ", "OK", 2000, &response)) {
    const int marker = response.indexOf("+CSQ:");
    if (marker >= 0) {
      state.signalQuality = response.substring(marker + 5).toInt();
    }
  }
  if (state.modemModel.length() == 0 && sendAT("AT+CGMM", "OK", 2000, &response)) {
    const String model = firstNonEmptyAtLine(response);
    if (model.length() > 0) {
      state.modemModel = model;
    }
  }
  if (sendAT("AT+CPSI?", "OK", 3000, &response)) {
    String cpsi = extractAtTagValue(response, "+CPSI:");
    if (cpsi.length() > 96) {
      cpsi = cpsi.substring(0, 96);
    }
    if (cpsi.length() > 0) {
      state.radioInfo = cpsi;
      const int comma = cpsi.indexOf(',');
      state.radioMode = comma >= 0 ? cpsi.substring(0, comma) : cpsi;
      state.radioMode.trim();
    }
  }
  if (sendAT("AT+CNMP?", "OK", 2000, &response)) {
    state.cnmp = extractAtTagValue(response, "+CNMP:").toInt();
  }
  if (sendAT("AT+CAVIMS?", "OK", 2000, &response)) {
    state.imsVoice = extractAtTagValue(response, "+CAVIMS:").toInt();
  }
  if (sendAT("AT+CIREG?", "OK", 2000, &response)) {
    const String value = extractAtTagValue(response, "+CIREG:");
    const int comma = value.lastIndexOf(',');
    if (comma >= 0) {
      state.imsReg = value.substring(comma + 1).toInt();
    } else if (value.length() > 0) {
      state.imsReg = value.toInt();
    }
  }
  if (sendAT("AT+CIMI", "OK", 2000, &response)) {
    const String imsi = firstNonEmptyAtLine(response);
    if (imsi.length() >= 5) {
      state.imsi = imsi;
    }
  }

  state.networkRegistered = networkStatRegistered(state.cregStat) ||
                            networkStatRegistered(state.ceregStat) ||
                            networkStatRegistered(state.cgregStat);
  noteSubscriberIdentity();
  state.predictedVoicePath = inferVoicePath();

  Serial.printf("[modem] net registered=%s radio=%s voice=%s ims=%d creg=%d cereg=%d op=%s\n",
                state.networkRegistered ? "yes" : "no",
                state.radioMode.c_str(),
                state.predictedVoicePath.c_str(),
                state.imsReg,
                state.cregStat,
                state.ceregStat,
                state.operatorName.c_str());
}

void configureCellularApn() {
  state.apn = COF_MODEM_APN;
  sendAT("AT+COPS=0", "OK", 5000);
  sendAT(String("AT+CGDCONT=1,\"IP\",\"") + COF_MODEM_APN + "\"", "OK", 3000);
  sendAT(String("AT+CGAUTH=1,1,\"") + COF_MODEM_APN_USER + "\",\"" + COF_MODEM_APN_PASS + "\"", "OK", 3000);
  sendAT("AT+CGATT=1", "OK", 15000);
  sendAT("AT+CGSMS=1", "OK", 3000);
  sendAT("AT+CSMP=17,167,0,0", "OK", 3000);
  sendAT("AT+CEMODE=1", "OK", 3000);
  sendAT("AT+CEVDP=3", "OK", 3000);
  sendAT("AT+CAVIMS=1", "OK", 3000);
  sendAT("AT+CIREG=2", "OK", 2000);
  sendAT("AT+CGDCONT=2,\"IPV4V6\",\"ims\"", "OK", 3000);
  sendAT("AT+CRC=1", "OK", 2000);
  sendAT("AT+CVHU=0", "OK", 2000);

  String smscResponse;
  if (sendAT("AT+CSCA?", "OK", 3000, &smscResponse)) {
    state.smsc = extractQuoted(smscResponse);
  }
  if (state.smsc.length() < 8) {
    sendAT(String("AT+CSCA=\"") + COF_MODEM_SMSC + "\"", "OK", 3000);
    state.smsc = COF_MODEM_SMSC;
  }

  setStatus("Wait network");
  for (int attempt = 0; attempt < 8; attempt++) {
    feedWatchdog();
    refreshCellularStatus();
    if (state.networkRegistered) {
      setStatus("Network OK");
      return;
    }
    delay(2000);
  }
  setStatus("Network wait");
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
  if (sendAT("AT+CGMM", "OK", 2000, &response)) {
    state.modemModel = firstNonEmptyAtLine(response);
  }
  if (sendAT("AT+CPIN?", "OK", 2000, &response)) {
    state.simReady = response.indexOf("READY") >= 0;
  }

  state.modemAudioPlaybackSupported = sendAT("AT+CCMXPLAY=?", "OK", 3000);
  state.modemFileTransferSupported = sendAT("AT+CFTRANRX=?", "OK", 3000);

  if (state.simReady) {
    configureCellularApn();
  } else {
    setStatus("Modem OK no SIM");
  }
  return true;
}

void pollModem() {
  if (!state.modemReady) {
    initModem();
    return;
  }

  const bool wasRegistered = state.networkRegistered;
  refreshCellularStatus();
  if (state.networkRegistered && !wasRegistered) {
    setStatus("Network OK");
    if (state.mqttConnected) {
      publishDeviceStatus("online", true);
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

int parseClccStat(const String& response) {
  const int tag = response.indexOf("+CLCC:");
  if (tag < 0) {
    return -1;
  }
  const int first = response.indexOf(',', tag);
  const int second = first >= 0 ? response.indexOf(',', first + 1) : -1;
  const int third = second >= 0 ? response.indexOf(',', second + 1) : -1;
  if (second < 0 || third < 0) {
    return -1;
  }
  return response.substring(second + 1, third).toInt();
}

String classifyCallUrc(const String& raw) {
  String urc = raw;
  urc.toUpperCase();
  if (urc.indexOf("BUSY") >= 0) {
    return "Call busy";
  }
  if (urc.indexOf("NO DIALTONE") >= 0) {
    return "Call no dialtone";
  }
  if (urc.indexOf("NO ANSWER") >= 0) {
    return "Call no answer";
  }
  if (urc.indexOf("NO CARRIER") >= 0) {
    return "Call no carrier";
  }
  if (urc.indexOf("VOICE CALL: BEGIN") >= 0) {
    return "Call connected";
  }
  return "";
}

void refreshRadioMode() {
  String response;
  if (!sendAT("AT+CPSI?", "OK", 2000, &response)) {
    return;
  }
  String cpsi = extractAtTagValue(response, "+CPSI:");
  if (cpsi.length() > 96) {
    cpsi = cpsi.substring(0, 96);
  }
  if (cpsi.length() == 0) {
    return;
  }
  state.radioInfo = cpsi;
  const int comma = cpsi.indexOf(',');
  state.radioMode = comma >= 0 ? cpsi.substring(0, comma) : cpsi;
  state.radioMode.trim();
}

bool radioIsGsm() {
  return state.radioMode.indexOf("GSM") >= 0;
}

bool radioHasService() {
  return state.radioMode.length() > 0 &&
         state.radioMode.indexOf("NO SERVICE") < 0 &&
         state.radioMode.indexOf("No Service") < 0 &&
         state.networkRegistered;
}

bool imsVoiceReady() {
  return state.imsReg == 1;
}

bool waitForRadioService(uint32_t timeoutMs, bool gsmOnly) {
  const uint32_t startedAt = millis();
  while (millis() - startedAt < timeoutMs) {
    feedWatchdog();
    if (state.mqttConnected) {
      mqttClient.loop();
    }
    refreshCellularStatus();
    if (radioHasService() && (!gsmOnly || radioIsGsm())) {
      return true;
    }
    waitWithWatchdog(2000);
  }
  return radioHasService() && (!gsmOnly || radioIsGsm());
}

String queryCallFailCause() {
  String response;
  if (!sendAT("AT+CEER", "OK", 3000, &response)) {
    return "";
  }
  String value = extractAtTagValue(response, "+CEER:");
  if (value.length() == 0) {
    value = firstNonEmptyAtLine(response);
  }
  value.replace("\"", "");
  value.replace("\r", " ");
  value.replace("\n", " ");
  value.trim();
  if (value.length() > 48) {
    value = value.substring(0, 48);
  }
  return value;
}

String voiceContextSuffix(const String& bearer, const String& ceer = "");
void restoreAutoRadio();
String waitForOutgoingCall(uint32_t timeoutMs);

void persistSkipGsm(bool skip) {
  state.skipGsmVoice = skip;
  preferences.putBool("skipGsm", skip);
}

String prepareVoiceBearer() {
  refreshCellularStatus();
  if (imsVoiceReady()) {
    return "ims";
  }
  if (radioIsGsm()) {
    return "gsm";
  }
  return "csfb";
}

void bounceRadioForCsfb() {
  setStatus("Voice retry");
  Serial.println("[call] bouncing radio for CSFB retry");
  sendAT("AT+CNMP=13", "OK", 8000);
  waitWithWatchdog(4000);
  sendAT("AT+CNMP=2", "OK", 10000);
  state.forcedGsmForCall = false;
  persistSkipGsm(true);
  waitForRadioService(25000, false);
}

bool shouldRetryVoice(const String& result) {
  return result.startsWith("Call failed") ||
         result.startsWith("Call no carrier") ||
         result.startsWith("Call not connected") ||
         result.startsWith("Call dial timeout") ||
         result.startsWith("No voice radio");
}

String playConnectedCallAudio() {
  waitWithWatchdog(1500);
  setStatus("Playing audio");
  sendAT("AT+CCMXPLAY=\"" + state.modemAudioPath + "\",1,0", "OK", 5000);
  readModemUntil(45000, "+AUDIOSTATE: audio play stop");
  sendAT("ATH", "OK", 5000);
  setStatus("Call done");
  return "Call done";
}

String dialAndMaybePlay(const String& phone, const String& bearer) {
  if (!radioHasService()) {
    return "No voice radio" + voiceContextSuffix(bearer);
  }

  refreshRadioMode();
  state.radioAtDial = state.radioMode;
  state.radioAtConnect = "";

  setStatus("Calling");
  if (!sendAT("ATD" + phone + ";", "OK", 10000)) {
    const String ceer = queryCallFailCause();
    sendAT("ATH", "OK", 3000);
    return "Call failed" + voiceContextSuffix(bearer, ceer);
  }

  const String progress = waitForOutgoingCall(35000);
  refreshRadioMode();
  state.radioAtConnect = state.radioMode;
  const String observed = observeVoicePath(state.radioAtDial, state.radioAtConnect);
  if (progress != "Call connected") {
    const String ceer = queryCallFailCause();
    sendAT("ATH", "OK", 3000);
    return progress + voiceContextSuffix(observed, ceer);
  }

  persistObservedVoicePath(observed);
  state.predictedVoicePath = observed;
  return playConnectedCallAudio() + voiceContextSuffix(observed);
}

void restoreAutoRadio() {
  if (!state.forcedGsmForCall) {
    return;
  }
  state.forcedGsmForCall = false;
  sendAT("AT+CNMP=2", "OK", 10000);
  waitForRadioService(20000, false);
}

String voiceContextSuffix(const String& bearer, const String& ceer) {
  String suffix = " [";
  suffix += bearer.length() > 0 ? bearer : inferVoicePath();
  suffix += " ";
  if (state.radioAtDial.length() > 0 && state.radioAtConnect.length() > 0 &&
      state.radioAtDial != state.radioAtConnect) {
    suffix += state.radioAtDial;
    suffix += "->";
    suffix += state.radioAtConnect;
  } else {
    suffix += state.radioMode.length() > 0 ? state.radioMode : "?";
  }
  suffix += " IMS=";
  suffix += String(state.imsReg);
  if (state.operatorName.length() > 0) {
    suffix += " ";
    suffix += state.operatorName;
  }
  if (ceer.length() > 0) {
    suffix += " CEER=";
    suffix += ceer;
  }
  suffix += "]";
  return suffix;
}

String waitForOutgoingCall(uint32_t timeoutMs) {
  bool sawDialing = false;
  bool sawAlerting = false;
  const uint32_t startedAt = millis();
  uint32_t lastRadioSampleMs = startedAt;

  while (millis() - startedAt < timeoutMs) {
    feedWatchdog();
    if (state.mqttConnected) {
      mqttClient.loop();
    }

    const String urc = readModemUntil(700, "");
    const String urcResult = classifyCallUrc(urc);
    if (urcResult.length() > 0) {
      if (urcResult == "Call no carrier" && sawAlerting) {
        return "Call no answer";
      }
      return urcResult;
    }

    String clcc;
    if (sendAT("AT+CLCC", "OK", 1500, &clcc)) {
      const String clccResult = classifyCallUrc(clcc);
      if (clccResult.length() > 0) {
        return clccResult;
      }
      const int stat = parseClccStat(clcc);
      if (stat == 2) {
        sawDialing = true;
        setStatus("Dialing");
      } else if (stat == 3) {
        sawAlerting = true;
        setStatus("Ringing");
      } else if (stat == 0) {
        refreshRadioMode();
        return "Call connected";
      }
    }
    if (millis() - lastRadioSampleMs >= 3000) {
      lastRadioSampleMs = millis();
      refreshRadioMode();
    }
    delay(300);
  }

  if (sawAlerting) {
    return "Call ringing timeout";
  }
  if (sawDialing) {
    return "Call dial timeout";
  }
  return "Call not connected";
}

String placeCallAndPlayAudio(const String& phoneOverride = "", bool adminTest = false) {
  if (!adminTest && !COF_ENABLE_CALLS) {
    setStatus("Calls disabled");
    Serial.println("[call] Set COF_ENABLE_CALLS to 1 and COF_PHONE_NUMBER before testing calls.");
    return "Calls disabled";
  }
  if (!adminTest && !state.callingEnabled) {
    setStatus("Calls off cfg");
    Serial.println("[call] calling.enabled=false in device config");
    return "Calls off cfg";
  }

  if (!state.modemReady || !state.simReady) {
    setStatus("No modem/SIM");
    return "No modem/SIM";
  }

  String phone = phoneOverride;
  phone.trim();
  if (!phoneLooksValid(phone)) {
    phone = state.manifestPhoneNumber;
    phone.trim();
  }
  if (!phoneLooksValid(phone)) {
    setStatus("No phone cfg");
    return "No phone cfg";
  }

  if (adminTest) {
    setStatus("Sync test audio");
    checkManifest(false);
  }

  state.callInProgress = true;
  String bearer = prepareVoiceBearer();
  String result = dialAndMaybePlay(phone, bearer);
  if (shouldRetryVoice(result)) {
    bounceRadioForCsfb();
    refreshCellularStatus();
    bearer = imsVoiceReady() ? "ims after bounce" : "csfb retry";
    result = dialAndMaybePlay(phone, bearer);
  }

  restoreAutoRadio();
  state.callInProgress = false;
  return result;
}

String resolveTestPhone(const String& phoneOverride) {
  String phone = phoneOverride;
  phone.trim();
  if (!phoneLooksValid(phone)) {
    phone = state.manifestPhoneNumber;
    phone.trim();
  }
  return phone;
}

String sendTestSms(const String& phoneOverride, const String& text) {
  if (!state.modemReady || !state.simReady) {
    setStatus("No modem/SIM");
    return "No modem/SIM";
  }

  const String phone = resolveTestPhone(phoneOverride);
  if (!phoneLooksValid(phone)) {
    setStatus("No phone cfg");
    return "No phone cfg";
  }

  String body = text;
  body.trim();
  if (body.length() == 0) {
    body = "CallOnFail prueba SMS";
  }
  if (body.length() > 160) {
    body = body.substring(0, 160);
  }

  if (!sendAT("AT+CMGF=1", "OK", 3000)) {
    setStatus("SMS mode fail");
    return "SMS mode fail";
  }

  flushModemInput();
  Serial.println("[modem] >> AT+CMGS=\"" + phone + "\"");
  ModemSerial.print("AT+CMGS=\"");
  ModemSerial.print(phone);
  ModemSerial.print("\"\r");
  if (!modemWaitForPrompt(10000)) {
    setStatus("SMS prompt fail");
    return "SMS prompt fail";
  }

  ModemSerial.print(body);
  ModemSerial.write(static_cast<uint8_t>(0x1A));
  const String response = readModemUntil(60000, "OK");
  Serial.println("[modem] << " + response);
  if (response.indexOf("+CMGS") < 0 && response.indexOf("OK") < 0) {
    String err = response;
    err.replace("\r", " ");
    err.replace("\n", " ");
    err.trim();
    if (err.length() > 80) {
      err = err.substring(0, 80);
    }
    setStatus("SMS failed");
    return err.length() > 0 ? ("SMS failed: " + err) : "SMS failed: timeout";
  }

  setStatus("SMS sent");
  return "SMS sent";
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
  Serial.printf("Cellular: registered=%s creg=%d cereg=%d op=%s apn=%s smsc=%s\n",
                state.networkRegistered ? "yes" : "no",
                state.cregStat,
                state.ceregStat,
                state.operatorName.c_str(),
                state.apn.c_str(),
                state.smsc.c_str());
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

  if (state.mqttConnected && pendingStatusReportCommand) {
    pendingStatusReportCommand = false;
    publishDeviceStatus("online", true);
  }

  if (pendingTestCallCommand && !state.callInProgress && !state.otaInProgress && !state.audioSyncInProgress) {
    pendingTestCallCommand = false;
    const String result = placeCallAndPlayAudio(pendingTestCallPhone, true);
    pendingTestCallPhone = "";
    connectMqttIfNeeded();
    const bool ok = result.startsWith("Call done");
    publishDeviceEvent("test_call", ok ? "info" : "warning", result);
  }

  if (pendingTestSmsCommand && !state.callInProgress && !state.otaInProgress && !state.audioSyncInProgress) {
    pendingTestSmsCommand = false;
    const String result = sendTestSms(pendingTestSmsPhone, pendingTestSmsText);
    pendingTestSmsPhone = "";
    pendingTestSmsText = "";
    connectMqttIfNeeded();
    const bool ok = result == "SMS sent";
    publishDeviceEvent("test_sms", ok ? "info" : "warning", result);
  }

  if (state.mqttConnected && now - lastTelemetryPublishMs >= state.telemetryIntervalMs) {
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
      checkManifest(false);
    } else if (didInitialManifestCheck && now - lastManifestMs >= kManifestIntervalMs) {
      lastManifestMs = now;
      checkManifest(false);
    }
  }

  delay(20);
}
