#pragma once
// Umbrella header for all shared state. Include this and nothing else from
// the cof_* headers; the order below is load-bearing.
//
// Declarations only: the definitions (with initialisers) live in main.cpp, so
// exactly one translation unit owns each symbol. Do not add initialisers here
// -- that would create a tentative definition in every TU, which links under
// -fcommon but breaks the moment the toolchain flips to -fno-common.
//
// Verified before the split: no global's initialiser reads another global, so
// splitting them across translation units cannot introduce a static
// initialisation-order bug.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <IPAddress.h>
#include <DallasTemperature.h>
#include <OneWire.h>
#include <U8g2lib.h>
#include <Adafruit_SHT31.h>
#include <Client.h>
#include "cof_config.h"

// ---- types ----
enum LteIpStack : uint8_t { kLteStackUnknown = 0, kLteStackNetopen, kLteStackCnact };

enum class PathPreference : uint8_t { Auto, Ethernet, Wifi };

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
  // The canned asset the manifest keeps on the modem (C:/cof_test.wav). It is
  // only a fallback: a call tries to download the spoken TTS audio first, and
  // only when that download is impossible does it play this instead of not
  // dialing at all. See placeCallAndPlayAudio().
  String modemFallbackAudioPath = COF_MODEM_AUDIO_PATH;
  bool modemFallbackAudioReady = false;
  String manifestFirmwareVersion = "";
  String manifestFirmwareUrl = "";
  String manifestFirmwareSha256 = "";
  String manifestAudioVersion = "";
  String manifestAudioUrl = "";
  String manifestPhoneNumber = COF_PHONE_NUMBER;
};

struct DeferredEvent {
  String type;
  String severity;
  String message;
  String commandId;
};

enum CallEndSource {
  kCallEndRemote,
  kCallEndAudioDone,
  kCallEndTimeout
};

// ---- objects and variables (declaration only: no constructor arguments) ----
extern HardwareSerial ModemSerial;
extern U8G2_SH1106_128X64_NONAME_F_HW_I2C display;
extern OneWire oneWire;
extern DallasTemperature ds18b20;
extern WiFiClient mqttPlainClient;
extern WiFiClientSecure mqttTlsClient;
extern PubSubClient mqttClient;
extern Adafruit_SHT31 sht31;
extern Preferences preferences;
extern RuntimeState state;
extern uint32_t lastDisplayMs;
extern uint32_t lastSensorMs;
extern uint32_t lastModemMs;
extern uint32_t lastSmsPollMs;
extern uint32_t lastMqttReconnectMs;
extern uint32_t lastMqttOkMs;
extern uint8_t lanMqttFailCount;
extern uint32_t wifiBackupDueMs;
extern uint8_t wifiAuthFailCount;
extern uint32_t lastEthProbeMs;
extern uint8_t ethProbeFails;
extern uint32_t noLanSinceMs;
extern uint32_t ethernetUpAtMs;
extern uint32_t ethernetHoldoffUntilMs;
extern IPAddress cachedMqttIp;
extern IPAddress resolvedBrokerIp;
extern volatile bool brokerResolveDone;
extern bool brokerResolveInFlight;
extern bool brokerResolveFailed;
extern uint32_t brokerResolveStartedMs;
extern uint32_t lastBrokerResolveMs;
extern String brokerResolveName;
extern uint32_t lastTelemetryPublishMs;
extern uint32_t lastCellularStatusMs;
extern uint32_t lastManifestMs;
extern bool didInitialManifestCheck;
extern bool lastButtonPressed;
extern uint32_t buttonPressedAtMs;
extern String serialCommandBuffer;
extern bool pendingConfigReport;
extern bool pendingConfigApplied;
extern int pendingConfigVersion;
extern String pendingConfigHash;
extern String pendingConfigError;
extern bool pendingOtaCommand;
extern bool pendingStatusReportCommand;
extern bool pendingModemProbeCommand;
extern String pendingModemProbeCommandId;
extern bool pendingTestCallCommand;
extern String pendingTestCallPhone;
extern String pendingTestCallAudioUrl;
extern String pendingTestCallAudioFormat;
extern String pendingTestCallCommandId;
extern bool reportTestCallProgress;
extern String pendingCallUrcs;
extern String pendingModemUrcs;
extern String modemCallLog;
extern bool pendingTestSmsCommand;
extern String pendingTestSmsPhone;
extern String pendingTestSmsText;
extern String pendingTestSmsCommandId;
extern bool pendingCommandAck;
extern String pendingCommandId;
extern String pendingCommandName;
extern String pendingCommandStatus;
extern String pendingCommandMessage;
extern bool pendingMqttBounce;
extern bool pendingNetworkStatusReport;
extern uint32_t lastLteAttemptMs;
extern bool reportLteProgress;
extern bool lastLteFail;
extern LteIpStack lteIpStack;
extern String lteTraceLog;
extern String pendingLteTraceMessage;
extern bool pendingLteTracePublish;
extern bool pendingLteTraceOk;
extern bool ethInternetUp;
extern bool wifiInternetUp;
extern uint8_t wifiProbeFails;
extern uint32_t lastWifiProbeMs;
extern uint32_t lastEthRecoverProbeMs;
extern bool ethRecoverPending;
extern uint8_t modemRecoveryStage;
extern uint32_t lastModemRecoveryMs;
extern uint32_t ltePreemptSinceMs;
extern uint32_t lastLteRetryDelayMs;
extern uint32_t lastLteDataEventMs;
extern uint8_t lteMqttConnectFails;
extern uint32_t lastSilenceProbeMs;
extern DeferredEvent deferredEvents[kDeferredEventMax];
extern size_t deferredEventCount;
extern bool otaConfirmPending;
extern uint32_t otaConfirmBootMs;
extern bool otaConfirmedThisBoot;

// Prototypes, then the class: LteMqttClient's inline methods need both the
// variables above and the AT helpers declared in cof_api.h.
#include "cof_api.h"
#include "lte_mqtt_client.h"

extern LteMqttClient lteMqttClient;

