#include "StringUtils.h"

String util_trimCopy(const String& input) {
  String copy = input;
  copy.trim();
  return copy;
}

String util_escapeJson(const String& input) {
  String out;
  out.reserve(input.length() * 2);
  for (size_t i = 0; i < input.length(); ++i) {
    char c = input.charAt(i);
    if (c == '"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else out += c;
  }
  return out;
}
