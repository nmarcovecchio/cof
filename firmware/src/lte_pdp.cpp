#include "cof_config.h"
#include "cof_state.h"
#include <Arduino.h>
#include <Client.h>

// Extracted verbatim from main.cpp, which used to hold every function

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
String resolveLteMqttPeer(const char* host) {
  // The cache is a shortcut, not an authority. It is learned from a LAN connect
  // and, while MQTT rides LTE, nothing refreshes it: the connect path only caches
  // when `!lteMqttTransport` and the resolver only adopts a changed answer while
  // MQTT is down. So a broker that moves leaves a stale address here forever, and
  // because this branch used to win over the module's own DNS, the LTE connect
  // kept dialing the dead IP - which took MQTT down entirely, not just the LAN
  // probes. That healed only through the 6-minute silence reboot.
  //
  // A failed connect is the signal that the address may be wrong, so the next
  // attempt asks the module's DNS instead. Kept as a flag rather than always
  // re-resolving because AT+CDNSGIP costs an AT round-trip (~10 s timeout) and
  // the cached value is correct in the common case.
  if (!lteForceDnsResolve && cachedMqttIp != IPAddress((uint32_t)0)) {
    return cachedMqttIp.toString();
  }
  String resp;
  if (sendAT(String("AT+CDNSGIP=\"") + host + "\"", "+CDNSGIP:", 10000, &resp)) {
    const String ip = lastQuoted(resp);
    if (ip.length() >= 7 && ip.indexOf('.') > 0 && ip != "0.0.0.0") {
      Serial.println("[lte] DNS " + ip);
      // Fresh answer from the module: make it the cache, so the LAN probes stop
      // testing whatever stale address they were on. This is the only path that
      // can repoint the cache while MQTT is up on LTE.
      IPAddress parsed;
      if (parsed.fromString(ip)) {
        cachedMqttIp = parsed;
      }
      return ip;
    }
  }
  // The module's DNS did not answer. Prefer the stale cached address over the bare
  // hostname: CIPOPEN wants an IP literal, so a hostname here would fail the open
  // outright, while a stale IP at least has a chance of still being right.
  if (cachedMqttIp != IPAddress((uint32_t)0)) {
    return cachedMqttIp.toString();
  }
  return String(host);
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
      // Unquoted. Two layouts exist:
      //   +IPADDR: 10.84.17.161          (address only)
      //   +CGPADDR: 1,10.83.214.110       (CID first, then address)
      // The CID is always the first comma-separated field, so take the LAST one.
      int start = idx + static_cast<int>(strlen(tags[i]));
      while (start < static_cast<int>(resp.length()) &&
             (resp[start] == ' ' || resp[start] == ':')) {
        start++;
      }
      int end = start;
      while (end < static_cast<int>(resp.length()) && resp[end] != '\r' && resp[end] != '\n') {
        end++;
      }
      String field = resp.substring(start, end);
      const int lastComma = field.lastIndexOf(',');
      if (lastComma >= 0) {
        field = field.substring(lastComma + 1);
      }
      ipOut = field;
    }
    ipOut.trim();
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
  ltePdpDown = false;
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

#if COF_LTE_MQTT_NATIVE
  // Native MQTT (AT+CMQTT*): per the A76XX AT manual ch.18, AT+CMQTTSTART
  // activates the PDP context itself, so there is no NETOPEN and no IP to query
  // here. Pin the APN/auth and mark the data path ready; the actual network
  // attach happens inside cmqttConnect via AT+CMQTTSTART. Running NETOPEN first
  // would fight the service's own PDP (result code 23 "network is opened").
  sendAT(String("AT+CGDCONT=1,\"IP\",\"") + COF_MODEM_APN + "\"", "OK", 5000);
  sendAT(String("AT+CGAUTH=1,1,\"") + COF_MODEM_APN_USER + "\",\"" + COF_MODEM_APN_PASS + "\"", "OK", 3000);
  state.ltePdpCid = 1;
  state.lteDataUp = true;
  state.lteIpAddress = "-";
  ltePdpDown = false;
  lastLteRetryDelayMs = kLteRetryIntervalMs;
  lteMqttConnectFails = 0;
  pendingNetworkStatusReport = true;
  finishLteAttempt(true, "LTE ready (native MQTT)");
  return true;
#else
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
#endif

  finishLteAttempt(false, "LTE PDP fail");
  lastLteRetryDelayMs = std::min<uint32_t>(lastLteRetryDelayMs * 2U, kLteRetryMaxIntervalMs);
  return false;
}
static const char* lteSessionDropReason = nullptr;

void noteLteSessionDrop(const char* reason) {
  if (reason == nullptr || reason[0] == '\0') {
    return;
  }
  if (lteSessionDropReason == nullptr) {
    lteSessionDropReason = reason;
  }
}

const char* takeLteSessionDrop() {
  const char* reason = lteSessionDropReason;
  lteSessionDropReason = nullptr;
  return reason;
}

void stopLtePdp() {
  noteLteSessionDrop("stop");
#if COF_LTE_MQTT_NATIVE
  // Native MQTT: release the CMQTT client and service. cmqttTearDown() is a
  // no-op when the service was never started, so calling it unconditionally is
  // safe (e.g. when LAN MQTT comes up and LTE was never used). CMQTTSTOP also
  // releases the PDP that CMQTTSTART dialed, so there is no NETCLOSE/CNACT to
  // run on this path.
  cmqttTearDown();
#else
  lteMqttClient.stop();
  // Tear down exactly one stack, and only the one that is actually up.
  //
  // These used to be two independent `if`s both keyed on `state.lteDataUp`, so
  // under NETOPEN (the common case: lteDataUp is true) BOTH ran. The second one
  // sent `AT+CNACT=<ltePdpCid>,0`, and `ltePdpCid` is 1 under NETOPEN - but the
  // CNACT context is always 0 (see activateCnactPdp), so the module answered
  // ERROR. Observed on hardware in the 2026-09-22 `lte_data` trace:
  // `AT+CNACT=1,0 -> ERROR`, right after `AT+NETCLOSE -> +NETCLOSE: 2`.
  //
  // The two stacks are alternatives (detectLteIpStack picks one), never both, so
  // the teardown is mutually exclusive. The CNACT context is written literally:
  // the CID field is only meaningful for NETOPEN and using it here was the bug.
  if (lteIpStack == kLteStackCnact) {
    sendAT("AT+CNACT=0,0", "OK", 8000);
  } else if (state.lteDataUp || lteIpStack == kLteStackNetopen) {
    sendAT("AT+CIPCLOSE=0", "OK", 5000);
    sendAT("AT+NETCLOSE", "+NETCLOSE:", 12000);
  }
#endif
  state.lteDataUp = false;
  state.lteMqttTransport = false;
  state.lteIpAddress = "-";
  ltePdpDown = false;
  lastLteAttemptMs = 0;
  lastLteRetryDelayMs = kLteRetryIntervalMs;
  lteMqttConnectFails = 0;
  pendingNetworkStatusReport = true;
}
void releaseLteMqttForModem() {
  if (!state.lteMqttTransport) {
    return;
  }
#if COF_LTE_MQTT_NATIVE
  cmqttDisconnect();
#else
  mqttClient.disconnect();
  lteMqttClient.stop();
#endif
  state.mqttConnected = false;
  lastMqttReconnectMs = 0;
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
