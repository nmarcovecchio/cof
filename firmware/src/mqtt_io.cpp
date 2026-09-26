#include "cof_config.h"
#include "cof_state.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>

// Extracted verbatim from main.cpp, which used to hold every function

bool mqttUsesTls() {
  return state.mqttPort == 8883 || state.mqttPort == 8884;
}
void requestMqttBounce(const char* reason) {
  pendingMqttBounce = true;
  Serial.printf("[mqtt] bounce requested: %s\n", reason);
}
void bounceMqttForRouteChange() {
#if COF_LTE_MQTT_NATIVE
  if (state.lteMqttTransport) {
    cmqttDisconnect();
  } else
#endif
  if (mqttClient.connected() || state.mqttConnected) {
    mqttClient.disconnect();
  }
  state.mqttConnected = false;
  lastMqttReconnectMs = 0;
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
      } else if (pendingCommandName == "modem_probe") {
        pendingModemProbeCommand = true;
        pendingModemProbeCommandId = pendingCommandId;
        pendingCommandStatus = "accepted";
        pendingCommandMessage = "Modem probe scheduled";
      } else if (pendingCommandName == "status_report") {
        pendingStatusReportCommand = true;
        pendingCommandStatus = "accepted";
        pendingCommandMessage = "Status report scheduled";
      } else if (pendingCommandName == "test_call") {
        pendingTestCallCommand = true;
        pendingTestCallPhone = doc["phone"] | "";
        pendingTestCallPhone.trim();
        // audio_url / audio_format are still sent by the server but no longer
        // read: the call only ever plays the pre-recorded file on the modem.
        pendingTestCallAudioSha = doc["call_audio"]["text_sha256"] | "";
        pendingTestCallAudioSha.trim();
        pendingTestCallCommandId = pendingCommandId;
        pendingCommandStatus = "accepted";
        pendingCommandMessage = "Test call scheduled";
      } else if (pendingCommandName == "test_sms") {
        pendingTestSmsCommand = true;
        pendingTestSmsPhone = doc["phone"] | "";
        pendingTestSmsPhone.trim();
        pendingTestSmsText = doc["text"] | "CallOnFail prueba SMS";
        pendingTestSmsText.trim();
        pendingTestSmsCommandId = pendingCommandId;
        pendingCommandStatus = "accepted";
        pendingCommandMessage = "Test SMS scheduled";
      } else if (pendingCommandName == "set_wifi") {
        const String ssid = doc["ssid"] | "";
        const String password = doc["password"] | "";
        if (ssid.length() == 0 || ssid.length() > 32) {
          pendingCommandMessage = "invalid ssid";
        } else if (password.length() > 64) {
          pendingCommandMessage = "invalid password";
        } else {
          connectWiFi(ssid, password, true);
          pendingCommandStatus = "accepted";
          pendingCommandMessage = "WiFi saved, connecting to " + ssid;
        }
      } else if (pendingCommandName == "clear_wifi") {
        clearSavedWiFi();
        pendingCommandStatus = "accepted";
        pendingCommandMessage = "WiFi cleared";
        pendingNetworkStatusReport = true;
      } else {
        pendingCommandMessage = "unsupported command";
      }
    }

    pendingCommandAck = true;
  }
}
bool publishMqttJson(const String& suffix, JsonDocument& doc, bool retained, uint8_t qos) {
  if (!state.mqttConnected) {
    return false;
  }

  char payload[4096];
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
#if COF_LTE_MQTT_NATIVE
  const bool ok = state.lteMqttTransport
      ? cmqttPublish(topic, reinterpret_cast<const uint8_t*>(payload), length, retained, qos)
      : mqttClient.publish(topic.c_str(), reinterpret_cast<const uint8_t*>(payload), length, retained);
#else
  const bool ok = mqttClient.publish(topic.c_str(), reinterpret_cast<const uint8_t*>(payload), length, retained);
  (void)qos;
#endif
  Serial.printf("[mqtt] publish topic=%s ok=%s payload=%s\n", topic.c_str(), ok ? "yes" : "no", payload);
  if (ok) {
    lastMqttOkMs = millis();
    lanMqttFailCount = 0;
  } else {
    state.mqttConnected = false;
#if COF_LTE_MQTT_NATIVE
    if (state.lteMqttTransport) {
      cmqttDisconnect();
    } else
#endif
    {
      mqttClient.disconnect();
      noteLanMqttFailure("publish");
    }
  }
  return ok;
}
void fillCellularJson(JsonObject cellular) {
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
}
void fillNetworkJson(JsonObject network) {
  network["active"] = activeNetworkName();
  const bool internetUp = lanHasInternet() || state.lteDataUp;
  network["internet"] = internetUp;
  // A LAN interface with an IP but no reachable broker is the trap this used to
  // fall into: link and DHCP are up, so everything looked healthy while there
  // was no internet at all.
  network["degraded"] = lanConnected() && !lanHasInternet() && !state.lteDataUp;
  JsonObject ethernet = network["ethernet"].to<JsonObject>();
  ethernet["up"] = state.ethernetConnected;
  ethernet["ip"] = state.ethernetConnected ? state.ipAddress : "-";
  ethernet["internet"] = state.ethernetConnected && ethInternetUp;
  JsonObject wifi = network["wifi"].to<JsonObject>();
  wifi["configured"] = state.wifiConfigured;
  wifi["up"] = state.wifiConnected;
  wifi["ssid"] = state.wifiSsid;
  wifi["ip"] = state.wifiConnected ? state.wifiIpAddress : "-";
  wifi["internet"] = state.wifiConnected && wifiInternetUp;
  wifi["rssi"] = state.wifiConnected ? WiFi.RSSI() : 0;
  JsonObject lte = network["lte"].to<JsonObject>();
  lte["up"] = state.lteDataUp;
  lte["ip"] = state.lteDataUp ? state.lteIpAddress : "-";
}
// Flat 1/0 flags mirroring SENSOR_ALIASES in backend/app/alarms.py. Publishing
// them in every telemetry frame is what lets an alarm rule on a link or internet
// drop evaluate on the regular push, even when the path that just died is the
// only one available.
void fillConnectivityJson(JsonDocument& doc) {
  // Gated on the health flags, not on link presence. A cable plugged into a router
  // with no uplink still brings PHY + DHCP up, so reporting link presence here
  // would keep the "ethernet ok" alarm quiet in exactly the outage it exists to
  // catch. Note the ethernet flag intentionally does NOT honour the holdoff: the
  // holdoff is a routing-preference delay, and reporting "ethernet ok" off it
  // would flap the alarm on every brief unplug.
  doc["network_ethernet_ok"] = (state.ethernetConnected && ethInternetUp) ? 1 : 0;
  doc["network_wifi_ok"] = (state.wifiConnected && wifiInternetUp) ? 1 : 0;
  doc["network_internet_ok"] = (lanHasInternet() || state.lteDataUp) ? 1 : 0;
}
void publishDeviceStatus(const char* status, bool retained) {
  JsonDocument doc;
  doc["device_id"] = state.mqttDeviceId;
  doc["status"] = status;
  doc["firmware"] = COF_FIRMWARE_VERSION;
  doc["ip"] = currentIpAddress();
  doc["ethernet"] = state.ethernetConnected;
  doc["wifi"] = state.wifiConnected;
  fillNetworkJson(doc["network"].to<JsonObject>());
  fillConnectivityJson(doc);
  doc["modem_ready"] = state.modemReady;
  doc["sim_ready"] = state.simReady;
  doc["lte_signal"] = state.signalQuality;
  doc["reported_config_version"] = state.reportedConfigVersion;
  doc["reset_reason"] = state.bootResetReason;

  doc["hardware_profile"] = "cof-wt32-a7672-v1";
  JsonObject capabilities = doc["capabilities"].to<JsonObject>();
  capabilities["ethernet"] = true;
  capabilities["wifi"] = true;
  capabilities["lte_data"] = true;
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
  fillCellularJson(discovered["cellular"].to<JsonObject>());
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
void deferDeviceEvent(const char* type, const char* severity, const String& message, const String& commandId) {
  // Coalesce duplicates: the radio recovery ladder and the LTE trace can emit the
  // same message repeatedly while MQTT is down, and without this the deferred
  // queue fills with near-identical events that then flush in a burst on
  // reconnect (13x "step 1/5" observed 2026-09-25). Only the newest copy matters.
  for (size_t i = 0; i < deferredEventCount; i++) {
    if (deferredEvents[i].type == type && deferredEvents[i].message == message &&
        deferredEvents[i].severity == severity) {
      return;
    }
  }
  if (deferredEventCount >= kDeferredEventMax) {
    // Keep the newest: a fresh command result matters more than a stale trace.
    Serial.printf("[event] deferred queue full, dropping oldest (%s)\n", deferredEvents[0].type.c_str());
    for (size_t i = 1; i < deferredEventCount; i++) {
      deferredEvents[i - 1] = deferredEvents[i];
    }
    deferredEventCount--;
  }
  DeferredEvent& slot = deferredEvents[deferredEventCount++];
  slot.type = type;
  slot.severity = severity;
  slot.message = message;
  slot.commandId = commandId;
  Serial.printf("[event] deferred %s: %s\n", type, message.c_str());
}
void flushDeferredEvents() {
  if (!state.mqttConnected || deferredEventCount == 0) {
    return;
  }
  if (!publishDeviceEvent(deferredEvents[0].type.c_str(),
                          deferredEvents[0].severity.c_str(),
                          deferredEvents[0].message,
                          deferredEvents[0].commandId)) {
    return;
  }
  for (size_t i = 1; i < deferredEventCount; i++) {
    deferredEvents[i - 1] = deferredEvents[i];
  }
  deferredEventCount--;
}
bool publishDeviceEvent(const char* type, const char* severity, const String& message, const String& commandId) {
  JsonDocument doc;
  doc["device_id"] = state.mqttDeviceId;
  doc["firmware"] = COF_FIRMWARE_VERSION;
  doc["type"] = type;
  doc["severity"] = severity;
  doc["message"] = message;
  if (commandId.length() > 0) {
    doc["command_id"] = commandId;
  }
  if (!publishMqttJson("event", doc, false, 1)) {
    // Queue instead of dropping: this used to silently discard the result of an
    // SMS test (transmitSms() takes MQTT down first via releaseLteMqttForModem)
    // and the backend would then time the command out with no explanation.
    deferDeviceEvent(type, severity, message, commandId);
    return false;
  }
  flushDeferredEvents();
  return true;
}
void publishLteDataTrace() {
  if (!pendingLteTracePublish || !state.mqttConnected) {
    return;
  }

  // Throttle: a persistently failing PDP used to emit a full modem dump every
  // 10 s forever, which flooded the event stream and pushed the SMS/call traces
  // out of the modem panel. Identical messages coalesce into one event per
  // window; a changed message is still reported at most once per window.
  const uint32_t now = millis();
  if (lastLteDataEventMs != 0 && now - lastLteDataEventMs < kLteDataEventMinIntervalMs) {
    return;
  }

  JsonDocument doc;
  doc["device_id"] = state.mqttDeviceId;
  doc["firmware"] = COF_FIRMWARE_VERSION;
  doc["type"] = "lte_data";
  doc["severity"] = pendingLteTraceOk ? "info" : "warning";
  doc["message"] = withFirmware(pendingLteTraceMessage);
  if (lteTraceLog.length() > 0) {
    doc["modem_log"] = lteTraceLog;
  }
  // Head of the MQTT-over-LTE session. Published separately from `modem_log`
  // because that one is a tail-only ring and drops the first attempt - the only
  // one that carries the send ACK, the CONNACK and PubSubClient's verdict. See
  // LteMqttClient::noteHandshake and BACKLOG 6b.
  if (lteMqttClient.handshake.length() > 0) {
    doc["handshake"] = lteMqttClient.handshake;
  }
  if (publishMqttJson("event", doc, false, 1)) {
    pendingLteTracePublish = false;
    lastLteDataEventMs = now == 0 ? 1 : now;
    // Published: the capture has served its purpose, so the next LTE session
    // starts clean. This is the ONLY place it is cleared (not per connect) so a
    // failure that never manages to publish is not erased by its own retries.
    lteMqttClient.handshake = "";
    return;
  }
  if (lteTraceLog.length() <= 900) {
    return;
  }
  doc["modem_log"] = lteTraceLog.substring(lteTraceLog.length() - 900);
  if (publishMqttJson("event", doc, false, 1)) {
    pendingLteTracePublish = false;
    lastLteDataEventMs = now == 0 ? 1 : now;
    lteMqttClient.handshake = "";
  }
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
  fillNetworkJson(doc["network"].to<JsonObject>());
  fillConnectivityJson(doc);
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
  fillCellularJson(doc["cellular"].to<JsonObject>());
  doc["input_1"] = lastButtonPressed;
  doc["output_1"] = false;
  doc["output_2"] = false;
  publishMqttJson("telemetry", doc, false, 0);
}
#if COF_LTE_MQTT_NATIVE
// Native CMQTT connect/reconnect path for the LTE transport. Mirrors the LAN
// PubSubClient block below: backoff, connect, will, subscribe, then the same
// first-publish burst. cmqttLoop() runs here every pass while connected, which
// drains inbound URCs and folds +CMQTTCONNLOST / +CMQTTNONET into the connection
// state (no separate watchdog ping is needed - the modem keepalives by itself).
static void connectMqttNativeIfNeeded() {
  if (cmqttIsConnected()) {
    cmqttLoop();
    if (!cmqttIsConnected()) {
      // A URC dropped us (CONNLOST, or NONET which also set ltePdpDown).
      state.mqttConnected = false;
      if (ltePdpDown) {
        ltePdpDown = false;
        Serial.println("[lte] network closed (out of service), releasing PDP");
        stopLtePdp();
      }
      lteMqttConnectFails = 0;
    } else {
      state.mqttConnected = true;
    }
    return;
  }

  state.mqttConnected = false;
  const uint32_t now = millis();
  if (now - lastMqttReconnectMs < kMqttReconnectIntervalMs) {
    return;
  }
  lastMqttReconnectMs = now;

  const String clientId = state.mqttDeviceId + "-" + String(static_cast<uint32_t>(ESP.getEfuseMac()), HEX);
  const String willTopic = mqttTopic("status");
  const String willPayload = "{\"status\":\"offline\",\"device_id\":\"" + state.mqttDeviceId + "\"}";
  const String username = state.mqttUsername;
  const String password = state.mqttPassword;

  Serial.printf("[mqtt] connecting (native CMQTT) host=%s port=%d device=%s\n",
                state.mqttHost.c_str(), state.mqttPort, state.mqttDeviceId.c_str());

  reportLteProgress = true;
  feedWatchdog();
  const bool ok = cmqttConnect(clientId, willTopic, willPayload,
                               state.mqttHost, state.mqttPort, username, password);
  if (!ok) {
    setStatus("MQTT fail");
    lteMqttConnectFails++;
    if (lteMqttConnectFails >= 3) {
      lteMqttConnectFails = 0;
      Serial.println("[lte] repeated MQTT connect failures, rebuilding PDP");
      stopLtePdp();
      lastLteAttemptMs = 0;
    }
    lteTraceLog = modemCallLog;
    pendingLteTraceMessage = "LTE MQTT fail (native)";
    pendingLteTraceOk = false;
    pendingLteTracePublish = true;
    reportLteProgress = false;
    return;
  }

  lteMqttConnectFails = 0;
  lteTraceLog = modemCallLog;
  pendingLteTraceMessage = "LTE MQTT OK (native)";
  pendingLteTraceOk = true;
  pendingLteTracePublish = true;
  reportLteProgress = false;

  cmqttSubscribe(mqttTopic("config/desired"), 1);
  cmqttSubscribe(mqttTopic("command"), 1);

  // The native path never ran NETOPEN, so state.lteIpAddress is still "-".
  // CMQTTSTART already activated the PDP on CID 1 (CGDCONT=1); read the real
  // address so the OLED "L" line and the status/telemetry report the LTE IP
  // instead of "-" (observed: "L --" on the OLED while MQTT rode LTE).
  {
    String ip;
    if (queryLteIp(ip)) {
      state.lteIpAddress = ip;
    }
  }

  state.mqttConnected = true;
  lastMqttOkMs = millis();
  lastSilenceProbeMs = 0;
  publishDeviceStatus("online", true);
  publishTelemetryNow();
  lastTelemetryPublishMs = millis();
  publishLteDataTrace();
  lastCellularStatusMs = millis();
  setStatus("MQTT OK");
}
#endif

void connectMqttIfNeeded() {
  maintainLteFallback();

  if (state.ethernetConnected && !ETH.linkUp()) {
    markEthernetDown("link bit");
  }

  if (pendingMqttBounce) {
    pendingMqttBounce = false;
    bounceMqttForRouteChange();
  }

  if (!state.mqttConfigured || !networkConnected()) {
    return;
  }

#if COF_LTE_MQTT_NATIVE
  // Native CMQTT path. The decision is cheap and side-effect free; the full
  // PubSubClient setup (configureMqttClientTransport) still runs only on the LAN
  // path below, right before a connect, exactly as it did pre-CMQTT.
  {
    const bool lanLooksUsable = lanConnected() && !ethernetHoldoffActive() && lanHasInternet();
    if (state.lteDataUp && !lanLooksUsable) {
      state.lteMqttTransport = true;
      connectMqttNativeIfNeeded();
      return;
    }
    state.lteMqttTransport = false;
  }
#endif

  if (mqttClient.connected()) {
    if (!mqttClient.loop()) {
      state.mqttConnected = false;
      if (state.ethernetConnected && !state.lteMqttTransport) {
        markEthernetDown("mqtt loop");
      } else if (state.lteMqttTransport && (!lteMqttClient.sockOpen || ltePdpDown)) {
        // The AT socket dropped. Distinguish the two cases (see noteUrc()):
        //  - ltePdpDown (+CIPEVENT: NETWORK CLOSED UNEXPECTEDLY): the radio/network
        //    library died, so the PDP is dead and state.lteDataUp is stale. Tear it
        //    down now so ensureLtePdp() rebuilds it and pollModem() can run the
        //    radio recovery ladder instead of dialing a dead context.
        //  - plain socket close (+IPCLOSE/+CASTATE): the PDP is still alive; the
        //    next connect just reopens the socket (CIPOPEN). Tearing the PDP down
        //    here turned a ~5 s broker close into a ~45 s NETCLOSE/NETOPEN rebuild.
        if (ltePdpDown) {
          ltePdpDown = false;
          Serial.println("[lte] network closed (out of service), releasing PDP");
          stopLtePdp();
        } else if (!lteMqttClient.sockOpen) {
          Serial.println("[lte] socket closed, PDP kept alive");
        }
        lteMqttConnectFails = 0;
      }
    } else {
      state.mqttConnected = true;
    }
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

  feedWatchdog();
  const bool ok = mqttClient.connect(
      clientId.c_str(),
      username,
      password,
      willTopic.c_str(),
      1,
      true,
      willPayload.c_str());

  if (!ok) {
    Serial.printf("[mqtt] connect failed state=%d lte=%s\n",
                  mqttClient.state(),
                  state.lteMqttTransport ? "yes" : "no");
    setStatus("MQTT fail");
    if (state.lteMqttTransport) {
      // PubSubClient's own verdict is the whole point of the capture: the state
      // tells apart "TCP never carried the CONNECT" from "broker answered and
      // refused" (bad credentials, ACL, client id). Without it the trace only
      // shows our TCP-level success and we cannot tell the two apart.
      // Cast is deliberate: state() is int8_t, and String(int8_t) would pick the
      // char constructor and log a garbage byte.
      lteMqttClient.noteHandshake(
          String("mqtt connect failed, state=") + String(static_cast<int>(mqttClient.state())));
      // A failed connect over the cellular path must not wedge the PDP: back the
      // retry off and tear the PDP down once it looks hopeless so the next
      // attempt rebuilds it from scratch.
      lteMqttConnectFails++;
      // The address may be stale (see resolveLteMqttPeer): ask the module's DNS on
      // the next attempt instead of dialing the same cached IP again.
      lteForceDnsResolve = true;
      if (lteMqttConnectFails >= 3) {
        lteMqttConnectFails = 0;
        Serial.println("[lte] repeated MQTT connect failures, rebuilding PDP");
        stopLtePdp();
        lastLteAttemptMs = 0;
      }
      lteTraceLog = modemCallLog;
      pendingLteTraceMessage = "LTE MQTT fail";
      pendingLteTraceOk = false;
      pendingLteTracePublish = true;
      reportLteProgress = false;
    } else {
      noteLanMqttFailure("connect");
    }
    return;
  }

  lanMqttFailCount = 0;
  lteMqttConnectFails = 0;
  // The address worked, so stop forcing a re-resolve (see resolveLteMqttPeer).
  lteForceDnsResolve = false;

  if (!state.lteMqttTransport) {
    cachedMqttIp = mqttUsesTls() ? mqttTlsClient.remoteIP() : mqttPlainClient.remoteIP();
    Serial.printf("[mqtt] path ip=%s\n", cachedMqttIp.toString().c_str());
    // Each real connect is a free, authoritative answer to "where is the broker
    // right now", so restart the hourly resolve clock from it. The hourly timer
    // exists to catch a DNS repoint that happens while we happen to be connected.
    lastBrokerResolveMs = millis();
    if (lastBrokerResolveMs == 0) {
      lastBrokerResolveMs = 1;
    }
    if (state.ethernetConnected) {
      ethInternetUp = true;
    } else if (state.wifiConnected) {
      wifiInternetUp = true;
    }
  } else {
    lteTraceLog = modemCallLog;
    lteMqttClient.noteHandshake("mqtt connected ok");
    pendingLteTraceMessage = "LTE MQTT OK";
    pendingLteTraceOk = true;
    pendingLteTracePublish = true;
    reportLteProgress = false;
  }

  state.mqttConnected = true;
  lastMqttOkMs = millis();
  lastSilenceProbeMs = 0;
  mqttClient.subscribe(mqttTopic("config/desired").c_str(), 1);
  mqttClient.subscribe(mqttTopic("command").c_str(), 1);
  publishDeviceStatus("online", true);
  publishTelemetryNow();
  lastTelemetryPublishMs = millis();
  publishLteDataTrace();
  lastCellularStatusMs = millis();
  setStatus("MQTT OK");
}
void enforceMqttSilenceWatchdog() {
  if (state.callInProgress || state.otaInProgress || state.audioSyncInProgress) {
    return;
  }
  if (!state.mqttConfigured) {
    return;
  }

  const uint32_t now = millis();
  if (now - lastMqttOkMs >= kMqttSilenceRestartMs) {
    Serial.printf("[mqtt] no successful publish for %lu ms, restarting\n",
                  static_cast<unsigned long>(now - lastMqttOkMs));
    setStatus("MQTT watchdog");
    delay(300);
    ESP.restart();
  }

#if COF_LTE_MQTT_NATIVE
  if (state.lteMqttTransport) {
    // The A7672's CMQTT stack runs keepalive natively (keepalive_time in
    // CMQTTCONNECT) and surfaces passive loss as +CMQTTCONNLOST / +CMQTTNONET,
    // which cmqttLoop() folds back into the connection state. There is no
    // PubSubClient socket to ping, so nothing to probe here; the reconnect path
    // in connectMqttNativeIfNeeded() is what reacts to a dropped broker link.
    if (!cmqttIsConnected()) {
      lastSilenceProbeMs = 0;
    }
    return;
  }
#endif

  if (!mqttClient.connected()) {
    lastSilenceProbeMs = 0;
    return;
  }

  // A quiet link is normal: telemetry is published every state.telemetryIntervalMs
  // and nothing else may be sent for a whole cycle. Prove the connection is alive
  // with a cheap MQTT-level ping before declaring it dead, and only then force a
  // reconnect. Without this, the reconnect fired every kMqttSilenceReconnectMs and
  // the reconnect path republishes status+telemetry, which is what produced the
  // observed ~20 s telemetry cadence.
  if (now - lastMqttOkMs < kMqttSilenceReconnectMs) {
    return;
  }
  if (lastSilenceProbeMs != 0 && now - lastSilenceProbeMs < kSilenceProbeIntervalMs) {
    return;
  }
  lastSilenceProbeMs = now == 0 ? 1 : now;
  // PubSubClient keeps the connection warm by itself: loop() emits PINGREQ once
  // the keepalive elapses and returns false if the PINGRESP never arrives, so it
  // is already a real liveness probe. Pinging from here too would make
  // pingOutstanding collide with loop()'s own ping bookkeeping.
  if (mqttClient.loop()) {
    lastMqttOkMs = millis();
    return;
  }
  Serial.printf("[mqtt] keepalive lost after %lu ms, reconnecting\n",
                static_cast<unsigned long>(now - lastMqttOkMs));
  mqttClient.disconnect();
  state.mqttConnected = false;
  lastMqttReconnectMs = 0;
  lastSilenceProbeMs = 0;
}
void configureMqttClientTransport() {
  // Prefer LAN whenever it is genuinely usable: Ethernet first (by
  // applyPreferredRoute), then WiFi, and only then LTE. `ltePreemptSinceMs` is
  // deliberately not part of this test: serviceNetworkPaths() only arms it after
  // a probe has already set the interface's health flag, so adding it here would
  // only create a window where a dead LAN can reclaim MQTT.
  const bool lanLooksUsable = lanConnected() && !ethernetHoldoffActive() && lanHasInternet();
#if COF_LTE_MQTT_NATIVE
  if (state.lteDataUp && !lanLooksUsable) {
    // Native CMQTT owns the cellular path entirely; PubSubClient is not involved
    // (cmqttConnect() dials tcp:// and activates its own PDP context). The
    // reportLteProgress flag is raised in connectMqttNativeIfNeeded() during the
    // actual connect attempt, not here - this runs every pass and must stay
    // side-effect free so modemCallLog isn't polluted with steady-state traffic.
    state.lteMqttTransport = true;
  } else {
    state.lteMqttTransport = false;
    if (mqttUsesTls()) {
      mqttTlsClient.setInsecure();
      mqttClient.setClient(mqttTlsClient);
    } else {
      mqttClient.setClient(mqttPlainClient);
    }
    mqttClient.setSocketTimeout(kMqttSocketTimeoutSeconds);
  }
#else
  if (state.lteDataUp && !lanLooksUsable) {
    mqttClient.setClient(lteMqttClient);
    state.lteMqttTransport = true;
    mqttClient.setSocketTimeout(30);
    reportLteProgress = true;
  } else {
    state.lteMqttTransport = false;
    if (mqttUsesTls()) {
      mqttTlsClient.setInsecure();
      mqttClient.setClient(mqttTlsClient);
    } else {
      mqttClient.setClient(mqttPlainClient);
    }
    mqttClient.setSocketTimeout(kMqttSocketTimeoutSeconds);
  }
#endif
  mqttClient.setServer(state.mqttHost.c_str(), state.mqttPort);
  mqttClient.setCallback(onMqttMessage);
  mqttClient.setBufferSize(4096);
  mqttClient.setKeepAlive(kMqttKeepAliveSeconds);
  mqttPlainClient.setTimeout(kMqttSocketTimeoutSeconds * 1000);
  mqttTlsClient.setTimeout(kMqttSocketTimeoutSeconds * 1000);
}
