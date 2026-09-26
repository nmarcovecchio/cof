#include "cof_config.h"
#include "cof_state.h"
#include <Arduino.h>

// ---------------------------------------------------------------------------
// Native MQTT-over-LTE using the A7672's built-in `AT+CMQTT*` service.
//
// This replaces the hand-rolled TCP socket emulation (AT+CIPOPEN / AT+CIPSEND /
// AT+CIPRXGET) for the cellular path. The module's own MQTT stack:
//   * activates the PDP context itself (AT+CMQTTSTART),
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

// Result code of a CMQTT command, e.g. "+CMQTTCONNECT: 0,0" -> 0. Returns -1
// when the tag (or the comma) is absent.
static int cmqttResult(const String& resp, const char* tag) {
  const int at = resp.indexOf(tag);
  if (at < 0) {
    return -1;
  }
  const int lineEnd = resp.indexOf('\n', at);
  const String line = lineEnd < 0 ? resp.substring(at) : resp.substring(at, lineEnd);
  const int comma = line.lastIndexOf(',');
  return comma >= 0 ? line.substring(comma + 1).toInt() : -1;
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

bool cmqttConnect(const String& clientId, const String& willTopic, const String& willPayload,
                  const String& host, int port, const String& username, const String& password) {
  cmqttTearDown();

  if (!cmqttServiceUp) {
    // Activates the PDP context itself; ~12 s worst case per the app note.
    String resp;
    if (!sendAT("AT+CMQTTSTART", "+CMQTTSTART:", 12000, &resp)) {
      Serial.println("[cmqtt] CMQTTSTART no response");
      return false;
    }
    if (cmqttResult(resp, "+CMQTTSTART:") != 0) {
      Serial.printf("[cmqtt] CMQTTSTART err: %s\n", resp.c_str());
      cmqttTearDown();
      return false;
    }
    cmqttServiceUp = true;
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
  const String pubCmd = String("AT+CMQTTPUB=0,") + String(qos) + "," +
                        String(retained ? 1 : 0) + ",60";
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
  // Full teardown: DISC (if still up) -> REL -> STOP, back to a clean slate.
  cmqttDisconnect();
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
