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
#include <esp_task_wdt.h>
#include <mbedtls/sha256.h>
#include "lwip/netif.h"

#include <cstring>
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
constexpr uint32_t kModemRetryNoLanMs = 5000;
constexpr uint32_t kLteRetryIntervalMs = 10000;
constexpr uint32_t kSmsPollIntervalMs = 5000;
constexpr uint32_t kMqttReconnectIntervalMs = 5000;
constexpr uint32_t kMqttKeepAliveSeconds = 10;
constexpr uint32_t kMqttSocketTimeoutSeconds = 3;
constexpr uint8_t kLanMqttFailLimit = 2;
constexpr uint32_t kWifiBackupDelayMs = 1500;
constexpr uint8_t kWifiAuthFailLimit = 3;
// Path health. A LAN interface is only trusted as "the internet" after a real
// broker reachability probe: a router with no uplink still hands out DHCP
// leases, and treating that as internet used to pin the device on a dead path
// (lanConnected() stayed true, so LTE never engaged, and nothing re-probed
// Ethernet once LTE was up). See docs/device/NETWORK_PATHS.md.
constexpr uint32_t kEthProbeIntervalMs = 10000;
constexpr uint8_t kEthProbeFailLimit = 2;
constexpr uint8_t kWifiProbeFailLimit = 2;
constexpr uint32_t kWifiGraceBeforeLteMs = 8000;
constexpr uint32_t kLteBootGraceMs = 15000;
constexpr uint32_t kEthernetHoldoffMs = 20000;
// Ethernet marked down with the cable still in: its DHCP lease survives a
// router reboot, so no GOT_IP event ever fires again. Re-probe on this cadence.
constexpr uint32_t kPathRecoverProbeIntervalMs = 60UL * 1000UL;
// While MQTT runs over LTE, settle a recovered better path for this long before
// tearing the PDP down, so a flapping link cannot cause a reconnect storm.
constexpr uint32_t kPathPreemptSettleMs = 3000;
// Publishing liveness. This MUST stay above the largest telemetry interval we
// accept (60 s, enforced in loadSavedMqttConfig) or it forces a disconnect and
// reconnect every cycle - and each reconnect republishes telemetry, which is
// what produced the observed ~20 s cadence.
constexpr uint32_t kMqttSilenceReconnectMs = 90UL * 1000UL;
constexpr uint32_t kSilenceProbeIntervalMs = 45UL * 1000UL;
constexpr uint32_t kMqttSilenceRestartMs = 6UL * 60UL * 1000UL;
// lte_data traces carry a whole modem dump and were republished on every failed
// PDP attempt (every 10 s, forever). Back the retries off and throttle the event.
constexpr uint32_t kLteRetryMaxIntervalMs = 120UL * 1000UL;
constexpr uint32_t kLteDataEventMinIntervalMs = 30UL * 1000UL;
// Events produced while MQTT is down (SMS/call results) used to be dropped by
// publishMqttJson. Queue a bounded number and flush them on reconnect.
constexpr size_t kDeferredEventMax = 8;
constexpr uint32_t kTelemetryPublishIntervalMs = 60000;
constexpr uint32_t kCellularStatusIntervalMs = 5UL * 60UL * 1000UL;
constexpr uint32_t kManifestInitialDelayMs = 15000;
constexpr uint32_t kManifestIntervalMs = 60UL * 60UL * 1000UL;
constexpr uint32_t kWatchdogTimeoutSeconds = 60;

// After an OTA the bootloader holds the new image in ESP_OTA_IMG_PENDING_VERIFY
// (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y). If we never confirm, the next reset
// rolls back to the previous slot. That is the safety net for a remote device
// we cannot reach physically, so only confirm once the device proves it works:
// MQTT connected (which implies link + broker reachable) plus a minimum uptime
// so we do not confirm during a flapping reconnect.
constexpr uint32_t kOtaConfirmMinUptimeMs = 20UL * 1000UL;
constexpr uint32_t kOtaConfirmTimeoutMs = 5UL * 60UL * 1000UL;

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
  bool lteDataUp = false;
  bool lteMqttTransport = false;
  uint8_t ltePdpCid = 0;
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
  String lteIpAddress = "-";
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
  String manifestFirmwareSha256 = "";
  String manifestAudioVersion = "";
  String manifestAudioUrl = "";
  String manifestPhoneNumber = COF_PHONE_NUMBER;
};

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
uint32_t noLanSinceMs = 0;
uint32_t ethernetUpAtMs = 0;
uint32_t ethernetHoldoffUntilMs = 0;
IPAddress cachedMqttIp;
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
bool pendingTestCallCommand = false;
String pendingTestCallPhone = "";
String pendingTestCallAudioUrl = "";
String pendingTestCallAudioFormat = "";
String pendingTestCallCommandId = "";
bool reportTestCallProgress = false;
String pendingCallUrcs;
String pendingModemUrcs;
String modemCallLog;
constexpr uint16_t kModemCallLogMax = 1800;
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
enum LteIpStack : uint8_t { kLteStackUnknown = 0, kLteStackNetopen, kLteStackCnact };
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
uint32_t ltePreemptSinceMs = 0;
uint32_t lastLteRetryDelayMs = kLteRetryIntervalMs;
uint32_t lastLteDataEventMs = 0;
uint8_t lteMqttConnectFails = 0;
uint32_t lastSilenceProbeMs = 0;

// Events produced while MQTT is down, drained in order once it is back.
struct DeferredEvent {
  String type;
  String severity;
  String message;
  String commandId;
};
DeferredEvent deferredEvents[kDeferredEventMax];
size_t deferredEventCount = 0;

bool csAttached();
void onMqttMessage(char* topic, byte* payload, unsigned int length);
void configureMqttClientTransport();
void maintainLteFallback();
void releaseLteMqttForModem();
void requestMqttBounce(const char* reason);
void connectWiFi(const String& ssid, const String& password, bool saveCredentials);
void forgetWifiRadio();
void pauseWiFiRadio();
void startWifiRadio();
void scheduleWifiBackup(uint32_t delayMs);
void maintainWifiBackup();
void pollEthernetPath();
void markEthernetDown(const char* reason);
void feedWatchdog();
bool probeMqttOverEthernet();
bool ethernetHoldoffActive();
void refreshCellularStatus();
bool initModem();
void pollWifiPath();
void serviceNetworkPaths();
// Which interface the lwIP default route should use. Auto follows the health
// flags (Ethernet > WiFi); the explicit values pin the route for a reachability
// probe, which is only meaningful if the packet leaves through the interface
// under test.
enum class PathPreference : uint8_t { Auto, Ethernet, Wifi };
void applyPreferredRoute(PathPreference pref = PathPreference::Auto);
void stopLtePdp();
void deferDeviceEvent(const char* type, const char* severity, const String& message, const String& commandId);
void flushDeferredEvents();
bool probeMqttOverWifi();
bool probeMqttOnInterface(bool ethernet);
bool ethernetHoldoffActive();
bool publishDeviceEvent(const char* type, const char* severity, const String& message, const String& commandId);
bool refreshSimReady();
bool sendAT(const String& command, const String& expected = "OK", uint32_t timeoutMs = 2000, String* responseOut = nullptr);
void flushModemInput();
String readModemUntil(uint32_t timeoutMs, const String& token = "");
bool modemWaitForPrompt(uint32_t timeoutMs);
void feedWatchdog();
void initOtaRollbackGuard();
void serviceOtaRollbackGuard();

void setStatus(const String& line) {
  state.statusLine = line;
  Serial.println("[status] " + line);
}

bool mqttUsesTls() {
  return state.mqttPort == 8883 || state.mqttPort == 8884;
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

bool lanConnected() {
  return state.ethernetConnected || state.wifiConnected;
}

bool networkConnected() {
  return lanConnected() || state.lteDataUp;
}

const char* activeNetworkName() {
  if (state.lteMqttTransport && state.lteDataUp) {
    return "lte";
  }
  if (state.ethernetConnected && ethInternetUp) {
    return "ethernet";
  }
  if (state.wifiConnected && wifiInternetUp) {
    return "wifi";
  }
  if (state.lteDataUp) {
    return "lte";
  }
  if (state.ethernetConnected) {
    return "ethernet";
  }
  if (state.wifiConnected) {
    return "wifi";
  }
  return "none";
}

const char* mqttPathLetter() {
  if (!state.mqttConnected) {
    return "--";
  }
  if (state.lteMqttTransport) {
    return "L";
  }
  if (state.ethernetConnected) {
    return "E";
  }
  if (state.wifiConnected) {
    return "W";
  }
  return "?";
}

void applyPreferredRoute(PathPreference pref) {
  // Auto gates the Ethernet branch on ethInternetUp: an Ethernet interface that
  // is up but not carrying the internet (a router with no uplink - the exact case
  // that used to wedge the device on WiFi while DHCP was up) must not win the
  // default route, otherwise every new TCP connection, including the MQTT
  // reconnect, keeps leaving through the dead path.
  //
  // PathPreference::Ethernet also accepts an interface that still has an IP but
  // is not flagged connected yet: that is the state inside the holdoff window,
  // where markEthernetUp() must probe Ethernet specifically before re-accepting
  // it, and probing over the wrong interface would give a false positive.
  bool useEthernet = false;
  bool useWifi = false;
  switch (pref) {
    case PathPreference::Ethernet:
      useEthernet = state.ethernetConnected || ETH.localIP() != IPAddress((uint32_t)0);
      break;
    case PathPreference::Wifi:
      useWifi = state.wifiConnected;
      break;
    case PathPreference::Auto:
      useEthernet = state.ethernetConnected && ethInternetUp;
      useWifi = state.wifiConnected;
      break;
  }
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  if (useEthernet) {
    ETH.setDefault();
    Serial.println("[net] default route ETH");
  } else if (useWifi) {
    WiFi.setDefault();
    Serial.println("[net] default route WiFi");
  }
#else
  esp_netif_t* netif = nullptr;
  if (useEthernet) {
    netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
  } else if (useWifi) {
    netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  }
  if (netif != nullptr) {
    auto* lwipIf = static_cast<struct netif*>(esp_netif_get_netif_impl(netif));
    if (lwipIf != nullptr) {
      netif_set_default(lwipIf);
      Serial.printf("[net] default route %s\n", useEthernet ? "ETH" : "WiFi");
    }
  }
#endif
}

void forgetWifiRadio() {
  wifiBackupDueMs = 0;
  wifiAuthFailCount = 0;
  WiFi.setAutoReconnect(false);
  WiFi.persistent(true);
  WiFi.disconnect(false, true);
  delay(50);
  WiFi.persistent(false);
  state.wifiConnected = false;
  state.wifiIpAddress = "-";
  wifiInternetUp = false;
  wifiProbeFails = 0;
}

void pauseWiFiRadio() {
  wifiBackupDueMs = 0;
  wifiAuthFailCount = 0;
  WiFi.setAutoReconnect(false);
  if (WiFi.getMode() == WIFI_OFF) {
    return;
  }
  WiFi.disconnect(false);
  state.wifiConnected = false;
  state.wifiIpAddress = "-";
  wifiInternetUp = false;
  wifiProbeFails = 0;
  Serial.println("[wifi] paused (ethernet primary)");
}

void startWifiRadio() {
  if (!state.wifiConfigured) {
    return;
  }
  const String ssid = preferences.getString("wifiSsid", "");
  const String password = preferences.getString("wifiPass", "");
  if (ssid.length() == 0) {
    return;
  }
  if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == ssid) {
    state.wifiConnected = true;
    state.wifiIpAddress = WiFi.localIP().toString();
    return;
  }
  wifiAuthFailCount = 0;
  state.wifiSsid = ssid;
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid.c_str(), password.c_str());
  setStatus("WiFi connecting");
  Serial.printf("[wifi] connecting to %s\n", ssid.c_str());
}

