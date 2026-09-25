#pragma once

// Self-sufficient on purpose: the ETH_* constants below are typed with
// eth_clock_mode_t / eth_phy_type_t from ETH.h, and this header is included
// before the module-specific includes, so it cannot rely on them.
#include <Arduino.h>
#include <ETH.h>

// Firmware version shown on OLED and used by OTA comparison.
// NOTE: must be strictly lower than ota/manifest.json for a device to update.
#define COF_FIRMWARE_VERSION "0.2.77"

// Raw GitHub manifest. After merging, keep this URL pointing at main.
#define COF_MANIFEST_URL "https://raw.githubusercontent.com/nmarcovecchio/cof/main/ota/manifest.json"

// Safety switch: set to 1 only after configuring COF_PHONE_NUMBER.
#define COF_ENABLE_CALLS 0
#define COF_PHONE_NUMBER "+549XXXXXXXXXX"

// WT32-ETH01 / WT32-S1 pin assignment.
#define COF_PIN_I2C_SDA 32
#define COF_PIN_I2C_SCL 33
#define COF_PIN_ONEWIRE 14
#define COF_PIN_ZMPT_ADC 36
#define COF_PIN_MODEM_RX 5
#define COF_PIN_MODEM_TX 17

// I2C defaults.
#define COF_OLED_ADDRESS 0x3C
#define COF_SHT31_ADDRESS 0x44

// Modem filesystem target for the canned fallback audio, played only when the
// per-rule pre-recorded audio and the TTS download are both unavailable.
// The per-rule files live under C:/a_<sha>.amr and are managed separately.
#define COF_MODEM_AUDIO_PATH "C:/cof_fallback.wav"

// Claro Argentina packet data. Voice/SMS still need CS/LTE attach.
#define COF_MODEM_APN "internet.claro.com.ar"
#define COF_MODEM_APN_USER "clarogprs"
#define COF_MODEM_APN_PASS "clarogprs777"
#define COF_MODEM_SMSC "+5491115030500"

// Default MQTT endpoint (TLS). Override from Serial with:
// mqtt HOST PORT DEVICE_ID [USER PASSWORD]
// Device username should match device_uid for Mosquitto ACL.
#define COF_DEFAULT_MQTT_HOST "mqtt.callonfail.com.ar"
#define COF_DEFAULT_MQTT_PORT 1883
#define COF_DEFAULT_MQTT_DEVICE_ID "cof-test"
#define COF_DEFAULT_MQTT_USERNAME "cof-test"
// Overridden by the real credential over Serial and stored in Preferences.
// This repo is PUBLIC: never commit a real value here, not even temporarily.
#define COF_DEFAULT_MQTT_PASSWORD "change-me-device-mqtt"

// ---------------------------------------------------------------------------
// Timing and behaviour constants.
//
// Moved here from main.cpp when it was split into modules: every module needs
// some of these, so they belong to the shared layer rather than to any one .cpp.
// Do not duplicate them back into a module.
//
// eth_*_t below come from ETH.h, so this header must be included after it.
// ---------------------------------------------------------------------------
constexpr uint8_t kEthPhyAddr = 1;
constexpr int kEthMdcPin = 23;
constexpr int kEthMdioPin = 18;
constexpr int kEthPowerPin = 16;
constexpr eth_clock_mode_t kEthClockMode = ETH_CLOCK_GPIO0_IN;
constexpr eth_phy_type_t kEthPhyType = ETH_PHY_LAN8720;

