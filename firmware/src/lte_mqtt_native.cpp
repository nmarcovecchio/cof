#include "cof_config.h"
#include "cof_state.h"
#include <Arduino.h>

// ---------------------------------------------------------------------------
// Native MQTT-over-LTE using the A7672's built-in `AT+CMQTT*` service.
//
// This replaces the hand-rolled TCP socket emulation (AT+CIPOPEN / AT+CIPSEND /
// AT+CIPRXGET) for the cellular path. The module's own MQTT stack:
//   * runs over the PDP context that AT+CMQTTSTART activates itself (A76XX AT
//     manual ch.18); there is no NETOPEN/CNACT on this path,
//   * keeps the connection warm with the broker (keepalive_time in CONNECT),
//   * delivers inbound messages as URCs (+CMQTTRXSTART/TOPIC/PAYLOAD/END),
//   * and reports passive loss via +CMQTTCONNLOST / +CMQTTNONET.
//
// This is the "a phone switches WiFi->LTE in 2 s" behaviour: no polling, no
// manual PINGREQ, no duplicated socket/PDP state. See BACKLOG §6 (native CMQTT).
//
// Plaintext only (tcp://). The legacy LTE socket path was also plaintext over
// LTE (its NETOPEN branch always dials "TCP"), so there is no regression; TLS
// over LTE is a separate, deferred concern.
// ---------------------------------------------------------------------------

// Module state. Deliberately file-local: nothing outside this translation unit
// needs to know how the native client is internally staged.
static bool cmqttServiceUp = false;     // AT+CMQTTSTART succeeded
static bool cmqttClientAcquired = false;// AT+CMQTTACCQ succeeded
static bool cmqttBrokerUp = false;      // AT+CMQTTCONNECT ok, and no passive drop

// Inbound message assembly. The modem sends each subscribed message as a burst
// of URCs; long topics/payloads are split into several +CMQTTRXTOPIC/PAYLOAD
// chunks, so we accumulate until +CMQTTRXEND closes the message.
static String cmqttLineBuf;             // partial line across cmqttLoop() calls
static bool cmqttRxActive = false;      // saw +CMQTTRXSTART, not yet RXEND
static bool cmqttRxInPayload = false;   // topic done, accumulating payload
static int cmqttRxTopicTotal = 0;
static int cmqttRxPayloadTotal = 0;
static String cmqttRxTopic;
static String cmqttRxPayload;

// Bytes that arrived while another UART reader was draining the port (an AT
// command response inside readModemUntil(), or flushModemInput() right before a
// command). Those routines used to drop +CMQTTRX* bytes on the floor; by then
// the broker had already acked the PUBLISH, so a lost URC is an inbound command
// lost forever. They now hand the bytes here instead, and cmqttNextByte()
// replays them ahead of the live UART so cmqttLoop() still sees one contiguous
// stream.
static String cmqttHeldInput;

void cmqttHoldByte(char c) {
  cmqttHeldInput += c;
  if (cmqttHeldInput.length() > 4096) {
    cmqttHeldInput.remove(0, cmqttHeldInput.length() - 2048);
  }
}

// Next byte of the modem stream: held bytes first, then the live UART.
static bool cmqttNextByte(char& c) {
  if (cmqttHeldInput.length() > 0) {
    c = cmqttHeldInput[0];
    cmqttHeldInput.remove(0, 1);
    return true;
  }
  if (!ModemSerial.available()) {
    return false;
  }
  c = static_cast<char>(ModemSerial.read());
  return true;
}

static bool cmqttReadLine(String& line) {
  // Non-blocking: returns true once a full '\n'-terminated line is assembled,
  // false if the stream ran dry mid-line. The partial line lives in cmqttLineBuf
  // so the next call resumes where it left off.
  while (true) {
    char c;
    if (!cmqttNextByte(c)) {
      return false;
    }
    if (c == '\n') {
      line = cmqttLineBuf;
      cmqttLineBuf = "";
      line.trim();
      return true;
    }
    if (c != '\r') {
      cmqttLineBuf += c;
    }
    if (cmqttLineBuf.length() > 512) {
      // Defensive: a garbage line must not grow the buffer forever.
      line = cmqttLineBuf;
      cmqttLineBuf = "";
      line.trim();
      return true;
    }
  }
}

