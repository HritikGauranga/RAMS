#pragma once

#include <Arduino.h>

bool util_parseIPv4(const String& text, IPAddress& ip);
String util_ipToString(const IPAddress& ip);
