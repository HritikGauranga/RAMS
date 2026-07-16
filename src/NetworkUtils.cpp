#include "NetworkUtils.h"

bool util_parseIPv4(const String& text, IPAddress& ip) {
  int parts[4] = {0, 0, 0, 0};
  int p = 0;
  String token = "";
  for (size_t i = 0; i < text.length(); ++i) {
    char c = text.charAt(i);
    if (c == '.') {
      if (p > 2 || token.length() == 0) return false;
      parts[p++] = token.toInt();
      token = "";
      continue;
    }
    if (c < '0' || c > '9') return false;
    token += c;
  }
  if (p != 3 || token.length() == 0) return false;
  parts[3] = token.toInt();
  for (int i = 0; i < 4; ++i) {
    if (parts[i] < 0 || parts[i] > 255) return false;
  }
  ip = IPAddress(parts[0], parts[1], parts[2], parts[3]);
  return true;
}

String util_ipToString(const IPAddress& ip) {
  return String(ip[0]) + "." + String(ip[1]) + "." + String(ip[2]) + "." + String(ip[3]);
}