static bool cmqttReadExact(String& out, int n) {
  // Blocking, bounded: the payload follows the URC header as a contiguous burst
  // already sitting in the buffer/FIFO, so this normally returns in ms. The 3 s
  // deadline is only a guard against a module that stalls mid-message.
  const uint32_t startedAt = millis();
  while (n > 0 && millis() - startedAt < 3000) {
    feedWatchdog();
    char c;
    if (cmqttNextByte(c)) {
      out += c;
      n--;
    } else {
      delay(2);
    }
  }
  return n == 0;
}

// Result code of a CMQTT command. The A76XX manual formats results either as
// "+CMQTTCONNECT: 0,0" (code after the last comma) or "+CMQTTSTART: 0" (code
// after the colon, no comma). Returns -1 when the tag is absent.
static int cmqttResult(const String& resp, const char* tag) {
  const int at = resp.indexOf(tag);
  if (at < 0) {
    return -1;
  }
  const int lineEnd = resp.indexOf('\n', at);
  const String line = lineEnd < 0 ? resp.substring(at) : resp.substring(at, lineEnd);
  int delim = line.lastIndexOf(',');
  if (delim < 0) {
    delim = line.lastIndexOf(':');
  }
  if (delim < 0) {
    return -1;
  }
  String code = line.substring(delim + 1);
  code.trim();
  return code.toInt();
}

// Send a command that prompts with '>' and then takes raw data on the next
// line (WILLTOPIC / WILLMSG / SUB topic / TOPIC / PAYLOAD). Returns true when
// the trailing "OK" arrives.
static bool cmqttPromptWrite(const String& cmd, const String& data, uint32_t timeoutMs) {
  if (!sendAT(cmd, ">", timeoutMs)) {
    return false;
  }
  ModemSerial.write(reinterpret_cast<const uint8_t*>(data.c_str()), data.length());
  const String resp = readModemUntil(timeoutMs, "OK");
  appendModemLog('<', resp);
  return resp.indexOf("OK") >= 0;
}

bool cmqttIsConnected() {
  return cmqttBrokerUp;
}

bool cmqttIsRxBusy() {
  // True while a published message is mid-delivery (+CMQTTRXSTART seen, not yet
  // +CMQTTRXEND). serviceLteMqttHealth() skips its AT+CSQ probe in this window so
  // the probe cannot interleave with the raw topic/payload bytes that follow the
  // URC header.
  return cmqttRxActive;
}

static void cmqttResetAssembler() {
  cmqttLineBuf = "";
  cmqttHeldInput = "";
  cmqttRxActive = false;
  cmqttRxInPayload = false;
  cmqttRxTopic = "";
  cmqttRxPayload = "";
  cmqttRxTopicTotal = 0;
  cmqttRxPayloadTotal = 0;
}

// Pump inbound URCs until the UART has been quiet. A successful SUB is followed
// immediately by the retained message (config/desired is ~2 KB). Issuing the
// next AT command before that burst finishes is what produced +CMQTTSUB: 0,14
// ("client is busy"), a truncated JSON and, after CMQTTSTOP, the modem reboot
// loop (*ATREADY / +CMQTTSTART: 1).
static void cmqttDrainInbound(uint32_t quietMs, uint32_t maxMs) {
  const uint32_t startedAt = millis();
  uint32_t quietSince = 0;
  while (millis() - startedAt < maxMs) {
    feedWatchdog();
    cmqttLoop();
    const bool busy = cmqttIsRxBusy() || ModemSerial.available() || cmqttHeldInput.length() > 0;
    if (busy) {
      quietSince = 0;
      delay(2);
      continue;
    }
    if (quietSince == 0) {
      quietSince = millis();
    }
    if (millis() - quietSince >= quietMs) {
      return;
    }
    delay(10);
  }
  if (cmqttIsRxBusy()) {
    Serial.println("[cmqtt] inbound drain timed out with RX still open");
    appendModemLogForced("RX drain timeout");
  }
}

