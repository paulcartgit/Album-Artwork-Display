#include "shazam_client.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

bool shazamIdentify(const char* rapidApiKey,
                    const uint8_t* audioData, size_t audioLen,
                    ShazamResult& result) {
    result.found = false;

    Serial.printf("[Shazam] Sending %u bytes as multipart file upload\n", (unsigned)audioLen);

    // Build multipart/form-data body in PSRAM
    String boundary = "----ShazamBoundary9876543210";

    // File part header
    String partHeader = "--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n";
    String partFooter = "\r\n--" + boundary + "--\r\n";

    size_t totalLen = partHeader.length() + audioLen + partFooter.length();
    uint8_t* body = (uint8_t*)heap_caps_malloc(totalLen, MALLOC_CAP_SPIRAM);
    if (!body) {
        Serial.println("[Shazam] Body alloc failed");
        return false;
    }
    memcpy(body, partHeader.c_str(), partHeader.length());
    memcpy(body + partHeader.length(), audioData, audioLen);
    memcpy(body + partHeader.length() + audioLen, partFooter.c_str(), partFooter.length());

    // HTTPS POST to Shazam Song Recognition API
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, "https://shazam-song-recognition-api.p.rapidapi.com/recognize/file");
    http.setTimeout(20000);
    http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    http.addHeader("x-rapidapi-key", rapidApiKey);
    http.addHeader("x-rapidapi-host", "shazam-song-recognition-api.p.rapidapi.com");

    int code = http.POST(body, totalLen);
    heap_caps_free(body);

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
