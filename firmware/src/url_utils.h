#pragma once

// ─── Percent-encoding for URL query parameters ───

inline String urlEncode(const String& s) {
    String out;
    out.reserve(s.length() * 2);
    for (size_t i = 0; i < s.length(); i++) {
        char c = s.charAt(i);
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else if (c == ' ') {
            out += "%20";
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", (uint8_t)c);
            out += buf;
        }
    }
    return out;
}
