#include "cof_config.h"
#include "cof_state.h"
#include <Arduino.h>
#include <Wire.h>
#include <esp_task_wdt.h>

// Extracted verbatim from main.cpp, which used to hold every function

// Restore radio service after the modem gets stuck in a NO SERVICE state.
//
// Observed on hardware (2026-09-21): +CPIN: READY and AT+CIMI OK, but
// +CSQ: 99,99 and +CPSI: NO SERVICE,Online, with AT+CNACT? replying ERROR.
// AT+CMEE=2 and AT+CGATT=1 do not recover it and nothing else in the firmware
// did either; the only known cure was a physical power cycle, unavailable on a
// remote site. Escalate from the cheapest reset to the closest software
// equivalent of that power cycle.
//
// NOT A FIX - a mitigation. A healthy module never reaches stage 5, so when that
// log line appears the modem needs attention (power, antenna, SIM seating).
bool resetModemRadio(uint8_t stage) {
  if (!state.modemReady || state.callInProgress || state.otaInProgress) {
    return false;
  }
  setStatus("Modem reset");
  switch (stage) {
    case 1:
      // Gentlest possible nudge: ask the modem to re-select the operator. No
      // CGATT=0 detach and no PDP teardown here - detaching on a transient
      // "NO SERVICE" read *caused* the next registration flap and re-triggered
      // this ladder in a loop (8x "recovery started" on 2026-09-25). The
      // baseband reselects on its own; COPS=0 only nudges it. The grace window
      // in pollModem() already held off until NO SERVICE persisted.
      Serial.println("[modem] recovery 1/5: operator reselect (COPS=0)");
      sendAT("AT+COPS=0", "OK", 20000);
      break;
    case 2:
      // Re-attach to the packet domain WITHOUT detaching first. CGATT=1 is
      // idempotent when already attached and costs nothing; a plain re-attach
      // fixes a stale attach state without the detach flap.
      Serial.println("[modem] recovery 2/5: PS re-attach + operator auto");
      sendAT("AT+CGATT=1", "OK", 15000);
      sendAT("AT+COPS=0", "OK", 20000);
      break;
    case 3:
      // Radio off/on. This is the first step that actually cycles the RF; the
      // two gentler steps above cover the common transient reselection case.
      Serial.println("[modem] recovery 3/5: radio cycle (CFUN=0/1)");
      stopLtePdp();
      sendAT("AT+CFUN=0", "OK", 10000);
      waitWithWatchdog(2000);
      sendAT("AT+CFUN=1", "OK", 15000);
      sendAT("AT+CEMODE=1", "OK", 3000);
      sendAT("AT+CEVDP=3", "OK", 3000);
      sendAT("AT+COPS=0", "OK", 30000);
      break;
    case 4:
      // Software reset of the whole module. Watch the CPIN state afterwards: a
      // module that comes back with +CPIN: NOT READY points at SIM seating or
      // power rather than firmware.
      Serial.println("[modem] recovery 4/5: modem reset (CFUN=1,1)");
      sendAT("AT+CFUN=1,1", "OK", 5000);
      waitWithWatchdog(15000);
      // CFUN=1,1 drops ATE0, CGDCONT, CGAUTH, CGSMS, CMGF, CSCA and the voice
      // settings, so re-apply them. Without this the module recovers RF but comes
      // back without the APN/SMS configuration and the PDP cannot come up.
      initModem();
      break;
    default:
      // Stage 5, or AT+CFUN=1,1 is still settling. Reboot so the radio comes up
      // from a cold power-on anyway. Stage 4 already reset the module, so its
      // configuration has to be re-applied on the way back up; initModem() in
      // setup() covers that.
      Serial.println("[modem] recovery 5/5: modem still dead, restarting ESP32");
      stopLtePdp();
      setStatus("Restart (modem)");
      delay(300);
      ESP.restart();
      return false;
  }
  return waitForRadioService(45000, false);
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
// The A7672 emits *ATREADY: 1 (and *ATREADY: 0) only when the module (re)boots.
// A spontaneous reset or the recovery ladder's CFUN=1,1 both produce it, and it
// is the only unambiguous sign the AT channel has been torn down: everything
// cached about the module - PDP, socket, SIM, radio config, ATE0 - is now stale.
//
// Called ONLY from pollModem(), never from the AT read path. readModemUntil() and
// noteUrc() just set `modemRebootUrcSeen = true` when they see *ATREADY; that
// flag is acted on here, outside of any in-flight command. The previous design
// called this from readModemUntil()/flushModemInput() (i.e. mid-command, often
// inside initModem() itself) and reset modemReady during the very init that was
// re-establishing it, wedging the device in "L no AT" + "Wait SIM" (2026-09-25).
void noteModemRebootDetected() {
  if (!state.modemReady && !state.simReady && !state.lteDataUp && !state.lteMqttTransport) {
    return;  // already flagged; avoid log spam
  }
  Serial.println("[modem] *ATREADY detected: modem rebooted, flagging for re-init");
  state.modemReady = false;
  state.simReady = false;
  state.networkRegistered = false;
  state.lteDataUp = false;
  state.lteMqttTransport = false;
  state.lteIpAddress = "-";
  lteMqttClient.sockOpen = false;
  lteMqttClient.atCommandBusy = false;
  lteMqttClient.drainRx();
}
// A modem that reboots (CFUN=1,1 or spontaneous) comes back with echo ON; ATE0
// is only sent from initModem() and a reboot does not re-run it. Detect the
// echoed command in a response and turn echo off again so the next commands
// parse cleanly. Never re-issues the caller's command: it already ran.
void ensureEchoOffIfNeeded(const String& command, const String& response) {
  if (command.length() == 0 || command == "ATE0") {
    return;
  }
  if (!response.startsWith(command)) {
    return;
  }
  Serial.println("[modem] echo detected, re-sending ATE0");
  ModemSerial.print("ATE0\r\n");
  String discard;
  readModemUntil(1000, "OK");
}
void flushModemInput() {
  // Bound the drain: a module spewing (echo ON / boot-URC flood / line noise)
  // used to make this loop run forever and block every AT command that follows
  // (sendAT() calls it first). Cap the bytes per call; the next call drains more.
  int drained = 0;
  while (ModemSerial.available() && drained < 1024) {
    const char c = static_cast<char>(ModemSerial.read());
    drained++;
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
String readModemUntil(uint32_t timeoutMs, const String& token) {
  String response;
  bool rebootSeen = false;
  const uint32_t startedAt = millis();
  while (millis() - startedAt < timeoutMs) {
    feedWatchdog();
    // Pump MQTT on every path. `if (!loop())` below means "PINGRESP missing or
    // socket dead", not "no data available" (loop() returns true when idle), so
    // the return value is the liveness signal we actually want.
    //
    // Except while `LteMqttClient::write()` owns the line: this same loop runs
    // inside its `AT+CIPSEND` prompt/ACK waits, and both things the pump can do
    // - `available()` issuing `AT+CIPRXGET` and `loop()` emitting a PINGREQ -
    // are writes to the module that corrupt the exchange. The send is waiting
    // on the prompt it just paid for; nothing may talk over it.
    //
    // Native CMQTT (COF_LTE_MQTT_NATIVE) is excluded for the same reason and one
    // more: the module keepalives by itself, and cmqttLoop() reads the same UART
    // this function is draining, so pumping it here would steal the AT response.
#if COF_LTE_MQTT_NATIVE
    if (state.mqttConnected && !state.lteMqttTransport && !lteMqttClient.atCommandBusy) {
#else
    if (state.mqttConnected && !lteMqttClient.atCommandBusy) {
#endif
      if (mqttClient.loop()) {
        lastMqttOkMs = millis();
      } else {
        state.mqttConnected = false;
      }
    }
    // Drain a bounded chunk per pass and re-check the deadline inside the drain.
    // A module that keeps spewing (echo ON, boot-URC flood, noise) used to make
    // this inner loop run forever - never re-checking the timeout, never feeding
    // the watchdog - and grow `response` without limit until the heap ran out.
    // That is the "UART hangs everything" failure (see BACKLOG §6h / 0.2.77).
    for (int drained = 0; drained < 256 && millis() - startedAt < timeoutMs; drained++) {
      if (!ModemSerial.available()) {
        break;
      }
      const char c = static_cast<char>(ModemSerial.read());
      response += c;
      // A module reboot emits *ATREADY as a URC, possibly in the middle of the
      // command we are waiting on. Detect it cheaply at line boundaries.
      if (!rebootSeen && (c == '\n' || c == '\r') && response.indexOf("*ATREADY") >= 0) {
        rebootSeen = true;
      }
      if (token.length() == 0) {
        continue;
      }
      const int tagAt = response.indexOf(token);
      if (tagAt < 0) {
        continue;
      }
      if (!token.endsWith(":")) {
        if (rebootSeen) {
          modemRebootUrcSeen = true;
        }
        return response;
      }
      for (int i = tagAt + token.length(); i < response.length(); i++) {
        if (response[i] == '\n' || response[i] == '\r') {
          if (rebootSeen) {
            modemRebootUrcSeen = true;
          }
          return response;
        }
      }
    }
    // Keep only the tail: what we search for (the token) always arrives last, and
    // an unbounded buffer is exactly the OOM the drain cap is meant to prevent.
    if (response.length() > 4096) {
      response = response.substring(response.length() - 4096);
    }
    delay(10);
  }
  if (rebootSeen) {
    modemRebootUrcSeen = true;
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

  // A rebooted module echoes the command back; turn echo off for the commands
  // that follow. The command itself already ran, so do not re-issue it.
  ensureEchoOffIfNeeded(command, response);

  if (responseOut != nullptr) {
    *responseOut = response;
  }
  return expected.length() == 0 || response.indexOf(expected) >= 0;
}
bool modemWaitForPrompt(uint32_t timeoutMs) {
  const String response = readModemUntil(timeoutMs, ">");
  Serial.println("[modem] << " + response);
  return response.indexOf(">") >= 0;
}
bool csAttached() {
  return networkStatRegistered(state.cregStat);
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
  // initModem() IS the re-init that *ATREADY demands: it re-applies ATE0, APN,
  // SMS/voice settings and re-reads CPIN. Consume any *ATREADY seen during this
  // boot so pollModem() does not re-run the reset on a stale flag (§6h, 0.2.76).
  modemRebootUrcSeen = false;
  return true;
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
// Does the radio report a usable RF state? False for the stuck state where CSQ is
// 99 and CPSI says NO SERVICE. networkRegistered ORs CREG/CEREG/CGREG, and CEREG
// keeps reporting "registered" on a stale context long after the radio lost
// service, so registration alone must not be trusted: CPSI's NO SERVICE is
// authoritative and is checked first.
bool radioReportsService() {
  if (state.radioMode.length() > 0 &&
      (state.radioMode.indexOf("NO SERVICE") >= 0 ||
       state.radioMode.indexOf("No Service") >= 0)) {
    return false;
  }
  if (state.networkRegistered) {
    return true;
  }
  return state.radioMode.length() > 0;
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
int queryCpas() {
  String response;
  if (!sendAT("AT+CPAS", "OK", 3000, &response)) {
    return -1;
  }
  return extractAtTagValue(response, "+CPAS:").toInt();
}
bool radioIsOnline() {
  return state.radioInfo.indexOf("Online") >= 0 ||
         state.radioInfo.indexOf("ONLINE") >= 0;
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
void restoreAutoRadio() {
  if (!state.forcedGsmForCall) {
    return;
  }
  state.forcedGsmForCall = false;
  sendAT("AT+CNMP=2", "OK", 10000);
  waitForRadioService(20000, false);
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
