#include "cof_config.h"
#include "cof_state.h"
#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Adafruit_SHT31.h>
#include <DallasTemperature.h>
#include <OneWire.h>

// Extracted verbatim from main.cpp, which used to hold every function

String ds18b20AddressToString(const DeviceAddress address) {
  char buffer[17];
  for (uint8_t i = 0; i < 8; i++) {
    snprintf(&buffer[i * 2], 3, "%02X", address[i]);
  }
  buffer[16] = '\0';
  return String(buffer);
}
bool i2cDevicePresent(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}
void scanI2cBus() {
  Serial.printf("[i2c] scan SDA=%d SCL=%d\n", COF_PIN_I2C_SDA, COF_PIN_I2C_SCL);
  uint8_t found = 0;
  for (uint8_t address = 1; address < 127; address++) {
    if (i2cDevicePresent(address)) {
      Serial.printf("[i2c] found device at 0x%02X\n", address);
      found++;
    }
  }
  if (found == 0) {
    Serial.println("[i2c] no devices found");
  }
}
void initDisplay() {
  scanI2cBus();

  if (i2cDevicePresent(0x3C)) {
    state.oledAddress = 0x3C;
  } else if (i2cDevicePresent(0x3D)) {
    state.oledAddress = 0x3D;
  } else {
    state.oledReady = false;
    setStatus("OLED no I2C");
    return;
  }

  display.setI2CAddress(state.oledAddress << 1);
  display.begin();
  state.oledReady = true;
  setStatus("OLED 0x" + String(state.oledAddress, HEX) + " OK");
}
void detectPcf8574() {
  const uint8_t ranges[][2] = {
    {0x20, 0x27},
    {0x38, 0x3F},
  };

  for (const auto& range : ranges) {
    for (uint8_t address = range[0]; address <= range[1]; address++) {
      Wire.beginTransmission(address);
      if (Wire.endTransmission() == 0) {
        state.pcfReady = true;
        state.pcfAddress = address;
        Wire.beginTransmission(address);
        Wire.write(0xFF);
        Wire.endTransmission();
        Serial.printf("[i2c] PCF8574 found at 0x%02X\n", address);
        return;
      }
    }
  }

  state.pcfReady = false;
  Serial.println("[i2c] PCF8574 not found");
}
void readSensors() {
  if (state.sht31Ready) {
    state.shtTemperature = sht31.readTemperature();
    state.shtHumidity = sht31.readHumidity();
  }

  if (state.ds18b20Ready) {
    ds18b20.requestTemperatures();
    const float temp = ds18b20.getTempCByIndex(0);
    if (temp > -100.0F && temp < 125.0F) {
      state.dsTemperature = temp;
    }
  }

  state.zmptRaw = analogRead(COF_PIN_ZMPT_ADC);
}
void drawDisplay() {
  if (!state.oledReady) {
    return;
  }

  char line[24];
  display.clearBuffer();
  display.setFont(u8g2_font_5x8_tf);

  display.drawStr(0, 8, "CallOnFail");
  snprintf(line, sizeof(line), "%s", COF_FIRMWARE_VERSION);
  display.drawStr(92, 8, line);

  if (state.ethernetConnected) {
    snprintf(line, sizeof(line), "E %s", state.ipAddress.c_str());
  } else {
    snprintf(line, sizeof(line), "E --");
  }
  display.drawStr(0, 16, line);

  if (state.wifiConnected) {
    snprintf(line, sizeof(line), "W %s", state.wifiIpAddress.c_str());
  } else if (state.wifiConfigured) {
    String ssid = state.wifiSsid;
    if (ssid.length() > 12) {
      ssid = ssid.substring(0, 12);
    }
    if (wifiAuthFailCount >= kWifiAuthFailLimit) {
      snprintf(line, sizeof(line), "W auth %s", ssid.c_str());
    } else {
      snprintf(line, sizeof(line), "W ... %s", ssid.c_str());
    }
  } else {
    snprintf(line, sizeof(line), "W --");
  }
  display.drawStr(0, 24, line);

  if (state.lteDataUp) {
    snprintf(line, sizeof(line), "L %s", state.lteIpAddress.c_str());
  } else if (!state.modemReady) {
    snprintf(line, sizeof(line), "L no AT");
  } else if (!state.simReady) {
    snprintf(line, sizeof(line), millis() < 30000 ? "L SIM..." : "L no SIM");
  } else if (lanConnected()) {
    snprintf(line, sizeof(line), "L --");
  } else if (reportLteProgress) {
    snprintf(line, sizeof(line), "L try");
  } else if (lastLteFail) {
    snprintf(line, sizeof(line), "L fail");
  } else {
    snprintf(line, sizeof(line), "L ...");
  }
  display.drawStr(0, 32, line);

  if (state.sht31Ready && !isnan(state.shtTemperature) && !isnan(state.shtHumidity)) {
    snprintf(line, sizeof(line), "SHT %.1fC %.0f%%", state.shtTemperature, state.shtHumidity);
  } else {
    snprintf(line, sizeof(line), "SHT --");
  }
  display.drawStr(0, 40, line);

  if (state.ds18b20Ready && !isnan(state.dsTemperature)) {
    snprintf(line, sizeof(line), "DS18 %.1fC", state.dsTemperature);
  } else {
    snprintf(line, sizeof(line), "DS18 --");
  }
  display.drawStr(0, 48, line);

  snprintf(line, sizeof(line), "ADC %d MQTT %s", state.zmptRaw, mqttPathLetter());
  display.drawStr(0, 56, line);

  String footer = state.statusLine;
  if (footer.length() > 21) {
    footer = footer.substring(0, 21);
  }
  display.drawStr(0, 63, footer.c_str());

  display.sendBuffer();
}