void scheduleWifiBackup(uint32_t delayMs) {
  if (!state.wifiConfigured || state.ethernetConnected) {
    wifiBackupDueMs = 0;
    return;
  }
  const uint32_t due = millis() + delayMs;
  wifiBackupDueMs = due == 0 ? 1 : due;
}

void maintainWifiBackup() {
  if (state.ethernetConnected || wifiBackupDueMs == 0) {
    return;
  }
  if (static_cast<int32_t>(millis() - wifiBackupDueMs) < 0) {
    return;
  }
  wifiBackupDueMs = 0;
  startWifiRadio();
}

bool ethernetHoldoffActive() {
  return ethernetHoldoffUntilMs != 0 &&
         static_cast<int32_t>(millis() - ethernetHoldoffUntilMs) < 0;
}

bool lanHasInternet() {
  // True when any LAN path is trusted to reach the broker. Optimistic right
  // after DHCP, demoted by real connect/publish/probe failures.
  return (state.ethernetConnected && ethInternetUp) || (state.wifiConnected && wifiInternetUp);
}

// Reachability probe. It needs a broker IP, which we only learn from a real MQTT
// connect: a DNS lookup here would be the alternative, but WiFi.hostByName()
// blocks for up to 15 s inside the main loop (it waits on the lwIP DNS semaphore),
// which would stall sensors, display and MQTT. So while the broker IP is unknown
// we rely on the optimistic default plus connect-failure demotion instead.
bool lanPathReachable() {
  if (cachedMqttIp == IPAddress((uint32_t)0)) {
    return false;
  }
  if (state.ethernetConnected && probeMqttOnInterface(true)) {
    return true;
  }
  if (state.wifiConnected && probeMqttOnInterface(false)) {
    return true;
  }
  return false;
}

bool canUseLan() {
  if (!lanConnected()) {
    return false;
  }
  if (lanHasInternet()) {
    return true;
  }
  return lanPathReachable();
}

bool probeMqttOverEthernet() {
  if (cachedMqttIp == IPAddress((uint32_t)0)) {
    return false;
  }
  WiFiClient probe;
  feedWatchdog();
  const int ok = probe.connect(cachedMqttIp, state.mqttPort, 1500);
  probe.stop();
  return ok != 0;
}

bool probeMqttOverWifi() {
  if (cachedMqttIp == IPAddress((uint32_t)0)) {
    return false;
  }
  WiFiClient probe;
  feedWatchdog();
  const int ok = probe.connect(cachedMqttIp, state.mqttPort, 1500);
  probe.stop();
  return ok != 0;
}

// A probe only proves something about an interface if the packet actually leaves
// through it, so pin the lwIP default route for the duration of the probe and
// restore the normal preference afterwards. Without this, probing Ethernet while
// WiFi is associated - or probing WiFi while Ethernet still owns the route -
// would silently test the other interface and report a false result.
bool probeMqttOnInterface(bool ethernet) {
  if (ethernet && !state.ethernetConnected) {
    return false;
  }
  if (!ethernet && !state.wifiConnected) {
    return false;
  }
  applyPreferredRoute(ethernet ? PathPreference::Ethernet : PathPreference::Wifi);
  const bool ok = ethernet ? probeMqttOverEthernet() : probeMqttOverWifi();
  applyPreferredRoute();
  return ok;
}

// An interface is only "the internet" if the broker is reachable through it.
// A router with no uplink still gives link + DHCP, so reachability is the only
// honest signal. Probes use the cached broker IP, which we only learn from a
// real successful MQTT connect, so this never turns into a DNS dependency.
void pollEthernetPath() {
  if (!state.ethernetConnected || state.callInProgress || state.otaInProgress) {
    ethProbeFails = 0;
    return;
  }
  if (state.lteMqttTransport) {
    // While MQTT runs over LTE we must still watch Ethernet for recovery, just
    // slowly: this is what used to be skipped entirely (the early return left
    // Ethernet never re-probed, so LTE could not be released while it was up).
    const uint32_t now0 = millis();
    if (ethernetHoldoffActive()) {
      return;
    }
    if (lastEthRecoverProbeMs != 0 && now0 - lastEthRecoverProbeMs < kPathRecoverProbeIntervalMs) {
      return;
    }
    lastEthRecoverProbeMs = now0 == 0 ? 1 : now0;
    const bool reachable = probeMqttOnInterface(true);
    if (reachable) {
      ethProbeFails = 0;
      if (!ethInternetUp) {
        ethInternetUp = true;
        Serial.println("[net] ethernet recovered while on LTE");
        applyPreferredRoute();
      }
    } else if (ethInternetUp) {
      ethInternetUp = false;
    }
    return;
  }

  const uint32_t now = millis();
  if (lastEthProbeMs != 0 && now - lastEthProbeMs < kEthProbeIntervalMs) {
    return;
  }
  lastEthProbeMs = now == 0 ? 1 : now;
  if (ethernetUpAtMs != 0 && static_cast<int32_t>(now - ethernetUpAtMs) < 8000) {
    return;
  }
  if (cachedMqttIp == IPAddress((uint32_t)0)) {
    // No real connect yet, so we cannot probe. Give MQTT time to come up; if it
    // never does, assume the Ethernet path is not carrying the internet.
    if (!state.mqttConnected && ethernetUpAtMs != 0 &&
        static_cast<int32_t>(now - ethernetUpAtMs) >= 15000) {
      markEthernetDown("no mqtt path");
    }
    return;
  }

  const bool reachable = probeMqttOnInterface(true);
  if (reachable) {
    ethProbeFails = 0;
    if (!ethInternetUp) {
      ethInternetUp = true;
      Serial.println("[eth] path recovered");
      requestMqttBounce("eth recovered");
    }
    return;
  }
  ethProbeFails++;
  Serial.printf("[eth] path probe fail %u/%u ip=%s\n",
                ethProbeFails,
                kEthProbeFailLimit,
                cachedMqttIp.toString().c_str());
  if (ethProbeFails >= kEthProbeFailLimit) {
    markEthernetDown("path probe");
  }
}

// WiFi used to be trusted on a bare DHCP lease and could never be demoted, so a
// WiFi uplink without internet blocked the LTE fallback forever.
void pollWifiPath() {
  if (!state.wifiConnected || state.callInProgress || state.otaInProgress) {
    wifiProbeFails = 0;
    return;
  }
  if (state.ethernetConnected) {
    return;
  }
  if (cachedMqttIp == IPAddress((uint32_t)0)) {
    return;
  }
  const uint32_t now = millis();
  if (lastWifiProbeMs != 0 && now - lastWifiProbeMs < kEthProbeIntervalMs) {
    return;
  }
  lastWifiProbeMs = now == 0 ? 1 : now;
  // Pin the route to WiFi: this runs only when Ethernet is not connected, but a
  // pinned probe keeps the result honest if that ever changes.
  const bool reachable = probeMqttOnInterface(false);
  if (reachable) {
    wifiProbeFails = 0;
    if (!wifiInternetUp) {
      wifiInternetUp = true;
      Serial.println("[wifi] path recovered");
    }
    return;
  }
  wifiProbeFails++;
  Serial.printf("[wifi] path probe fail %u/%u ip=%s\n",
                wifiProbeFails,
                kWifiProbeFailLimit,
                cachedMqttIp.toString().c_str());
  if (wifiProbeFails >= kWifiProbeFailLimit) {
    Serial.println("[wifi] no internet over WiFi, dropping path");
    wifiInternetUp = false;
    wifiProbeFails = 0;
    if (state.mqttConfigured && !state.lteMqttTransport) {
      requestMqttBounce("wifi no internet");
    }
    pendingNetworkStatusReport = true;
  }
}

// Drives the passive failback from LTE to a recovered LAN path.
void serviceNetworkPaths() {
  if (state.callInProgress || state.otaInProgress || state.audioSyncInProgress) {
    ltePreemptSinceMs = 0;
    return;
  }
  const bool betterLan =
      (state.ethernetConnected && ethInternetUp) || (state.wifiConnected && wifiInternetUp);
  if (!state.lteMqttTransport || !betterLan) {
    ltePreemptSinceMs = 0;
    return;
  }
  const uint32_t now = millis();
  if (ltePreemptSinceMs == 0) {
    ltePreemptSinceMs = now == 0 ? 1 : now;
    return;
  }
  if (now - ltePreemptSinceMs < kPathPreemptSettleMs) {
    return;
  }
  ltePreemptSinceMs = 0;
  Serial.println("[net] LAN path recovered, releasing LTE");
  stopLtePdp();
  applyPreferredRoute();
  requestMqttBounce("lan recovered");
}

void markEthernetUp(const char* reason) {
  if (ethernetHoldoffActive()) {
    // Probe Ethernet specifically: with the route on Auto, a live WiFi would have
    // answered instead and this would re-accept a dead Ethernet.
    const bool probed = probeMqttOnInterface(true);
    if (!probed) {
      Serial.printf("[eth] ignore up (%s) holdoff probe=%s ip=%s\n",
                    reason,
                    cachedMqttIp == IPAddress((uint32_t)0) ? "no-ip" : "fail",
                    ETH.localIP().toString().c_str());
      return;
    }
    ethernetHoldoffUntilMs = 0;
  }

  const bool wasEthernet = state.ethernetConnected;
  state.ethernetConnected = true;
  state.ipAddress = ETH.localIP().toString();
  lanMqttFailCount = 0;
  ethProbeFails = 0;
  // Optimistic: a fresh IP is treated as a working path until the probe says
  // otherwise. This keeps boot fast and avoids double-switching.
  ethInternetUp = true;
  lastEthRecoverProbeMs = 0;
  noLanSinceMs = 0;
  ethernetUpAtMs = millis();
  if (ethernetUpAtMs == 0) {
    ethernetUpAtMs = 1;
  }
  lastLteFail = false;
  applyPreferredRoute();
  if (!wasEthernet) {
    requestMqttBounce(reason);
  }
  pendingNetworkStatusReport = true;
  setStatus("ETH IP " + state.ipAddress);
  Serial.printf("[eth] up (%s) ip=%s\n", reason, state.ipAddress.c_str());
}

