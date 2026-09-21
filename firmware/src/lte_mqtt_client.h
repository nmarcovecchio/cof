#pragma once
// LteMqttClient: Client implementation that speaks MQTT over the modem's
// transparent-mode socket. Extracted verbatim from main.cpp.
//
// Its inline methods read the shared variables AND call the AT helpers, so it
// must be included after both. cof_state.h does that (variables, then
// cof_api.h, then this file), which is why the includes below find everything
// in scope.

#include <Arduino.h>
#include <Client.h>

#include "cof_config.h"
#include "cof_state.h"
#include "cof_api.h"

class LteMqttClient : public Client {
 public:
  uint8_t sock = 0;
  bool sockOpen = false;
  uint8_t rxBuf[512];
  int rxLen = 0;
  int rxPos = 0;
  bool dataInd = false;

  void drainRx() {
    rxLen = 0;
    rxPos = 0;
    dataInd = false;
  }

  uint32_t lastRxPollMs = 0;

  bool useNetopen() const {
    return lteIpStack != kLteStackCnact;
  }

  void noteUrc(const String& line) {
    if (line.startsWith("+CADATAIND:") || line.startsWith("+CARECV:") ||
        line.startsWith("+CIPRXGET: 1")) {
      dataInd = true;
    } else if ((line.startsWith("+CASTATE:") && line.indexOf(",0") > 0) ||
               line.startsWith("+IPCLOSE:") ||
               line.startsWith("+CIPEVENT:")) {
      sockOpen = false;
    }
  }

  void pumpUrcs() {
    while (ModemSerial.available()) {
      String line;
      const uint32_t start = millis();
      while (millis() - start < 50) {
        if (!ModemSerial.available()) {
          delay(1);
          continue;
        }
        const char c = static_cast<char>(ModemSerial.read());
        if (c == '\n') {
          break;
        }
        if (c != '\r') {
          line += c;
        }
      }
      line.trim();
      if (line.length() == 0) {
        continue;
      }
      noteUrc(line);
      if (!line.startsWith("+CIPRXGET: 1") && !line.startsWith("+CADATAIND:") &&
          !line.startsWith("+CARECV:")) {
        pendingModemUrcs += line + "\n";
      }
    }
    if (pendingModemUrcs.indexOf("+CIPRXGET: 1") >= 0 ||
        pendingModemUrcs.indexOf("+CADATAIND:") >= 0) {
      dataInd = true;
    }
    if (pendingModemUrcs.indexOf("+IPCLOSE:") >= 0 ||
        pendingModemUrcs.indexOf("+CIPEVENT:") >= 0) {
      sockOpen = false;
    }
  }

  bool recvChunk() {
    if (rxPos < rxLen) {
      return true;
    }
    rxLen = 0;
    rxPos = 0;
    const bool netopen = useNetopen();
    if (netopen) {
      ModemSerial.print("AT+CIPRXGET=2,");
      ModemSerial.print(sock);
      ModemSerial.print(",512\r\n");
    } else {
      ModemSerial.print("AT+CARECV=");
      ModemSerial.print(sock);
      ModemSerial.print(",512\r\n");
    }
    String header;
    const uint32_t startedAt = millis();
    const char* tag = netopen ? "+CIPRXGET: 2," : "+CARECV:";
    while (millis() - startedAt < 3000) {
      feedWatchdog();
      while (ModemSerial.available()) {
        const char c = static_cast<char>(ModemSerial.read());
        header += c;
        const int tagAt = header.indexOf(tag);
        const int nl = tagAt >= 0 ? header.indexOf('\n', tagAt) : -1;
        if (tagAt >= 0 && nl > tagAt) {
          const String line = header.substring(tagAt, nl);
          int n = 0;
          if (netopen) {
            const int c1 = line.indexOf(',');
            const int c2 = c1 >= 0 ? line.indexOf(',', c1 + 1) : -1;
            const int c3 = c2 >= 0 ? line.indexOf(',', c2 + 1) : -1;
            if (c2 >= 0 && c3 > c2) {
              n = line.substring(c2 + 1, c3).toInt();
            } else if (c2 >= 0) {
              n = line.substring(c2 + 1).toInt();
            }
          } else {
            const int comma = line.indexOf(',');
            if (comma >= 0) {
              n = line.substring(comma + 1).toInt();
            }
          }
          if (n <= 0) {
            dataInd = false;
            return false;
          }
          if (n > static_cast<int>(sizeof(rxBuf))) {
            n = sizeof(rxBuf);
          }
          int got = 0;
          while (got < n && millis() - startedAt < 3000) {
            feedWatchdog();
            if (ModemSerial.available()) {
              rxBuf[got++] = static_cast<uint8_t>(ModemSerial.read());
            }
          }
          rxLen = got;
          rxPos = 0;
          dataInd = false;
          return rxLen > 0;
        }
        if (header.indexOf("ERROR") >= 0) {
          dataInd = false;
          return false;
        }
      }
      delay(5);
    }
    dataInd = false;
    return false;
  }

