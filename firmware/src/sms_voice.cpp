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
  publishModemProbe(
      String("HTTPREADFILE=") + (readFileOk ? "SUPPORTED" : "unsupported"),
      readFileOk, commandId);

  // End-to-end fetch. The capability answers above say the commands exist; they
  // say nothing about whether a GET works on this PDP stack, which CID it needs,
  // where the file lands, or what the read-file flag does. Those are exactly the
  // unknowns that would otherwise need a serial console, so run the real
  // sequence and report what happened.
  //
  // Against example.com on purpose: stable, ~1 KB, plain HTTP, and no dependency
  // on our own DNS or Caddy while the transport itself is still unproven.
  if (httpOk && readFileOk) {
    String before;
    sendAT("AT+FSMEM", "+FSMEM:", 5000, &before);
    publishModemProbe("FSMEM before " + probeFirstLine(before), true, commandId);

    const bool cidOk = sendAT("AT+HTTPPARA=\"CID\",1", "OK", 5000);
    const bool urlOk =
        sendAT("AT+HTTPPARA=\"URL\",\"http://example.com/\"", "OK", 5000);
    publishModemProbe(
        String("HTTPPARA cid=") + (cidOk ? "ok" : "fail") +
            " url=" + (urlOk ? "ok" : "fail"),
        urlOk, commandId);

    String action;
    const bool acted = sendAT("AT+HTTPACTION=0", "+HTTPACTION:", 45000, &action);
    publishModemProbe(
        "HTTPACTION " + (acted ? probeFirstLine(action) : String("no URC within 45s")),
        acted, commandId);

    // The whole design hinges on this one: does the body land in C:/ ?
    String read;
    const bool readOk =
        sendAT("AT+HTTPREADFILE=\"C:/probe_http.txt\",1", "OK", 20000, &read);
    publishModemProbe(String("HTTPREADFILE fi=") + (readOk ? "ok" : "fail"), readOk,
                      commandId);

    String after;
    sendAT("AT+FSMEM", "+FSMEM:", 5000, &after);
    publishModemProbe("FSMEM after " + probeFirstLine(after), true, commandId);

    // Best effort: not every unit implements FSLS, and an ERROR here is itself
    // an answer. It is the only way to see the filename the modem actually wrote.
    String ls;
    if (sendAT("AT+FSLS=C:/", "+FSLS:", 8000, &ls)) {
      publishModemProbe("FSLS " + probeFirstLine(ls), true, commandId);
    } else {
      publishModemProbe("FSLS unsupported", false, commandId);
    }
  }

  if (httpOk) {
    sendAT("AT+HTTPTERM", "OK", 5000);
  }

  publishModemProbe("end", true, commandId);
}

// --- CMQTT native-path probe ------------------------------------------------
//
// Answers the question the native MQTT design hinged on: does DISC (and the
// REL/STOP teardown), or a re-CONNECT over a live session, reboot this module
// (*ATREADY)? The BACKLOG (0.2.106 / 0.2.110) recorded both as reboot triggers
// on the A7672, but that was lab history, not a proof on THIS unit. This re-runs
// the exact sequence against a live session, over LAN so the result still
// reaches the panel if the modem does reboot.
//
// Outcome on cof-test (2026-09-29, fw 0.2.112/0.2.113): NEITHER reboots this
// A7672SA-FASE. Phase 1 (DISC/REL/STOP) is clean end-to-end, and phase 2
// (re-CONNECT over a live session) is rejected with +CMQTTCONNECT: 0,19
// ("client is used", A76XX MQTT app note) — a clean refusal, not a crash.
//
// LAN-only on purpose: on a LTE-only site the session being DISC'd is the only
// link the device has, so a reboot would take the reporting path down with it.
// Anonymous connect (the broker has allow_anonymous) with a distinct client id
// so the LAN PubSubClient is not kicked off its own client id by the takeover.

static String cmqttLinkSummary(const String& resp) {
  // Read-only summary of AT+CMQTTCONNECT?. The raw line can carry the server
  // address and credentials, so never publish it verbatim.
  const int at = resp.indexOf("+CMQTTCONNECT: 0");
  if (at < 0) {
    return "no client 0";
  }
  const int nl = resp.indexOf('\n', at);
  const String line = nl < 0 ? resp.substring(at) : resp.substring(at, nl);
  return line.indexOf("://") >= 0 ? "up (tcp session)" : "down (no session)";
}