constexpr uint32_t kDisplayIntervalMs = 1000;
constexpr uint32_t kSensorIntervalMs = 3000;
constexpr uint32_t kModemIntervalMs = 30000;
constexpr uint32_t kModemRetryNoLanMs = 5000;
// A dead modem (off, unseated SIM, UART fault) made pollModem() re-run the full
// initModem() on every poll - 5 s on LTE, 30 s on LAN - and each init blocks up
// to ~7 s on 5 AT attempts, starving sensors, display and MQTT. Back the retries
// off so the rest of the loop stays responsive while the modem is unreachable.
constexpr uint32_t kModemInitRetryMs = 15000;
constexpr uint32_t kLteRetryIntervalMs = 10000;
constexpr uint32_t kSmsPollIntervalMs = 5000;
constexpr uint32_t kMqttReconnectIntervalMs = 5000;
constexpr uint32_t kMqttKeepAliveSeconds = 30;
constexpr uint32_t kMqttSocketTimeoutSeconds = 3;
constexpr uint8_t kLanMqttFailLimit = 2;
constexpr uint32_t kWifiBackupDelayMs = 1500;
constexpr uint8_t kWifiAuthFailLimit = 3;
// Path health. A LAN interface is only trusted as "the internet" after a real
// broker reachability probe: a router with no uplink still hands out DHCP
// leases, and treating that as internet used to pin the device on a dead path
// (lanConnected() stayed true, so LTE never engaged, and nothing re-probed
// Ethernet once LTE was up). See docs/device/NETWORK_PATHS.md.
constexpr uint32_t kEthProbeIntervalMs = 10000;
constexpr uint8_t kEthProbeFailLimit = 2;
constexpr uint8_t kWifiProbeFailLimit = 2;
constexpr uint32_t kWifiGraceBeforeLteMs = 8000;
constexpr uint32_t kLteBootGraceMs = 15000;
constexpr uint32_t kEthernetHoldoffMs = 20000;
// Ethernet marked down with the cable still in: its DHCP lease survives a
// router reboot, so no GOT_IP event ever fires again. Re-probe on this cadence.
constexpr uint32_t kPathRecoverProbeIntervalMs = 60UL * 1000UL;
// lanPathReachable() is reached from canUseLan(), which maintainLteFallback()
// runs on every loop pass (~20 ms). Each call costs up to two 1500 ms probes plus
// two route flips, and the state that reaches it ("interface connected but
// flagged without internet") lasts as long as LTE is up - so it used to block the
// whole loop continuously. Same cadence as the path polls, which is the point:
// those already probe and cache the same verdict.
constexpr uint32_t kLanReachableProbeIntervalMs = 10000;
// While MQTT runs over LTE, settle a recovered better path for this long before
// tearing the PDP down, so a flapping link cannot cause a reconnect storm.
constexpr uint32_t kPathPreemptSettleMs = 3000;
// Publishing liveness. This MUST stay above the largest telemetry interval we
// accept or it forces a disconnect and reconnect every cycle - and each reconnect
// republishes telemetry, which is what produced the observed ~20 s cadence.
//
// kTelemetryIntervalMaxSeconds below is the number that matters here, and the
// previous comment claimed 60 s while the config path actually accepted 3600 s.
// The backend form now caps the interval at the same 300 s and rejects anything
// above it at save time, so a config above this watchdog can no longer be stored.
// Keep the two in sync when either changes.
constexpr uint32_t kTelemetryIntervalMinSeconds = 10;
constexpr uint32_t kTelemetryIntervalMaxSeconds = 300;
constexpr uint32_t kMqttSilenceReconnectMs = 90UL * 1000UL;
constexpr uint32_t kSilenceProbeIntervalMs = 45UL * 1000UL;
constexpr uint32_t kMqttSilenceRestartMs = 6UL * 60UL * 1000UL;
// Modem radio recovery. pollModem() runs every 30 s on LAN and 5 s without it, so
// this interval only has to be long enough for one recovery step to take effect.
// 150 s over 5 steps means a stuck radio escalates to a full ESP32 restart in
// about 12 min, instead of staying dead until someone drives to the site.
constexpr uint32_t kModemRecoveryIntervalMs = 150UL * 1000UL;
constexpr uint8_t kModemRecoveryMaxStage = 5;
// Anti-flap hysteresis for the radio recovery ladder. On a marginal LTE signal
// the modem flaps NO SERVICE <-> service; a single good read used to reset the
// ladder, so step 1 (CGATT detach/attach) re-ran on every flap and turned a
// minor hiccup into a ~45 s outage (13x "step 1/5" observed 2026-09-25). Only
// clear the stage once the radio has stayed healthy for this long.
constexpr uint32_t kModemRecoveryHoldMs = 60UL * 1000UL;
// The recovery ladder runs while MQTT is down, so its events pile up in the
// deferred queue and flush in a burst on reconnect. Rate-limit the event to
// once per this window (in addition to publishing only stage transitions).
constexpr uint32_t kModemRecoveryEventMinIntervalMs = 10UL * 60UL * 1000UL;
// lte_data traces carry a whole modem dump and were republished on every failed
// PDP attempt (every 10 s, forever). Back the retries off and throttle the event.
constexpr uint32_t kLteRetryMaxIntervalMs = 120UL * 1000UL;
constexpr uint32_t kLteDataEventMinIntervalMs = 30UL * 1000UL;
// Events produced while MQTT is down (SMS/call results) used to be dropped by
// publishMqttJson. Queue a bounded number and flush them on reconnect.
constexpr size_t kDeferredEventMax = 8;
constexpr uint32_t kTelemetryPublishIntervalMs = 60000;
constexpr uint32_t kCellularStatusIntervalMs = 5UL * 60UL * 1000UL;
constexpr uint32_t kManifestInitialDelayMs = 15000;
constexpr uint32_t kManifestIntervalMs = 60UL * 60UL * 1000UL;
constexpr uint32_t kWatchdogTimeoutSeconds = 60;

// After an OTA the bootloader holds the new image in ESP_OTA_IMG_PENDING_VERIFY
// (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y). If we never confirm, the next reset
// rolls back to the previous slot. That is the safety net for a remote device
// we cannot reach physically, so only confirm once the device proves it works:
// MQTT connected (which implies link + broker reachable) plus a minimum uptime
// so we do not confirm during a flapping reconnect.
constexpr uint32_t kOtaConfirmMinUptimeMs = 20UL * 1000UL;
constexpr uint32_t kOtaConfirmTimeoutMs = 5UL * 60UL * 1000UL;

// Non-blocking lwIP DNS for the broker hostname.
//
// cachedMqttIp is only learned from a successful MQTT connect and lives in RAM,
// so after every reboot it is 0.0.0.0 until that first connect - and the
// reachability probes (probeMqttOverEthernet / lanPathReachable) all bail out on
// an unset IP. That left the boot window relying entirely on MQTT connect
// failures to demote a dead LAN path.
//
// We cannot use the synchronous WiFi.hostByName() to close that: it waits up to
// 15-16 s on the lwIP DNS semaphore, which is exactly why it was rejected for
// probe use in the first place. So this drives the same lwIP call the core uses
// (dns_gethostbyname, see WiFiGenericClass::hostByName) but never waits: the
// callback only stores the result and the loop picks it up on its next pass.
//
// Resolved once at boot, then refreshed hourly. The refresh matters because the
// broker hostname can be repointed by DNS, and a probe holding a stale address
// would keep declaring a healthy path dead (or worse, declare a dead one alive).
constexpr uint32_t kBrokerResolveIntervalMs = 60UL * 60UL * 1000UL;
constexpr uint32_t kBrokerResolveRetryMs = 15UL * 1000UL;

constexpr uint16_t kModemCallLogMax = 1800;
