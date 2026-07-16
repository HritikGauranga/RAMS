#include "PhoneUtils.h"
#include "Shared.h"

bool util_isValidPhoneFormat(const String& number) {
  String trimmed = number;
  trimmed.trim();
  if (trimmed.length() == 0) return false;
  if (trimmed.length() > PHONE_NUMBER_LENGTH - 1) return false;
  if (trimmed.charAt(0) == '+') {
    size_t digitCount = trimmed.length() - 1;
    if (digitCount < 10 || digitCount > 15) return false;
    for (size_t i = 1; i < trimmed.length(); ++i) {
      char c = trimmed.charAt(i);
      if (c < '0' || c > '9') return false;
    }
    return true;
  } else {
    size_t digitCount = trimmed.length();
    if (digitCount < 10 || digitCount > 15) return false;
    for (size_t i = 0; i < trimmed.length(); ++i) {
      char c = trimmed.charAt(i);
      if (c < '0' || c > '9') return false;
    }
    return true;
  }
}

bool util_normalizePhoneNumber(const String& input, String& normalized) {
  normalized = "";
  String number = input;
  number.trim();
  if (number.length() == 0) return false;

  bool plusSeen = false;
  for (size_t i = 0; i < number.length(); ++i) {
    char c = number.charAt(i);
    if (c == '+') {
      if (i != 0 || plusSeen) return false;
      plusSeen = true;
      normalized += c;
      continue;
    }
    if (c >= '0' && c <= '9') {
      normalized += c;
      continue;
    }
    return false;
  }

  size_t digitStart = (normalized.length() > 0 && normalized.charAt(0) == '+') ? 1 : 0;
  size_t digitCount = normalized.length() - digitStart;
  return digitCount >= 10 && digitCount <= 15;
}
