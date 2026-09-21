#pragma once
// Prototypes of every free function, grouped by the module that owns it.
//
// A single API header on purpose: modules call each other in both directions
// (net_paths <-> mqtt_io, sms_voice <-> modem_at), so per-module headers would
// have to include one another. Declaration order does not matter.
//
// Deliberately does NOT include cof_state.h: cof_state.h includes this file,
// and including back would close the guard before the types are in scope.
// Always include cof_state.h first.

// ---- owned by at_parse --------------------------------------
int atUrcCode(const String& resp, const char* tag);
bool ceerLooksNoAnswer(const String& ceer);
bool ceerLooksRejected(const String& ceer);
String compactAtText(const String& raw);
int compareVersions(const String& a, const String& b);
String extractAtTagValue(const String& response, const char* tag);
String extractQuoted(const String& response);
String firstNonEmptyAtLine(const String& response);
String lastQuoted(const String& text);
bool looksLikeIp(const String& ip);
String mqttTopic(const String& suffix);
String nthQuoted(const String& line, int want);
int parseCeerCode(const String& ceer);
int parseCommaStat(const String& response, const char* tag);
bool phoneLooksValid(const String& phone);
String withFirmware(const String& message);

// ---- owned by core ------------------------------------------
void handleButton();
void handleSerialCommand(const String& command);
void handleSerialInput();
void loop();
void maintainLteFallback();
bool networkStatRegistered(int stat);
int parseClccStat(const String& response);
void pollModem();
void printRuntimeStatus();
void printSerialHelp();
void publishTestCallProgress(const String& message);
void publishTestCallResult(const String& result, bool ok, const String& commandId = "");
String queryCallFailCause();
bool readTestButton();
void setStatus(const String& line);
void setup();

// ---- owned by lte_pdp ---------------------------------------
bool activateCnactPdp(String& ipOut);
bool activateNetopenPdp(String& ipOut);
void detectLteIpStack();
bool ensureLtePdp();
void finishLteAttempt(bool ok, const String& message);
bool markLtePdpUp(const String& ip, uint8_t cid);
bool netopenIsActive();
bool netopenResultOk(const String& resp);
bool parseLteIp(const String& resp, String& ipOut);
bool queryLteIp(String& ipOut);
void releaseLteMqttForModem();
String resolveLteMqttPeer(const char* host);
void restorePacketServices();
void stopLtePdp();

// ---- owned by modem_at --------------------------------------
void appendModemLog(char direction, const String& text);
void beginInternalWatchdog();
void configureCellularApn();
bool cpinResponseReady(const String& response);
bool csAttached();
void feedWatchdog();
void flushModemInput();
bool initModem();
bool modemLineInteresting(const String& line);
bool modemWaitForPrompt(uint32_t timeoutMs);
void noteSubscriberIdentity();
int queryCpas();
bool radioHasService();
bool radioIsGsm();
bool radioIsOnline();
bool radioReportsService();
String readModemUntil(uint32_t timeoutMs, const String& token = "");
void refreshCellularStatus();
void refreshRadioMode();
bool refreshSimReady();
bool resetModemRadio(uint8_t stage);
void restoreAutoRadio();
bool sendAT(const String& command, const String& expected = "OK", uint32_t timeoutMs = 2000, String* responseOut = nullptr);
bool waitForRadioService(uint32_t timeoutMs, bool gsmOnly);
bool waitUntilModemReady(bool forCall, uint32_t timeoutMs);
void waitWithMqtt(uint32_t ms);
void waitWithWatchdog(uint32_t ms);

// ---- owned by mqtt_io ---------------------------------------
void bounceMqttForRouteChange();
void configureMqttClientTransport();
void connectMqttIfNeeded();
void deferDeviceEvent(const char* type, const char* severity, const String& message, const String& commandId);
void enforceMqttSilenceWatchdog();
void fillCellularJson(JsonObject cellular);
void fillConnectivityJson(JsonDocument& doc);
void fillNetworkJson(JsonObject network);
void flushDeferredEvents();
bool mqttUsesTls();
void onMqttMessage(char* topic, byte* payload, unsigned int length);
bool publishDeviceEvent(const char* type, const char* severity, const String& message, const String& commandId = "");
void publishDeviceStatus(const char* status, bool retained = true);
void publishLteDataTrace();
bool publishMqttJson(const String& suffix, JsonDocument& doc, bool retained = false, uint8_t qos = 0);
void publishTelemetryNow();
void requestMqttBounce(const char* reason);