// 0 = started, 1 = bare ERROR (service already running), 2 = failed.
// +CMQTTSTART: 1 is "failed", not "already started". STOP after a code 1 while
// the module is still printing *ATREADY / PB DONE reboots it and the next START
// fails again — the loop in the 0.2.86 field log.
static int cmqttTryStart() {
  String resp;
  const bool sawTag = sendAT("AT+CMQTTSTART", "+CMQTTSTART:", 12000, &resp);
  if (!sawTag) {
    if (resp.indexOf("ERROR") >= 0 && resp.indexOf("+CMQTTSTART:") < 0) {
      return 1;
    }
    Serial.printf("[cmqtt] CMQTTSTART no result: %s\n", resp.c_str());
    return 2;
  }
  if (cmqttResult(resp, "+CMQTTSTART:") != 0) {
    Serial.printf("[cmqtt] CMQTTSTART err: %s\n", resp.c_str());
    if (resp.indexOf("*ATREADY") >= 0 || resp.indexOf("PB DONE") >= 0 ||
        resp.indexOf("SMS DONE") >= 0) {
      modemRebootUrcSeen = true;
    }
    return 2;
  }
  cmqttServiceUp = true;
  return 0;
}

static void cmqttForceRelease() {
  // OTA reboots the ESP32 and clears our flags, but the modem keeps the CMQTT
  // client it acquired on the previous firmware. cmqttTearDown() then sends
  // nothing (it trusts the flags). A bare AT+CMQTTSTOP answers ERROR while that
  // client is still held, and the next AT+CMQTTSTART stays ERROR — the 0.2.87
  // field log, radio Online the whole time. DISC, then REL, then STOP, results
  // ignored: a client that was never connected just answers ERROR.
  sendAT("AT+CMQTTDISC=0,60", "+CMQTTDISC:", 10000);
  sendAT("AT+CMQTTREL=0", "OK", 5000);
  sendAT("AT+CMQTTSTOP", "+CMQTTSTOP:", 12000);
  cmqttBrokerUp = false;
  cmqttClientAcquired = false;
  cmqttServiceUp = false;
  cmqttResetAssembler();
}

static void cmqttWaitForModemBoot() {
  // *ATREADY is followed by +CPIN, SMS DONE, +CGEV and finally PB DONE. CMQTTSTART
  // before PB DONE answers +CMQTTSTART: 1.
  Serial.println("[cmqtt] modem booting, waiting for PB DONE");
  const uint32_t startedAt = millis();
  while (millis() - startedAt < 15000) {
    feedWatchdog();
    if (!ModemSerial.available()) {
      delay(50);
      continue;
    }
    const String resp = readModemUntil(2000, "PB DONE");
    if (resp.indexOf("PB DONE") >= 0) {
      break;
    }
  }
  cmqttResetAssembler();
  modemRebootUrcSeen = false;
}

static int cmqttDial(const String& serverAddr, const String& username, const String& password) {
  String connectCmd = "AT+CMQTTCONNECT=0,\"" + serverAddr + "\"," +
                      String(kMqttKeepAliveSeconds) + ",1";
  if (username.length() > 0) {
    connectCmd += ",\"" + username + "\",\"" + password + "\"";
  }
  String resp;
  if (!sendAT(connectCmd, "+CMQTTCONNECT:", 25000, &resp)) {
    appendModemLogForced("CONNECT " + serverAddr + " -> no result");
    return -1;
  }
  const int code = cmqttResult(resp, "+CMQTTCONNECT:");
  appendModemLogForced("CONNECT " + serverAddr + " -> " + String(code));
  return code;
}

// CMQTTSTART can answer 0 before CID 1 has an address. CONNECT in that window
// is +CMQTTCONNECT: 0,3 (sock connect fail), which is what 0.2.88 logged twice
// while the broker itself was reachable.
static bool cmqttWaitForPdp() {
  for (int i = 0; i < 6; i++) {
    String resp;
    String ip;
    if (sendAT("AT+CGPADDR=1", "OK", 3000, &resp) && parseLteIp(resp, ip) && looksLikeIp(ip)) {
      state.lteIpAddress = ip;
      appendModemLogForced("PDP " + ip);
      return true;
    }
    waitWithWatchdog(1000);
  }
  appendModemLogForced("PDP no ip");
  return false;
}