void markEthernetDown(const char* reason) {
  ethernetHoldoffUntilMs = millis() + kEthernetHoldoffMs;
  if (ethernetHoldoffUntilMs == 0) {
    ethernetHoldoffUntilMs = 1;
  }
  ethInternetUp = false;
  ethProbeFails = 0;
  lastEthRecoverProbeMs = 0;
  if (!state.ethernetConnected) {
    return;
  }
  Serial.printf("[eth] down (%s)\n", reason);
  state.ethernetConnected = false;
  state.ipAddress = "-";
  applyPreferredRoute();
  requestMqttBounce(reason);
  pendingNetworkStatusReport = true;
  setStatus("ETH down");
  lastLteAttemptMs = 0;
  startWifiRadio();
}

// A LAN path is only usable if it actually reaches the broker. `hadInternet`
// remembers whether this interface was the one carrying MQTT, so we can tell a
// real outage from a probe failure on a backup interface that was never used.
void noteLanPathFailure(bool ethernet, const char* reason, bool hadInternet) {
  if (state.lteMqttTransport) {
    return;
  }
  if (ethernet) {
    if (!state.ethernetConnected) {
      return;
    }
    lanMqttFailCount++;
    Serial.printf("[eth] mqtt fail %u/%u (%s)\n", lanMqttFailCount, kLanMqttFailLimit, reason);
    if (hadInternet || lanMqttFailCount >= kLanMqttFailLimit) {
      markEthernetDown(reason);
    }
    return;
  }
  if (!state.wifiConnected) {
    return;
  }
  wifiInternetUp = false;
  wifiProbeFails = 0;
  pendingNetworkStatusReport = true;
  Serial.printf("[wifi] path unusable (%s)\n", reason);
  if (hadInternet) {
    requestMqttBounce(reason);
  }
}

void noteLanMqttFailure(const char* reason) {
  if (state.ethernetConnected) {
    noteLanPathFailure(true, reason, ethInternetUp);
    return;
  }
  // WiFi used to be ignored here outright, which is why a WiFi link with a DHCP
  // lease but no upstream internet pinned the device forever: lanConnected()
  // stayed true so maintainLteFallback() never engaged LTE.
  if (state.wifiConnected) {
    noteLanPathFailure(false, reason, wifiInternetUp);
  }
}

void requestMqttBounce(const char* reason) {
  pendingMqttBounce = true;
  Serial.printf("[mqtt] bounce requested: %s\n", reason);
}

void bounceMqttForRouteChange() {
  if (mqttClient.connected() || state.mqttConnected) {
    mqttClient.disconnect();
  }
  state.mqttConnected = false;
  lastMqttReconnectMs = 0;
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

// --- OTA rollback guard -----------------------------------------------------
// Runs once per boot. See kOtaConfirm* constants for the policy.
bool otaConfirmPending = false;
uint32_t otaConfirmBootMs = 0;
bool otaConfirmedThisBoot = false;

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

void onNetworkEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      state.ethernetStarted = true;
      ETH.setHostname("callonfail");
      setStatus("ETH start");
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      setStatus("ETH cable OK");
      if (ETH.localIP() != IPAddress((uint32_t)0)) {
        markEthernetUp("cable");
      }
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      markEthernetUp("got ip");
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      markEthernetDown("disconnected");
      break;
    case ARDUINO_EVENT_ETH_STOP:
      state.ethernetStarted = false;
      markEthernetDown("stopped");
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      if (!state.wifiConfigured) {
        Serial.printf("[wifi] ignoring unsolicited IP %s\n", WiFi.localIP().toString().c_str());
        forgetWifiRadio();
        pendingNetworkStatusReport = true;
        break;
      }
      wifiAuthFailCount = 0;
      state.wifiConnected = true;
      state.wifiSsid = WiFi.SSID();
      state.wifiIpAddress = WiFi.localIP().toString();
      if (!state.ethernetConnected) {
        applyPreferredRoute();
        requestMqttBounce("wifi got ip");
      }
      pendingNetworkStatusReport = true;
      setStatus("WiFi IP " + state.wifiIpAddress);
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      const uint8_t reason = info.wifi_sta_disconnected.reason;
      const bool lostWifiPath = state.wifiConnected && !state.ethernetConnected;
      state.wifiConnected = false;
      state.wifiIpAddress = "-";
      wifiInternetUp = false;
      wifiProbeFails = 0;
      if (lostWifiPath) {
        requestMqttBounce("wifi disconnected");
      }
      if (state.wifiConfigured) {
        pendingNetworkStatusReport = true;
        setStatus("WiFi disconnected");
      }
      if (reason == 2 || reason == 15 || reason == 202 || reason == 203 || reason == 204) {
        wifiAuthFailCount++;
        Serial.printf("[wifi] auth/handshake fail %u/%u reason=%u\n",
                      wifiAuthFailCount, kWifiAuthFailLimit, reason);
        if (wifiAuthFailCount >= kWifiAuthFailLimit) {
          WiFi.setAutoReconnect(false);
          WiFi.disconnect(false);
          setStatus("WiFi auth fail");
        }
      }
      break;
    }
    default:
      break;
  }
}

void beginEthernet() {
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
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

  pendingNetworkStatusReport = true;
  startWifiRadio();
}

void beginSavedWiFi() {
  const String ssid = preferences.getString("wifiSsid", "");
  state.wifiConfigured = ssid.length() > 0;
  state.wifiSsid = ssid;

  if (!state.wifiConfigured) {
    Serial.println("[wifi] no saved credentials");
    forgetWifiRadio();
    return;
  }

  startWifiRadio();
}

void clearSavedWiFi() {
  preferences.remove("wifiSsid");
  preferences.remove("wifiPass");
  state.wifiConfigured = false;
  state.wifiSsid = "";
  forgetWifiRadio();
  pendingNetworkStatusReport = true;
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
        pendingTestCallAudioUrl = doc["audio_url"] | "";
        pendingTestCallAudioUrl.trim();
        pendingTestCallAudioFormat = doc["audio_format"] | "";
        pendingTestCallAudioFormat.trim();
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
  if (state.lteDataUp) {
    return state.lteIpAddress;
  }
  return "-";
}

bool modemLineInteresting(const String& line) {
  String upper = line;
  upper.toUpperCase();
  return upper.indexOf("ATD") >= 0 || upper.indexOf("ATH") >= 0 ||
         upper.indexOf("CLCC") >= 0 || upper.indexOf("CEER") >= 0 ||
         upper.indexOf("BUSY") >= 0 || upper.indexOf("CARRIER") >= 0 ||
         upper.indexOf("VOICE") >= 0 || upper.indexOf("AUDIO") >= 0 ||
         upper.indexOf("COLP") >= 0 || upper.indexOf("CCMX") >= 0 ||
         upper.indexOf("NO ANSWER") >= 0 || upper.indexOf("CHUP") >= 0;
}

void appendModemLog(char direction, const String& text) {
  if (!state.callInProgress && !reportTestCallProgress && !reportLteProgress) {
    return;
  }
  String line = text;
  line.replace("\r", " ");
  line.replace("\n", " | ");
  line.trim();
  if (line.length() == 0) {
    return;
  }
  if (!reportLteProgress && !modemLineInteresting(line)) {
    return;
  }
  String entry = String(direction == '>' ? ">> " : "<< ") + line;
  if (entry.length() > 140) {
    entry = entry.substring(0, 140);
  }
  while (modemCallLog.length() + entry.length() + 1 > kModemCallLogMax) {
    const int cut = modemCallLog.indexOf('\n');
    if (cut < 0) {
      modemCallLog = "";
      break;
    }
    modemCallLog = modemCallLog.substring(cut + 1);
  }
  if (modemCallLog.length() > 0) {
    modemCallLog += '\n';
  }
  modemCallLog += entry;
}

bool publishMqttJson(const String& suffix, JsonDocument& doc, bool retained = false, uint8_t qos = 0) {
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
  const bool ok = mqttClient.publish(topic.c_str(), reinterpret_cast<const uint8_t*>(payload), length, retained);
  Serial.printf("[mqtt] publish topic=%s ok=%s payload=%s\n", topic.c_str(), ok ? "yes" : "no", payload);
  (void)qos;
  if (ok) {
    lastMqttOkMs = millis();
    lanMqttFailCount = 0;
  } else {
    state.mqttConnected = false;
    mqttClient.disconnect();
    noteLanMqttFailure("publish");
  }
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
  doc["network_ethernet_ok"] = state.ethernetConnected ? 1 : 0;
  doc["network_wifi_ok"] = state.wifiConnected ? 1 : 0;
  doc["network_internet_ok"] = (lanHasInternet() || state.lteDataUp) ? 1 : 0;
}

