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

static bool cmqttReadLine(String& line) {
  // Non-blocking: returns true once a full '\n'-terminated line is assembled,
  // false if the buffer ran dry mid-line. The partial line lives in cmqttLineBuf
  // so the next call resumes where it left off.
  while (true) {
    if (!ModemSerial.available()) {
      return false;
    }
    const char c = static_cast<char>(ModemSerial.read());
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
  // Blocking, bounded: the data follows the URC header as a contiguous burst
  // already sitting in the UART FIFO, so this normally returns in ms. The 3 s
  // deadline is only a guard against a module that stalls mid-message.
  const uint32_t startedAt = millis();
  while (n > 0 && millis() - startedAt < 3000) {
    feedWatchdog();
    if (ModemSerial.available()) {
      out += static_cast<char>(ModemSerial.read());
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

// Start the MQTT service. Per the A76XX AT manual (ch.18), AT+CMQTTSTART
// activates the PDP context itself and answers "OK\r\n+CMQTTSTART: 0" on
// success; a bare ERROR means the service was already running.
static bool cmqttStartService() {
  String resp;
  if (!sendAT("AT+CMQTTSTART", "+CMQTTSTART:", 12000, &resp)) {
    return false;   // timeout, or bare ERROR ("already started")
  }
  if (cmqttResult(resp, "+CMQTTSTART:") != 0) {
    Serial.printf("[cmqtt] CMQTTSTART err: %s\n", resp.c_str());
    return false;
  }
  cmqttServiceUp = true;
  return true;
}

bool cmqttConnect(const String& clientId, const String& willTopic, const String& willPayload,
                  const String& host, int port, const String& username, const String& password) {
  cmqttTearDown();

  if (!cmqttServiceUp) {
    if (!cmqttStartService()) {
      // A bare ERROR from CMQTTSTART means "service already started" (the modem
      // kept its state across an ESP32 OTA reboot - OTA never resets the modem).
      // Stop the stale service and retry once.
      Serial.println("[cmqtt] CMQTTSTART failed; stopping stale service and retrying");
      sendAT("AT+CMQTTSTOP", "+CMQTTSTOP:", 12000);
      if (!cmqttStartService()) {
        return false;
      }
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

  String serverAddr = "tcp://" + host + ":" + String(port);
  String connectCmd = "AT+CMQTTCONNECT=0,\"" + serverAddr + "\"," +
                      String(kMqttKeepAliveSeconds) + ",1";
  if (username.length() > 0) {
    connectCmd += ",\"" + username + "\",\"" + password + "\"";
  }
  String resp;
  if (!sendAT(connectCmd, "+CMQTTCONNECT:", 20000, &resp)) {
    Serial.println("[cmqtt] CMQTTCONNECT no response");
    return false;
  }
  if (cmqttResult(resp, "+CMQTTCONNECT:") != 0) {
    Serial.printf("[cmqtt] CMQTTCONNECT err: %s\n", resp.c_str());
    return false;
  }

  cmqttBrokerUp = true;
  Serial.println("[cmqtt] connected to " + serverAddr);
  return true;
}

bool cmqttSubscribe(const String& topic, uint8_t qos) {
  if (!cmqttBrokerUp) {
    return false;
  }
  const String cmd = String("AT+CMQTTSUB=0,") + String(topic.length()) + "," + String(qos);
  if (!cmqttPromptWrite(cmd, topic, 5000)) {
    return false;
  }
  return true;
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
  cmqttLineBuf = "";
  cmqttRxActive = false;
  cmqttRxInPayload = false;
  cmqttRxTopic = "";
  cmqttRxPayload = "";
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
      cmqttDispatchMessage();
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