void runCmqttProbe(const String& commandId) {
  if (!state.modemReady) {
    publishModemProbe("modem not ready", false, commandId);
    return;
  }
  if (state.lteMqttTransport) {
    publishModemProbe("LTE MQTT is up; refusing to DISC the live session", false, commandId);
    return;
  }
  if (!state.mqttConnected) {
    publishModemProbe("requires LAN MQTT up (Ethernet/WiFi) so the result survives a reboot",
                      false, commandId);
    return;
  }

  publishModemProbe("start fw=" COF_FIRMWARE_VERSION, true, commandId);

  // Own the UART for the whole probe. On LAN the modem is quiet (CMQTT was torn
  // down by maintainLteFallback), and taking it also stops pollModem()'s AT from
  // interleaving mid-probe. No DISC is sent on the way in (see lte_pdp.cpp).
  takeModemForVoiceSms(kModemUartSms);

  String resp;

  // Read-only baseline.
  if (sendAT("AT+CMQTTCONNECT?", "OK", 5000, &resp)) {
    publishModemProbe("baseline " + cmqttLinkSummary(resp), true, commandId);
  } else {
    publishModemProbe("baseline CMQTTCONNECT? no answer", false, commandId);
  }
  if (sendAT("AT+CMQTTACCQ?", "OK", 5000, &resp)) {
    publishModemProbe("clients " + probeFirstLine(resp), true, commandId);
  }

  // Bring up a live session so DISC targets a real broker link.
  const String server = String("tcp://") + state.mqttHost + ":" + String(state.mqttPort);
  const String connectAt =
      String("AT+CMQTTCONNECT=0,\"") + server + "\"," + String(kMqttKeepAliveSeconds) + ",1";

  const bool started = sendAT("AT+CMQTTSTART", "+CMQTTSTART:", 12000, &resp);
  publishModemProbe(String("CMQTTSTART ") + (started ? "ok" : "fail " + probeFirstLine(resp)),
                    started, commandId);

  const bool acquired = sendAT("AT+CMQTTACCQ=0,\"cof-probe\"", "OK", 5000);
  publishModemProbe(String("CMQTTACCQ ") + (acquired ? "ok" : "fail"), acquired, commandId);

  const bool connected = sendAT(connectAt, "+CMQTTCONNECT:", 20000, &resp);
  publishModemProbe(String("CMQTTCONNECT ") + (connected ? "ok " + server : "fail " + probeFirstLine(resp)),
                    connected, commandId);

  if (sendAT("AT+CMQTTCONNECT?", "OK", 5000, &resp)) {
    publishModemProbe("before DISC: " + cmqttLinkSummary(resp), true, commandId);
  }

  // The destructive test. *ATREADY is the module's own "I rebooted" URC and can
  // arrive from any of DISC/REL/STOP; if it does, later CMQTT commands must be
  // skipped (sendAT refuses them anyway until re-init).
  bool rebooted = modemRebootUrcSeen;
  const bool disc = sendAT("AT+CMQTTDISC=0,120", "+CMQTTDISC:", 15000, &resp);
  if (modemRebootUrcSeen) {
    rebooted = true;
  }
  publishModemProbe(String("DISC ") + (disc ? "ok " : "fail ") + probeFirstLine(resp) +
                        (modemRebootUrcSeen ? " + *ATREADY (REBOOT)" : ""),
                    disc, commandId);

  if (modemRebootUrcSeen) {
    publishModemProbe("REL skipped (module rebooted)", false, commandId);
    publishModemProbe("STOP skipped (module rebooted)", false, commandId);
  } else {
    const bool rel = sendAT("AT+CMQTTREL=0", "OK", 5000, &resp);
    if (modemRebootUrcSeen) {
      rebooted = true;
    }
    publishModemProbe(String("REL ") + (rel ? "ok" : "fail"), rel, commandId);

    const bool stop = sendAT("AT+CMQTTSTOP", "+CMQTTSTOP:", 12000, &resp);
    if (modemRebootUrcSeen) {
      rebooted = true;
    }
    publishModemProbe(String("STOP ") + (stop ? "ok" : "fail"), stop, commandId);
  }

  if (modemRebootUrcSeen) {
    publishModemProbe("MODEM REBOOTED during DISC/QUIT (*ATREADY seen)", false, commandId);
  } else {
    publishModemProbe("no reboot (*ATREADY) during DISC/QUIT", true, commandId);
  }

  // Phase 2 — re-CONNECT over a live session. The OTHER historical *ATREADY
  // suspect: 0.2.106/0.2.108 issued CMQTTCONNECT on a session that was already
  // up (no DISC first) and the module reset. Phase 1 proved the teardown path
  // clean; this re-runs "connect again while connected" to see if THAT is the
  // real trigger. Runs only if phase 1 did not already reboot the module.
  if (modemRebootUrcSeen) {
    publishModemProbe("phase2 skipped (module rebooted in phase 1)", false, commandId);
  } else {
    publishModemProbe("phase2 re-CONNECT over live session", true, commandId);
    const bool started2 = sendAT("AT+CMQTTSTART", "+CMQTTSTART:", 12000, &resp);
    publishModemProbe(String("P2 CMQTTSTART ") + (started2 ? "ok" : "fail " + probeFirstLine(resp)),
                      started2, commandId);
    const bool acquired2 = sendAT("AT+CMQTTACCQ=0,\"cof-probe\"", "OK", 5000);
    publishModemProbe(String("P2 CMQTTACCQ ") + (acquired2 ? "ok" : "fail"), acquired2, commandId);
    const bool conn1 = sendAT(connectAt, "+CMQTTCONNECT:", 20000, &resp);
    publishModemProbe(String("P2 CONNECT#1 ") + (conn1 ? "ok " + server : "fail " + probeFirstLine(resp)),
                      conn1, commandId);

    if (!modemRebootUrcSeen) {
      if (sendAT("AT+CMQTTCONNECT?", "OK", 5000, &resp)) {
        publishModemProbe("P2 before reCONNECT: " + cmqttLinkSummary(resp), true, commandId);
      }
      // The suspect: CONNECT again on the live session.
      const bool conn2 = sendAT(connectAt, "+CMQTTCONNECT:", 20000, &resp);
      publishModemProbe(String("P2 reCONNECT ") + (conn2 ? "ok " : "fail ") + probeFirstLine(resp) +
                            (modemRebootUrcSeen ? " + *ATREADY (REBOOT)" : ""),
                        conn2, commandId);
    }

    if (modemRebootUrcSeen) {
      publishModemProbe("P2 MODEM REBOOTED during re-CONNECT (*ATREADY seen)", false, commandId);
    } else {
      publishModemProbe("P2 no reboot during re-CONNECT", true, commandId);
      const bool disc2 = sendAT("AT+CMQTTDISC=0,120", "+CMQTTDISC:", 15000, &resp);
      publishModemProbe(String("P2 DISC ") + (disc2 ? "ok " : "fail ") + probeFirstLine(resp), disc2, commandId);
      const bool rel2 = sendAT("AT+CMQTTREL=0", "OK", 5000);
      publishModemProbe(String("P2 REL ") + (rel2 ? "ok" : "fail"), rel2, commandId);
      const bool stop2 = sendAT("AT+CMQTTSTOP", "+CMQTTSTOP:", 12000, &resp);
      publishModemProbe(String("P2 STOP ") + (stop2 ? "ok" : "fail"), stop2, commandId);
    }
  }

  releaseModemToMqtt(false);
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
// Locks the radio to GSM-only and waits for it to attach. Returns false when the
// site has no usable 2G, i.e. when the lock would leave the radio on NO SERVICE
// with no call placed. In that case the modem is already restored to auto (CNMP=2)
// before returning, so the caller must not run another radio escalation.
//
// History: 2026-09-29 (cof-test, 0.2.114) a call over LTE never dialed, went silent
// ~4 min and rebooted the modem. The trace showed `+CNMP: 13` and
// `+CPSI: NO SERVICE` afterwards: the unconditional lock burned 45 s + 30 s of
// blocking waits (no telemetry, no MQTT watchdog) and the reboot re-ran initModem()
// with CNMP still at 13. Gate on real 2G evidence and bound the budget.
bool lockGsmForCall() {
  setStatus("GSM lock");
  Serial.println("[call] lock GSM (CNMP=13) after prepared CSFB failed");
  publishTestCallProgress("Checking 2G availability");
  if (!gsmAccessPlausible()) {
    // No operator advertises GSM here: locking to CNMP=13 can only end in
    // NO SERVICE. Do not touch the radio; let the caller finish with the result
    // of the dial that already ran.
    Serial.println("[call] no 2G in scan, skipping GSM lock");
    appendModemLogForced("GSM lock skipped: no 2G in COPS scan");
    publishTestCallProgress("No 2G here, skipping GSM lock");
    return false;
  }
  sendAT("ATH", "OK", 3000);
  sendAT("AT+CHUP", "OK", 3000);
  sendAT("AT+CNMP=13", "OK", 10000);
  state.forcedGsmForCall = true;
  // Only pay the second (modem-ready) wait if the radio actually found GSM. The
  // old sequence spent 45 s + 30 s regardless, which starved the cooperative loop.
  if (!waitForRadioService(45000, true)) {
    Serial.println("[call] GSM lock found no service, restoring auto");
    appendModemLogForced("GSM lock timed out, restoring CNMP=2");
    publishTestCallProgress("GSM lock timed out");
    restoreAutoRadio();
    return false;
  }
  waitUntilModemReady(true, 30000);
  return true;
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
  // Validate first; only then take the UART mutex. Early exits used to DISC
  // MQTT and leave reclaim to the caller, and could break LAN checkManifest.
  if (!modemCsWorkAllowed()) {
    setStatus("Modem not ready");
    return "Modem not ready";
  }
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

  // Remember LTE before take: the release path then asks the module what
  // survived the call (CSFB can drop the bearer) instead of tearing CMQTT down.
  const bool wasOnLte = state.lteDataUp || state.lteMqttTransport;
  takeModemForVoiceSms(kModemUartVoice);
  state.callInProgress = true;
  pendingCallUrcs = "";
  if (!modemUartDebug) {
    modemCallLog = "";
  } else {
    appendModemLogForced("--- call start ---");
  }
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
  bool gsmLockSkipped = false;
  if (shouldRetryVoice(result) && !state.skipGsmVoice) {
    publishTestCallProgress("Locking GSM");
    // Only retry through the GSM lock when it actually took: lockGsmForCall()
    // returns false when there is no 2G here and already restored the radio, so
    // the call ends with the first attempt's result instead of dialing again on a
    // radio that is still on LTE (or, worse, on NO SERVICE).
    if (lockGsmForCall()) {
      refreshCellularStatus();
      bearer = "gsm lock";
      publishTestCallProgress("Retrying call");
      result = dialAndMaybePlay(phone, bearer);
    } else {
      // No GSM lock happened: do not run the CFUN bounce either, or a LTE-only
      // site pays bounce + failed lock + bounce for a single alarm call.
      Serial.println("[call] GSM lock not possible, keeping first result");
      gsmLockSkipped = true;
    }
  } else if (shouldRetryVoice(result) && !preparedCs) {
    publishTestCallProgress("Resetting radio");
    bounceRadioForCsfb();
    refreshCellularStatus();
    bearer = imsVoiceReady() ? "ims after bounce" : "csfb retry";
    publishTestCallProgress("Retrying call");
    result = dialAndMaybePlay(phone, bearer);
  }

  // Make the reason discoverable in the panel even when the call could not be
  // published live: the result event carries the modem log, but a one-line tag on
  // the message is what a reader sees first (this is exactly what was missing on
  // 2026-09-29, when the call over LTE just went silent).
  if (gsmLockSkipped) {
    result += " (no 2G here, GSM lock skipped)";
  }

  restoreAutoRadio();
  restorePacketServices();
  releaseModemToMqtt(wasOnLte);
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
  // Caller must hold the UART (takeModemForVoiceSms) when MQTT rides LTE.
  // Do not DISC/reconnect here: a half teardown + CONNECT was what wedged SMS.
  if (!sendAT("AT+CMGF=1", "OK", 3000)) {
    appendModemLogForced("SMS CMGF fail");
    setStatus("SMS mode fail");
    return "SMS mode fail";
  }
  appendModemLogForced("SMS CMGF ok");

  flushModemInput();
  Serial.println("[modem] >> AT+CMGS=\"" + phone + "\"");
  ModemSerial.print("AT+CMGS=\"");
  ModemSerial.print(phone);
  ModemSerial.print("\"\r");
  if (!modemWaitForPrompt(10000)) {
    appendModemLogForced("SMS prompt fail");
    setStatus("SMS prompt fail");
    return "SMS prompt fail";
  }
  appendModemLogForced("SMS prompt ok");

  ModemSerial.print(body);
  ModemSerial.write(static_cast<uint8_t>(0x1A));
  const String response = readModemUntil(60000, "OK");
  Serial.println("[modem] << " + response);
  appendModemLogForced("SMS rsp " + response);
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
  if (!modemCsWorkAllowed()) {
    setStatus("Modem not ready");
    return "Modem not ready";
  }
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

  // Same UART mutex as voice: borrow the UART (no DISC), send on a quiet modem,
  // then resume the CMQTT session the module kept (no REL/STOP).
  const bool wasOnLte = state.lteDataUp || state.lteMqttTransport;
  takeModemForVoiceSms(kModemUartSms);

  if (!waitUntilModemReady(false, 25000)) {
    restorePacketServices();
    if (!waitUntilModemReady(false, 20000)) {
      releaseModemToMqtt(wasOnLte);
      setStatus("SMS not ready");
      return "SMS not ready";
    }
  }

  String result = transmitSms(phone, body);
  if (!result.startsWith("SMS sent")) {
    // Still holding the UART: restore radio/packet, retry, then reclaim MQTT.
    restorePacketServices();
    result = transmitSms(phone, body);
  }
  releaseModemToMqtt(wasOnLte);
  return result;
}