bool cmqttConnect(const String& clientId, const String& willTopic, const String& willPayload,
                  const String& host, int port, const String& username, const String& password) {
  cmqttTearDown();

  if (!cmqttServiceUp) {
    int started = cmqttTryStart();
    if (started == 1) {
      // Bare ERROR: the service survived the ESP32 reboot. STOP alone answers
      // ERROR while the old client is still acquired, so release it first.
      Serial.println("[cmqtt] service still up on the modem, releasing it");
      cmqttForceRelease();
      started = cmqttTryStart();
    } else if (started == 2) {
      // Code 1 / timeout. Do not STOP: on a module that just rebooted, STOP
      // collides with the boot URCs and resets it again.
      if (modemRebootUrcSeen) {
        cmqttWaitForModemBoot();
      } else {
        waitWithWatchdog(2000);
      }
      started = cmqttTryStart();
    }
    if (started != 0) {
      return false;
    }
  }

  if (!cmqttClientAcquired) {
    if (!sendAT("AT+CMQTTACCQ=0,\"" + clientId + "\"", "OK", 5000)) {
      Serial.println("[cmqtt] CMQTTACCQ fail");
      return false;
    }
    cmqttClientAcquired = true;
  }

  // Last will so the broker marks us offline when the modem drops silently.
  if (willTopic.length() > 0 && willPayload.length() > 0) {
    cmqttPromptWrite(String("AT+CMQTTWILLTOPIC=0,") + String(willTopic.length()), willTopic, 5000);
    cmqttPromptWrite(String("AT+CMQTTWILLMSG=0,") + String(willPayload.length()) + ",1", willPayload, 5000);
  }

  if (!cmqttWaitForPdp()) {
    return false;
  }

  // Dial the address Ethernet already reached when we have it. The modem's
  // resolver is a separate step: a hostname CONNECT that dies with code 3
  // (sock connect fail, not 25 DNS error) never opens TCP, even when the
  // broker answers from the LAN.
  String cached;
  if (static_cast<uint32_t>(cachedMqttIp) != 0) {
    cached = cachedMqttIp.toString();
  }
  const String primary = cached.length() > 0 ? cached : host;
  const String secondary = (cached.length() > 0 && cached != host) ? host : String("");

  int code = cmqttDial("tcp://" + primary + ":" + String(port), username, password);
  if (code == 3) {
    // Same client, no STOP. Tearing the service down and dialing again at once
    // is the second identical code 3 in the 0.2.88 log.
    waitWithWatchdog(3000);
    code = cmqttDial("tcp://" + primary + ":" + String(port), username, password);
  }
  if (code == 3 && secondary.length() > 0) {
    code = cmqttDial("tcp://" + secondary + ":" + String(port), username, password);
  }
  if (code == 3) {
    String resp;
    if (sendAT("AT+CDNSGIP=\"" + host + "\"", "+CDNSGIP:", 10000, &resp)) {
      const String resolved = nthQuoted(resp, 2);
      if (looksLikeIp(resolved) && resolved != primary) {
        appendModemLogForced("DNS " + resolved);
        code = cmqttDial("tcp://" + resolved + ":" + String(port), username, password);
      }
    }
  }
  if (code != 0) {
    Serial.printf("[cmqtt] CMQTTCONNECT code %d\n", code);
    return false;
  }

  cmqttBrokerUp = true;
  Serial.println("[cmqtt] connected");
  return true;
}

// Length form (the one this A7672 accepts): AT+CMQTTSUB=0,<len>,<qos> then the
// raw topic, then an async +CMQTTSUB: 0,<code>. OK alone is not success — the
// result arrives after OK, and code 14 means "client is busy".
static int cmqttSubscribeLength(const String& topic, uint8_t qos) {
  const String cmd = String("AT+CMQTTSUB=0,") + String(topic.length()) + "," + String(qos);
  if (!sendAT(cmd, ">", 5000)) {
    return -1;   // no prompt: this firmware wants the other form
  }
  ModemSerial.write(reinterpret_cast<const uint8_t*>(topic.c_str()), topic.length());
  const String resp = readModemUntil(8000, "+CMQTTSUB:");
  const int code = cmqttResult(resp, "+CMQTTSUB:");
  appendModemLogForced("SUB " + topic + " -> " + String(code));
  return code;
}

