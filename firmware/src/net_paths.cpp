#include "cof_config.h"
#include "cof_state.h"
#include <Arduino.h>
#include <ETH.h>
#include <WiFi.h>
#include <esp_netif.h>
#include <esp_netif_net_stack.h>
#include "lwip/dns.h"
#include "lwip/netif.h"

// Extracted verbatim from main.cpp, which used to hold every function

// Broker hostname resolution. Runs in the lwIP TCP/IP thread, so it must do
// nothing but stash the answer and set a flag: no Serial, no MQTT, no String.
void onBrokerResolved(const char* name, const ip_addr_t* addr, void* arg) {
  (void)name;
  (void)arg;
  if (addr != nullptr) {
    resolvedBrokerIp = addr->u_addr.ip4.addr;
  }
  brokerResolveDone = true;
}
// Kick off a resolve and pick up the previous one's result. Non-blocking by
// design: if the answer is not ready this returns an empty IP and the caller
// keeps using whatever it already had, so nothing stalls the main loop.
IPAddress resolveBrokerHost(const String& host, bool force) {
  const uint32_t now = millis();

  if (brokerResolveDone) {
    brokerResolveDone = false;
    brokerResolveInFlight = false;
    lastBrokerResolveMs = now == 0 ? 1 : now;
    if (static_cast<uint32_t>(resolvedBrokerIp) != 0) {
      brokerResolveFailed = false;
      Serial.printf("[dns] broker %s -> %s\n", host.c_str(), resolvedBrokerIp.toString().c_str());
      return resolvedBrokerIp;
    }
    // NXDOMAIN, timeout, or no usable DNS server. Retry on the SHORT interval: a
    // boot-time attempt can legitimately fail before the interface's resolver is
    // usable, and waiting a full hourly refresh would reopen the exact window this
    // was added to close.
    brokerResolveFailed = true;
    Serial.printf("[dns] broker resolve failed for %s\n", host.c_str());
    return IPAddress((uint32_t)0);
  }

  if (brokerResolveInFlight) {
    // Backstop: lwIP's own timeout is ~14 s. Without this a resolve that never
    // calls back would wedge the state machine in "in flight" forever.
    if (now - brokerResolveStartedMs >= 15000) {
      brokerResolveInFlight = false;
      brokerResolveFailed = true;
      lastBrokerResolveMs = now == 0 ? 1 : now;
    }
    return IPAddress((uint32_t)0);
  }

  // Hourly while it works, every 15 s while it does not.
  const uint32_t interval = brokerResolveFailed ? kBrokerResolveRetryMs : kBrokerResolveIntervalMs;
  if (!force && lastBrokerResolveMs != 0 && now - lastBrokerResolveMs < interval) {
    return IPAddress((uint32_t)0);
  }

  lastBrokerResolveMs = now == 0 ? 1 : now;
  brokerResolveStartedMs = lastBrokerResolveMs;
  brokerResolveDone = false;
  resolvedBrokerIp = IPAddress((uint32_t)0);
  brokerResolveName = host;
  ip_addr_t addr;
  ip_addr_set_zero(&addr);
  const err_t err = dns_gethostbyname(brokerResolveName.c_str(), &addr, onBrokerResolved, nullptr);
  if (err == ERR_OK) {
    // Answered from lwIP's own cache, so the callback never runs. Take the value
    // from `addr` directly - otherwise the next pass would see "done" with an
    // empty result and record a success as a failure.
    resolvedBrokerIp = addr.u_addr.ip4.addr;
    brokerResolveDone = true;
  } else if (err == ERR_INPROGRESS) {
    brokerResolveInFlight = true;
  } else {
    brokerResolveFailed = true;
  }
  return IPAddress((uint32_t)0);
}
// Called from the main loop before the path polls, so their cachedMqttIp is
// populated as early as the network allows.
void serviceBrokerResolve() {
  if (state.mqttHost.isEmpty()) {
    return;
  }
  // An IP literal needs no DNS.
  IPAddress literal;
  if (literal.fromString(state.mqttHost)) {
    if (cachedMqttIp == IPAddress((uint32_t)0)) {
      cachedMqttIp = literal;
    }
    return;
  }
  if (state.callInProgress || state.otaInProgress || state.audioSyncInProgress) {
    return;
  }
  if (!state.ethernetConnected && !state.wifiConnected) {
    // No LAN interface, and while MQTT rides LTE a probe over lwIP would leave
    // through whatever route happens to be installed - not worth resolving.
    return;
  }
  const IPAddress resolved = resolveBrokerHost(state.mqttHost, false);
  if (static_cast<uint32_t>(resolved) == 0) {
    return;
  }
  if (cachedMqttIp == IPAddress((uint32_t)0)) {
    // Nothing verified yet: this is the boot case that motivated the resolver.
    cachedMqttIp = resolved;
    return;
  }
  // A repoint. Adopt the DNS answer only while MQTT is DOWN, so a transient or
  // poisoned DNS answer cannot knock us off an address that is demonstrably
  // working - while MQTT is connected, the current target is proven good and the
  // real connect keeps the final word anyway. This is what makes a broker IP
  // change heal without waiting for the 6-minute silence reboot that used to be
  // the only way out, since the probes would otherwise keep testing the old
  // address and demote a perfectly healthy LAN path.
  if (!state.mqttConnected && resolved != cachedMqttIp) {
    Serial.printf("[dns] broker repointed %s -> %s\n",
                  cachedMqttIp.toString().c_str(),
                  resolved.toString().c_str());
    cachedMqttIp = resolved;
  }
}
bool lanConnected() {
  return state.ethernetConnected || state.wifiConnected;
}
bool networkConnected() {
  return lanConnected() || state.lteDataUp;
}
const char* activeNetworkName() {
  if (state.lteMqttTransport && state.lteDataUp) {
    return "lte";
  }
  if (state.ethernetConnected && ethInternetUp) {
    return "ethernet";
  }
  if (state.wifiConnected && wifiInternetUp) {
    return "wifi";
  }
  if (state.lteDataUp) {
    return "lte";
  }
  if (state.ethernetConnected) {
    return "ethernet";
  }
  if (state.wifiConnected) {
    return "wifi";
  }
  return "none";
}
const char* mqttPathLetter() {
  if (!state.mqttConnected) {
    return "--";
  }
  if (state.lteMqttTransport) {
    return "L";
  }
  if (state.ethernetConnected) {
    return "E";
  }
  if (state.wifiConnected) {
    return "W";
  }
  return "?";
}
void applyPreferredRoute(PathPreference pref) {
  // Auto gates the Ethernet branch on ethInternetUp: an Ethernet interface that
  // is up but not carrying the internet (a router with no uplink - the exact case
  // that used to wedge the device on WiFi while DHCP was up) must not win the
  // default route, otherwise every new TCP connection, including the MQTT
  // reconnect, keeps leaving through the dead path.
  //
  // PathPreference::Ethernet also accepts an interface that still has an IP but
  // is not flagged connected yet: that is the state inside the holdoff window,
  // where markEthernetUp() must probe Ethernet specifically before re-accepting
  // it, and probing over the wrong interface would give a false positive.
  bool useEthernet = false;
  bool useWifi = false;
  switch (pref) {
    case PathPreference::Ethernet:
      useEthernet = state.ethernetConnected || ETH.localIP() != IPAddress((uint32_t)0);
      break;
    case PathPreference::Wifi:
      useWifi = state.wifiConnected;
      break;
    case PathPreference::Auto:
      useEthernet = state.ethernetConnected && ethInternetUp;
      useWifi = state.wifiConnected;
      break;
  }
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  if (useEthernet) {
    ETH.setDefault();
    Serial.println("[net] default route ETH");
  } else if (useWifi) {
    WiFi.setDefault();
    Serial.println("[net] default route WiFi");
  }
#else
  esp_netif_t* netif = nullptr;
  if (useEthernet) {
    netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
  } else if (useWifi) {
    netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  }
  if (netif != nullptr) {
    auto* lwipIf = static_cast<struct netif*>(esp_netif_get_netif_impl(netif));
    if (lwipIf != nullptr) {
      netif_set_default(lwipIf);
      Serial.printf("[net] default route %s\n", useEthernet ? "ETH" : "WiFi");
    }
  }
#endif
}
void forgetWifiRadio() {
  wifiBackupDueMs = 0;
  wifiAuthFailCount = 0;
  WiFi.setAutoReconnect(false);
  WiFi.persistent(true);
  WiFi.disconnect(false, true);
  delay(50);
  WiFi.persistent(false);
  state.wifiConnected = false;
  state.wifiIpAddress = "-";
  wifiInternetUp = false;
  wifiProbeFails = 0;
}
void pauseWiFiRadio() {
  wifiBackupDueMs = 0;
  wifiAuthFailCount = 0;
  WiFi.setAutoReconnect(false);
  if (WiFi.getMode() == WIFI_OFF) {
    return;
  }
  WiFi.disconnect(false);
  state.wifiConnected = false;
  state.wifiIpAddress = "-";
  wifiInternetUp = false;
  wifiProbeFails = 0;
  Serial.println("[wifi] paused (ethernet primary)");
}
void startWifiRadio() {
  if (!state.wifiConfigured) {
    return;
  }
  const String ssid = preferences.getString("wifiSsid", "");
  const String password = preferences.getString("wifiPass", "");
  if (ssid.length() == 0) {
    return;
  }
  if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == ssid) {
    state.wifiConnected = true;
    state.wifiIpAddress = WiFi.localIP().toString();
    return;
  }
  wifiAuthFailCount = 0;
  state.wifiSsid = ssid;
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid.c_str(), password.c_str());
  setStatus("WiFi connecting");
  Serial.printf("[wifi] connecting to %s\n", ssid.c_str());
}
void scheduleWifiBackup(uint32_t delayMs) {
  if (!state.wifiConfigured || state.ethernetConnected) {
    wifiBackupDueMs = 0;
    return;
  }
  const uint32_t due = millis() + delayMs;
  wifiBackupDueMs = due == 0 ? 1 : due;
}
void maintainWifiBackup() {
  if (state.ethernetConnected || wifiBackupDueMs == 0) {
    return;
  }
  if (static_cast<int32_t>(millis() - wifiBackupDueMs) < 0) {
    return;
  }
  wifiBackupDueMs = 0;
  startWifiRadio();
}
bool ethernetHoldoffActive() {
  return ethernetHoldoffUntilMs != 0 &&
         static_cast<int32_t>(millis() - ethernetHoldoffUntilMs) < 0;
}
bool lanHasInternet() {
  // True when any LAN path is trusted to reach the broker. Optimistic right
  // after DHCP, demoted by real connect/publish/probe failures.
  return (state.ethernetConnected && ethInternetUp) || (state.wifiConnected && wifiInternetUp);
}
// Reachability probe. It needs a broker IP, which we only learn from a real MQTT
// connect: a DNS lookup here would be the alternative, but WiFi.hostByName()
// blocks for up to 15 s inside the main loop (it waits on the lwIP DNS semaphore),
// which would stall sensors, display and MQTT. So while the broker IP is unknown
// we rely on the optimistic default plus connect-failure demotion instead.
bool lanPathReachable() {
  if (cachedMqttIp == IPAddress((uint32_t)0)) {
    return false;
  }
  // This runs from canUseLan(), which maintainLteFallback() calls on every loop
  // pass - and the state that reaches it (an interface connected but flagged
  // without internet) persists for as long as LTE carries MQTT. Without this
  // throttle every pass paid two 1500 ms probes plus two route flips, stalling
  // sensors, display and MQTT itself. The verdict is latched between probes so
  // the answer stays fresh without the cost.
  const uint32_t now = millis();
  if (lastLanReachableProbeMs != 0 && now - lastLanReachableProbeMs < kLanReachableProbeIntervalMs) {
    return lanReachableLatch;
  }
  lastLanReachableProbeMs = now == 0 ? 1 : now;
  lanReachableLatch = false;
  if (state.ethernetConnected && probeMqttOnInterface(true)) {
    lanReachableLatch = true;
    return true;
  }
  if (state.wifiConnected && probeMqttOnInterface(false)) {
    lanReachableLatch = true;
    return true;
  }
  return false;
}
bool canUseLan() {
  if (!lanConnected()) {
    return false;
  }
  if (lanHasInternet()) {
    return true;
  }
  return lanPathReachable();
}
bool probeMqttOverEthernet() {
  if (cachedMqttIp == IPAddress((uint32_t)0)) {
    return false;
  }
  WiFiClient probe;
  feedWatchdog();
  const int ok = probe.connect(cachedMqttIp, state.mqttPort, 1500);
  probe.stop();
  return ok != 0;
}
bool probeMqttOverWifi() {
  if (cachedMqttIp == IPAddress((uint32_t)0)) {
    return false;
  }
  WiFiClient probe;
  feedWatchdog();
  const int ok = probe.connect(cachedMqttIp, state.mqttPort, 1500);
  probe.stop();
  return ok != 0;
}
// A probe only proves something about an interface if the packet actually leaves
// through it, so pin the lwIP default route for the duration of the probe and
// restore the normal preference afterwards. Without this, probing Ethernet while
// WiFi is associated - or probing WiFi while Ethernet still owns the route -
// would silently test the other interface and report a false result.
bool probeMqttOnInterface(bool ethernet) {
  if (ethernet) {
    // Do NOT gate on state.ethernetConnected: Ethernet flagged down inside the
    // holdoff still holds its DHCP lease, and this probe is the only way back. An
    // early return on the flag made the recovery probe impossible - it bailed out
    // before ever testing the link, so a perfectly good cable was never
    // re-accepted and the device stayed on the worse path forever.
    if (ETH.localIP() == IPAddress((uint32_t)0)) {
      return false;
    }
  } else if (!state.wifiConnected) {
    return false;
  }
  applyPreferredRoute(ethernet ? PathPreference::Ethernet : PathPreference::Wifi);
  const bool ok = ethernet ? probeMqttOverEthernet() : probeMqttOverWifi();
  applyPreferredRoute();
  return ok;
}
// An interface is only "the internet" if the broker is reachable through it.
// A router with no uplink still gives link + DHCP, so reachability is the only
// honest signal. Probes use the cached broker IP, which we only learn from a
// real successful MQTT connect, so this never turns into a DNS dependency.
void pollEthernetPath() {
  if (state.callInProgress || state.otaInProgress) {
    ethProbeFails = 0;
    return;
  }

  if (!state.ethernetConnected) {
    // Ethernet flagged down. It is only worth probing if it is physically here
    // (link up) and still holds a lease, which is the router-reboot case: the
    // DHCP lease survives, so no GOT_IP event ever fires again and without this
    // poll nothing would ever bring Ethernet back. Derived from the hardware on
    // every pass rather than latched from an event, so a cable replug that
    // happened without a matching event still recovers.
    if (ETH.linkUp() && ETH.localIP() != IPAddress((uint32_t)0)) {
      ethRecoverPending = true;
    }
    if (!ethRecoverPending) {
      return;
    }
    const uint32_t nowDown = millis();
    if (lastEthRecoverProbeMs != 0 && nowDown - lastEthRecoverProbeMs < kPathRecoverProbeIntervalMs) {
      return;
    }
    lastEthRecoverProbeMs = nowDown == 0 ? 1 : nowDown;
    if (probeMqttOnInterface(true)) {
      ethRecoverPending = false;
      markEthernetUp("recovered probe");
    }
    return;
  }

  if (state.lteMqttTransport) {
    // Ethernet is up but MQTT rides LTE. Keep probing slowly so LTE can be
    // released: this is what used to be skipped entirely, because the early
    // return left Ethernet never re-probed while the PDP was carrying traffic.
    const uint32_t nowLte = millis();
    if (ethernetHoldoffActive()) {
      return;
    }
    if (lastEthRecoverProbeMs != 0 && nowLte - lastEthRecoverProbeMs < kPathRecoverProbeIntervalMs) {
      return;
    }
    lastEthRecoverProbeMs = nowLte == 0 ? 1 : nowLte;
    if (probeMqttOnInterface(true)) {
      ethProbeFails = 0;
      if (!ethInternetUp) {
        ethInternetUp = true;
        Serial.println("[net] ethernet recovered while on LTE");
        applyPreferredRoute();
      }
    } else if (ethInternetUp) {
      ethInternetUp = false;
    }
    return;
  }

  const uint32_t now = millis();
  if (lastEthProbeMs != 0 && now - lastEthProbeMs < kEthProbeIntervalMs) {
    return;
  }
  lastEthProbeMs = now == 0 ? 1 : now;
  if (ethernetUpAtMs != 0 && static_cast<int32_t>(now - ethernetUpAtMs) < 8000) {
    return;
  }
  if (cachedMqttIp == IPAddress((uint32_t)0)) {
    // No real connect yet, so we cannot probe. Give MQTT time to come up; if it
    // never does, assume the Ethernet path is not carrying the internet.
    if (!state.mqttConnected && ethernetUpAtMs != 0 &&
        static_cast<int32_t>(now - ethernetUpAtMs) >= 15000) {
      markEthernetDown("no mqtt path");
    }
    return;
  }

  const bool reachable = probeMqttOnInterface(true);
  if (reachable) {
    ethProbeFails = 0;
    if (!ethInternetUp) {
      ethInternetUp = true;
      Serial.println("[eth] path recovered");
      requestMqttBounce("eth recovered");
    }
    return;
  }
  ethProbeFails++;
  Serial.printf("[eth] path probe fail %u/%u ip=%s\n",
                ethProbeFails,
                kEthProbeFailLimit,
                cachedMqttIp.toString().c_str());
  if (ethProbeFails >= kEthProbeFailLimit) {
    markEthernetDown("path probe");
  }
}
// WiFi used to be trusted on a bare DHCP lease and could never be demoted, so a
// WiFi uplink without internet blocked the LTE fallback forever.
void pollWifiPath() {
  if (!state.wifiConnected || state.callInProgress || state.otaInProgress) {
    wifiProbeFails = 0;
    return;
  }
  // Ethernet outranks WiFi, so while it is connected and healthy we have no
  // reason to spend probes on WiFi. The exception matters: if Ethernet is
  // connected but has no internet, WiFi must still be probed so it can be
  // promoted the moment its own path comes back. Gating on the connection alone
  // meant a recovered WiFi was never re-checked while a dead Ethernet held the
  // port, which is one of the "check periodically whether internet came back"
  // cases.
  if (state.ethernetConnected && ethInternetUp) {
    return;
  }
  if (cachedMqttIp == IPAddress((uint32_t)0)) {
    return;
  }
  const uint32_t now = millis();
  if (lastWifiProbeMs != 0 && now - lastWifiProbeMs < kEthProbeIntervalMs) {
    return;
  }
  lastWifiProbeMs = now == 0 ? 1 : now;
  // Pin the route to WiFi: this runs only when Ethernet is not connected, but a
  // pinned probe keeps the result honest if that ever changes.
  const bool reachable = probeMqttOnInterface(false);
  if (reachable) {
    wifiProbeFails = 0;
    if (!wifiInternetUp) {
      wifiInternetUp = true;
      Serial.println("[wifi] path recovered");
    }
    return;
  }
  wifiProbeFails++;
  Serial.printf("[wifi] path probe fail %u/%u ip=%s\n",
                wifiProbeFails,
                kWifiProbeFailLimit,
                cachedMqttIp.toString().c_str());
  if (wifiProbeFails >= kWifiProbeFailLimit) {
    Serial.println("[wifi] no internet over WiFi, dropping path");
    wifiInternetUp = false;
    wifiProbeFails = 0;
    if (state.mqttConfigured && !state.lteMqttTransport) {
      requestMqttBounce("wifi no internet");
    }
    pendingNetworkStatusReport = true;
  }
}
// Drives the passive failback from LTE to a recovered LAN path.
void serviceNetworkPaths() {
  if (state.callInProgress || state.otaInProgress || state.audioSyncInProgress) {
    ltePreemptSinceMs = 0;
    return;
  }
  const bool betterLan =
      (state.ethernetConnected && ethInternetUp) || (state.wifiConnected && wifiInternetUp);
  if (!state.lteMqttTransport || !betterLan) {
    ltePreemptSinceMs = 0;
    return;
  }
  const uint32_t now = millis();
  if (ltePreemptSinceMs == 0) {
    ltePreemptSinceMs = now == 0 ? 1 : now;
    return;
  }
  if (now - ltePreemptSinceMs < kPathPreemptSettleMs) {
    return;
  }
  ltePreemptSinceMs = 0;
  Serial.println("[net] LAN path recovered, releasing LTE");
  stopLtePdp();
  applyPreferredRoute();
  requestMqttBounce("lan recovered");
}
void markEthernetUp(const char* reason) {
  if (ethernetHoldoffActive()) {
    // Probe Ethernet specifically: with the route on Auto, a live WiFi would have
    // answered instead and this would re-accept a dead Ethernet.
    const bool probed = probeMqttOnInterface(true);
    if (!probed) {
      Serial.printf("[eth] ignore up (%s) holdoff probe=%s ip=%s\n",
                    reason,
                    cachedMqttIp == IPAddress((uint32_t)0) ? "no-ip" : "fail",
                    ETH.localIP().toString().c_str());
      return;
    }
    ethernetHoldoffUntilMs = 0;
  }

  const bool wasEthernet = state.ethernetConnected;
  state.ethernetConnected = true;
  state.ipAddress = ETH.localIP().toString();
  lanMqttFailCount = 0;
  ethProbeFails = 0;
  // Optimistic: a fresh IP is treated as a working path until the probe says
  // otherwise. This keeps boot fast and avoids double-switching.
  ethInternetUp = true;
  ethRecoverPending = false;
  lastEthRecoverProbeMs = 0;
  noLanSinceMs = 0;
  ethernetUpAtMs = millis();
  if (ethernetUpAtMs == 0) {
    ethernetUpAtMs = 1;
  }
  lastLteFail = false;
  applyPreferredRoute();
  if (!wasEthernet) {
    requestMqttBounce(reason);
  }
  pendingNetworkStatusReport = true;
  setStatus("ETH IP " + state.ipAddress);
  Serial.printf("[eth] up (%s) ip=%s\n", reason, state.ipAddress.c_str());
}
void markEthernetDown(const char* reason) {
  ethernetHoldoffUntilMs = millis() + kEthernetHoldoffMs;
  if (ethernetHoldoffUntilMs == 0) {
    ethernetHoldoffUntilMs = 1;
  }
  ethInternetUp = false;
  ethProbeFails = 0;
  ethRecoverPending = false;
  lastEthRecoverProbeMs = 0;
  if (!state.ethernetConnected) {
    return;
  }
  Serial.printf("[eth] down (%s)\n", reason);
  state.ethernetConnected = false;
  state.ipAddress = "-";
  applyPreferredRoute();
  requestMqttBounce(reason);
  pendingNetworkStatusReport = true;
  setStatus("ETH down");
  lastLteAttemptMs = 0;
  startWifiRadio();
}
// A LAN path is only usable if it actually reaches the broker. `hadInternet`
// remembers whether this interface was the one carrying MQTT, so we can tell a
// real outage from a probe failure on a backup interface that was never used.
void noteLanPathFailure(bool ethernet, const char* reason, bool hadInternet) {
  if (state.lteMqttTransport) {
    return;
  }
  if (ethernet) {
    if (!state.ethernetConnected) {
      return;
    }
    lanMqttFailCount++;
    Serial.printf("[eth] mqtt fail %u/%u (%s)\n", lanMqttFailCount, kLanMqttFailLimit, reason);
    if (hadInternet || lanMqttFailCount >= kLanMqttFailLimit) {
      markEthernetDown(reason);
    }
    return;
  }
  if (!state.wifiConnected) {
    return;
  }
  wifiInternetUp = false;
  wifiProbeFails = 0;
  pendingNetworkStatusReport = true;
  Serial.printf("[wifi] path unusable (%s)\n", reason);
  if (hadInternet) {
    requestMqttBounce(reason);
  }
}
void noteLanMqttFailure(const char* reason) {
  if (state.ethernetConnected) {
    noteLanPathFailure(true, reason, ethInternetUp);
    return;
  }
  // WiFi used to be ignored here outright, which is why a WiFi link with a DHCP
  // lease but no upstream internet pinned the device forever: lanConnected()
  // stayed true so maintainLteFallback() never engaged LTE.
  if (state.wifiConnected) {
    noteLanPathFailure(false, reason, wifiInternetUp);
  }
}
void onNetworkEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      state.ethernetStarted = true;
      ETH.setHostname("callonfail");
      setStatus("ETH start");
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      setStatus("ETH cable OK");
      if (ETH.localIP() != IPAddress((uint32_t)0)) {
        markEthernetUp("cable");
      }
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      markEthernetUp("got ip");
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      markEthernetDown("disconnected");
      break;
    case ARDUINO_EVENT_ETH_STOP:
      state.ethernetStarted = false;
      markEthernetDown("stopped");
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      if (!state.wifiConfigured) {
        Serial.printf("[wifi] ignoring unsolicited IP %s\n", WiFi.localIP().toString().c_str());
        forgetWifiRadio();
        pendingNetworkStatusReport = true;
        break;
      }
      wifiAuthFailCount = 0;
      state.wifiConnected = true;
      state.wifiSsid = WiFi.SSID();
      state.wifiIpAddress = WiFi.localIP().toString();
      // A fresh association starts clean: clear the stale probe fail count, and
      // if MQTT is riding LTE, probe WiFi on the next loop pass instead of
      // waiting for the next 10 s throttle tick. A healthy WiFi then wins back
      // the primary path in ~3 s (settle) instead of ~13 s. The probe still
      // decides, so an AP with no uplink is not falsely promoted (see §6e).
      wifiProbeFails = 0;
      if (state.lteMqttTransport) {
        lastWifiProbeMs = 0;
      }
      if (!state.ethernetConnected) {
        applyPreferredRoute();
        requestMqttBounce("wifi got ip");
      }
      pendingNetworkStatusReport = true;
      setStatus("WiFi IP " + state.wifiIpAddress);
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      const uint8_t reason = info.wifi_sta_disconnected.reason;
      const bool lostWifiPath = state.wifiConnected && !state.ethernetConnected;
      state.wifiConnected = false;
      state.wifiIpAddress = "-";
      wifiInternetUp = false;
      wifiProbeFails = 0;
      if (lostWifiPath) {
        requestMqttBounce("wifi disconnected");
      }
      if (state.wifiConfigured) {
        pendingNetworkStatusReport = true;
        setStatus("WiFi disconnected");
      }
      if (reason == 2 || reason == 15 || reason == 202 || reason == 203 || reason == 204) {
        wifiAuthFailCount++;
        Serial.printf("[wifi] auth/handshake fail %u/%u reason=%u\n",
                      wifiAuthFailCount, kWifiAuthFailLimit, reason);
        if (wifiAuthFailCount >= kWifiAuthFailLimit) {
          WiFi.setAutoReconnect(false);
          WiFi.disconnect(false);
          setStatus("WiFi auth fail");
        }
      }
      break;
    }
    default:
      break;
  }
}
void beginEthernet() {
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.onEvent(onNetworkEvent);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ETH.begin(kEthPhyType, kEthPhyAddr, kEthMdcPin, kEthMdioPin, kEthPowerPin, kEthClockMode);
#else
  ETH.begin(kEthPhyAddr, kEthPowerPin, kEthMdcPin, kEthMdioPin, kEthPhyType, kEthClockMode);
#endif
}
void connectWiFi(const String& ssid, const String& password, bool saveCredentials) {
  if (ssid.length() == 0) {
    Serial.println("[wifi] missing SSID");
    return;
  }

  state.wifiConfigured = true;
  state.wifiSsid = ssid;
  if (saveCredentials) {
    preferences.putString("wifiSsid", ssid);
    preferences.putString("wifiPass", password);
  }

  pendingNetworkStatusReport = true;
  startWifiRadio();
}
void beginSavedWiFi() {
  const String ssid = preferences.getString("wifiSsid", "");
  state.wifiConfigured = ssid.length() > 0;
  state.wifiSsid = ssid;

  if (!state.wifiConfigured) {
    Serial.println("[wifi] no saved credentials");
    forgetWifiRadio();
    return;
  }

  startWifiRadio();
}
void clearSavedWiFi() {
  preferences.remove("wifiSsid");
  preferences.remove("wifiPass");
  state.wifiConfigured = false;
  state.wifiSsid = "";
  forgetWifiRadio();
  pendingNetworkStatusReport = true;
  setStatus("WiFi cleared");
}
String currentIpAddress() {
  if (state.ethernetConnected) {
    return state.ipAddress;
  }
  if (state.wifiConnected) {
    return state.wifiIpAddress;
  }
  if (state.lteDataUp) {
    return state.lteIpAddress;
  }
  return "-";
}
