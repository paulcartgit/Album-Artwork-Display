#include "shazam_client.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>
#include <esp_heap_caps.h>

bool shazamIdentify(const char* rapidApiKey,
                    const uint8_t* audioData, size_t audioLen,
                    ShazamResult& result) {
    result.found = false;

    // Base64-encode the audio data
    size_t b64Len = 0;
    mbedtls_base64_encode(nullptr, 0, &b64Len, audioData, audioLen);

    uint8_t* b64Buf = (uint8_t*)heap_caps_malloc(b64Len + 1, MALLOC_CAP_SPIRAM);
    if (!b64Buf) {
        Serial.println("[Shazam] Base64 alloc failed");
        return false;
    }

    size_t written = 0;
    mbedtls_base64_encode(b64Buf, b64Len + 1, &written, audioData, audioLen);
    b64Buf[written] = 0;

    Serial.printf("[Shazam] Audio %u bytes → base64 %u bytes\n",
                  (unsigned)audioLen, (unsigned)written);

    // HTTPS POST to Shazam detect endpoint
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, "https://shazam.p.rapidapi.com/songs/detect");
    http.setTimeout(20000);
    http.addHeader("Content-Type", "text/plain");
    http.addHeader("x-rapidapi-key", rapidApiKey);
    http.addHeader("x-rapidapi-host", "shazam.p.rapidapi.com");

    int code = http.POST(b64Buf, written);
    heap_caps_free(b64Buf);

    if (code != HTTP_CODE_OK) {
        String body = http.getString();
        Serial.printf("[Shazam] HTTP %d: %s\n", code, body.c_str());
        http.end();
        return false;
    }

    String response = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, response)) {
        Serial.println("[Shazam] JSON parse failed");
        return false;
    }

    // Check if a track was found
    if (!doc["track"].is<JsonObject>()) {
        Serial.println("[Shazam] No track in response");
        Serial.printf("[Shazam] Response: %s\n", response.c_str());
        return false;
    }

    result.title  = doc["track"]["title"].as<String>();
    result.artist = doc["track"]["subtitle"].as<String>();

    // Extract album from metadata sections
    JsonArray sections = doc["track"]["sections"].as<JsonArray>();
    for (JsonObject section : sections) {
        if (section["type"] == "SONG") {
            JsonArray metadata = section["metadata"].as<JsonArray>();
            for (JsonObject meta : metadata) {
                if (String(meta["title"].as<const char*>()) == "Album") {
                    result.album = meta["text"].as<String>();
                }
            }
        }
    }

    // Extract cover art URL if available
    if (doc["track"]["images"]["coverarthq"].is<const char*>()) {
        result.coverArtUrl = doc["track"]["images"]["coverarthq"].as<String>();
    } else if (doc["track"]["images"]["coverart"].is<const char*>()) {
        result.coverArtUrl = doc["track"]["images"]["coverart"].as<String>();
    }

    result.found = result.artist.length() > 0 && result.title.length() > 0;

    Serial.printf("[Shazam] Found: %s — %s (%s)\n",
                  result.artist.c_str(), result.title.c_str(), result.album.c_str());
    if (result.coverArtUrl.length() > 0)
        Serial.printf("[Shazam] Cover: %s\n", result.coverArtUrl.c_str());

    return result.found;
}
