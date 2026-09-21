# Network paths: Ethernet, WiFi and LTE

The device has three ways to reach the MQTT broker. This document describes how
one is chosen and, more importantly, how a path that *looks* fine but carries no
internet is detected.

All of this lives in `firmware/src/main.cpp`.

## Priority

Ethernet > WiFi > LTE. The decision is spread over three places, all with the
same order:

| Where | Purpose |
|---|---|
| `applyPreferredRoute()` | sets the lwIP default route (raw TCP clients) |
| `configureMqttClientTransport()` | picks the MQTT client (TLS/plain vs. AT socket) |
| `activeNetworkName()` / `mqttPathLetter()` | what gets reported to the backend |

LTE is a special case: it is a raw `AT+CIPOPEN`/`CAOPEN` socket (`LteMqttClient`),
not an lwIP interface, so the default route is irrelevant while MQTT rides it.

## The rule: an interface is only "the internet" if it reaches the broker

Link up + DHCP lease is **not** evidence of internet. A router with no uplink
happily gives both. Treating that as healthy was the cause of a real outage: the
device reported Ethernet and LTE available and could not reach anything.

So each LAN interface carries an explicit health flag:

- `ethInternetUp` / `wifiInternetUp`
- Set optimistically to `true` when the interface gets its IP (keeps boot fast).
- Demoted to `false` by a real failure: a failed MQTT connect, a failed publish,
  or a failed broker probe.
- A successful MQTT connect (or probe) promotes it back to `true`.

`lanHasInternet()` is `(ethernetConnected && ethInternetUp) || (wifiConnected && wifiInternetUp)`.
`network_internet_ok` additionally counts LTE.

`lanConnected()` (link/IP only) and `lanHasInternet()` (actual reachability) are
deliberately different, and `maintainLteFallback()` uses the latter to decide
whether LTE is needed.

### Why the probe uses the broker IP and not DNS

`probeMqttOverEthernet()` / `probeMqttOverWifi()` open a TCP connection to the
cached broker IP. `cachedMqttIp` is only learned from a *successful* MQTT
connect, so until the first connect there is nothing to probe.

The alternative, `WiFi.hostByName()`, blocks the main loop for **up to 15 s**
(it waits on the lwIP DNS semaphore), stalling sensors, the display and MQTT.
That is not acceptable in `loop()`, so before the broker IP is known the code
relies on the optimistic default plus connect-failure demotion instead.

### A probe must leave through the interface it claims to test

`probeMqttOnInterface(bool ethernet)` pins the default route to the interface
under test, probes, and restores the normal preference. Without this, probing
Ethernet while WiFi is associated would test WiFi and report a false result.

## Recovery

- `pollEthernetPath()` probes every `kEthProbeIntervalMs` (10 s) and demotes
  Ethernet after `kEthProbeFailLimit` (2) consecutive failures.
- Passively, `markEthernetDown()` arms a `kEthernetHoldoffMs` (20 s) holdoff and
  starts the WiFi radio. `markEthernetUp()` requires a successful probe to accept
  a link that comes back inside that holdoff.
- While MQTT runs over LTE, `pollEthernetPath()` keeps probing Ethernet every
  `kPathRecoverProbeIntervalMs` (60 s). `serviceNetworkPaths()` then waits
  `kPathPreemptSettleMs` (3 s) of *stable* recovery before tearing the PDP down,
  so a flapping link cannot cause a reconnect storm.
- When Ethernet is demoted, `applyPreferredRoute()` will not fall back to a WiFi
  that is itself unhealthy, otherwise every new TCP connection (including the
  MQTT reconnect) leaves through a dead path.

## Publishing liveness

`enforceMqttSilenceWatchdog()` reboots after `kMqttSilenceRestartMs` (6 min)
without a successful publish, and reconnects after `kMqttSilenceReconnectMs`
(90 s) of silence has been confirmed by a failed `mqttClient.loop()`.

Notes:

- `kMqttSilenceReconnectMs` **must stay above** the largest accepted telemetry
  interval (60 s, enforced in `loadSavedMqttConfig`). It used to be 20 s, which
  is below the 60 s telemetry period, so the watchdog declared the link dead
  every cycle, forced a reconnect, and the reconnect republishes status +
  telemetry - producing a permanent ~20 s telemetry cadence on every path.
- `PubSubClient::loop()` returns `true` when the connection is idle and `false`
  only when the socket died or a `PINGRESP` never arrived. From the outside the
  library also emits `PINGREQ` itself once the keepalive elapses, so `loop()` is
  already a real liveness probe. Do not call `ping()` / `publish()` for this.

## Alarms

Link and internet dropouts reuse the normal alarm pipeline. The firmware publishes
flat flags in every telemetry frame and status message:

| Field | Meaning |
|---|---|
| `network_ethernet_ok` | 1 = Ethernet link up |
| `network_wifi_ok` | 1 = WiFi associated |
| `network_internet_ok` | 1 = any path reaches the broker (LTE included) |

`SENSOR_ALIASES` in `backend/app/alarms.py` maps the `net_ethernet` /
`net_wifi` / `net_internet` sensor ids to those fields, and
`ensure_network_sensors()` in `backend/app/main.py` injects the matching sensor
rows into the config form so they appear in the rule sensor dropdown without a
config migration. A disconnection rule is `operator: lt`, `threshold: 1`.

Telemetry also carries a diagnosis of the bad state in `network`:

- `network.internet` - any path reaches the broker
- `network.degraded` - a LAN interface has an IP but nothing reaches the broker
- `network.ethernet.internet` / `network.wifi.internet` - per-interface health

## SMS and calls over LTE

`transmitSms()` does **not** tear the LTE MQTT socket down on the happy path
anymore. It only calls `releaseLteMqttForModem()` before the *retry*, because
destroying MQTT was what lost the command result: `publishDeviceEvent()` was
called while `state.mqttConnected` was false, and `publishMqttJson()` dropped it
silently, so the backend timed the command out with no explanation.

Events produced while MQTT is down are queued in a small bounded buffer
(`deferDeviceEvent`) and flushed in order on reconnect. `waitWithMqtt()` pumps
`mqttClient.loop()` during long AT waits so the keepalive survives.

`pollModem()` / `pollIncomingSms()` remain gated on `!lteMqttTransport`:
interleaving AT housekeeping with an open `AT+CIPOPEN`/`CAOPEN` session can
corrupt the modem socket.

## Why 20 s telemetry, in one line

`kMqttSilenceReconnectMs` (20 s) < telemetry interval (60 s) => forced reconnect
every 20 s => reconnect republishes telemetry. Fixed by raising the threshold to
90 s and letting `loop()` be the probe.
