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
static void cmqttDropAfterReboot();

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
// -2 means "no +CMQTTSUB result seen yet". Set from the URC, including when
// readModemUntil already swallowed it into the prompt response.
static int cmqttPendingSubCode = -2;

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
  if (modemUartDebug) {
    if (data.length() > kModemUartDebugLineMax) {
      appendModemLogForced(String(">> [raw ") + String(data.length()) + "b]");
    } else {
      appendModemLog('>', data);
    }
  }
  const String resp = readModemUntil(timeoutMs, "OK");
  appendModemLog('<', resp);
  const int subAt = resp.indexOf("+CMQTTSUB:");
  if (subAt >= 0) {
    cmqttPendingSubCode = cmqttResult(resp.substring(subAt), "+CMQTTSUB:");
  }
  return resp.indexOf("OK") >= 0;
}

bool cmqttIsConnected() {
  return cmqttBrokerUp;
}

bool cmqttServiceIsUp() {
  // "CMQTTSTART succeeded and we have not stopped it". Lets the reconnect path
  // tell a CONNECT that failed over a dead bearer (service up, worth a CGACT)
  // from a CMQTTSTART that never came up (CMQTTSTART dials the bearer itself).
  return cmqttServiceUp;
}

// Soft PUB failures (TOPIC/PAYLOAD/no URC) while we still think the broker is up.
// Two in a row → ask the module whether the session is actually alive, then
// either cool publishing down (alive) or clear our flags so the next pass can
// CONNECT again (dead). DISC (0.2.106) and CONNECT-without-DISC on a live
// session (0.2.107) both rebooted this A7672 (*ATREADY), so neither is issued
// on a session that might still be up.
static uint8_t cmqttPubSoftFails = 0;
static uint32_t cmqttPubCooldownUntilMs = 0;

// Read-only liveness check of the module's own MQTT client, so a recovery does
// not have to guess. `AT+CMQTTCONNECT?` answers `+CMQTTCONNECT: 0,"tcp://host:port"`
// for a connected client and `+CMQTTCONNECT: 0,""` - or lists no client 0 at all
// once the module has reset its CMQTT stack - when it is not. It changes nothing
// on the module, which is the whole point: DISC (0.2.106) and a blind CONNECT on
// a live session (0.2.107) are both reboot triggers here.
enum CmqttLinkState { kCmqttLinkUnknown, kCmqttLinkUp, kCmqttLinkDown };

static CmqttLinkState cmqttProbeBrokerLink() {
  String resp;
  if (!sendAT("AT+CMQTTCONNECT?", "OK", 5000, &resp)) {
    // sendAT refuses AT+CMQTT* after *ATREADY, and a rebooted module has no
    // session left either way.
    return modemRebootUrcSeen ? kCmqttLinkDown : kCmqttLinkUnknown;
  }
  const int at = resp.indexOf("+CMQTTCONNECT: 0,");
  if (at < 0) {
    return kCmqttLinkDown;
  }
  const int lineEnd = resp.indexOf('\n', at);
  const String line = lineEnd < 0 ? resp.substring(at) : resp.substring(at, lineEnd);
  return line.indexOf("://") >= 0 ? kCmqttLinkUp : kCmqttLinkDown;
}