bool cmqttSubscribe(const String& topic, uint8_t qos) {
  if (!cmqttBrokerUp) {
    return false;
  }
  // Finish whatever the previous SUB already started pushing (retained config)
  // before touching the UART again.
  cmqttDrainInbound(200, 8000);

  int code = cmqttSubscribeLength(topic, qos);
  if (code == 14) {
    // Busy with the previous delivery. Let it finish, then retry once.
    Serial.printf("[cmqtt] SUB %s busy (14), draining and retrying\n", topic.c_str());
    cmqttDrainInbound(200, 8000);
    code = cmqttSubscribeLength(topic, qos);
  }
  if (code < 0) {
    // Length form gave no '>'. Only then try the parameter form. On this
    // module the parameter form answers ERROR (argtopic is edit-mode) and
    // sending it while a delivery is in flight is what corrupted the config.
    const String paramCmd = "AT+CMQTTSUB=0,\"" + topic + "\"," + String(qos);
    String resp;
    if (sendAT(paramCmd, "+CMQTTSUB:", 8000, &resp)) {
      code = cmqttResult(resp, "+CMQTTSUB:");
    }
    appendModemLogForced("SUB " + topic + " param -> " + String(code));
  }

  // The retained publish follows +CMQTTSUB: 0,0 immediately. Absorb it before
  // the caller issues another AT command.
  cmqttDrainInbound(250, 8000);

  const bool ok = code == 0;
  Serial.printf("[cmqtt] SUB %s ok=%s code=%d\n", topic.c_str(), ok ? "yes" : "no", code);
  return ok;
}

bool cmqttPublish(const String& topic, const uint8_t* payload, size_t len, bool retained, uint8_t qos) {
  if (!cmqttBrokerUp) {
    return false;
  }
  // Topic first, then payload, then the actual PUB. The modem clears topic and
  // payload after each PUB (per the app note), so both must be set every time.
  if (!cmqttPromptWrite(String("AT+CMQTTTOPIC=0,") + String(topic.length()), topic, 5000)) {
    cmqttBrokerUp = false;
    return false;
  }
  String body;
  body.reserve(len);
  for (size_t i = 0; i < len; i++) {
    body += static_cast<char>(payload[i]);
  }
  if (!cmqttPromptWrite(String("AT+CMQTTPAYLOAD=0,") + String(len), body, 5000)) {
    cmqttBrokerUp = false;
    return false;
  }
  String resp;
  // A76XX: AT+CMQTTPUB=<client>,<qos>,<pub_timeout>[,<retained>[,<dup>]]
  const String pubCmd = String("AT+CMQTTPUB=0,") + String(qos) + ",60," +
                        String(retained ? 1 : 0);
  if (!sendAT(pubCmd, "+CMQTTPUB:", 20000, &resp)) {
    cmqttBrokerUp = false;
    return false;
  }
  if (cmqttResult(resp, "+CMQTTPUB:") != 0) {
    Serial.printf("[cmqtt] CMQTTPUB err: %s\n", resp.c_str());
    cmqttBrokerUp = false;
    return false;
  }
  return true;
}

void cmqttDisconnect() {
  // Graceful disconnect, keeps the service + client so the next connect is fast.
  if (cmqttBrokerUp) {
    String resp;
    sendAT("AT+CMQTTDISC=0,120", "+CMQTTDISC:", 15000, &resp);
  }
  cmqttBrokerUp = false;
}

void cmqttTearDown() {
  // The module already rebooted (*ATREADY). Its CMQTT state is gone. DISC/REL/
  // STOP during the boot URCs is the loop: STOP resets it again, the next
  // START answers +CMQTTSTART: 1, and we STOP once more.
  if (modemRebootUrcSeen) {
    Serial.println("[cmqtt] modem already rebooted, skipping DISC/REL/STOP");
    cmqttBrokerUp = false;
    cmqttClientAcquired = false;
    cmqttServiceUp = false;
    cmqttResetAssembler();
    return;
  }
  // Full teardown back to a clean slate. DISC is issued unconditionally (when a
  // client was ever acquired): our cmqttBrokerUp flag can be false while the
  // modem still holds the broker connection - a failed publish or a route bounce
  // clears only our flag, not the module's socket. Skipping DISC in that state
  // made REL answer ERROR ("client is busy") and STOP answer ERROR too, because
  // the module refuses to release a connected client. The DISC result is ignored:
  // a client that never connected just answers ERROR, which is harmless.
  if (cmqttClientAcquired) {
    sendAT("AT+CMQTTDISC=0,120", "+CMQTTDISC:", 15000);
  }
  cmqttBrokerUp = false;
  if (cmqttClientAcquired) {
    sendAT("AT+CMQTTREL=0", "OK", 5000);
    cmqttClientAcquired = false;
  }
  if (cmqttServiceUp) {
    sendAT("AT+CMQTTSTOP", "+CMQTTSTOP:", 12000);
    cmqttServiceUp = false;
  }
  // Reset the inbound assembler so a half-read message never leaks across a
  // reconnect.
  cmqttResetAssembler();
}

