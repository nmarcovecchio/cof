#include "cof_config.h"
#include "cof_state.h"
#include <Arduino.h>
#include <HTTPClient.h>

// Extracted verbatim from main.cpp, which used to hold every function

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
String takePendingCallUrcs() {
  const String out = pendingCallUrcs;
  pendingCallUrcs = "";
  return out;
}
String stopPlaybackAndCollect() {
  String resp;
  sendAT("AT+CCMXSTOP", "OK", 2000, &resp);
  return takePendingCallUrcs() + resp;
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

bool fallbackAudioAvailable() {
  // Playback capability, not file-transfer: the asset is already on the modem at
  // this point, so what matters is that this module can play a file into a call.
  // Whether the asset is really there is carried by modemFallbackAudioReady,
  // which is only set by a successful manifest audio sync.
  return state.modemFallbackAudioReady &&
         state.modemFallbackAudioPath.length() > 0 &&
         state.modemAudioPlaybackSupported;
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
    // Per-rule audio lives in its own namespace and is tracked by
    // syncRuleAudio()'s own index. It must NOT touch audioVersion/fallback:
    // this function's "audioVersion" is the *manifest* asset tracker, and
    // treating a rule file as the fallback would (a) point the offline fallback
    // at a rule-specific file and (b) make the real fallback asset look stale,
    // re-downloading it on every manifest check.
    const bool isRuleAudio = fileName.startsWith("a_");
    if (!isRuleAudio) {
      preferences.putString("audioVersion", audioVersion);
      // The canned manifest asset is the one we keep as the offline fallback.
      // The admin TTS upload uses audioVersion "tts" and writes to C:/tts.amr,
      // which is deleted and rewritten on the next call - it must NOT be
      // remembered as the fallback, or a failed download would play a stale
      // phrase.
      if (audioVersion != "tts") {
        preferences.putString("fallbackAudioPath", modemPath);
        state.modemFallbackAudioPath = modemPath;
        state.modemFallbackAudioReady = true;
        Serial.printf("[audio] fallback asset stored: %s\n", modemPath.c_str());
      }
    }
    setStatus("Audio synced");
    return "";
  }

  setStatus("Audio upload fail");
  return "TTS modem upload fail";
}
// --- Modem audio/storage probe ---------------------------------------------
//
// Answers "does this modem support the paths the alarm audio depends on?"
// without a serial console: every line goes out as an MQTT `modem_probe` event,
// readable from the device page. Meant to be run over OTA.
//
// Context: the firmware downloads audio/manifest/firmware with HTTPClient,
// which needs an lwIP interface (Ethernet or WiFi). A site whose only path is
// LTE cannot receive any of them. The modem has its own HTTP/FTP stack and can
// write straight to its C: - these probes establish whether that is usable
// here, which would let such a site fetch audio without lwIP.
//
// Read-only on purpose: nothing below deletes or rewrites the alarm assets.
// Whether it is safe to break a call is enforced at the call site.
static constexpr size_t kProbeMessageMax = 200;

static String probeFirstLine(const String& raw) {
  int start = 0;
  while (start < static_cast<int>(raw.length())) {
    const int nl = raw.indexOf('\n', start);
    String line = (nl < 0) ? raw.substring(start) : raw.substring(start, nl);
    line.trim();
    // "OK" is the AT envelope, not the answer. Skip blanks plus the echo.
    if (line.length() > 0 && !line.equalsIgnoreCase("OK") && !line.equalsIgnoreCase("ERROR")) {
      return line;
    }
    if (nl < 0) {
      break;
    }
    start = nl + 1;
  }
  return "";
}

static void publishModemProbe(const String& body, bool ok, const String& commandId) {
  String message = "modem_probe: " + body;
  if (message.length() > kProbeMessageMax) {
    message = message.substring(0, kProbeMessageMax) + "...";
  }
  publishDeviceEvent("modem_probe", ok ? "info" : "warning", message, commandId);
  // Keep the broker serviced between probes: some of these take seconds and the
  // keepalive would otherwise lapse mid-probe.
  waitWithMqtt(50);
}