void publishDeviceStatus(const char* status, bool retained = true) {
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

void deferDeviceEvent(const char* type, const char* severity, const String& message, const String& commandId) {
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

bool publishDeviceEvent(const char* type, const char* severity, const String& message, const String& commandId = "") {
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

void publishInboundSms(const String& from, const String& text) {
  JsonDocument doc;
  doc["device_id"] = state.mqttDeviceId;
  doc["firmware"] = COF_FIRMWARE_VERSION;
  doc["type"] = "inbound_sms";
  doc["severity"] = "info";
  doc["from"] = from;
  doc["text"] = text;
  String message = from + ": " + text;
  if (message.length() > 200) {
    message = message.substring(0, 200);
  }
  doc["message"] = message;
  publishMqttJson("event", doc, false, 1);
}

String withFirmware(const String& message) {
  return message + " [" + COF_FIRMWARE_VERSION + "]";
}

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

void publishTestCallResult(const String& result, bool ok, const String& commandId = "") {
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
  if (publishMqttJson("event", doc, false, 1)) {
    pendingLteTracePublish = false;
    lastLteDataEventMs = now == 0 ? 1 : now;
    return;
  }
  if (lteTraceLog.length() <= 900) {
    return;
  }
  doc["modem_log"] = lteTraceLog.substring(lteTraceLog.length() - 900);
  if (publishMqttJson("event", doc, false, 1)) {
    pendingLteTracePublish = false;
    lastLteDataEventMs = now == 0 ? 1 : now;
  }
}

void finishLteAttempt(bool ok, const String& message) {
  reportLteProgress = false;
  lastLteFail = !ok;
  lteTraceLog = modemCallLog;
  pendingLteTraceMessage = message;
  pendingLteTraceOk = ok;
  pendingLteTracePublish = true;
  setStatus(message);
  Serial.println("[lte] " + message);
  publishLteDataTrace();
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
  if (phone.length() < 9 || phone.length() > 16 || !phone.startsWith("+")) {
    return false;
  }
  for (unsigned i = 1; i < phone.length(); i++) {
    if (phone[i] < '0' || phone[i] > '9') {
      return false;
    }
  }
  return true;
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

  if (mqttClient.connected()) {
    if (!mqttClient.loop()) {
      state.mqttConnected = false;
      if (state.ethernetConnected && !state.lteMqttTransport) {
        markEthernetDown("mqtt loop");
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
      // A failed connect over the cellular path must not wedge the PDP: back the
      // retry off and tear the PDP down once it looks hopeless so the next
      // attempt rebuilds it from scratch.
      lteMqttConnectFails++;
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

  if (!state.lteMqttTransport) {
    cachedMqttIp = mqttUsesTls() ? mqttTlsClient.remoteIP() : mqttPlainClient.remoteIP();
    Serial.printf("[mqtt] path ip=%s\n", cachedMqttIp.toString().c_str());
    if (state.ethernetConnected) {
      ethInternetUp = true;
    } else if (state.wifiConnected) {
      wifiInternetUp = true;
    }
  } else {
    lteTraceLog = modemCallLog;
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

bool httpGetString(const String& url, String& out, uint32_t timeoutMs = 15000) {
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
    const char c = static_cast<char>(ModemSerial.read());
    if (state.callInProgress) {
      pendingCallUrcs += c;
    } else {
      pendingModemUrcs += c;
      if (pendingModemUrcs.length() > 2048) {
        pendingModemUrcs.remove(0, pendingModemUrcs.length() - 1024);
      }
    }
  }
}

String takePendingCallUrcs() {
  const String out = pendingCallUrcs;
  pendingCallUrcs = "";
  return out;
}

String readModemUntil(uint32_t timeoutMs, const String& token) {
  String response;
  const uint32_t startedAt = millis();
  while (millis() - startedAt < timeoutMs) {
    feedWatchdog();
    // Pump MQTT on every path. `if (!loop())` below means "PINGRESP missing or
    // socket dead", not "no data available" (loop() returns true when idle), so
    // the return value is the liveness signal we actually want.
    if (state.mqttConnected) {
      if (mqttClient.loop()) {
        lastMqttOkMs = millis();
      } else {
        state.mqttConnected = false;
      }
    }
    while (ModemSerial.available()) {
      const char c = static_cast<char>(ModemSerial.read());
      response += c;
      if (token.length() == 0) {
        continue;
      }
      const int tagAt = response.indexOf(token);
      if (tagAt < 0) {
        continue;
      }
      if (!token.endsWith(":")) {
        return response;
      }
      for (int i = tagAt + token.length(); i < response.length(); i++) {
        if (response[i] == '\n' || response[i] == '\r') {
          return response;
        }
      }
    }
    delay(10);
  }
  return response;
}

bool sendAT(const String& command, const String& expected, uint32_t timeoutMs, String* responseOut) {
  flushModemInput();
  Serial.println("[modem] >> " + command);
  appendModemLog('>', command);
  ModemSerial.print(command);
  ModemSerial.print("\r\n");
  String response = readModemUntil(timeoutMs, expected);
  response.trim();
  Serial.println("[modem] << " + response);
  appendModemLog('<', response);

  if (responseOut != nullptr) {
    *responseOut = response;
  }
  return expected.length() == 0 || response.indexOf(expected) >= 0;
}

String lastQuoted(const String& text) {
  const int last = text.lastIndexOf('"');
  const int prev = last > 0 ? text.lastIndexOf('"', last - 1) : -1;
  if (prev < 0 || last <= prev + 1) {
    return "";
  }
  return text.substring(prev + 1, last);
}

String resolveLteMqttPeer(const char* host) {
  if (cachedMqttIp != IPAddress((uint32_t)0)) {
    return cachedMqttIp.toString();
  }
  String resp;
  if (sendAT(String("AT+CDNSGIP=\"") + host + "\"", "+CDNSGIP:", 10000, &resp)) {
    const String ip = lastQuoted(resp);
    if (ip.length() >= 7 && ip.indexOf('.') > 0 && ip != "0.0.0.0") {
      Serial.println("[lte] DNS " + ip);
      return ip;
    }
  }
  return String(host);
}

int atUrcCode(const String& resp, const char* tag) {
  const int tagAt = resp.indexOf(tag);
  if (tagAt < 0) {
    return -1;
  }
  const int comma = resp.indexOf(',', tagAt);
  if (comma >= 0) {
    return resp.substring(comma + 1).toInt();
  }
  return resp.substring(tagAt + static_cast<int>(strlen(tag))).toInt();
}

class LteMqttClient : public Client {
 public:
  uint8_t sock = 0;
  bool sockOpen = false;
  uint8_t rxBuf[512];
  int rxLen = 0;
  int rxPos = 0;
  bool dataInd = false;

  void drainRx() {
    rxLen = 0;
    rxPos = 0;
    dataInd = false;
  }

  uint32_t lastRxPollMs = 0;

  bool useNetopen() const {
    return lteIpStack != kLteStackCnact;
  }

  void noteUrc(const String& line) {
    if (line.startsWith("+CADATAIND:") || line.startsWith("+CARECV:") ||
        line.startsWith("+CIPRXGET: 1")) {
      dataInd = true;
    } else if ((line.startsWith("+CASTATE:") && line.indexOf(",0") > 0) ||
               line.startsWith("+IPCLOSE:") ||
               line.startsWith("+CIPEVENT:")) {
      sockOpen = false;
    }
  }

  void pumpUrcs() {
    while (ModemSerial.available()) {
      String line;
      const uint32_t start = millis();
      while (millis() - start < 50) {
        if (!ModemSerial.available()) {
          delay(1);
          continue;
        }
        const char c = static_cast<char>(ModemSerial.read());
        if (c == '\n') {
          break;
        }
        if (c != '\r') {
          line += c;
        }
      }
      line.trim();
      if (line.length() == 0) {
        continue;
      }
      noteUrc(line);
      if (!line.startsWith("+CIPRXGET: 1") && !line.startsWith("+CADATAIND:") &&
          !line.startsWith("+CARECV:")) {
        pendingModemUrcs += line + "\n";
      }
    }
    if (pendingModemUrcs.indexOf("+CIPRXGET: 1") >= 0 ||
        pendingModemUrcs.indexOf("+CADATAIND:") >= 0) {
      dataInd = true;
    }
    if (pendingModemUrcs.indexOf("+IPCLOSE:") >= 0 ||
        pendingModemUrcs.indexOf("+CIPEVENT:") >= 0) {
      sockOpen = false;
    }
  }

  bool recvChunk() {
    if (rxPos < rxLen) {
      return true;
    }
    rxLen = 0;
    rxPos = 0;
    const bool netopen = useNetopen();
    if (netopen) {
      ModemSerial.print("AT+CIPRXGET=2,");
      ModemSerial.print(sock);
      ModemSerial.print(",512\r\n");
    } else {
      ModemSerial.print("AT+CARECV=");
      ModemSerial.print(sock);
      ModemSerial.print(",512\r\n");
    }
    String header;
    const uint32_t startedAt = millis();
    const char* tag = netopen ? "+CIPRXGET: 2," : "+CARECV:";
    while (millis() - startedAt < 3000) {
      feedWatchdog();
      while (ModemSerial.available()) {
        const char c = static_cast<char>(ModemSerial.read());
        header += c;
        const int tagAt = header.indexOf(tag);
        const int nl = tagAt >= 0 ? header.indexOf('\n', tagAt) : -1;
        if (tagAt >= 0 && nl > tagAt) {
          const String line = header.substring(tagAt, nl);
          int n = 0;
          if (netopen) {
            const int c1 = line.indexOf(',');
            const int c2 = c1 >= 0 ? line.indexOf(',', c1 + 1) : -1;
            const int c3 = c2 >= 0 ? line.indexOf(',', c2 + 1) : -1;
            if (c2 >= 0 && c3 > c2) {
              n = line.substring(c2 + 1, c3).toInt();
            } else if (c2 >= 0) {
              n = line.substring(c2 + 1).toInt();
            }
          } else {
            const int comma = line.indexOf(',');
            if (comma >= 0) {
              n = line.substring(comma + 1).toInt();
            }
          }
          if (n <= 0) {
            dataInd = false;
            return false;
          }
          if (n > static_cast<int>(sizeof(rxBuf))) {
            n = sizeof(rxBuf);
          }
          int got = 0;
          while (got < n && millis() - startedAt < 3000) {
            feedWatchdog();
            if (ModemSerial.available()) {
              rxBuf[got++] = static_cast<uint8_t>(ModemSerial.read());
            }
          }
          rxLen = got;
          rxPos = 0;
          dataInd = false;
          return rxLen > 0;
        }
        if (header.indexOf("ERROR") >= 0) {
          dataInd = false;
          return false;
        }
      }
      delay(5);
    }
    dataInd = false;
    return false;
  }

  int connect(IPAddress ip, uint16_t port) override {
    return connect(ip.toString().c_str(), port);
  }

  int connect(const char* host, uint16_t port) override {
    stop();
    if (!state.lteDataUp) {
      return 0;
    }
    String resp;
    if (useNetopen()) {
      sendAT(String("AT+CIPCLOSE=") + String(sock), "OK", 5000);
      sendAT("AT+CIPRXGET=1", "OK", 3000);
      const String peer = resolveLteMqttPeer(host);
      String cmd = String("AT+CIPOPEN=") + String(sock) + ",\"TCP\",\"" + peer + "\"," + String(port);
      if (!sendAT(cmd, "+CIPOPEN:", 25000, &resp)) {
        Serial.println("[lte] CIPOPEN fail");
        sockOpen = false;
        return 0;
      }
      const int err = atUrcCode(resp, "+CIPOPEN:");
      if (err != 0) {
        Serial.printf("[lte] CIPOPEN err %d %s\n", err, resp.c_str());
        sendAT(String("AT+CIPCLOSE=") + String(sock), "OK", 5000);
        sockOpen = false;
        return 0;
      }
      Serial.printf("[lte] TCP %s:%u via %s NETOPEN\n", host, port, peer.c_str());
    } else {
      const char* proto = mqttUsesTls() ? "SSL" : "TCP";
      if (mqttUsesTls()) {
        sendAT("AT+CASSLCFG=0,\"ssl\",1", "OK", 3000);
      }
      String cmd = String("AT+CAOPEN=0,") + String(sock) + ",\"" + proto + "\",\"" + host + "\"," +
                   String(port);
      if (!sendAT(cmd, "+CAOPEN:", 25000, &resp) || atUrcCode(resp, "+CAOPEN:") != 0) {
        Serial.printf("[lte] CAOPEN err %s\n", resp.c_str());
        sockOpen = false;
        return 0;
      }
      Serial.printf("[lte] TCP %s:%u %s CNACT\n", host, port, proto);
    }
    drainRx();
    sockOpen = true;
    lastRxPollMs = millis();
    return 1;
  }

  size_t write(uint8_t b) override {
    return write(&b, 1);
  }

  size_t write(const uint8_t* buf, size_t size) override {
    if (!sockOpen || buf == nullptr || size == 0) {
      return 0;
    }
    size_t sent = 0;
    while (sent < size) {
      size_t chunk = size - sent;
      if (chunk > 1024) {
        chunk = 1024;
      }
      if (useNetopen()) {
        ModemSerial.print("AT+CIPSEND=");
      } else {
        ModemSerial.print("AT+CASEND=");
      }
      ModemSerial.print(sock);
      ModemSerial.print(",");
      ModemSerial.print(static_cast<unsigned>(chunk));
      ModemSerial.print("\r\n");
      if (!modemWaitForPrompt(5000)) {
        sockOpen = false;
        return sent;
      }
      ModemSerial.write(buf + sent, chunk);
      const String token = useNetopen() ? "+CIPSEND:" : "OK";
      const String resp = readModemUntil(15000, token);
      if (resp.indexOf("ERROR") >= 0 || (useNetopen() && resp.indexOf("+CIPSEND:") < 0) ||
          (!useNetopen() && resp.indexOf("OK") < 0)) {
        sockOpen = false;
        return sent;
      }
      sent += chunk;
    }
    return sent;
  }

  int available() override {
    if (!sockOpen) {
      return 0;
    }
    if (rxPos < rxLen) {
      return rxLen - rxPos;
    }
    pumpUrcs();
    if (!dataInd && useNetopen() && millis() - lastRxPollMs >= 250) {
      lastRxPollMs = millis();
      dataInd = true;
    }
    if (dataInd) {
      recvChunk();
    }
    return rxLen - rxPos;
  }

  int read() override {
    if (available() <= 0) {
      return -1;
    }
    return rxBuf[rxPos++];
  }

  int read(uint8_t* buf, size_t size) override {
    if (buf == nullptr || size == 0) {
      return 0;
    }
    int n = 0;
    while (n < static_cast<int>(size) && available() > 0) {
      buf[n++] = rxBuf[rxPos++];
    }
    return n;
  }

  int peek() override {
    if (available() <= 0) {
      return -1;
    }
    return rxBuf[rxPos];
  }

  void flush() override {}

  void stop() override {
    if (sockOpen) {
      if (useNetopen()) {
        sendAT(String("AT+CIPCLOSE=") + String(sock), "OK", 8000);
      } else {
        sendAT(String("AT+CACLOSE=") + String(sock), "OK", 5000);
      }
    }
    sockOpen = false;
    drainRx();
  }

  uint8_t connected() override {
    return sockOpen ? 1 : 0;
  }

  operator bool() {
    return connected();
  }
};

LteMqttClient lteMqttClient;

bool looksLikeIp(const String& ip) {
  return ip.length() >= 7 && ip != "0.0.0.0" && ip.indexOf('.') > 0;
}

bool parseLteIp(const String& resp, String& ipOut) {
  const char* tags[] = {"+IPADDR:", "+CNACT:", "+CGPADDR:"};
  for (uint8_t i = 0; i < 3; i++) {
    const int idx = resp.indexOf(tags[i]);
    if (idx < 0) {
      continue;
    }
    const int q1 = resp.indexOf('"', idx);
    const int q2 = q1 >= 0 ? resp.indexOf('"', q1 + 1) : -1;
    if (q1 >= 0 && q2 > q1 + 1) {
      ipOut = resp.substring(q1 + 1, q2);
    } else {
      int start = idx + static_cast<int>(strlen(tags[i]));
      while (start < static_cast<int>(resp.length()) &&
             (resp[start] == ' ' || resp[start] == ':')) {
        start++;
      }
      int end = start;
      while (end < static_cast<int>(resp.length()) && resp[end] != '\r' && resp[end] != '\n' &&
             resp[end] != ',') {
        end++;
      }
      ipOut = resp.substring(start, end);
      ipOut.trim();
    }
    ipOut.replace("\"", "");
    if (looksLikeIp(ipOut)) {
      return true;
    }
  }
  return false;
}

void detectLteIpStack() {
  if (lteIpStack != kLteStackUnknown) {
    return;
  }
  sendAT("AT+CMEE=2", "OK", 2000);
  String resp;
  if (sendAT("AT+NETOPEN?", "OK", 3000, &resp) && resp.indexOf("+NETOPEN:") >= 0) {
    lteIpStack = kLteStackNetopen;
    Serial.println("[lte] stack=NETOPEN");
    return;
  }
  if (sendAT("AT+CNACT=?", "OK", 3000)) {
    lteIpStack = kLteStackCnact;
    Serial.println("[lte] stack=CNACT");
    return;
  }
  lteIpStack = kLteStackNetopen;
  Serial.println("[lte] stack=NETOPEN (default)");
}

bool netopenIsActive() {
  String resp;
  sendAT("AT+NETOPEN?", "OK", 3000, &resp);
  return resp.indexOf("+NETOPEN: 1") >= 0;
}

bool queryLteIp(String& ipOut) {
  String resp;
  if (sendAT("AT+IPADDR", "OK", 4000, &resp) && parseLteIp(resp, ipOut)) {
    return true;
  }
  if (sendAT("AT+CGPADDR=1", "OK", 4000, &resp) && parseLteIp(resp, ipOut)) {
    return true;
  }
  if (sendAT("AT+CNACT?", "OK", 3000, &resp) && parseLteIp(resp, ipOut)) {
    return true;
  }
  return false;
}

bool netopenResultOk(const String& resp) {
  if (resp.indexOf("already opened") >= 0) {
    return true;
  }
  const int tag = resp.lastIndexOf("+NETOPEN:");
  if (tag < 0) {
    return false;
  }
  return resp.substring(tag + 9).toInt() == 0;
}

bool activateNetopenPdp(String& ipOut) {
  sendAT(String("AT+CGDCONT=1,\"IP\",\"") + COF_MODEM_APN + "\"", "OK", 5000);
  sendAT(String("AT+CGAUTH=1,1,\"") + COF_MODEM_APN_USER + "\",\"" + COF_MODEM_APN_PASS + "\"",
         "OK", 3000);
  sendAT("AT+CSOCKSETPN=1", "OK", 3000);
  sendAT("AT+CIPMODE=0", "OK", 2000);
  sendAT("AT+CIPTIMEOUT=30000,20000,15000", "OK", 3000);

  auto tryOpen = [&]() -> bool {
    if (netopenIsActive()) {
      return queryLteIp(ipOut);
    }
    String resp;
    sendAT("AT+NETOPEN", "+NETOPEN:", 25000, &resp);
    if (!netopenResultOk(resp) && !netopenIsActive()) {
      return false;
    }
    for (int attempt = 0; attempt < 6; attempt++) {
      feedWatchdog();
      if (queryLteIp(ipOut)) {
        return true;
      }
      waitWithWatchdog(1000);
    }
    return false;
  };

  if (tryOpen()) {
    return true;
  }

  sendAT("AT+NETCLOSE", "+NETCLOSE:", 12000);
  sendAT("AT+CGAUTH=1,0", "OK", 3000);
  return tryOpen();
}

bool activateCnactPdp(String& ipOut) {
  sendAT(String("AT+CNCFG=0,1,\"") + COF_MODEM_APN + "\"", "OK", 5000);
  sendAT("AT+CNACT=0,1", "OK", 20000);
  for (int attempt = 0; attempt < 8; attempt++) {
    feedWatchdog();
    String resp;
    sendAT("AT+CNACT?", "OK", 3000, &resp);
    if (parseLteIp(resp, ipOut) && (resp.indexOf("0,1") >= 0 || resp.indexOf(",1,\"") >= 0)) {
      return true;
    }
    waitWithWatchdog(1000);
  }
  sendAT("AT+CNACT=0,0", "OK", 5000);
  return false;
}

bool markLtePdpUp(const String& ip, uint8_t cid) {
  state.ltePdpCid = cid;
  state.lteDataUp = true;
  state.lteIpAddress = ip;
  lastLteRetryDelayMs = kLteRetryIntervalMs;
  lteMqttConnectFails = 0;
  pendingNetworkStatusReport = true;
  finishLteAttempt(true, "LTE IP " + ip);
  return true;
}

bool ensureLtePdp() {
  if (state.lteDataUp) {
    return true;
  }
  const uint32_t now = millis();
  if (lastLteAttemptMs != 0 && now - lastLteAttemptMs < lastLteRetryDelayMs) {
    return false;
  }
  lastLteAttemptMs = now == 0 ? 1 : now;
  if (!state.modemReady) {
    initModem();
    if (!state.modemReady) {
      finishLteAttempt(false, "LTE no AT");
      return false;
    }
  }
  if (!state.simReady && !refreshSimReady()) {
    return false;
  }

  reportLteProgress = true;
  modemCallLog = "";
  setStatus("LTE data");
  refreshCellularStatus();
  if (!state.networkRegistered) {
    finishLteAttempt(false, "LTE not registered");
    // Exponential backoff: without this a modem that is simply out of coverage
    // retried every 10 s forever, emitting a full modem dump each time.
    lastLteRetryDelayMs = std::min<uint32_t>(lastLteRetryDelayMs * 2U, kLteRetryMaxIntervalMs);
    return false;
  }

  sendAT("AT+CMEE=2", "OK", 2000);
  sendAT("AT+CGATT=1", "OK", 15000);
  detectLteIpStack();

  String ip;
  if (lteIpStack != kLteStackCnact && activateNetopenPdp(ip)) {
    return markLtePdpUp(ip, 1);
  }
  if (activateCnactPdp(ip)) {
    lteIpStack = kLteStackCnact;
    return markLtePdpUp(ip, 0);
  }
  if (lteIpStack == kLteStackCnact && activateNetopenPdp(ip)) {
    lteIpStack = kLteStackNetopen;
    return markLtePdpUp(ip, 1);
  }

  finishLteAttempt(false, "LTE PDP fail");
  lastLteRetryDelayMs = std::min<uint32_t>(lastLteRetryDelayMs * 2U, kLteRetryMaxIntervalMs);
  return false;
}

void stopLtePdp() {
  lteMqttClient.stop();
  if (state.lteDataUp || lteIpStack == kLteStackNetopen) {
    sendAT("AT+CIPCLOSE=0", "OK", 5000);
    sendAT("AT+NETCLOSE", "+NETCLOSE:", 12000);
  }
  if (state.lteDataUp || lteIpStack == kLteStackCnact) {
    sendAT(String("AT+CNACT=") + String(state.ltePdpCid) + ",0", "OK", 8000);
  }
  state.lteDataUp = false;
  state.lteMqttTransport = false;
  state.lteIpAddress = "-";
  lastLteAttemptMs = 0;
  lastLteRetryDelayMs = kLteRetryIntervalMs;
  lteMqttConnectFails = 0;
  pendingNetworkStatusReport = true;
}

void releaseLteMqttForModem() {
  if (!state.lteMqttTransport) {
    return;
  }
  mqttClient.disconnect();
  state.mqttConnected = false;
  lteMqttClient.stop();
  lastMqttReconnectMs = 0;
}

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

void configureMqttClientTransport() {
  // Prefer LAN whenever it is genuinely usable: Ethernet first (by
  // applyPreferredRoute), then WiFi, and only then LTE. `ltePreemptSinceMs` is
  // deliberately not part of this test: serviceNetworkPaths() only arms it after
  // a probe has already set the interface's health flag, so adding it here would
  // only create a window where a dead LAN can reclaim MQTT.
  const bool lanLooksUsable = lanConnected() && !ethernetHoldoffActive() && lanHasInternet();
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
  mqttClient.setServer(state.mqttHost.c_str(), state.mqttPort);
  mqttClient.setCallback(onMqttMessage);
  mqttClient.setBufferSize(4096);
  mqttClient.setKeepAlive(kMqttKeepAliveSeconds);
  mqttPlainClient.setTimeout(kMqttSocketTimeoutSeconds * 1000);
  mqttTlsClient.setTimeout(kMqttSocketTimeoutSeconds * 1000);
}

String stopPlaybackAndCollect() {
  String resp;
  sendAT("AT+CCMXSTOP", "OK", 2000, &resp);
  return takePendingCallUrcs() + resp;
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

  char line[24];
  display.clearBuffer();
  display.setFont(u8g2_font_5x8_tf);

  display.drawStr(0, 8, "CallOnFail");
  snprintf(line, sizeof(line), "%s", COF_FIRMWARE_VERSION);
  display.drawStr(92, 8, line);

  if (state.ethernetConnected) {
    snprintf(line, sizeof(line), "E %s", state.ipAddress.c_str());
  } else {
    snprintf(line, sizeof(line), "E --");
  }
  display.drawStr(0, 16, line);

  if (state.wifiConnected) {
    snprintf(line, sizeof(line), "W %s", state.wifiIpAddress.c_str());
  } else if (state.wifiConfigured) {
    String ssid = state.wifiSsid;
    if (ssid.length() > 12) {
      ssid = ssid.substring(0, 12);
    }
    if (wifiAuthFailCount >= kWifiAuthFailLimit) {
      snprintf(line, sizeof(line), "W auth %s", ssid.c_str());
    } else {
      snprintf(line, sizeof(line), "W ... %s", ssid.c_str());
    }
  } else {
    snprintf(line, sizeof(line), "W --");
  }
  display.drawStr(0, 24, line);

  if (state.lteDataUp) {
    snprintf(line, sizeof(line), "L %s", state.lteIpAddress.c_str());
  } else if (!state.modemReady) {
    snprintf(line, sizeof(line), "L no AT");
  } else if (!state.simReady) {
    snprintf(line, sizeof(line), millis() < 30000 ? "L SIM..." : "L no SIM");
  } else if (lanConnected()) {
    snprintf(line, sizeof(line), "L --");
  } else if (reportLteProgress) {
    snprintf(line, sizeof(line), "L try");
  } else if (lastLteFail) {
    snprintf(line, sizeof(line), "L fail");
  } else {
    snprintf(line, sizeof(line), "L ...");
  }
  display.drawStr(0, 32, line);

  if (state.sht31Ready && !isnan(state.shtTemperature) && !isnan(state.shtHumidity)) {
    snprintf(line, sizeof(line), "SHT %.1fC %.0f%%", state.shtTemperature, state.shtHumidity);
  } else {
    snprintf(line, sizeof(line), "SHT --");
  }
  display.drawStr(0, 40, line);

  if (state.ds18b20Ready && !isnan(state.dsTemperature)) {
    snprintf(line, sizeof(line), "DS18 %.1fC", state.dsTemperature);
  } else {
    snprintf(line, sizeof(line), "DS18 --");
  }
  display.drawStr(0, 48, line);

  snprintf(line, sizeof(line), "ADC %d MQTT %s", state.zmptRaw, mqttPathLetter());
  display.drawStr(0, 56, line);

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
  sendAT("AT+CMGF=1", "OK", 3000);
  sendAT("AT+CNMI=2,1,0,0,0", "OK", 3000);
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

bool cpinResponseReady(const String& response) {
  const int idx = response.indexOf("+CPIN:");
  if (idx < 0) {
    return false;
  }
  return response.indexOf("NOT READY", idx) < 0 && response.indexOf("READY", idx) >= 0;
}

bool refreshSimReady() {
  if (!state.modemReady) {
    return false;
  }
  String response;
  if (!sendAT("AT+CPIN?", "OK", 3000, &response)) {
    return state.simReady;
  }
  if (!cpinResponseReady(response)) {
    return false;
  }
  if (!state.simReady) {
    state.simReady = true;
    configureCellularApn();
  }
  return true;
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

  state.simReady = false;
  for (int attempt = 0; attempt < 12; attempt++) {
    feedWatchdog();
    if (sendAT("AT+CPIN?", "OK", 2000, &response) && cpinResponseReady(response)) {
      state.simReady = true;
      break;
    }
    waitWithWatchdog(500);
  }

  state.modemAudioPlaybackSupported = sendAT("AT+CCMXPLAY=?", "OK", 3000);
  state.modemFileTransferSupported = sendAT("AT+CFTRANRX=?", "OK", 3000);

  if (state.simReady) {
    configureCellularApn();
  } else {
    setStatus("Wait SIM");
  }
  return true;
}

String nthQuoted(const String& line, int want) {
  int seen = 0;
  int start = -1;
  for (int i = 0; i < line.length(); i++) {
    if (line[i] != '"') {
      continue;
    }
    if (start < 0) {
      start = i + 1;
    } else {
      seen++;
      if (seen == want) {
        return line.substring(start, i);
      }
      start = -1;
    }
  }
  return "";
}

void publishSmsRecords(const String& response, const char* tag) {
  int search = 0;
  while (search < static_cast<int>(response.length())) {
    const int pos = response.indexOf(tag, search);
    if (pos < 0) {
      break;
    }
    const int lineEnd = response.indexOf('\n', pos);
    if (lineEnd < 0) {
      break;
    }
    const String header = response.substring(pos, lineEnd);
    int next = response.indexOf(tag, lineEnd);
    int okAt = response.indexOf("\nOK", lineEnd);
    int bodyEnd = response.length();
    if (next >= 0 && next < bodyEnd) {
      bodyEnd = next;
    }
    if (okAt >= 0 && okAt < bodyEnd) {
      bodyEnd = okAt;
    }
    String body = response.substring(lineEnd + 1, bodyEnd);
    body.replace("\r", "");
    body.trim();
    const String from = nthQuoted(header, 2);
    if (from.length() > 0 || body.length() > 0) {
      Serial.println("[sms] inbound from " + from + " body=" + body);
      publishInboundSms(from, body);
    }
    if (strcmp(tag, "+CMGL:") == 0) {
      const int colon = header.indexOf(':');
      const int index = colon >= 0 ? header.substring(colon + 1).toInt() : -1;
      if (index >= 0) {
        sendAT("AT+CMGD=" + String(index), "OK", 3000);
      }
    }
    search = bodyEnd;
  }
}

void processPendingSmsUrcs() {
  flushModemInput();
  int guard = 0;
  while (pendingModemUrcs.indexOf("+CMTI:") >= 0 && guard++ < 8) {
    const int idx = pendingModemUrcs.indexOf("+CMTI:");
    const int comma = pendingModemUrcs.indexOf(',', idx);
    const int end = pendingModemUrcs.indexOf('\n', idx);
    const int index = comma >= 0 ? pendingModemUrcs.substring(comma + 1).toInt() : -1;
    if (end >= 0) {
      pendingModemUrcs.remove(idx, end - idx + 1);
    } else {
      pendingModemUrcs = "";
    }
    if (index < 0) {
      continue;
    }
    String response;
    if (sendAT("AT+CMGR=" + String(index), "OK", 8000, &response)) {
      publishSmsRecords(response, "+CMGR:");
      sendAT("AT+CMGD=" + String(index), "OK", 3000);
    }
  }
}

void pollIncomingSms() {
  // Skipped while MQTT rides the cellular socket: interleaving AT housekeeping
  // with an open AT+CIPOPEN/CAOPEN session can corrupt the modem socket, so the
  // caller keeps the !lteMqttTransport guard here.
  if (!state.modemReady || !state.simReady || state.callInProgress || !state.mqttConnected) {
    return;
  }
  if (pendingTestSmsCommand || pendingTestCallCommand) {
    return;
  }
  processPendingSmsUrcs();
  String response;
  if (sendAT("AT+CMGL=\"REC UNREAD\"", "OK", 8000, &response)) {
    publishSmsRecords(response, "+CMGL:");
  }
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
}

String ttsModemPathFor(const String& url, const String& format) {
  String fmt = format;
  fmt.toLowerCase();
  String path = url;
  path.toLowerCase();
  if (fmt.indexOf("amr") >= 0 || path.endsWith(".amr")) {
    return "C:/tts.amr";
  }
  return "C:/tts.wav";
}

String uploadAudioToModem(const String& url, const String& modemPath, const String& audioVersion) {
  if (!networkConnected()) {
    return "TTS no network";
  }
  if (!state.modemReady || !state.modemFileTransferSupported) {
    return "TTS modem no FS";
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(20);
  HTTPClient http;
  http.setTimeout(30000);
  http.useHTTP10(true);
  http.setReuse(false);

  if (!http.begin(client, url)) {
    return "TTS HTTP begin fail";
  }
  http.addHeader("Accept-Encoding", "identity");

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[audio] download failed: %d\n", code);
    http.end();
    return "TTS HTTP " + String(code);
  }

  int size = http.getSize();
  constexpr int kMaxAudioBytes = 180000;
  if (size > kMaxAudioBytes) {
    http.end();
    return "TTS too large";
  }

  uint8_t* blob = nullptr;
  int collected = 0;
  WiFiClient* stream = http.getStreamPtr();
  if (size > 0) {
    blob = static_cast<uint8_t*>(malloc(static_cast<size_t>(size)));
    if (blob == nullptr) {
      http.end();
      return "TTS no RAM";
    }
    while (collected < size) {
      feedWatchdog();
      const int n = stream->readBytes(blob + collected, size - collected);
      if (n <= 0) {
        free(blob);
        http.end();
        return "TTS HTTP read fail";
      }
      collected += n;
    }
  } else {
    blob = static_cast<uint8_t*>(malloc(kMaxAudioBytes));
    if (blob == nullptr) {
      http.end();
      return "TTS no RAM";
    }
    const uint32_t startedAt = millis();
    while (http.connected() && collected < kMaxAudioBytes && millis() - startedAt < 30000) {
      feedWatchdog();
      const int avail = stream->available();
      if (avail <= 0) {
        delay(10);
        continue;
      }
      const int n = stream->readBytes(blob + collected, min(avail, kMaxAudioBytes - collected));
      if (n <= 0) {
        break;
      }
      collected += n;
    }
    size = collected;
  }
  http.end();

  const int minBytes = modemPath.endsWith(".amr") || modemPath.endsWith(".AMR") ? 12 : 44;
  if (size < minBytes) {
    free(blob);
    return "TTS empty file";
  }

  state.audioSyncInProgress = true;
  setStatus("Audio to modem");

  String fileName = modemPath;
  const int slash = fileName.lastIndexOf('/');
  if (slash >= 0) {
    fileName = fileName.substring(slash + 1);
  }
  sendAT("AT+FSCD=C:", "OK", 3000);
  sendAT("AT+FSDEL=" + fileName, "OK", 3000);
  flushModemInput();
  ModemSerial.printf("AT+CFTRANRX=\"%s\",%d\r\n", modemPath.c_str(), size);
  if (!modemWaitForPrompt(10000)) {
    free(blob);
    state.audioSyncInProgress = false;
    setStatus("Audio prompt fail");
    return "TTS modem prompt fail";
  }

  int remaining = size;
  const uint8_t* cursor = blob;
  while (remaining > 0) {
    feedWatchdog();
    const int chunk = min(512, remaining);
    ModemSerial.write(cursor, chunk);
    cursor += chunk;
    remaining -= chunk;
    delay(1);
  }
  free(blob);

  const String response = readModemUntil(20000, "OK");
  state.audioSyncInProgress = false;
  if (response.indexOf("OK") >= 0) {
    preferences.putString("audioVersion", audioVersion);
    setStatus("Audio synced");
    return "";
  }

  setStatus("Audio upload fail");
  return "TTS modem upload fail";
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

int parseClccStatAt(const String& response, int tag) {
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

int parseClccStat(const String& response) {
  return parseClccStatAt(response, response.indexOf("+CLCC:"));
}

int lastClccStat(const String& response) {
  int last = -1;
  int from = 0;
  while (from >= 0) {
    const int tag = response.indexOf("+CLCC:", from);
    if (tag < 0) {
      break;
    }
    last = parseClccStatAt(response, tag);
    from = tag + 6;
  }
  return last;
}

int queryClccStat() {
  String clcc;
  if (!sendAT("AT+CLCC", "OK", 1500, &clcc)) {
    return -1;
  }
  return parseClccStat(clcc);
}

String compactAtText(const String& raw) {
  String compact = raw;
  compact.toUpperCase();
  compact.replace(" ", "");
  compact.replace("\r", "");
  compact.replace("\n", "");
  return compact;
}

void publishCallModemSignal(const String& raw) {
  String upper = raw;
  upper.toUpperCase();
  const char* keys[] = {
      "BUSY", "NO CARRIER", "NO ANSWER", "NO DIALTONE",
      "VOICE CALL", "+CLCC:", "+COLP:"};
  int hit = -1;
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
    const int at = upper.indexOf(keys[i]);
    if (at >= 0 && (hit < 0 || at < hit)) {
      hit = at;
    }
  }
  if (hit < 0) {
    return;
  }
  String line = raw.substring(hit);
  const int nl = line.indexOf('\n');
  if (nl >= 0) {
    line = line.substring(0, nl);
  }
  line.replace("\r", "");
  line.trim();
  if (line.length() > 80) {
    line = line.substring(0, 80);
  }
  if (line.length() > 0) {
    publishTestCallProgress(String("Modem: ") + line);
  }
}

String classifyCallUrc(const String& raw) {
  String urc = raw;
  urc.toUpperCase();
  const String compact = compactAtText(raw);
  if (urc.indexOf("BUSY") >= 0) {
    return "Call rejected";
  }
  if (urc.indexOf("NO DIALTONE") >= 0) {
    return "Call no dialtone";
  }
  if (urc.indexOf("NO ANSWER") >= 0) {
    return "Call no answer";
  }
  if (urc.indexOf("NO CARRIER") >= 0 || compact.indexOf("VOICECALL:END") >= 0) {
    return "Call no carrier";
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

int parseCeerCode(const String& ceer) {
  for (int i = 0; i < ceer.length(); i++) {
    if (isDigit(ceer[i])) {
      return ceer.substring(i).toInt();
    }
  }
  return -1;
}

// 3GPP TS 24.008 / SIMCom CEER: classify by who released the call and why.
// Duration of ring/audio is not a call-state signal and must not decide the result.
enum CallEndSource {
  kCallEndRemote,
  kCallEndAudioDone,
  kCallEndTimeout
};

bool ceerLooksRejected(const String& ceer) {
  String upper = ceer;
  upper.toUpperCase();
  if (upper.indexOf("NO SERVICE") >= 0) {
    return false;
  }
  const int code = parseCeerCode(ceer);
  return code == 17 || code == 21 || code == 22 ||
         upper.indexOf("BUSY") >= 0 || upper.indexOf("REJECT") >= 0 ||
         upper.indexOf("USER BUSY") >= 0;
}

bool ceerLooksNoAnswer(const String& ceer) {
  String upper = ceer;
  upper.toUpperCase();
  const int code = parseCeerCode(ceer);
  return code == 18 || code == 19 ||
         upper.indexOf("NO ANSWER") >= 0 || upper.indexOf("NO USER") >= 0;
}

String classifyHangup(const String& ceer, bool sawAlerting, CallEndSource source) {
  if (source == kCallEndAudioDone) {
    return "Call done";
  }
  if (source == kCallEndTimeout) {
    return sawAlerting ? "Call no answer" : "Call not connected";
  }
  if (ceerLooksRejected(ceer)) {
    return "Call rejected";
  }
  if (!sawAlerting) {
    return "Call no carrier";
  }
  if (ceerLooksNoAnswer(ceer)) {
    return "Call no answer";
  }
  // Remote DISCONNECT after alerting. Cause 16/31 means a user requested
  // clearing (answered then hung up, or declined as normal clearing).
  // That is not "no answer": an unanswered phone does not send DISCONNECT.
  return "Call done";
}

int queryCpas() {
  String response;
  if (!sendAT("AT+CPAS", "OK", 3000, &response)) {
    return -1;
  }
  return extractAtTagValue(response, "+CPAS:").toInt();
}

bool callIsIdle() {
  const int cpas = queryCpas();
  if (cpas == 3 || cpas == 4) {
    sendAT("ATH", "OK", 3000);
    sendAT("AT+CHUP", "OK", 3000);
    return false;
  }
  return cpas == 0;
}

bool radioIsOnline() {
  return state.radioInfo.indexOf("Online") >= 0 ||
         state.radioInfo.indexOf("ONLINE") >= 0;
}

bool smsStackReady() {
  String response;
  if (!sendAT("AT+CMGF=1", "OK", 3000)) {
    return false;
  }
  if (!sendAT("AT+CPMS?", "OK", 3000, &response)) {
    return false;
  }
  return response.indexOf("+CPMS:") >= 0;
}

bool isCallReady() {
  if (!callIsIdle()) {
    return false;
  }
  refreshCellularStatus();
  return csAttached() &&
         radioHasService() &&
         radioIsOnline() &&
         state.signalQuality >= 1 &&
         state.signalQuality != 99;
}

bool isSmsReady() {
  if (!callIsIdle()) {
    return false;
  }
  refreshCellularStatus();
  return state.networkRegistered && radioHasService() && smsStackReady();
}

bool waitUntilModemReady(bool forCall, uint32_t timeoutMs) {
  setStatus(forCall ? "Wait call ready" : "Wait SMS ready");
  const uint32_t startedAt = millis();
  while (millis() - startedAt < timeoutMs) {
    feedWatchdog();
    if (state.mqttConnected) {
      mqttClient.loop();
    }
    if (forCall ? isCallReady() : isSmsReady()) {
      waitWithWatchdog(1500);
      return true;
    }
    waitWithWatchdog(1500);
  }
  return forCall ? isCallReady() : isSmsReady();
}

String voiceContextSuffix(const String& bearer, const String& ceer = "");
void restoreAutoRadio();
String conductOutgoingCall(uint32_t timeoutMs, String* ceerOut);

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
  Serial.println("[call] RF off/on to attach CS before dial");
  sendAT("ATH", "OK", 3000);
  sendAT("AT+CHUP", "OK", 3000);
  sendAT("AT+CFUN=4", "OK", 8000);
  waitWithWatchdog(3000);
  sendAT("AT+CFUN=1", "OK", 15000);
  sendAT("AT+CNMP=2", "OK", 10000);
  sendAT("AT+CEMODE=1", "OK", 3000);
  sendAT("AT+CEVDP=3", "OK", 3000);
  state.forcedGsmForCall = false;
  persistSkipGsm(false);
  waitForRadioService(45000, false);
  waitUntilModemReady(true, 30000);
}

void lockGsmForCall() {
  setStatus("GSM lock");
  Serial.println("[call] lock GSM (CNMP=13) after prepared CSFB failed");
  sendAT("ATH", "OK", 3000);
  sendAT("AT+CHUP", "OK", 3000);
  sendAT("AT+CNMP=13", "OK", 10000);
  state.forcedGsmForCall = true;
  waitForRadioService(45000, true);
  waitUntilModemReady(true, 30000);
}

void restorePacketServices() {
  setStatus("Restore data");
  sendAT("ATH", "OK", 2000);
  sendAT("AT+CHUP", "OK", 2000);
  sendAT("AT+CNMP=2", "OK", 10000);
  waitForRadioService(25000, false);
  sendAT("AT+CGATT=1", "OK", 15000);
  sendAT("AT+CGSMS=1", "OK", 3000);
  sendAT("AT+CMGF=1", "OK", 3000);
  sendAT("AT+CSMP=17,167,0,0", "OK", 3000);
  sendAT("AT+CNMI=2,1,0,0,0", "OK", 3000);
  waitUntilModemReady(false, 20000);
}

bool shouldRetryVoice(const String& result) {
  return result.startsWith("Call failed") ||
         result.startsWith("Call no carrier") ||
         result.startsWith("Call not connected") ||
         result.startsWith("Call dial timeout") ||
         result.startsWith("Call not ready") ||
         result.startsWith("No voice radio");
}

String dialAndMaybePlay(const String& phone, const String& bearer) {
  if (!waitUntilModemReady(true, 25000)) {
    return "Call not ready" + voiceContextSuffix(bearer);
  }

  refreshRadioMode();
  state.radioAtDial = state.radioMode;
  state.radioAtConnect = "";

  sendAT("AT+CRC=1", "OK", 2000);
  sendAT("AT+CVHU=0", "OK", 2000);
  sendAT("AT+COLP=1", "OK", 2000);
  sendAT("AT+CLCC=1", "OK", 2000);
  sendAT("AT+CEMODE=1", "OK", 3000);
  sendAT("AT+CEVDP=3", "OK", 3000);

  setStatus("Calling");
  if (!sendAT("ATD" + phone + ";", "OK", 10000)) {
    const String ceer = queryCallFailCause();
    sendAT("ATH", "OK", 3000);
    return "Call failed" + voiceContextSuffix(bearer, ceer);
  }

  String hangupCeer;
  const String progress = conductOutgoingCall(120000, &hangupCeer);
  refreshRadioMode();
  state.radioAtConnect = state.radioMode;
  const String observed = observeVoicePath(state.radioAtDial, state.radioAtConnect);
  if (progress.startsWith("Call done")) {
    persistObservedVoicePath(observed);
    state.predictedVoicePath = observed;
  } else if (!progress.startsWith("Call rejected") &&
             !progress.startsWith("Call no answer") &&
             !progress.startsWith("Call no carrier") &&
             !progress.startsWith("Call no dialtone")) {
    sendAT("AT+CCMXSTOP", "OK", 2000);
    sendAT("ATH", "OK", 3000);
  }
  const String ceer = hangupCeer.length() > 0 ? hangupCeer : queryCallFailCause();
  return progress + voiceContextSuffix(observed, ceer);
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

String conductOutgoingCall(uint32_t timeoutMs, String* ceerOut) {
  bool sawDialing = false;
  bool sawAlerting = false;
  bool sawNoCarrier = false;
  bool playing = false;
  bool audioDone = false;
  bool reportedCsfbWait = false;
  const uint32_t startedAt = millis();
  uint32_t voicePathAt = 0;
  uint32_t audioDoneAt = 0;
  constexpr uint32_t kCsfbIgnoreMs = 40000;
  constexpr uint32_t kLeadInMs = 4000;
  constexpr uint32_t kTrailMs = 2000;

  auto storeCeer = [&](const String& ceer) {
    if (ceerOut != nullptr) {
      *ceerOut = ceer;
    }
  };

  while (millis() - startedAt < timeoutMs) {
    feedWatchdog();
    if (state.mqttConnected) {
      mqttClient.loop();
    }

    String urc = pendingCallUrcs;
    pendingCallUrcs = "";
    urc += readModemUntil(800, "");
    appendModemLog('<', urc);
    const String urcResult = classifyCallUrc(urc);
    const int clccStat = lastClccStat(urc);
    if (urcResult.length() > 0 || clccStat == 6) {
      publishCallModemSignal(urc);
    }
    if (compactAtText(urc).indexOf("VOICECALL:BEGIN") >= 0 && voicePathAt == 0) {
      voicePathAt = millis();
    }
    if (urc.indexOf("+AUDIOSTATE:") >= 0 && urc.indexOf("play stop") >= 0) {
      audioDone = true;
      if (audioDoneAt == 0) {
        audioDoneAt = millis();
        publishTestCallProgress("Audio finished");
      }
      setStatus("Audio done");
    }
    if (urc.indexOf("+CLCC:") >= 0) {
      int from = 0;
      while (from >= 0) {
        const int tag = urc.indexOf("+CLCC:", from);
        if (tag < 0) {
          break;
        }
        const int stat = parseClccStatAt(urc, tag);
        if (stat == 2) {
          sawDialing = true;
          setStatus("Dialing");
        } else if (stat == 3) {
          if (!sawAlerting) {
            publishTestCallProgress("Ringing");
          }
          sawAlerting = true;
          setStatus("Ringing");
        } else if (stat == 0 && voicePathAt == 0) {
          voicePathAt = millis();
        }
        from = tag + 6;
      }
    }

    if (urcResult.length() > 0 || clccStat == 6) {
      if (urcResult == "Call rejected" || urcResult == "Call no answer" ||
          urcResult == "Call no dialtone") {
        sendAT("AT+CCMXSTOP", "OK", 2000);
        const String ceer = queryCallFailCause();
        storeCeer(ceer);
        return urcResult;
      }
      sawNoCarrier = true;
      if (sawAlerting || millis() - startedAt >= kCsfbIgnoreMs) {
        sendAT("AT+CCMXSTOP", "OK", 2000);
        const String ceer = queryCallFailCause();
        storeCeer(ceer);
        publishTestCallProgress("Remote hangup");
        return classifyHangup(ceer, sawAlerting, kCallEndRemote);
      }
      setStatus("CSFB wait");
      if (!reportedCsfbWait) {
        reportedCsfbWait = true;
        publishTestCallProgress("CSFB in progress");
      }
    }

    if (voicePathAt > 0 && !playing && !audioDone &&
        state.modemAudioPath.length() > 0 &&
        millis() - voicePathAt >= kLeadInMs) {
      publishTestCallProgress("Playing audio");
      String playResp;
      sendAT("AT+CCMXPLAY=\"" + state.modemAudioPath + "\",1,0", "OK", 5000,
             &playResp);
      pendingCallUrcs = playResp + pendingCallUrcs;
      playing = true;
    }

    if (audioDone && audioDoneAt > 0 && millis() - audioDoneAt >= kTrailMs) {
      sendAT("ATH", "OK", 3000);
      const String ceer = queryCallFailCause();
      storeCeer(ceer);
      return classifyHangup(ceer, sawAlerting, kCallEndAudioDone);
    }
  }

  const String afterStop = stopPlaybackAndCollect();
  publishCallModemSignal(afterStop);
  const String stopResult = classifyCallUrc(afterStop);
  const int stopClcc = lastClccStat(afterStop);
  if (stopResult == "Call rejected" || stopResult == "Call no answer" ||
      stopResult == "Call no dialtone") {
    const String ceer = queryCallFailCause();
    storeCeer(ceer);
    return stopResult;
  }
  if (stopResult == "Call no carrier" || stopClcc == 6) {
    const String ceer = queryCallFailCause();
    storeCeer(ceer);
    publishTestCallProgress("Remote hangup");
    return classifyHangup(ceer, sawAlerting, kCallEndRemote);
  }

  String clccNow;
  sendAT("AT+CLCC", "OK", 1500, &clccNow);
  clccNow += takePendingCallUrcs();
  if (clccNow.indexOf("+CLCC:") >= 0) {
    publishCallModemSignal(clccNow);
    publishTestCallProgress("Timeout, call still up");
    sendAT("ATH", "OK", 3000);
    const String ceer = queryCallFailCause();
    storeCeer(ceer);
    if (sawAlerting) {
      return classifyHangup(ceer, true, kCallEndTimeout);
    }
  } else {
    publishTestCallProgress("Timeout, call already gone");
    const String ceer = queryCallFailCause();
    storeCeer(ceer);
    if (sawAlerting) {
      return classifyHangup(ceer, true, kCallEndRemote);
    }
  }
  if (sawDialing) {
    return "Call dial timeout";
  }
  if (sawNoCarrier) {
    return "Call no carrier";
  }
  return "Call not connected";
}

String placeCallAndPlayAudio(const String& phoneOverride = "", bool adminTest = false, const String& audioUrl = "", const String& audioFormat = "") {
  releaseLteMqttForModem();
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

  if (!state.modemReady) {
    initModem();
  }
  if (!state.simReady) {
    refreshSimReady();
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

  const String previousAudioPath = state.modemAudioPath;
  if (adminTest) {
    if (audioUrl.length() > 0) {
      publishTestCallProgress("Downloading TTS audio");
      const String ttsPath = ttsModemPathFor(audioUrl, audioFormat);
      const String audioErr = uploadAudioToModem(audioUrl, ttsPath, "tts");
      if (audioErr.length() > 0) {
        return audioErr;
      }
      state.modemAudioPath = ttsPath;
    } else {
      setStatus("Sync test audio");
      checkManifest(false);
    }
  }

  state.callInProgress = true;
  pendingCallUrcs = "";
  modemCallLog = "";
  refreshCellularStatus();
  bool preparedCs = false;
  if (!imsVoiceReady() && !radioIsGsm()) {
    publishTestCallProgress("Preparing CS radio");
    bounceRadioForCsfb();
    refreshCellularStatus();
    preparedCs = true;
  }

  String bearer = prepareVoiceBearer();
  if (preparedCs && !imsVoiceReady()) {
    bearer = radioIsGsm() ? "gsm after bounce" : "csfb prepared";
  }
  publishTestCallProgress("Dialing, waiting for voice");
  String result = dialAndMaybePlay(phone, bearer);
  if (shouldRetryVoice(result) && !state.skipGsmVoice) {
    publishTestCallProgress("Locking GSM");
    lockGsmForCall();
    refreshCellularStatus();
    bearer = "gsm lock";
    publishTestCallProgress("Retrying call");
    result = dialAndMaybePlay(phone, bearer);
  } else if (shouldRetryVoice(result) && !preparedCs) {
    publishTestCallProgress("Resetting radio");
    bounceRadioForCsfb();
    refreshCellularStatus();
    bearer = imsVoiceReady() ? "ims after bounce" : "csfb retry";
    publishTestCallProgress("Retrying call");
    result = dialAndMaybePlay(phone, bearer);
  }

  restoreAutoRadio();
  restorePacketServices();
  state.callInProgress = false;
  state.modemAudioPath = previousAudioPath;
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

// Pumps MQTT while waiting out a modem operation so a long AT exchange does not
// drop the broker connection. On LTE the MQTT keepalive is only 10 s while SMS
// commands can take up to 70 s, so without this the connection dies and the
// result event has nowhere to go.
void waitWithMqtt(uint32_t ms) {
  const uint32_t startedAt = millis();
  while (millis() - startedAt < ms) {
    feedWatchdog();
    if (!state.callInProgress && !state.otaInProgress && !state.audioSyncInProgress) {
      if (mqttClient.connected()) {
        state.mqttConnected = true;
        // loop() returns false only when the socket died or a PINGRESP never
        // arrived; a true return also means the keepalive was serviced.
        if (mqttClient.loop()) {
          lastMqttOkMs = millis();
        } else {
          state.mqttConnected = false;
        }
      }
    }
    delay(10);
  }
}

String transmitSms(const String& phone, const String& body) {
  if (!lanHasInternet()) {
    // MQTT is riding the modem. Do not tear the LTE socket down: on LTE the SMS
    // and the PDP coexist (AT+CGSMS=1 prefers the CS bearer), and killing MQTT
    // is what used to lose the command result. Only release the socket when the
    // command will actually collide with the IP stack, i.e. when it fails.
    mqttClient.loop();
    state.mqttConnected = mqttClient.connected();
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

String sendTestSms(const String& phoneOverride, const String& text) {
  if (!state.modemReady) {
    initModem();
  }
  if (!state.simReady) {
    const uint32_t startedAt = millis();
    while (!state.simReady && millis() - startedAt < 20000) {
      feedWatchdog();
      if (refreshSimReady()) {
        break;
      }
      waitWithMqtt(1000);
    }
  }
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

  if (!waitUntilModemReady(false, 25000)) {
    restorePacketServices();
    if (!waitUntilModemReady(false, 20000)) {
      setStatus("SMS not ready");
      return "SMS not ready";
    }
  }

  String result = transmitSms(phone, body);
  if (!result.startsWith("SMS sent")) {
    // First attempt failed: only now is it worth giving up the LTE MQTT socket,
    // restore the packet services and try once more.
    releaseLteMqttForModem();
    restorePacketServices();
    result = transmitSms(phone, body);
  }
  return result;
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
    const String callAudioUrl = pendingTestCallAudioUrl;
    const String callAudioFormat = pendingTestCallAudioFormat;
    const String callCommandId = pendingTestCallCommandId;
    pendingTestCallCommand = false;
    pendingTestCallPhone = "";
    pendingTestCallAudioUrl = "";
    pendingTestCallAudioFormat = "";
    pendingTestCallCommandId = "";
    reportTestCallProgress = true;
    const String result = placeCallAndPlayAudio(callPhone, true, callAudioUrl, callAudioFormat);
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
