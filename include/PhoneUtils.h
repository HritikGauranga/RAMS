#pragma once

#include <Arduino.h>

bool util_isValidPhoneFormat(const String& number);
bool util_normalizePhoneNumber(const String& input, String& normalized);