void runModemProbe(const String& commandId) {
  if (!state.modemReady) {
    publishModemProbe("modem not ready", false, commandId);
    return;
  }

  publishModemProbe("start fw=" COF_FIRMWARE_VERSION, true, commandId);

  // Storage: total and used bytes on C:. The gap to the largest audio the
  // firmware can transfer is what decides how many assets could fit.
  String resp;
  if (sendAT("AT+FSMEM", "+FSMEM:", 5000, &resp)) {
    const String line = probeFirstLine(resp);
    publishModemProbe(line.length() > 0 ? line : "FSMEM ok", true, commandId);
  } else {
    publishModemProbe("FSMEM unsupported/err", false, commandId);
  }

  // Alert-tone capability, best effort: absent from the V1.06 command manual,
  // so an ERROR here is an answer too (means "do not design around it").
  if (sendAT("AT+CCALB?", "OK", 3000, &resp)) {
    const String line = probeFirstLine(resp);
    publishModemProbe("CCALB " + (line.length() > 0 ? line : "ok"), true, commandId);
  } else {
    publishModemProbe("CCALB unsupported", false, commandId);
  }

  // Capability flags the firmware already relies on for audio upload/playback.
  publishModemProbe(
      "caps fs=" + String(state.modemFileTransferSupported ? "YES" : "NO") +
          " play=" + String(state.modemAudioPlaybackSupported ? "YES" : "NO"),
      true, commandId);

  // The actual question the LTE-only design turns on: can the modem open HTTP
  // on its own, and does it support writing the response body to a file?
  const bool httpOk = sendAT("AT+HTTPINIT", "OK", 8000);
  publishModemProbe(String("HTTPINIT ") + (httpOk ? "ok" : "fail"), httpOk, commandId);

  const bool readFileOk = sendAT("AT+HTTPREADFILE=?", "OK", 3000);
  if (httpOk) {
    sendAT("AT+HTTPTERM", "OK", 5000);
  }
  publishModemProbe(
      String("HTTPREADFILE=") + (readFileOk ? "SUPPORTED" : "unsupported"),
      readFileOk, commandId);

  publishModemProbe("end", true, commandId);
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
bool imsVoiceReady() {
  return state.imsReg == 1;
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
bool callIsIdle() {
  const int cpas = queryCpas();
  if (cpas == 3 || cpas == 4) {
    sendAT("ATH", "OK", 3000);
    sendAT("AT+CHUP", "OK", 3000);
    return false;
  }
  return cpas == 0;
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
String placeCallAndPlayAudio(const String& phoneOverride, bool adminTest, const String& audioSha) {
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
    // A pre-recorded file already on the modem is the best case, and the only
    // one that matters: every asset is self-contained, because all placeholders
    // are resolved at save time. Playing it needs no internet at all, which is
    // the whole point of the feature.
    //
    // If the device does not have it yet, the audio cannot be played from the
    // URL: downloading needs lwIP, and a site whose only path is LTE has no
    // route (MQTT rides the modem's AT socket). So ask for a config sync and
    // fall back to whatever is on the modem, rather than attempting a download
    // that can only fail and used to leave the alarm with no call at all.
    const String localRuleAudio = ruleAudioPathForSha(audioSha);
    if (localRuleAudio.length() > 0) {
      publishTestCallProgress("Using call audio on device");
      state.modemAudioPath = localRuleAudio;
    } else if (fallbackAudioAvailable()) {
      publishTestCallProgress("Rule audio not on device, using fallback");
      Serial.printf("[call] no local audio for %s; playing %s\n", audioSha.c_str(),
                    state.modemFallbackAudioPath.c_str());
      state.modemAudioPath = state.modemFallbackAudioPath;
    } else {
      setStatus("Sync test audio");
      checkManifest(false);
      return "Audio not on device";
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
