#include "cof_config.h"
#include "cof_state.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>

// Extracted verbatim from main.cpp, which used to hold every function

bool applyDesiredConfig(JsonDocument& doc) {
  pendingConfigError = "";

  int telemetrySeconds = doc["telemetry_interval_seconds"] | 60;
  if (telemetrySeconds < static_cast<int>(kTelemetryIntervalMinSeconds) ||
      telemetrySeconds > static_cast<int>(kTelemetryIntervalMaxSeconds)) {
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
void initOtaRollbackGuard() {
  otaConfirmBootMs = millis();
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running == nullptr) {
    return;
  }
  esp_ota_img_states_t otaState = ESP_OTA_IMG_UNDEFINED;
  if (esp_ota_get_state_partition(running, &otaState) != ESP_OK) {
    return;
  }
  if (otaState == ESP_OTA_IMG_PENDING_VERIFY) {
    otaConfirmPending = true;
    Serial.println("[ota] new image pending verify; will confirm once healthy");
  }
}
// Confirm the slot when the device proves it works. Never confirm early: doing
// so would silently disable the rollback that protects us.
void serviceOtaRollbackGuard() {
  if (!otaConfirmPending) {
    return;
  }
  const uint32_t uptime = millis() - otaConfirmBootMs;

  if (state.mqttConnected && uptime >= kOtaConfirmMinUptimeMs) {
    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
      otaConfirmPending = false;
      otaConfirmedThisBoot = true;
      Serial.println("[ota] image confirmed valid; rollback cancelled");
    } else {
      Serial.printf("[ota] confirm failed: %d\n", static_cast<int>(err));
    }
    return;
  }

  if (uptime >= kOtaConfirmTimeoutMs) {
    // We never got healthy in time. Do not confirm. The watchdog will reset us
    // and the bootloader will fall back to the previous working image.
    Serial.println("[ota] image never became healthy; rebooting to roll back");
    setStatus("OTA rollback");
    delay(200);
    ESP.restart();
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
  // Clamp on load too, not just on config apply: a device that already stored a
  // value above the cap (the old 3600 ceiling was reachable from the form) would
  // otherwise keep reconnecting every cycle forever, because this interval lands
  // above kMqttSilenceReconnectMs.
  state.telemetryIntervalMs =
      static_cast<uint32_t>(constrain(telemetrySeconds,
                                     static_cast<int>(kTelemetryIntervalMinSeconds),
                                     static_cast<int>(kTelemetryIntervalMaxSeconds))) * 1000UL;
  state.callingEnabled = preferences.getBool("callEn", preferences.getBool("callingEnabled", COF_ENABLE_CALLS != 0));
  // Offline audio fallback. `audioVersion` is only set by a successful manifest
  // audio sync (the admin TTS upload writes "tts" and reuses C:/tts.amr, which is
  // deleted on the next call), so it is the proof that the canned asset is
  // really on the modem. Without that proof we do not claim a fallback exists.
  state.modemFallbackAudioPath = preferences.getString("fallbackAudioPath", COF_MODEM_AUDIO_PATH);
  const String storedAudioVersion = preferences.getString("audioVersion", "");
  state.modemFallbackAudioReady =
      storedAudioVersion.length() > 0 && storedAudioVersion != "tts";
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
bool httpGetString(const String& url, String& out, uint32_t timeoutMs) {
  if (!lanConnected()) {
    return false;
  }

  applyPreferredRoute();

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
// Stream the image into flash while hashing it, then refuse to commit unless the
// digest matches the manifest. Downloading and flashing without an integrity
// check is unforgivable on a device we cannot reach physically.
bool performOta(const String& url, const String& newVersion, const String& expectedSha256) {
  if (!networkConnected()) {
    return false;
  }

  if (expectedSha256.length() != 64) {
    Serial.printf("[ota] refusing update: bad sha256 in manifest (len=%u)\n",
                  static_cast<unsigned>(expectedSha256.length()));
    setStatus("OTA no sha256");
    return false;
  }

  applyPreferredRoute();

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

  mbedtls_sha256_context shaCtx;
  mbedtls_sha256_init(&shaCtx);
  mbedtls_sha256_starts_ret(&shaCtx, 0);  // 0 = SHA-256, not SHA-224

  WiFiClient& stream = http.getStream();
  uint8_t buffer[1024];
  size_t written = 0;
  bool writeFailed = false;

  while (written < static_cast<size_t>(size)) {
    const size_t remaining = static_cast<size_t>(size) - written;
    const size_t chunk = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    const int read = stream.readBytes(buffer, chunk);
    if (read <= 0) {
      writeFailed = true;
      break;
    }
    if (Update.write(buffer, static_cast<size_t>(read)) != static_cast<size_t>(read)) {
      writeFailed = true;
      break;
    }
    mbedtls_sha256_update_ret(&shaCtx, buffer, static_cast<size_t>(read));
    written += static_cast<size_t>(read);
    feedWatchdog();
  }

  if (writeFailed || written != static_cast<size_t>(size)) {
    Serial.printf("[ota] incomplete write: %u/%d\n", static_cast<unsigned>(written), size);
    mbedtls_sha256_free(&shaCtx);
    Update.abort();
    http.end();
    state.otaInProgress = false;
    return false;
  }

  uint8_t digest[32];
  mbedtls_sha256_finish_ret(&shaCtx, digest);
  mbedtls_sha256_free(&shaCtx);

  char hex[65];
  for (size_t i = 0; i < sizeof(digest); ++i) {
    snprintf(hex + (i * 2), 3, "%02x", digest[i]);
  }
  hex[64] = '\0';
  String actual(hex);
  String expected = expectedSha256;
  expected.trim();
  expected.toLowerCase();

  if (actual != expected) {
    Serial.printf("[ota] sha256 mismatch: got %s want %s\n", actual.c_str(), expected.c_str());
    setStatus("OTA bad hash");
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
  state.manifestFirmwareSha256 = doc["firmware"]["sha256"] | "";
  state.manifestAudioVersion = doc["audio"]["version"] | "";
  state.manifestAudioUrl = doc["audio"]["url"] | "";
  state.modemAudioPath = doc["audio"]["modem_path"] | COF_MODEM_AUDIO_PATH;
  state.manifestPhoneNumber = doc["config"]["phone_number"] | COF_PHONE_NUMBER;

  if (allowFirmwareUpdate && state.manifestFirmwareVersion.length() > 0 &&
      state.manifestFirmwareUrl.length() > 0 &&
      compareVersions(state.manifestFirmwareVersion, COF_FIRMWARE_VERSION) > 0) {
    performOta(state.manifestFirmwareUrl, state.manifestFirmwareVersion,
               state.manifestFirmwareSha256);
    return;
  }

  const String currentAudioVersion = preferences.getString("audioVersion", "");
  if (state.manifestAudioVersion.length() > 0 && state.manifestAudioUrl.length() > 0 &&
      state.manifestAudioVersion != currentAudioVersion) {
    const String audioErr = uploadAudioToModem(state.manifestAudioUrl, state.modemAudioPath, state.manifestAudioVersion);
    if (audioErr.length() > 0) {
      Serial.println("[audio] " + audioErr);
    }
    return;
  }

  setStatus("Manifest OK");
}