// Dispatch a completed inbound message into the shared callback. onMqttMessage
// only reads the buffers, so the const casts are safe.
static void cmqttDispatchMessage() {
  const String topic = cmqttRxTopic;
  const String payload = cmqttRxPayload;
  Serial.printf("[mqtt] message topic=%s payload=%s\n", topic.c_str(), payload.c_str());
  setStatus("MQTT msg");
  char topicBuf[256];
  topic.toCharArray(topicBuf, sizeof(topicBuf));
  onMqttMessage(topicBuf,
                reinterpret_cast<byte*>(const_cast<char*>(payload.c_str())),
                static_cast<unsigned int>(payload.length()));
}

void cmqttLoop() {
  if (!cmqttServiceUp) {
    return;
  }

  String line;
  while (cmqttReadLine(line)) {
    // Only CMQTT URCs are logged: an inbound command could vanish silently
    // because the parser handled just RXSTART/TOPIC/PAYLOAD/END and discarded
    // everything else. Surfacing any *other* +CMQTT* form (e.g. +CMQTTRECV)
    // makes an unexpected delivery shape visible in the event's modem_log
    // instead of dropping it. Filtering here also keeps the plain AT-response
    // bytes that readModemUntil() mirrors in from becoming log noise.
    if (line.startsWith("+CMQTT")) {
      appendModemLogForced(line.substring(0, 120));
    }
    // Passive loss of the connection / network. Both must force a reconnect.
    if (line.startsWith("+CMQTTCONNLOST")) {
      cmqttBrokerUp = false;
      continue;
    }
    if (line.startsWith("+CMQTTNONET")) {
      cmqttBrokerUp = false;
      ltePdpDown = true;   // the network library died: rebuild the PDP
      continue;
    }

    if (line.startsWith("+CMQTTRXSTART:")) {
      // +CMQTTRXSTART: <client>,<topic_total>,<payload_total>
      int c1 = line.indexOf(',');
      int c2 = c1 >= 0 ? line.indexOf(',', c1 + 1) : -1;
      cmqttRxTopicTotal = c2 >= 0 ? line.substring(c2 + 1).toInt() : 0;
      cmqttRxPayloadTotal = -1;
      if (c1 >= 0 && c2 > c1) {
        cmqttRxTopicTotal = line.substring(c1 + 1, c2).toInt();
        cmqttRxPayloadTotal = line.substring(c2 + 1).toInt();
      }
      cmqttRxTopic = "";
      cmqttRxPayload = "";
      cmqttRxActive = true;
      cmqttRxInPayload = false;
      continue;
    }

    // Alternate single-line delivery form used by the MQTT-EX firmware:
    // +CMQTTRECV: <client>,"<topic>",<payload_len>,"<payload>"
    // Parsed positionally, not with nthQuoted: the payload is JSON and contains
    // its own quotes, so only the first topic-quote pair and the final quote are
    // meaningful delimiters.
    if (line.startsWith("+CMQTTRECV:")) {
      const int topicStart = line.indexOf('"');
      const int topicEnd = topicStart >= 0 ? line.indexOf('"', topicStart + 1) : -1;
      if (topicStart >= 0 && topicEnd > topicStart) {
        const int payloadStart = line.indexOf('"', topicEnd + 1);
        const int payloadEnd = line.lastIndexOf('"');
        cmqttRxTopic = line.substring(topicStart + 1, topicEnd);
        if (payloadStart >= 0 && payloadEnd > payloadStart) {
          cmqttRxPayload = line.substring(payloadStart + 1, payloadEnd);
        } else {
          cmqttRxPayload = "";
        }
        cmqttDispatchMessage();
      }
      cmqttRxTopic = "";
      cmqttRxPayload = "";
      continue;
    }

    if (!cmqttRxActive) {
      continue;   // any other URC while idle is irrelevant to MQTT RX
    }

    if (line.startsWith("+CMQTTRXTOPIC:")) {
      // +CMQTTRXTOPIC: <client>,<sub_topic_len>  then <sub_topic_len> raw bytes
      const int comma = line.lastIndexOf(',');
      const int subLen = comma >= 0 ? line.substring(comma + 1).toInt() : 0;
      if (subLen > 0) {
        String chunk;
        if (cmqttReadExact(chunk, subLen)) {
          cmqttRxTopic += chunk;
        }
      }
      if (cmqttRxTopic.length() >= cmqttRxTopicTotal) {
        cmqttRxInPayload = true;
      }
      continue;
    }

    if (line.startsWith("+CMQTTRXPAYLOAD:")) {
      const int comma = line.lastIndexOf(',');
      const int subLen = comma >= 0 ? line.substring(comma + 1).toInt() : 0;
      if (subLen > 0) {
        String chunk;
        if (cmqttReadExact(chunk, subLen)) {
          cmqttRxPayload += chunk;
        }
      }
      continue;
    }

    if (line.startsWith("+CMQTTRXEND:")) {
      // Drop a short read. Interleaving an AT command into the burst (0.2.86)
      // delivered a truncated config and the firmware applied it as
      // "config v0 rejected: invalid JSON".
      const bool topicOk = cmqttRxTopicTotal <= 0 ||
                           cmqttRxTopic.length() == cmqttRxTopicTotal;
      const bool payloadOk = cmqttRxPayloadTotal < 0 ||
                             static_cast<int>(cmqttRxPayload.length()) == cmqttRxPayloadTotal;
      if (topicOk && payloadOk && cmqttRxTopic.length() > 0) {
        cmqttDispatchMessage();
      } else {
        Serial.printf("[cmqtt] drop rx topic=%u/%d payload=%u/%d\n",
                      static_cast<unsigned>(cmqttRxTopic.length()), cmqttRxTopicTotal,
                      static_cast<unsigned>(cmqttRxPayload.length()), cmqttRxPayloadTotal);
        appendModemLogForced("RX drop short");
      }
      cmqttRxActive = false;
      cmqttRxInPayload = false;
      cmqttRxTopic = "";
      cmqttRxPayload = "";
      continue;
    }
  }
}

