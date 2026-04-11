#include "google_photos.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

String googlePhotosGetUrl(const char* gasUrl) {
    if (!gasUrl || strlen(gasUrl) == 0) return "";

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, gasUrl);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(10000);

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[Photos] HTTP %d\n", code);
        http.end();
        return "";
    }

    JsonDocument doc;
    if (deserializeJson(doc, http.getString())) {
        http.end();
        return "";
    }
    http.end();

    const char* url = doc["url"];
    if (!url) return "";

    Serial.printf("[Photos] Got URL: %s\n", url);
    return String(url);
}
