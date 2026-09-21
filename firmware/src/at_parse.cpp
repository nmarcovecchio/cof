#include "cof_config.h"
#include "cof_state.h"
#include <Arduino.h>

// Extracted verbatim from main.cpp, which used to hold every function

String mqttTopic(const String& suffix) {
  return "devices/" + state.mqttDeviceId + "/" + suffix;
}
String withFirmware(const String& message) {
  return message + " [" + COF_FIRMWARE_VERSION + "]";
}
bool phoneLooksValid(const String& phone) {
  if (phone.length() < 9 || phone.length() > 16 || !phone.startsWith("+")) {
    return false;
  }
  for (unsigned i = 1; i < phone.length(); i++) {
    if (phone[i] < '0' || phone[i] > '9') {
      return false;
    }
  }
  return true;
}
int compareVersions(const String& a, const String& b) {
  int ai = 0;
  int bi = 0;
  while (ai < a.length() || bi < b.length()) {
    long av = 0;
    long bv = 0;
    while (ai < a.length() && a[ai] != '.') {
      if (isDigit(a[ai])) {
        av = av * 10 + (a[ai] - '0');
      }
      ai++;
    }
    while (bi < b.length() && b[bi] != '.') {
      if (isDigit(b[bi])) {
        bv = bv * 10 + (b[bi] - '0');
      }
      bi++;
    }
    if (av != bv) {
      return av > bv ? 1 : -1;
    }
    ai++;
    bi++;
  }
  return 0;
}
String lastQuoted(const String& text) {
  const int last = text.lastIndexOf('"');
  const int prev = last > 0 ? text.lastIndexOf('"', last - 1) : -1;
  if (prev < 0 || last <= prev + 1) {
    return "";
  }
  return text.substring(prev + 1, last);
}
int atUrcCode(const String& resp, const char* tag) {
  const int tagAt = resp.indexOf(tag);
  if (tagAt < 0) {
    return -1;
  }
  const int comma = resp.indexOf(',', tagAt);
  if (comma >= 0) {
    return resp.substring(comma + 1).toInt();
  }
  return resp.substring(tagAt + static_cast<int>(strlen(tag))).toInt();
}
bool looksLikeIp(const String& ip) {
  return ip.length() >= 7 && ip != "0.0.0.0" && ip.indexOf('.') > 0;
}
int parseCommaStat(const String& response, const char* tag) {
  const int tagAt = response.indexOf(tag);
  if (tagAt < 0) {
    return -1;
  }
  const int comma = response.indexOf(',', tagAt);
  if (comma < 0) {
    return -1;
  }
  return response.substring(comma + 1).toInt();
}
String extractQuoted(const String& response) {
  const int start = response.indexOf('"');
  if (start < 0) {
    return "";
  }
  const int end = response.indexOf('"', start + 1);
  if (end < 0) {
    return "";
  }
  return response.substring(start + 1, end);
}
String firstNonEmptyAtLine(const String& response) {
  int start = 0;
  while (start < static_cast<int>(response.length())) {
    int end = response.indexOf('\n', start);
    if (end < 0) {
      end = response.length();
    }
    String line = response.substring(start, end);
    line.replace("\r", "");
    line.trim();
    start = end + 1;
    if (line.length() == 0 || line == "OK" || line == "ERROR" || line.startsWith("AT")) {
      continue;
    }
    return line;
  }
  return "";
}
String extractAtTagValue(const String& response, const char* tag) {
  const int idx = response.indexOf(tag);
  if (idx < 0) {
    return "";
  }
  int start = idx + static_cast<int>(strlen(tag));
  while (start < static_cast<int>(response.length()) &&
         (response[start] == ' ' || response[start] == ':')) {
    start++;
  }
  int end = start;
  while (end < static_cast<int>(response.length()) &&
         response[end] != '\r' && response[end] != '\n') {
    end++;
  }
  String value = response.substring(start, end);
  value.trim();
  return value;
}
String nthQuoted(const String& line, int want) {
  int seen = 0;
  int start = -1;
  for (int i = 0; i < line.length(); i++) {
    if (line[i] != '"') {
      continue;
    }
    if (start < 0) {
      start = i + 1;
    } else {
      seen++;
      if (seen == want) {
        return line.substring(start, i);
      }
      start = -1;
    }
  }
  return "";
}
String compactAtText(const String& raw) {
  String compact = raw;
  compact.toUpperCase();
  compact.replace(" ", "");
  compact.replace("\r", "");
  compact.replace("\n", "");
  return compact;
}
int parseCeerCode(const String& ceer) {
  for (int i = 0; i < ceer.length(); i++) {
    if (isDigit(ceer[i])) {
      return ceer.substring(i).toInt();
    }
  }
  return -1;
}
bool ceerLooksRejected(const String& ceer) {
  String upper = ceer;
  upper.toUpperCase();
  if (upper.indexOf("NO SERVICE") >= 0) {
    return false;
  }
  const int code = parseCeerCode(ceer);
  return code == 17 || code == 21 || code == 22 ||
         upper.indexOf("BUSY") >= 0 || upper.indexOf("REJECT") >= 0 ||
         upper.indexOf("USER BUSY") >= 0;
}
bool ceerLooksNoAnswer(const String& ceer) {
  String upper = ceer;
  upper.toUpperCase();
  const int code = parseCeerCode(ceer);
  return code == 18 || code == 19 ||
         upper.indexOf("NO ANSWER") >= 0 || upper.indexOf("NO USER") >= 0;
}