// ---------------------------------------------------------------------------
// Silent-death supervision (see kLteHealthProbeMs in cof_config.h).
//
// While MQTT rides the modem's native CMQTT stack, pollModem() and
// refreshCellularStatus() are gated off: their AT chatter would steal inbound
// +CMQTTRX URC bytes. The designed loss signals are +CMQTTCONNLOST / +CMQTTNONET,
// but a radio that dies *silently* emits neither and leaves the device "connected"
// over a dead bearer. One cheap AT+CSQ per kLteHealthProbeMs catches +CSQ: 99,99
// (no service); once it persists kModemNoServiceGraceMs we tear the PDP down,
// which drops lteMqttTransport so pollModem()'s radio-recovery ladder takes over.
//
// URC safety: cmqttLoop() drains anything already buffered before we touch the
// UART, and the probe is skipped while a message is mid-assembly. The residual
// risk of a command arriving inside the ~200 ms probe window is accepted - the
// backend publishes commands at QoS 1 and re-delivers.
void serviceLteMqttHealth() {
  if (!state.lteMqttTransport || !cmqttIsConnected()) {
    lteNoServiceSinceMs = 0;
    return;
  }
  cmqttLoop();   // dispatch anything already buffered before touching the UART
  const uint32_t now = millis();
  if (now - lastLteHealthProbeMs < kLteHealthProbeMs) {
    return;
  }
  if (cmqttIsRxBusy()) {
    return;      // a message is mid-delivery; do not interleave AT here
  }
  lastLteHealthProbeMs = now == 0 ? 1 : now;

  String resp;
  if (!sendAT("AT+CSQ", "OK", 2000, &resp)) {
    return;      // no answer; the reconnect path / silence watchdog react
  }
  const int marker = resp.indexOf("+CSQ:");
  const int csq = marker >= 0 ? resp.substring(marker + 5).toInt() : -1;
  state.signalQuality = csq;

  if (csq == 99) {   // +CSQ: 99,99 = no service
    if (lteNoServiceSinceMs == 0) {
      lteNoServiceSinceMs = now == 0 ? 1 : now;
    } else if (now - lteNoServiceSinceMs >= kModemNoServiceGraceMs) {
      Serial.println("[lte] CSQ 99,99 sustained: releasing PDP for radio recovery");
      lteNoServiceSinceMs = 0;
      noServiceSinceMs = 1;   // arm pollModem()'s ladder to fire immediately
      stopLtePdp();           // drops lteMqttTransport so pollModem() runs
    }
    return;
  }
  lteNoServiceSinceMs = 0;
}
