// main.cpp -- lifecycle, sensors, display and the cooperative loop.
//
// Function bodies live in the sibling .cpp files; the globals are DEFINED here
// (declared in cof_state.h) so this file owns the shared state.

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
#include <esp_netif.h>
#include <esp_netif_net_stack.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <mbedtls/sha256.h>
#include "lwip/dns.h"
#include "lwip/netif.h"

#include <cstring>
#include "cof_config.h"


#include "cof_state.h"

HardwareSerial ModemSerial(2);
WiFiClient mqttPlainClient;
WiFiClientSecure mqttTlsClient;
PubSubClient mqttClient;
U8G2_SH1106_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);
Adafruit_SHT31 sht31;
OneWire oneWire(COF_PIN_ONEWIRE);
DallasTemperature ds18b20(&oneWire);
Preferences preferences;
RuntimeState state;
uint32_t lastDisplayMs = 0;
uint32_t lastSensorMs = 0;
uint32_t lastModemMs = 0;
uint32_t lastSmsPollMs = 0;
uint32_t lastMqttReconnectMs = 0;
uint32_t lastMqttOkMs = 0;
uint8_t lanMqttFailCount = 0;
uint32_t wifiBackupDueMs = 0;
uint8_t wifiAuthFailCount = 0;
uint32_t lastEthProbeMs = 0;
uint8_t ethProbeFails = 0;
uint32_t lastLanReachableProbeMs = 0;
bool lanReachableLatch = false;
uint32_t noLanSinceMs = 0;
uint32_t ethernetUpAtMs = 0;
uint32_t ethernetHoldoffUntilMs = 0;
IPAddress cachedMqttIp;
IPAddress resolvedBrokerIp;
bool lteForceDnsResolve = false;
volatile bool brokerResolveDone = false;
bool brokerResolveInFlight = false;
bool brokerResolveFailed = false;
uint32_t brokerResolveStartedMs = 0;
uint32_t lastBrokerResolveMs = 0;
// Stable storage for the name being resolved: lwIP keeps this pointer for the
// whole async lookup, and state.mqttHost can be rewritten by a config apply.
String brokerResolveName;
uint32_t lastTelemetryPublishMs = 0;
uint32_t lastCellularStatusMs = 0;
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
bool pendingModemProbeCommand = false;
String pendingModemProbeCommandId = "";
bool pendingTestCallCommand = false;
String pendingTestCallPhone = "";
String pendingTestCallAudioSha = "";
String pendingTestCallCommandId = "";
bool reportTestCallProgress = false;
String pendingCallUrcs;
String pendingModemUrcs;
String modemCallLog;
bool pendingTestSmsCommand = false;
String pendingTestSmsPhone = "";
String pendingTestSmsText = "";
String pendingTestSmsCommandId = "";
bool pendingCommandAck = false;
String pendingCommandId = "";
String pendingCommandName = "";
String pendingCommandStatus = "";
String pendingCommandMessage = "";
bool pendingMqttBounce = false;
bool pendingNetworkStatusReport = false;
uint32_t lastLteAttemptMs = 0;
bool reportLteProgress = false;
bool lastLteFail = false;
LteIpStack lteIpStack = kLteStackUnknown;
String lteTraceLog;
String pendingLteTraceMessage;
bool pendingLteTracePublish = false;
bool pendingLteTraceOk = false;
// Path health. Optimistic when an interface gets its IP (so boot is not slowed
// down by a probe), demoted only when the broker probe actually fails.
bool ethInternetUp = false;
bool wifiInternetUp = false;
uint8_t wifiProbeFails = 0;
uint32_t lastWifiProbeMs = 0;
uint32_t lastEthRecoverProbeMs = 0;
// Ethernet is flagged down but still physically present (link up + lease), so it
// is worth probing to get the better path back. Derived from ETH.linkUp() rather
// than latched from an event, so a replug without a matching event still recovers.
bool ethRecoverPending = false;
uint8_t modemRecoveryStage = 0;
uint32_t lastModemRecoveryMs = 0;
// Anti-flap hysteresis (see kModemRecoveryHoldMs): when the radio reports
// healthy, this records for how long it has done so. The ladder stage is only
// cleared after the hold elapses, so a marginal signal that flaps NO SERVICE
// <-> service does not re-run the disruptive step 1 on every flap.
uint32_t modemHealthySinceMs = 0;
uint32_t lastModemRecoveryEventMs = 0;
uint32_t ltePreemptSinceMs = 0;
uint32_t lastLteRetryDelayMs = kLteRetryIntervalMs;
uint32_t lastLteDataEventMs = 0;
uint8_t lteMqttConnectFails = 0;
uint32_t lastSilenceProbeMs = 0;
// Events produced while MQTT is down, drained in order once it is back.
DeferredEvent deferredEvents[kDeferredEventMax];
size_t deferredEventCount = 0;
// Which interface the lwIP default route should use. Auto follows the health
// flags (Ethernet > WiFi); the explicit values pin the route for a reachability
// probe, which is only meaningful if the packet leaves through the interface
// under test.
void setStatus(const String& line) {
  state.statusLine = line;
  Serial.println("[status] " + line);
}
// Human-readable reset reason, captured once at boot so the panel can tell a
// recovery-ladder ESP.restart() ("software") from a crash ("panic"/"wdt"). See
// BACKLOG §6.
const char* resetReasonName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_EXT: return "external_pin";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    default: return "unknown";
  }
}
// --- OTA rollback guard -----------------------------------------------------
// Runs once per boot. See kOtaConfirm* constants for the policy.
bool otaConfirmPending = false;
uint32_t otaConfirmBootMs = 0;
bool otaConfirmedThisBoot = false;
void publishTestCallProgress(const String& message) {
  if (!reportTestCallProgress) {
    return;
  }
  setStatus(message);
  if (state.mqttConnected) {
    // Keep the keepalive serviced so the broker does not drop us mid-call.
    if (mqttClient.loop()) {
      lastMqttOkMs = millis();
    } else {
      state.mqttConnected = false;
    }
    publishDeviceEvent("test_call", "info", withFirmware(message));
  }
}
void publishTestCallResult(const String& result, bool ok, const String& commandId) {
  JsonDocument doc;
  doc["device_id"] = state.mqttDeviceId;
  doc["firmware"] = COF_FIRMWARE_VERSION;
  doc["type"] = "test_call";
  doc["severity"] = ok ? "info" : "warning";
  doc["message"] = withFirmware(result);
  if (commandId.length() > 0) {
    doc["command_id"] = commandId;
  }
  if (modemCallLog.length() > 0) {
    doc["modem_log"] = modemCallLog;
  }
  publishMqttJson("event", doc, false, 1);
}
LteMqttClient lteMqttClient;
void maintainLteFallback() {
  if (state.callInProgress || state.otaInProgress || state.audioSyncInProgress) {
    return;
  }
  if (static_cast<int32_t>(millis()) < static_cast<int32_t>(kLteBootGraceMs)) {
    return;
  }

  const bool lanMqttOk = state.mqttConnected && !state.lteMqttTransport && lanConnected();
  if (lanMqttOk) {
    noLanSinceMs = 0;
    lastLteFail = false;
    lastLteAttemptMs = 0;
    if (state.lteDataUp || state.lteMqttTransport) {
      Serial.println("[lte] LAN MQTT ok, stopping PDP");
      stopLtePdp();
      lastMqttReconnectMs = 0;
    }
    return;
  }

  // A LAN interface is only a reason to stay off LTE if it actually reaches the
  // broker. Previously a bare DHCP lease was enough, so a router with no uplink
  // kept lanConnected() true and LTE never engaged.
  const bool lanUsable = canUseLan();
  if (lanUsable) {
    noLanSinceMs = 0;
    if (!state.lteDataUp) {
      lastLteFail = false;
    }
    lastLteAttemptMs = 0;
    lastLteRetryDelayMs = kLteRetryIntervalMs;
    return;
  }

  if (noLanSinceMs == 0) {
    noLanSinceMs = millis();
    if (noLanSinceMs == 0) {
      noLanSinceMs = 1;
    }
  }
  const bool waitingWifi = state.wifiConfigured && !state.wifiConnected &&
                           wifiAuthFailCount < kWifiAuthFailLimit && !state.lteDataUp;
  if (waitingWifi && static_cast<int32_t>(millis() - noLanSinceMs) < static_cast<int32_t>(kWifiGraceBeforeLteMs)) {
    startWifiRadio();
    return;
  }
  ensureLtePdp();
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
bool networkStatRegistered(int stat) {
  return stat == 1 || stat == 5 || stat == 9 || stat == 10;
}
void pollModem() {
  if (!state.modemReady) {
    initModem();
    return;
  }
  if (!state.simReady) {
    refreshSimReady();
    if (!state.simReady) {
      return;
    }
  }

  const bool wasRegistered = state.networkRegistered;
  refreshCellularStatus();
  if (state.networkRegistered && !wasRegistered) {
    setStatus("Network OK");
    if (state.mqttConnected) {
      publishDeviceStatus("online", true);
    }
  }

  // Radio stuck without service. This used to recover only via a physical power
  // cycle, which is not available on a remote site, so the device could sit dead
  // while Ethernet, WiFi and LTE all looked "available". Escalate a modem reset
  // from the cheapest step, spaced out so one attempt has time to take effect.
  if (state.networkRegistered && radioReportsService()) {
    // The footer "Modem reset" is cosmetic and can clear immediately.
    if (state.statusLine == "Modem reset" || state.statusLine == "Restart (modem)") {
      setStatus("Network OK");
    }
    // The ladder stage, however, only clears after the radio has stayed healthy
    // for kModemRecoveryHoldMs. A single good read on a flapping radio must not
    // reset the escalation: that re-ran step 1 (CGATT detach/attach) every flap
    // and amplified a marginal-signal hiccup into a ~45 s outage (13x "step 1/5"
    // observed 2026-09-25). See kModemRecoveryHoldMs.
    if (modemRecoveryStage > 0) {
      const uint32_t healthyNow = millis();
      if (modemHealthySinceMs == 0) {
        modemHealthySinceMs = healthyNow == 0 ? 1 : healthyNow;
      } else if (healthyNow - modemHealthySinceMs >= kModemRecoveryHoldMs) {
        modemRecoveryStage = 0;
        lastModemRecoveryMs = 0;
        modemHealthySinceMs = 0;
      }
    } else {
      modemHealthySinceMs = 0;
    }
    return;
  }
  modemHealthySinceMs = 0;
  if (state.callInProgress || state.otaInProgress || state.audioSyncInProgress) {
    return;
  }
  const uint32_t now = millis();
  if (modemRecoveryStage >= kModemRecoveryMaxStage) {
    // Already at the most aggressive step; it logs and restarts there.
    return;
  }
  if (lastModemRecoveryMs != 0 && now - lastModemRecoveryMs < kModemRecoveryIntervalMs) {
    return;
  }
  lastModemRecoveryMs = now == 0 ? 1 : now;
  modemRecoveryStage++;
  Serial.printf("[modem] no service, recovery step %u/%u\n",
                modemRecoveryStage,
                static_cast<unsigned>(kModemRecoveryMaxStage));
  // Surface only the episode boundaries to the panel, and rate-limit them: the
  // ladder runs while MQTT is down, so per-step events pile up in the deferred
  // queue and flush in a burst on reconnect (13x "step 1/5" observed 2026-09-25).
  // Intermediate steps stay on the OLED footer and the serial log.
  if ((modemRecoveryStage == 1 || modemRecoveryStage >= kModemRecoveryMaxStage) &&
      (lastModemRecoveryEventMs == 0 || now - lastModemRecoveryEventMs >= kModemRecoveryEventMinIntervalMs)) {
    lastModemRecoveryEventMs = now == 0 ? 1 : now;
    publishDeviceEvent("modem_recovery", "warning",
                       modemRecoveryStage == 1
                           ? "Radio NO SERVICE, recovery started"
                           : "Radio NO SERVICE, recovery exhausted (restarting)");
  }
  resetModemRadio(modemRecoveryStage);
}
int parseClccStat(const String& response) {
  return parseClccStatAt(response, response.indexOf("+CLCC:"));
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
// 3GPP TS 24.008 / SIMCom CEER: classify by who released the call and why.
// Duration of ring/audio is not a call-state signal and must not decide the result.
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
  Serial.printf("WiFi: %s configured=%s SSID=%s IP=%s RSSI=%d\n",
                state.wifiConnected ? "OK" : "NO",
                state.wifiConfigured ? "YES" : "NO",
                state.wifiSsid.c_str(),
                state.wifiIpAddress.c_str(),
                state.wifiConnected ? WiFi.RSSI() : 0);
  Serial.printf("LTE data: %s IP=%s cid=%u mqtt_via_lte=%s\n",
                state.lteDataUp ? "OK" : "NO",
                state.lteIpAddress.c_str(),
                state.ltePdpCid,
                state.lteMqttTransport ? "YES" : "NO");
  Serial.printf("Active network: %s\n", activeNetworkName());
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
  Serial.printf("Boot reset reason: %s\n", state.bootResetReason.c_str());
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
  initOtaRollbackGuard();
  Serial.println();
  Serial.println("CallOnFail boot");
  Serial.println("Firmware " COF_FIRMWARE_VERSION);
  state.bootResetReason = resetReasonName();
  Serial.printf("[boot] reset reason: %s\n", state.bootResetReason.c_str());
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
  lastMqttOkMs = millis();
}
void loop() {
  const uint32_t now = millis();
  feedWatchdog();
  serviceOtaRollbackGuard();
  handleSerialInput();

  if (now - lastSensorMs >= kSensorIntervalMs) {
    lastSensorMs = now;
    readSensors();
  }

  if (now - lastDisplayMs >= kDisplayIntervalMs) {
    lastDisplayMs = now;
    drawDisplay();
  }

  // Before the path polls: they all gate on cachedMqttIp, which is otherwise only
  // learned from a successful MQTT connect.
  serviceBrokerResolve();
  pollEthernetPath();
  pollWifiPath();
  serviceNetworkPaths();
  connectMqttIfNeeded();
  maintainWifiBackup();
  enforceMqttSilenceWatchdog();

  if (state.mqttConnected && pendingNetworkStatusReport) {
    pendingNetworkStatusReport = false;
    publishDeviceStatus("online", true);
  }
  publishLteDataTrace();
  flushDeferredEvents();

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

  // Gated off a live call so the probe's AT traffic cannot abort an active CSFB
  // call; it just stays pending until the call finishes.
  if (pendingModemProbeCommand && !state.callInProgress && !state.otaInProgress &&
      !state.audioSyncInProgress) {
    const String probeCommandId = pendingModemProbeCommandId;
    pendingModemProbeCommand = false;
    pendingModemProbeCommandId = "";
    runModemProbe(probeCommandId);
  }

  if (pendingTestSmsCommand && !state.callInProgress && !state.otaInProgress && !state.audioSyncInProgress) {
    const String smsPhone = pendingTestSmsPhone;
    const String smsText = pendingTestSmsText;
    const String smsCommandId = pendingTestSmsCommandId;
    pendingTestSmsCommand = false;
    pendingTestSmsPhone = "";
    pendingTestSmsText = "";
    pendingTestSmsCommandId = "";
    const String result = sendTestSms(smsPhone, smsText);
    connectMqttIfNeeded();
    const bool ok = result == "SMS sent";
    publishDeviceEvent("test_sms", ok ? "info" : "warning", result, smsCommandId);
  }

  if (pendingTestCallCommand && !state.callInProgress && !state.otaInProgress && !state.audioSyncInProgress) {
    const String callPhone = pendingTestCallPhone;
    const String callAudioSha = pendingTestCallAudioSha;
    const String callCommandId = pendingTestCallCommandId;
    pendingTestCallCommand = false;
    pendingTestCallPhone = "";
    pendingTestCallAudioSha = "";
    pendingTestCallCommandId = "";
    reportTestCallProgress = true;
    const String result = placeCallAndPlayAudio(callPhone, true, callAudioSha);
    reportTestCallProgress = false;
    connectMqttIfNeeded();
    const bool ok = result.startsWith("Call done");
    publishTestCallResult(result, ok, callCommandId);
  }

  if (state.mqttConnected && now - lastTelemetryPublishMs >= state.telemetryIntervalMs) {
    lastTelemetryPublishMs = now;
    publishTelemetryNow();
  }

  if (state.mqttConnected && !state.callInProgress && !state.otaInProgress &&
      !state.lteMqttTransport &&
      now - lastCellularStatusMs >= kCellularStatusIntervalMs) {
    lastCellularStatusMs = now;
    refreshCellularStatus();
    publishDeviceStatus("online", true);
  }

  const uint32_t modemEvery = lanConnected() ? kModemIntervalMs : kModemRetryNoLanMs;
  if (now - lastModemMs >= modemEvery && !state.callInProgress && !state.audioSyncInProgress &&
      !state.lteMqttTransport) {
    lastModemMs = now;
    pollModem();
  }

  if (now - lastSmsPollMs >= kSmsPollIntervalMs && !state.callInProgress && !state.audioSyncInProgress &&
      !state.lteMqttTransport &&
      !pendingTestSmsCommand && !pendingTestCallCommand) {
    lastSmsPollMs = now;
    pollIncomingSms();
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
