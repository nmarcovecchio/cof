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
  if (cachedMqttIp != IPAddress((uint32_t)0)) {
    return cachedMqttIp.toString();
  }
  String resp;
  if (sendAT(String("AT+CDNSGIP=\"") + host + "\"", "+CDNSGIP:", 10000, &resp)) {
    const String ip = lastQuoted(resp);
    if (ip.length() >= 7 && ip.indexOf('.') > 0 && ip != "0.0.0.0") {
      Serial.println("[lte] DNS " + ip);
      return ip;
    }
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
      int start = idx + static_cast<int>(strlen(tags[i]));
      while (start < static_cast<int>(resp.length()) &&
             (resp[start] == ' ' || resp[start] == ':')) {
        start++;
      }
      int end = start;
      while (end < static_cast<int>(resp.length()) && resp[end] != '\r' && resp[end] != '\n' &&
             resp[end] != ',') {
        end++;
      }
      ipOut = resp.substring(start, end);
      ipOut.trim();
    }
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

  finishLteAttempt(false, "LTE PDP fail");
  lastLteRetryDelayMs = std::min<uint32_t>(lastLteRetryDelayMs * 2U, kLteRetryMaxIntervalMs);
  return false;
}
void stopLtePdp() {
  lteMqttClient.stop();
  if (state.lteDataUp || lteIpStack == kLteStackNetopen) {
    sendAT("AT+CIPCLOSE=0", "OK", 5000);
    sendAT("AT+NETCLOSE", "+NETCLOSE:", 12000);
  }
  if (state.lteDataUp || lteIpStack == kLteStackCnact) {
    sendAT(String("AT+CNACT=") + String(state.ltePdpCid) + ",0", "OK", 8000);
  }
  state.lteDataUp = false;
  state.lteMqttTransport = false;
  state.lteIpAddress = "-";
  lastLteAttemptMs = 0;
  lastLteRetryDelayMs = kLteRetryIntervalMs;
  lteMqttConnectFails = 0;
  pendingNetworkStatusReport = true;
}
void releaseLteMqttForModem() {
  if (!state.lteMqttTransport) {
    return;
  }
  mqttClient.disconnect();
  state.mqttConnected = false;
  lteMqttClient.stop();
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