static void cmqttNotePubSoftFail(const char* why) {
  cmqttPubSoftFails++;
  appendModemLogForced(String(why) + " (" + String(cmqttPubSoftFails) + "/" +
                        String(kCmqttPubSoftFailLimit) + ")");
  if (cmqttPubSoftFails < kCmqttPubSoftFailLimit) {
    return;
  }
  cmqttPubSoftFails = 0;

  // Two failures in a row are either a busy module or a session that is already
  // gone, and the two need opposite treatment. 0.2.108 assumed "busy" and only
  // ever cooled down, so a dead session parked publishing forever: the broker
  // kept the device "online" until the 6-minute silence watchdog restarted the
  // ESP32, and that restart met the stale CMQTT service, sent CMQTTSTOP and
  // rebooted the modem too. Ask the module which case this is.
  const CmqttLinkState link = cmqttProbeBrokerLink();
  if (modemRebootUrcSeen) {
    noteLteSessionDrop("atready");
    cmqttDropAfterReboot();
    appendModemLogForced("PUB soft fail: module rebooted, re-init");
    return;
  }
  if (link == kCmqttLinkDown) {
    // Flags only - no DISC/REL/STOP. The module says there is no session to
    // release, so the next pass may CMQTTCONNECT (the manual's own recovery)
    // without the risk that made 0.2.107 reboot the A7672.
    noteLteSessionDrop("pub-dead");
    cmqttBrokerUp = false;
    cmqttPubCooldownUntilMs = 0;
    appendModemLogForced("PUB soft fail: module reports no session, reconnecting");
    Serial.println("[cmqtt] soft PUB fail limit: module reports no session, reconnecting");
    return;
  }
  // Still connected (or the probe was inconclusive): keep the session as 0.2.108.
  cmqttPubCooldownUntilMs = millis() + kCmqttPubSoftCooldownMs;
  if (cmqttPubCooldownUntilMs == 0) {
    cmqttPubCooldownUntilMs = 1;
  }
  appendModemLogForced("PUB soft fail limit, cooldown " +
                        String(kCmqttPubSoftCooldownMs / 1000UL) + "s (session live)");
  Serial.printf("[cmqtt] soft PUB fail limit: cooldown %lu s, session kept\n",
                static_cast<unsigned long>(kCmqttPubSoftCooldownMs / 1000UL));
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
static bool cmqttStartService(bool* alreadyRunning = nullptr) {
  if (alreadyRunning != nullptr) {
    *alreadyRunning = false;
  }
  String resp;
  if (!sendAT("AT+CMQTTSTART", "+CMQTTSTART:", 12000, &resp)) {
    // Tell the two failures apart. A bare ERROR with no result line is the
    // module saying the service is already running (our reboot, not its own);
    // a timeout or a "+CMQTTSTART: <err>" is a real failure to start.
    if (alreadyRunning != nullptr && resp.indexOf("ERROR") >= 0 &&
        resp.indexOf("+CMQTTSTART:") < 0) {
      *alreadyRunning = true;
    }
    return false;
  }
  if (cmqttResult(resp, "+CMQTTSTART:") != 0) {
    Serial.printf("[cmqtt] CMQTTSTART err: %s\n", resp.c_str());
    return false;
  }
  cmqttServiceUp = true;
  return true;
}

// The CMQTT service survived an ESP32 reboot. Read back what the module holds
// instead of stopping it: AT+CMQTTACCQ? lists the acquired clients and
// cmqttProbeBrokerLink() says whether client 0 is still connected. Both are read
// commands, so nothing here can trigger the *ATREADY that CMQTTSTOP does.
enum CmqttAdoptResult {
  kCmqttAdoptFailed,    // module would not say; caller falls back to STOP
  kCmqttAdoptService,   // service (and maybe the client) is ours again; CONNECT next
  kCmqttAdoptSession,   // client 0 is still connected: nothing left to do
};

static CmqttAdoptResult cmqttAdoptRunningService(const String& clientId) {
  String resp;
  if (!sendAT("AT+CMQTTACCQ?", "OK", 5000, &resp)) {
    return kCmqttAdoptFailed;
  }
  cmqttServiceUp = true;
  const int at = resp.indexOf("+CMQTTACCQ: 0,");
  if (at < 0) {
    // Service running, no client 0: the ACCQ below creates it as usual.
    appendModemLogForced("adopted CMQTT service (no client)");
    return kCmqttAdoptService;
  }
  const int lineEnd = resp.indexOf('\n', at);
  const String line = lineEnd < 0 ? resp.substring(at) : resp.substring(at, lineEnd);
  if (line.indexOf(clientId) < 0) {
    // Someone else's client id on index 0 (should not happen: it is derived from
    // the eFuse MAC). Publishing under it would break the broker ACL, so let the
    // caller take the expensive STOP path.
    appendModemLogForced("CMQTT client id mismatch on index 0");
    cmqttServiceUp = false;
    return kCmqttAdoptFailed;
  }
  // Re-issuing ACCQ for an existing index answers ERROR, so take the client too.
  cmqttClientAcquired = true;
  const CmqttLinkState link = cmqttProbeBrokerLink();
  if (link == kCmqttLinkDown) {
    appendModemLogForced("adopted CMQTT client, no session");
    return kCmqttAdoptService;
  }
  if (link == kCmqttLinkUnknown) {
    // The module would not say. Continuing would CMQTTCONNECT on a client that
    // may still be connected - the 0.2.107 reboot trigger. "Unknown" is treated
    // the same everywhere in this file: never act on it as if it meant "dead".
    // Fall back to the STOP path, which is the pre-0.2.109 behaviour.
    appendModemLogForced("CMQTT link unknown, cannot adopt");
    cmqttClientAcquired = false;
    cmqttServiceUp = false;
    return kCmqttAdoptFailed;
  }
  // Live session with our client id, and the module keeps its subscriptions
  // across our reboot: resume it. This is what turns a silence-watchdog restart
  // from a ~2 min modem reboot into a few seconds.
  cmqttBrokerUp = true;
  appendModemLogForced("adopted live CMQTT session");
  Serial.println("[cmqtt] adopted live CMQTT session (no STOP, no reconnect)");
  return kCmqttAdoptSession;
}

bool cmqttConnect(const String& clientId, const String& willTopic, const String& willPayload,
                  const String& host, int port, const String& username, const String& password) {
  if (modemRebootUrcSeen) {
    cmqttDropAfterReboot();
    return false;
  }
  // A dropped broker link (CONNLOST, or our flag cleared) still has the CMQTT
  // service and client. The manual says to CMQTTCONNECT again. DISC+REL+STOP
  // here releases the PDP CMQTTSTART dialed — new IP every time — and on this
  // module that teardown is what rebooted it (*ATREADY) about once a minute.
  if (!(cmqttServiceUp && cmqttClientAcquired)) {
    cmqttTearDown();
    if (modemRebootUrcSeen) {
      return false;
    }
  }

  if (!cmqttServiceUp) {
    bool alreadyRunning = false;
    if (!cmqttStartService(&alreadyRunning)) {
      // A bare ERROR from CMQTTSTART means "service already started": the modem
      // kept its CMQTT state across an ESP32 reboot (neither OTA nor the silence
      // watchdog resets the modem). Adopt what is already there instead of
      // stopping it - CMQTTSTOP is the command that rebooted this A7672 on every
      // such boot, and each of those reboots costs a full re-registration, which
      // is what eventually left the radio wedged.
      const CmqttAdoptResult adopted = (alreadyRunning && !modemRebootUrcSeen)
                                           ? cmqttAdoptRunningService(clientId)
                                           : kCmqttAdoptFailed;
      if (adopted == kCmqttAdoptSession) {
        return true;
      }
      if (adopted == kCmqttAdoptFailed) {
        Serial.println("[cmqtt] CMQTTSTART failed; stopping stale service and retrying");
        if (modemRebootUrcSeen) {
          cmqttDropAfterReboot();
          return false;
        }
        sendAT("AT+CMQTTSTOP", "+CMQTTSTOP:", 12000);
        if (modemRebootUrcSeen) {
          cmqttDropAfterReboot();
          return false;
        }
        if (!cmqttStartService()) {
          if (modemRebootUrcSeen) {
            cmqttDropAfterReboot();
          }
          return false;
        }
      }
    }
  }

  if (!cmqttClientAcquired) {
    if (!sendAT("AT+CMQTTACCQ=0,\"" + clientId + "\"", "OK", 5000)) {
      Serial.println("[cmqtt] CMQTTACCQ fail");
      if (modemRebootUrcSeen) {
        cmqttDropAfterReboot();
      }
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
    if (modemRebootUrcSeen) {
      cmqttDropAfterReboot();
    }
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
  // The async +CMQTTSUB arrives after the prompt OK. On this module a retained
  // message (config/desired, ~2 KB) starts streaming in the same window. A
  // second SUB while that is in flight gets +CMQTTSUB: 0,14 (client is busy)
  // and the topic never subscribes — that is why devices/.../command was deaf
  // and test_sms never produced a command_ack. Same AT as before; we just wait
  // the result out, and if an RX starts, drain it to +CMQTTRXEND before returning.
  cmqttPendingSubCode = -2;
  const String cmd = String("AT+CMQTTSUB=0,") + String(topic.length()) + "," + String(qos);
  if (!cmqttPromptWrite(cmd, topic, 5000)) {
    appendModemLogForced("SUB prompt fail " + topic);
    return false;
  }
  // Ready is the URC, not a pause: +CMQTTSUB is the subscribe result, and a
  // retained message that started must reach +CMQTTRXEND before the next AT.
  // 12 s is only the give-up if that URC never arrives. delay(5) is the poll
  // between UART reads while it is still outstanding.
  const uint32_t startedAt = millis();
  while (millis() - startedAt < 12000) {
    feedWatchdog();
    cmqttLoop();
    if (cmqttPendingSubCode != -2 && !cmqttRxActive && !ModemSerial.available() &&
        cmqttLineBuf.length() == 0) {
      appendModemLogForced("SUB result " + String(cmqttPendingSubCode) + " " + topic);
      return cmqttPendingSubCode == 0;
    }
    delay(5);
  }
  appendModemLogForced("SUB result timeout " + topic);
  return false;
}

bool cmqttPublish(const String& topic, const uint8_t* payload, size_t len, bool retained, uint8_t qos) {
  if (!cmqttBrokerUp) {
    return false;
  }
  if (cmqttPubCooldownUntilMs != 0) {
    const uint32_t now = millis();
    if (now < cmqttPubCooldownUntilMs) {
      return false;
    }
    cmqttPubCooldownUntilMs = 0;
  }
  // Topic first, then payload, then the actual PUB. The modem clears topic and
  // payload after each PUB (per the app note), so both must be set every time.
  if (!cmqttPromptWrite(String("AT+CMQTTTOPIC=0,") + String(topic.length()), topic, 5000)) {
    if (modemRebootUrcSeen || ltePdpDown) {
      cmqttBrokerUp = false;
      cmqttPubSoftFails = 0;
      cmqttPubCooldownUntilMs = 0;
    } else {
      cmqttNotePubSoftFail("PUB topic fail");
    }
    return false;
  }
  String body;
  body.reserve(len);
  for (size_t i = 0; i < len; i++) {
    body += static_cast<char>(payload[i]);
  }
  if (!cmqttPromptWrite(String("AT+CMQTTPAYLOAD=0,") + String(len), body, 5000)) {
    if (modemRebootUrcSeen || ltePdpDown) {
      cmqttBrokerUp = false;
      cmqttPubSoftFails = 0;
      cmqttPubCooldownUntilMs = 0;
    } else {
      cmqttNotePubSoftFail("PUB payload fail");
    }
    return false;
  }
  String resp;
  // A76XX: AT+CMQTTPUB=<client>,<qos>,<pub_timeout>[,<retained>[,<dup>]]
  // pub_timeout minimum is 60. sendAT gives up at 20s; a missing URC is not a
  // dead session. Treating it as one made the next loop STOP the service.
  const String pubCmd = String("AT+CMQTTPUB=0,") + String(qos) + ",60," +
                        String(retained ? 1 : 0);
  if (!sendAT(pubCmd, "+CMQTTPUB:", 20000, &resp)) {
    if (modemRebootUrcSeen) {
      noteLteSessionDrop("atready");
      cmqttBrokerUp = false;
      cmqttPubSoftFails = 0;
      cmqttPubCooldownUntilMs = 0;
    } else if (resp.indexOf("+CMQTTNONET") >= 0) {
      noteLteSessionDrop("nonet");
      cmqttBrokerUp = false;
      ltePdpDown = true;
      cmqttPubSoftFails = 0;
      cmqttPubCooldownUntilMs = 0;
    } else if (resp.indexOf("+CMQTTCONNLOST") >= 0) {
      noteLteSessionDrop("connlost");
      cmqttBrokerUp = false;
      cmqttPubSoftFails = 0;
      cmqttPubCooldownUntilMs = 0;
    } else {
      cmqttNotePubSoftFail("PUB no URC");
    }
    return false;
  }
  const int pubCode = cmqttResult(resp, "+CMQTTPUB:");
  if (pubCode != 0) {
    Serial.printf("[cmqtt] CMQTTPUB err: %s\n", resp.c_str());
    appendModemLogForced("PUB result " + String(pubCode));
    // 9 network not opened, 11 no connection, 26 socket closed by server.
    if (pubCode == 9 || pubCode == 11 || pubCode == 26) {
      noteLteSessionDrop("pub");
      cmqttBrokerUp = false;
      cmqttPubSoftFails = 0;
      cmqttPubCooldownUntilMs = 0;
    } else {
      cmqttNotePubSoftFail("PUB result");
    }
    return false;
  }
  cmqttPubSoftFails = 0;
  cmqttPubCooldownUntilMs = 0;
  return true;
}

static void cmqttDropAfterReboot() {
  cmqttServiceUp = false;
  cmqttClientAcquired = false;
  cmqttBrokerUp = false;
  cmqttPubSoftFails = 0;
  cmqttPubCooldownUntilMs = 0;
  cmqttLineBuf = "";
  cmqttRxActive = false;
  cmqttRxInPayload = false;
  cmqttRxTopic = "";
  cmqttRxPayload = "";
}

// Voice/SMS borrowed the UART and gave it back. Nothing was disconnected on the
// way in (DISC is one of the two commands that reboot this A7672), so the module
// normally still holds the session it keepalives by itself. Ask what survived
// instead of rebuilding: that is what used to cost a modem reboot on every test
// SMS and every alarm call (DISC on take + REL/STOP on release).
void cmqttResumeAfterUartHandover() {
  // Anything half-assembled when the UART was borrowed is unrecoverable: the raw
  // topic/payload bytes went to the voice/SMS reader. Drop it (QoS 1 means the
  // broker re-delivers) so cmqttLoop() does not resume inside a dead frame and
  // cmqttIsRxBusy() does not stay true forever, which would mute the CSQ probe.
  cmqttLineBuf = "";
  cmqttRxActive = false;
  cmqttRxInPayload = false;
  cmqttRxTopic = "";
  cmqttRxPayload = "";
  if (modemRebootUrcSeen) {
    // The module rebooted under the call/SMS (CFUN bounce for CSFB, or on its
    // own). cmqttTearDown() is flags-only in this state; pollModem() re-inits.
    cmqttTearDown();
    return;
  }
  if (!cmqttServiceUp || !cmqttBrokerUp) {
    return;   // nothing to resume; the normal connect path owns it from here
  }
  const CmqttLinkState link = cmqttProbeBrokerLink();
  if (modemRebootUrcSeen) {
    cmqttTearDown();
    return;
  }
  if (link == kCmqttLinkDown) {
    // A call longer than the keepalive, or a CSFB that dropped the bearer. Flags
    // only: the module says there is nothing to release, so the next pass may
    // CMQTTCONNECT without the blind-CONNECT risk of 0.2.107.
    noteLteSessionDrop("cs-handover");
    cmqttBrokerUp = false;
    cmqttPubSoftFails = 0;
    cmqttPubCooldownUntilMs = 0;
    appendModemLogForced("CS handover: session gone, will CONNECT");
    return;
  }
  // Live, or the module would not say. Either way leave it alone: a live session
  // resumes on the next pass, and an unknown one is settled by the soft-fail
  // probe on the first publish. Same rule as everywhere else - never act on
  // "unknown" as if it meant "dead".
  appendModemLogForced(link == kCmqttLinkUp ? "CS handover: session alive"
                                           : "CS handover: link unknown, kept");
}

void cmqttTearDown() {
  // A reboot already wiped the module's CMQTT client. Further DISC/REL/STOP
  // while it is still printing *ATREADY / SMS DONE is what left the radio in
  // NO SERVICE after clear_wifi.
  if (modemRebootUrcSeen) {
    cmqttDropAfterReboot();
    return;
  }
  // Full teardown back to a clean slate. This is the last resort, not a hot
  // path: LAN taking over for good, three failed CONNECTs, sustained CSQ 99,
  // the radio recovery ladder, or a credential wipe.
  //
  // DISC used to be issued unconditionally because our cmqttBrokerUp flag can be
  // false while the module still holds the connection, and skipping it made REL
  // and STOP answer ERROR ("client is busy"). That guess is no longer needed:
  // ask the module. DISC on a client that is NOT connected is what produced the
  // documented +CMQTTDISC: 0,11 → CMQTTREL ERROR → CMQTTSTOP-over-boot chain.
  if (cmqttClientAcquired) {
    const CmqttLinkState link = cmqttProbeBrokerLink();
    if (modemRebootUrcSeen) {
      cmqttDropAfterReboot();
      return;
    }
    if (link == kCmqttLinkDown) {
      appendModemLogForced("teardown: module reports no session, skip DISC");
    } else {
      sendAT("AT+CMQTTDISC=0,120", "+CMQTTDISC:", 15000);
      if (modemRebootUrcSeen) {
        cmqttDropAfterReboot();
        return;
      }
    }
  }
  cmqttBrokerUp = false;
  if (cmqttClientAcquired) {
    sendAT("AT+CMQTTREL=0", "OK", 5000);
    cmqttClientAcquired = false;
    if (modemRebootUrcSeen) {
      cmqttDropAfterReboot();
      return;
    }
  }
  if (cmqttServiceUp) {
    sendAT("AT+CMQTTSTOP", "+CMQTTSTOP:", 12000);
    cmqttServiceUp = false;
    if (modemRebootUrcSeen) {
      cmqttDropAfterReboot();
      return;
    }
  }
  // Reset the inbound assembler so a half-read message never leaks across a
  // reconnect.
  cmqttLineBuf = "";
  cmqttRxActive = false;
  cmqttRxInPayload = false;
  cmqttRxTopic = "";
  cmqttRxPayload = "";
  cmqttPubSoftFails = 0;
  cmqttPubCooldownUntilMs = 0;
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
  bool sawCmqtt = false;
  while (cmqttReadLine(line)) {
    // The module rebooted under a live session. This loop only ever inspected
    // `+CMQTT*` lines and dropped everything else, so it *consumed* the *ATREADY
    // and the reboot stayed invisible: the module's client was gone while
    // cmqttBrokerUp stayed true, every later PUB failed as a "soft" fail, and
    // nothing ever reconnected. Flag it exactly like readModemUntil() does and
    // let pollModem() run the re-init.
    if (textHasAtReady(line)) {
      appendModemLogForced(line);
      modemRebootUrcSeen = true;
      noteLteSessionDrop("atready");
      cmqttDropAfterReboot();
      captureLteUrcLog();
      return;
    }
    if (line.startsWith("+CMQTT")) {
      appendModemLogForced(line);
      sawCmqtt = true;
    }
    // Passive loss of the connection / network. Both must force a reconnect.
    if (line.startsWith("+CMQTTSUB:")) {
      cmqttPendingSubCode = cmqttResult(line, "+CMQTTSUB:");
      continue;
    }
    if (line.startsWith("+CMQTTCONNLOST")) {
      noteLteSessionDrop("connlost");
      cmqttBrokerUp = false;
      cmqttPubSoftFails = 0;
      cmqttPubCooldownUntilMs = 0;
      continue;
    }
    if (line.startsWith("+CMQTTNONET")) {
      // Broker/network lost the PDP link. Do NOT TearDown/STOP here: the carrier
      // often re-activates PDP (EPS PDN ACT) within seconds, and CMQTTSTOP on this
      // module triggers *ATREADY. Keep service+client; CONNECT again after backoff.
      noteLteSessionDrop("nonet");
      cmqttBrokerUp = false;
      ltePdpDown = true;
      cmqttPubSoftFails = 0;
      cmqttPubCooldownUntilMs = 0;
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
      appendModemLogForced("RX topic " + cmqttRxTopic);
      cmqttDispatchMessage();
      cmqttRxActive = false;
      cmqttRxInPayload = false;
      cmqttRxTopic = "";
      cmqttRxPayload = "";
      continue;
    }
  }
  if (sawCmqtt && !cmqttRxActive) {
    captureLteUrcLog();
  }
}

// ---------------------------------------------------------------------------
// Silent-death supervision (see kLteHealthProbeMs in cof_config.h).
//
// While MQTT rides the modem's native CMQTT stack, pollModem() and
// refreshCellularStatus() are gated off: their AT chatter would steal inbound
// +CMQTTRX URC bytes. The designed loss signals are +CMQTTCONNLOST / +CMQTTNONET,
// but a radio that dies *silently* emits neither and leaves the device "connected"
// over a dead bearer. AT+CSQ on kLteHealthProbeMs (minutes, not the telemetry
// period) catches +CSQ: 99,99; once it persists kModemNoServiceGraceMs we tear
// the PDP down, which drops lteMqttTransport so pollModem()'s ladder can run.
//
// URC safety: cmqttLoop() drains anything already buffered before we touch the
// UART, and the probe is skipped while a message is mid-assembly. The residual
// risk of a command arriving inside the ~200 ms probe window is accepted - the
// backend publishes commands at QoS 1 and re-delivers.
void serviceLteMqttHealth() {
  if (!modemUartOwnedByMqtt() || !state.lteMqttTransport || !cmqttIsConnected()) {
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
      noteLteSessionDrop("csq99");
      stopLtePdp();           // drops lteMqttTransport so pollModem() runs
    }
    return;
  }
  lteNoServiceSinceMs = 0;
}
