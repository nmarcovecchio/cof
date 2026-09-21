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
- Verified against the vendored `PubSubClient.cpp` (2.8.x): when
  `pingOutstanding` is set and `keepAlive` elapses again, `loop()` sets
  `MQTT_CONNECTION_TIMEOUT`, stops the client and returns `false`. `keepAlive` is
  `kMqttKeepAliveSeconds` = **10 s**, so any code path that can block MQTT for
  more than 10 s without pumping `mqttClient.loop()` will drop the connection.
  Note "block MQTT", not "take longer than 10 s": the paths that legitimately spin
  for longer pump MQTT as they go. `readModemUntil()` runs inside every `sendAT()`
  and `waitWithWatchdog()` replaces every blocking `delay()`, and both pump. So
  keeping MQTT up during a long AT operation is already handled, and batching
  `pollModem()` on a 30 s gate is fine for the same reason. What would not be fine
  is a new sleep that neither pumps nor uses `waitWithWatchdog()`.

## What polls, and what it costs

`loop()` runs continuously and ends with `delay(20)`, so a pass is ~20 ms plus
whatever blocking work that pass did. `mqttClient.connected()` is a state compare
and `mqttClient.loop()` on Ethernet/WiFi is a non-blocking socket read, so calling
both on every pass is effectively free. Nothing polls the connection on a timer.

The expensive work is rate-limited by *timestamps*, never by `delay()`. There is
no bare `delay(30000)` in the firmware. The misreadable one is `kModemIntervalMs`
(30 s): it gates how often `pollModem()` runs, and `pollModem()` issues ~7 AT
commands through `refreshCellularStatus()`. Running that every pass would saturate
the modem UART and starve every other task, so the gate is deliberate.

| Interval | Value | Gates | Why not faster |
|---|---|---|---|
| `kSensorIntervalMs` | 3 s | `readSensors()` | DS18B20 conversion time |
| `kSmsPollIntervalMs` | 5 s | `pollIncomingSms()` | AT round-trips on a shared UART |
| `kModemRetryNoLanMs` | 5 s | `pollModem()` when no LAN | needed when it is the only path |
| `kModemIntervalMs` | 30 s | `pollModem()` with LAN | ~7 AT commands per call |
| `kEthProbeIntervalMs` | 10 s | Ethernet broker probe | 1.5 s connect timeout |
| `kPathRecoverProbeIntervalMs` | 60 s | recovery probes while on LTE | one connect timeout each |
| `kCellularStatusIntervalMs` | 5 min | `refreshCellularStatus()` + status publish | ~7 AT commands per call |
| `kMqttKeepAliveSeconds` | 30 s | PINGREQ cadence (library) | see below |

The one genuinely hot path is `LteMqttClient::available()`, which runs an
`AT+CIPRXGET=2` round-trip when the RX buffer is empty, throttled by
`lastRxPollMs >= 250`. MQTT over the modem's AT socket means paying that ~4x/sec
while LTE carries MQTT. Raising 250 ms trades inbound-command latency for UART
load.

On keepalive: `kMqttKeepAliveSeconds` was 10 s and is now 30 s. It is not a poll of
connection state; it is how long the library tolerates silence before issuing a
PINGREQ. 10 s was more aggressive than the 15-60 s typical for MQTT, and on LTE
every PINGREQ is a modem round-trip (6/min instead of 2/min). 30 s trims that
traffic; the cost is that a silently dead socket is noticed up to ~20 s later.
`kMqttSilenceReconnectMs` (90 s) still covers the "no successful publish" case
independently, so nothing depends on the keepalive being short.

Note the keepalive also bounds how long MQTT may go unattended. Every long AT
operation pumps `mqttClient.loop()` (`readModemUntil()` inside `sendAT()`, and
`waitWithWatchdog()` in place of blocking `delay()`), so raising the keepalive
widens the margin for any path that does not pump, rather than making such a path
correct.

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

## Modem radio recovery

Observed on hardware (2026-09-21 06:30): the modem got stuck reporting

```
+CPIN: READY          <- SIM is fine
AT+CIMI -> 722310...  <- SIM readable
+CSQ: 99,99           <- no signal
+CPSI: NO SERVICE,Online
AT+CNACT? -> ERROR
```

`AT+CMEE=2` and `AT+CGATT=1` do not recover this, and neither did anything else
in the firmware. The only known cure was a physical power cycle, which is not
available on a remote site. Historically the device fell back to the 6-minute
MQTT-silence reboot, which does not touch module power.

It is also why the device can look like it has "all three paths available" and
still reach nothing: Ethernet and WiFi were fine but the cellular path was dead,
and a stale `CEREG` kept reporting registered. Note that `state.networkRegistered`
ORs CREG/CEREG/CGREG, and CEREG holds a stale "registered" long after the radio
lost service, so registration alone cannot be used as a health signal.

`pollModem()` watches `radioReportsService()` and escalates through
`resetModemRadio(stage)`, one step per `kModemRecoveryIntervalMs` (150 s):

| Stage | Action |
|---|---|
| 1 | detach/attach (`CGATT=0/1`) + `COPS=0` operator auto |
| 2 | `CFUN=0/1` + full network scan |
| 3 | `CFUN=4/1` radio cycle + `CEMODE`/`CEVDP` + `COPS=0` |
| 4 | `CFUN=1,1` module reset, then `initModem()` to re-apply ATE0, CGDCONT, CGAUTH, CGSMS, CMGF, CSCA and voice settings |
| 5 | restart the ESP32, so the module comes up from a cold power-on |

About 12 min from stuck to reboot. The stage counter resets the moment service
comes back.

**This is a mitigation, not a fix.** A healthy module never climbs the ladder, so
if `[modem] recovery 5/5` ever appears the hardware needs attention: module
power, antenna, or SIM seating. If the module returns from stage 4 with
`+CPIN: NOT READY`, that points at SIM contact or supply, not firmware.

## Why 20 s telemetry, in one line

`kMqttSilenceReconnectMs` (20 s) < telemetry interval (60 s) => forced reconnect
every 20 s => reconnect republishes telemetry. Fixed by raising the threshold to
90 s and letting `loop()` be the probe.
