#include "metadata_client.h"
#include "activity_log.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

// MusicBrainz asks for a User-Agent identifying the application and a contact.
// Sending a generic one is how clients get blocked.
static const char* MB_USER_AGENT =
    "NowPlayingFrame/1.0 ( https://github.com/paulcartgit/Album-Artwork-Display )";

// MusicBrainz permits one request per second per client, averaged. Artwork
// changes far more slowly than that, but a burst on first boot could trip it.
static const unsigned long MB_MIN_INTERVAL_MS = 1200;
static unsigned long s_lastRequest = 0;

static String urlEscape(const String& s) {
    String out;
    out.reserve(s.length() * 3);
    for (size_t i = 0; i < s.length(); i++) {
        char c = s.charAt(i);
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", (uint8_t)c);
            out += buf;
        }
    }
    return out;
}

// Lucene special characters would otherwise break the query, and album titles
// are full of them — brackets, colons, "AC/DC".
static String luceneEscape(const String& s) {
    String out;
    for (size_t i = 0; i < s.length(); i++) {
        char c = s.charAt(i);
        if (strchr("+-&|!(){}[]^\"~*?:\\/", c)) out += '\\';
        out += c;
    }
    return out;
}

bool metadataLookup(const char* artist, const char* album, ReleaseInfo& out) {
    out = ReleaseInfo();
    if (!artist || !artist[0] || !album || !album[0]) return false;

    unsigned long since = millis() - s_lastRequest;
    if (s_lastRequest != 0 && since < MB_MIN_INTERVAL_MS) {
        delay(MB_MIN_INTERVAL_MS - since);
    }
    s_lastRequest = millis();

    String query = "artist:\"" + luceneEscape(artist) + "\" AND release:\"" +
                   luceneEscape(album) + "\"";
    String url = "https://musicbrainz.org/ws/2/release/?query=" + urlEscape(query) +
                 "&fmt=json&limit=12";

    WiFiClientSecure client;
    // No credentials are sent and the response is public data, so a pinned root
    // would be maintenance for no security gain. Contrast the Shazam call,
    // which carries an API key and is pinned.
    client.setInsecure();

    HTTPClient http;
    http.begin(client, url);
    http.setConnectTimeout(5000);
    http.setTimeout(8000);
    http.addHeader("User-Agent", MB_USER_AGENT);
    http.addHeader("Accept", "application/json");

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[Meta] MusicBrainz HTTP %d\n", code);
        http.end();
        return false;
    }

    // Only the handful of fields we use, so the document stays small.
    JsonDocument filter;
    JsonObject rel = filter["releases"].add<JsonObject>();
    rel["date"] = true;
    rel["country"] = true;
    rel["label-info"][0]["catalog-number"] = true;
    rel["label-info"][0]["label"]["name"] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream(),
                                               DeserializationOption::Filter(filter));
    http.end();
    if (err) {
        Serial.printf("[Meta] JSON parse failed: %s\n", err.c_str());
        return false;
    }

    JsonArray releases = doc["releases"].as<JsonArray>();
    if (releases.isNull() || releases.size() == 0) return false;

    // Prefer the EARLIEST pressing that carries a catalogue number.
    //
    // Two separate corrections, both measured against real records. MusicBrainz
    // often lists a bare digital release ahead of the physical pressing, and
    // the pressing is what matters beside a turntable. And its first match is
    // frequently a reissue: Help! came back as a 1985 Parlophone repress until
    // this preferred the earliest, which is the 1965 Odeon original.
    JsonObject best;
    long bestYear = 999999;
    for (JsonObject r : releases) {
        JsonArray li = r["label-info"].as<JsonArray>();
        bool hasCat = !li.isNull() && li.size() > 0 &&
                      li[0]["catalog-number"].is<const char*>();
        if (!hasCat) continue;
        String date = r["date"] | "";
        long year = (date.length() >= 4) ? date.substring(0, 4).toInt() : 999998;
        if (year < bestYear) { bestYear = year; best = r; }
    }
    if (best.isNull()) best = releases[0];

    String date = best["date"] | "";
    if (date.length() >= 4) out.year = date.substring(0, 4);
    out.country = best["country"] | "";

    JsonArray li = best["label-info"].as<JsonArray>();
    if (!li.isNull() && li.size() > 0) {
        out.label = li[0]["label"]["name"] | "";
        out.catalogNumber = li[0]["catalog-number"] | "";
    }

    out.found = out.year.length() || out.label.length() || out.catalogNumber.length();
    if (out.found) {
        Serial.printf("[Meta] %s — %s: %s\n", artist, album,
                      metadataSummary(out).c_str());
    }
    return out.found;
}

String metadataSummary(const ReleaseInfo& info) {
    String s;
    auto add = [&](const String& part) {
        if (!part.length()) return;
        if (s.length()) s += " · ";     // middle dot
        s += part;
    };
    add(info.year);
    add(info.label);
    add(info.catalogNumber);
    return s;
}