  int connect(IPAddress ip, uint16_t port) override {
    return connect(ip.toString().c_str(), port);
  }

  int connect(const char* host, uint16_t port) override {
    stop();
    if (!state.lteDataUp) {
      return 0;
    }
    String resp;
    if (useNetopen()) {
      sendAT(String("AT+CIPCLOSE=") + String(sock), "OK", 5000);
      sendAT("AT+CIPRXGET=1", "OK", 3000);
      const String peer = resolveLteMqttPeer(host);
      String cmd = String("AT+CIPOPEN=") + String(sock) + ",\"TCP\",\"" + peer + "\"," + String(port);
      if (!sendAT(cmd, "+CIPOPEN:", 25000, &resp)) {
        Serial.println("[lte] CIPOPEN fail");
        sockOpen = false;
        return 0;
      }
      const int err = atUrcCode(resp, "+CIPOPEN:");
      if (err != 0) {
        Serial.printf("[lte] CIPOPEN err %d %s\n", err, resp.c_str());
        sendAT(String("AT+CIPCLOSE=") + String(sock), "OK", 5000);
        sockOpen = false;
        return 0;
      }
      Serial.printf("[lte] TCP %s:%u via %s NETOPEN\n", host, port, peer.c_str());
    } else {
      const char* proto = mqttUsesTls() ? "SSL" : "TCP";
      if (mqttUsesTls()) {
        sendAT("AT+CASSLCFG=0,\"ssl\",1", "OK", 3000);
      }
      String cmd = String("AT+CAOPEN=0,") + String(sock) + ",\"" + proto + "\",\"" + host + "\"," +
                   String(port);
      if (!sendAT(cmd, "+CAOPEN:", 25000, &resp) || atUrcCode(resp, "+CAOPEN:") != 0) {
        Serial.printf("[lte] CAOPEN err %s\n", resp.c_str());
        sockOpen = false;
        return 0;
      }
      Serial.printf("[lte] TCP %s:%u %s CNACT\n", host, port, proto);
    }
    drainRx();
    sockOpen = true;
    lastRxPollMs = millis();
    return 1;
  }

  size_t write(uint8_t b) override {
    return write(&b, 1);
  }

  size_t write(const uint8_t* buf, size_t size) override {
    if (!sockOpen || buf == nullptr || size == 0) {
      return 0;
    }
    size_t sent = 0;
    while (sent < size) {
      size_t chunk = size - sent;
      if (chunk > 1024) {
        chunk = 1024;
      }
      if (useNetopen()) {
        ModemSerial.print("AT+CIPSEND=");
      } else {
        ModemSerial.print("AT+CASEND=");
      }
      ModemSerial.print(sock);
      ModemSerial.print(",");
      ModemSerial.print(static_cast<unsigned>(chunk));
      ModemSerial.print("\r\n");
      if (!modemWaitForPrompt(5000)) {
        sockOpen = false;
        return sent;
      }
      ModemSerial.write(buf + sent, chunk);
      const String token = useNetopen() ? "+CIPSEND:" : "OK";
      const String resp = readModemUntil(15000, token);
      if (resp.indexOf("ERROR") >= 0 || (useNetopen() && resp.indexOf("+CIPSEND:") < 0) ||
          (!useNetopen() && resp.indexOf("OK") < 0)) {
        sockOpen = false;
        return sent;
      }
      sent += chunk;
    }
    return sent;
  }

  int available() override {
    if (!sockOpen) {
      return 0;
    }
    if (rxPos < rxLen) {
      return rxLen - rxPos;
    }
    pumpUrcs();
    if (!dataInd && useNetopen() && millis() - lastRxPollMs >= 250) {
      lastRxPollMs = millis();
      dataInd = true;
    }
    if (dataInd) {
      recvChunk();
    }
    return rxLen - rxPos;
  }

  int read() override {
    if (available() <= 0) {
      return -1;
    }
    return rxBuf[rxPos++];
  }

  int read(uint8_t* buf, size_t size) override {
    if (buf == nullptr || size == 0) {
      return 0;
    }
    int n = 0;
    while (n < static_cast<int>(size) && available() > 0) {
      buf[n++] = rxBuf[rxPos++];
    }
    return n;
  }

  int peek() override {
    if (available() <= 0) {
      return -1;
    }
    return rxBuf[rxPos];
  }

  void flush() override {}

  void stop() override {
    if (sockOpen) {
      if (useNetopen()) {
        sendAT(String("AT+CIPCLOSE=") + String(sock), "OK", 8000);
      } else {
        sendAT(String("AT+CACLOSE=") + String(sock), "OK", 5000);
      }
    }
    sockOpen = false;
    drainRx();
  }

  uint8_t connected() override {
    return sockOpen ? 1 : 0;
  }

  operator bool() {
    return connected();
  }
};