// ---- owned by net_paths -------------------------------------
const char* activeNetworkName();
void applyPreferredRoute(PathPreference pref = PathPreference::Auto);
void beginEthernet();
void beginSavedWiFi();
bool canUseLan();
void clearSavedWiFi();
void connectWiFi(const String& ssid, const String& password, bool saveCredentials);
String currentIpAddress();
bool ethernetHoldoffActive();
void forgetWifiRadio();
bool lanConnected();
bool lanHasInternet();
bool lanPathReachable();
void maintainWifiBackup();
void markEthernetDown(const char* reason);
void markEthernetUp(const char* reason);
const char* mqttPathLetter();
bool networkConnected();
void noteLanMqttFailure(const char* reason);
void noteLanPathFailure(bool ethernet, const char* reason, bool hadInternet);
void onBrokerResolved(const char* name, const ip_addr_t* addr, void* arg);
void onNetworkEvent(WiFiEvent_t event, WiFiEventInfo_t info);
void pauseWiFiRadio();
void pollEthernetPath();
void pollWifiPath();
bool probeMqttOnInterface(bool ethernet);
bool probeMqttOverEthernet();
bool probeMqttOverWifi();
IPAddress resolveBrokerHost(const String& host, bool force);
void scheduleWifiBackup(uint32_t delayMs);
void serviceBrokerResolve();
void serviceNetworkPaths();
void startWifiRadio();

// ---- owned by ota_config ------------------------------------
bool applyDesiredConfig(JsonDocument& doc);
void checkManifest(bool allowFirmwareUpdate);
void clearMqttConfig();
bool httpGetString(const String& url, String& out, uint32_t timeoutMs = 15000);
void initOtaRollbackGuard();
void loadSavedMqttConfig();
bool performOta(const String& url, const String& newVersion, const String& expectedSha256);
void publishCommandAck();
void publishConfigReported();
void saveMqttConfig(const String& host, int port, const String& deviceId, const String& username, const String& password);
void serviceOtaRollbackGuard();

// ---- owned by sensors_display -------------------------------
void detectPcf8574();
void drawDisplay();
String ds18b20AddressToString(const DeviceAddress address);
bool i2cDevicePresent(uint8_t address);
void initDisplay();
void readSensors();
void scanI2cBus();

// ---- owned by sms_voice -------------------------------------
void bounceRadioForCsfb();
bool callIsIdle();
String classifyCallUrc(const String& raw);
String classifyHangup(const String& ceer, bool sawAlerting, CallEndSource source);
String conductOutgoingCall(uint32_t timeoutMs, String* ceerOut);
String dialAndMaybePlay(const String& phone, const String& bearer);
bool imsVoiceReady();
String inferVoicePath();
bool isCallReady();
bool isSmsReady();
int lastClccStat(const String& response);
void lockGsmForCall();
String observeVoicePath(const String& radioDial, const String& radioConnect);
int parseClccStatAt(const String& response, int tag);
void persistObservedVoicePath(const String& path);
void persistSkipGsm(bool skip);
String placeCallAndPlayAudio(const String& phoneOverride = "", bool adminTest = false, const String& audioUrl = "", const String& audioFormat = "");
void pollIncomingSms();
String prepareVoiceBearer();
void processPendingSmsUrcs();
void publishCallModemSignal(const String& raw);
void publishInboundSms(const String& from, const String& text);
void publishSmsRecords(const String& response, const char* tag);
int queryClccStat();
String resolveTestPhone(const String& phoneOverride);
String sendTestSms(const String& phoneOverride, const String& text);
bool shouldRetryVoice(const String& result);
bool smsStackReady();
String stopPlaybackAndCollect();
String takePendingCallUrcs();
String transmitSms(const String& phone, const String& body);
String ttsModemPathFor(const String& url, const String& format);
String uploadAudioToModem(const String& url, const String& modemPath, const String& audioVersion);
String voiceContextSuffix(const String& bearer, const String& ceer = "");

