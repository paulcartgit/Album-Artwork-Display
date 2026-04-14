#include "web_server.h"
#include "web_portal.h"
#include "config.h"
#include "sd_manager.h"
#include "sonos_client.h"
#include "image_pipeline.h"
#include "activity_log.h"

#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <SD_MMC.h>
#include <WiFi.h>

// Globals declared in main.cpp
extern Settings   g_settings;
extern AppState   g_state;
extern String     g_currentArtist;
extern String     g_currentTitle;
extern String     g_currentAlbum;
extern volatile bool g_forceRefresh;
extern volatile bool g_testColors;
extern volatile bool g_forceListen;
extern uint8_t* g_lastAudio;
extern size_t   g_lastAudioLen;
extern uint32_t g_lastAudioChannels;
extern uint32_t g_lastAudioSampleRate;
extern unsigned long g_lastPollTime;
extern unsigned long g_lastNoMatchTime;
extern int g_vinylNoMatchCount;
extern unsigned long g_lastVinylMatchTime;
extern String g_lastArtUrl;

static AsyncWebServer server(80);

void webServerInit() {
    // ─── Serve HTML ───
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send_P(200, "text/html", INDEX_HTML);
    });

    // ─── Status API ───
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        const char* stateNames[] = {"BOOT","IDLE","DIGITAL","VINYL","ERROR"};
        int stateIdx = (int)g_state;
        doc["state"]      = stateIdx;
        doc["state_name"] = (stateIdx >= 0 && stateIdx < 5) ? stateNames[stateIdx] : "UNKNOWN";
        doc["artist"]     = g_currentArtist;
        doc["title"]      = g_currentTitle;
        doc["album"]      = g_currentAlbum;
        doc["art_url"]    = g_lastArtUrl;
        doc["ip"]         = WiFi.localIP().toString();
        doc["uptime"]     = millis() / 1000;

        // Timing: next Sonos poll
        unsigned long now = millis();
        unsigned long elapsed = now - g_lastPollTime;
        unsigned long pollInterval = g_settings.sonos_poll_ms;
        if (elapsed < pollInterval)
            doc["next_poll_sec"] = (pollInterval - elapsed) / 1000;
        else
            doc["next_poll_sec"] = 0;

        // Timing: vinyl recheck
        if (g_state == STATE_VINYL && g_lastVinylMatchTime != 0) {
            unsigned long since = now - g_lastVinylMatchTime;
            if (since < g_settings.vinyl_recheck_ms)
                doc["next_vinyl_check_sec"] = (g_settings.vinyl_recheck_ms - since) / 1000;
            else
                doc["next_vinyl_check_sec"] = 0;
            doc["vinyl_recheck_min"] = g_settings.vinyl_recheck_ms / 60000;
        }

        // Timing: no-match retry / cooldown
        if (g_lastNoMatchTime != 0) {
            unsigned long since = now - g_lastNoMatchTime;
            doc["no_match_retries"] = g_vinylNoMatchCount;
            if (g_vinylNoMatchCount >= 3) {
                if (since < g_settings.no_match_cooldown_ms)
                    doc["cooldown_remaining_sec"] = (g_settings.no_match_cooldown_ms - since) / 1000;
            } else {
                unsigned long retryDelay = 15000;
                if (since < retryDelay)
                    doc["retry_in_sec"] = (retryDelay - since) / 1000;
            }
        }

        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    // ─── Get settings (mask secrets) ───
    server.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        doc["sonos_ip"]                = g_settings.sonos_ip;
        doc["sonos_name"]              = g_settings.sonos_name;
        doc["shazam_api_key_set"]      = strlen(g_settings.shazam_api_key) > 0;
        doc["sonos_poll_ms"]           = g_settings.sonos_poll_ms;
        doc["vinyl_recheck_ms"]        = g_settings.vinyl_recheck_ms;
        doc["no_match_cooldown_ms"]    = g_settings.no_match_cooldown_ms;
        doc["idle_gallery_ms"]         = g_settings.idle_gallery_ms;
        doc["show_track_info"]         = g_settings.show_track_info;
        doc["bg_mode"]                 = g_settings.bg_mode;

        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    // ─── Save settings (JSON body) ───
    server.on("/api/settings", HTTP_POST,
        [](AsyncWebServerRequest* req) { /* handled in body callback */ },
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            // Accumulate body across chunks
            static String body;
            if (index == 0) body = "";
            body += String((char*)data, len);

            if (index + len >= total) {
                JsonDocument doc;
                if (deserializeJson(doc, body)) {
                    req->send(400, "application/json", "{\"error\":\"bad json\"}");
                    return;
                }

                if (doc["sonos_ip"].is<const char*>())
                    strlcpy(g_settings.sonos_ip, doc["sonos_ip"], sizeof(g_settings.sonos_ip));
                if (doc["sonos_name"].is<const char*>())
                    strlcpy(g_settings.sonos_name, doc["sonos_name"], sizeof(g_settings.sonos_name));
                if (doc["shazam_api_key"].is<const char*>())
                    strlcpy(g_settings.shazam_api_key, doc["shazam_api_key"], sizeof(g_settings.shazam_api_key));
                if (doc["sonos_poll_ms"].is<unsigned int>())
                    g_settings.sonos_poll_ms = doc["sonos_poll_ms"];
                if (doc["vinyl_recheck_ms"].is<unsigned int>())
                    g_settings.vinyl_recheck_ms = doc["vinyl_recheck_ms"];
                if (doc["no_match_cooldown_ms"].is<unsigned int>())
                    g_settings.no_match_cooldown_ms = doc["no_match_cooldown_ms"];
                if (doc["idle_gallery_ms"].is<unsigned int>())
                    g_settings.idle_gallery_ms = doc["idle_gallery_ms"];
                if (doc["show_track_info"].is<bool>())
                    g_settings.show_track_info = doc["show_track_info"];
                if (doc["bg_mode"].is<unsigned int>())
                    g_settings.bg_mode = doc["bg_mode"];

                sdWriteSettings(g_settings);
                req->send(200, "application/json", "{\"ok\":true}");
            }
        }
    );

    // ─── Serve history image (must register before /api/history) ───
    server.on("/api/history/image", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!req->hasParam("f")) {
            req->send(400, "text/plain", "missing f");
            return;
        }
        String file = req->getParam("f")->value();
        if (file.indexOf("..") >= 0 || file.indexOf("/") >= 0) {
            req->send(400, "text/plain", "invalid");
            return;
        }
        String path = "/history/" + file;
        if (!SD_MMC.exists(path)) {
            req->send(404, "text/plain", "not found");
            return;
        }
        req->send(SD_MMC, path, "image/jpeg");
    });

    // ─── Toggle history entry on/off ───
    server.on("/api/history/toggle", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!req->hasParam("f", true) || !req->hasParam("on", true)) {
            req->send(400, "application/json", "{\"error\":\"missing f or on\"}");
            return;
        }
        String file = req->getParam("f", true)->value();
        // Path traversal protection
        if (file.indexOf("..") >= 0 || file.indexOf("/") >= 0) {
            req->send(400, "application/json", "{\"error\":\"invalid name\"}");
            return;
        }
        bool on = req->getParam("on", true)->value() == "1";
        if (sdHistorySetEnabled(file.c_str(), on)) {
            req->send(200, "application/json", "{\"ok\":true}");
        } else {
            req->send(404, "application/json", "{\"error\":\"not found\"}");
        }
    });

    // ─── List album art history ───
    server.on("/api/history", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", sdHistoryList());
    });

    // ─── Force display refresh ───
    server.on("/api/refresh", HTTP_POST, [](AsyncWebServerRequest* req) {
        g_forceRefresh = true;
        req->send(200, "application/json", "{\"ok\":true}");
        Serial.println("[Web] Force refresh requested");
    });

    // ─── Test color pattern ───
    server.on("/api/test-colors", HTTP_POST, [](AsyncWebServerRequest* req) {
        g_testColors = true;
        req->send(200, "application/json", "{\"ok\":true}");
    });

    // ─── Force listen (audio identify) ───
    server.on("/api/listen", HTTP_POST, [](AsyncWebServerRequest* req) {
        g_forceListen = true;
        req->send(200, "application/json", "{\"ok\":true}");
    });

    // ─── Activity log ───
    server.on("/api/log", HTTP_GET, [](AsyncWebServerRequest* req) {
        LogEntry entries[LOG_MAX_ENTRIES];
        int count = activityLogGet(entries, LOG_MAX_ENTRIES);

        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();
        for (int i = 0; i < count; i++) {
            JsonObject obj = arr.add<JsonObject>();
            obj["t"] = entries[i].timestamp / 1000; // seconds
            obj["m"] = entries[i].message;
        }

        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    // ─── Download last audio recording as WAV ───
    server.on("/api/last-audio", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!g_lastAudio || g_lastAudioLen == 0) {
            req->send(404, "text/plain", "No audio recorded yet");
            return;
        }

        uint32_t sampleRate = g_lastAudioSampleRate;
        uint16_t channels = g_lastAudioChannels;
        uint16_t bitsPerSample = 16;
        uint32_t byteRate = sampleRate * channels * bitsPerSample / 8;
        uint16_t blockAlign = channels * bitsPerSample / 8;
        uint32_t dataLen = g_lastAudioLen;
        uint32_t fileLen = 44 + dataLen;

        // Build WAV header (44 bytes)
        uint8_t hdr[44];
        memcpy(hdr, "RIFF", 4);
        uint32_t riffSize = fileLen - 8;
        memcpy(hdr + 4, &riffSize, 4);
        memcpy(hdr + 8, "WAVEfmt ", 8);
        uint32_t fmtSize = 16;
        memcpy(hdr + 16, &fmtSize, 4);
        uint16_t audioFmt = 1; // PCM
        memcpy(hdr + 20, &audioFmt, 2);
        memcpy(hdr + 22, &channels, 2);
        memcpy(hdr + 24, &sampleRate, 4);
        memcpy(hdr + 28, &byteRate, 4);
        memcpy(hdr + 32, &blockAlign, 2);
        memcpy(hdr + 34, &bitsPerSample, 2);
        memcpy(hdr + 36, "data", 4);
        memcpy(hdr + 40, &dataLen, 4);

        // Capture pointer/len at request time (audio won't change mid-serve)
        uint8_t* audioPtr = g_lastAudio;
        size_t audioLen = g_lastAudioLen;

        AsyncWebServerResponse* response = req->beginChunkedResponse("audio/wav",
            [hdr, audioPtr, audioLen](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
                size_t totalLen = 44 + audioLen;
                if (index >= totalLen) return 0;

                size_t remaining = totalLen - index;
                size_t toSend = (remaining < maxLen) ? remaining : maxLen;
                size_t sent = 0;

                // Send from header (first 44 bytes)
                if (index < 44) {
                    size_t hdrBytes = 44 - index;
                    if (hdrBytes > toSend) hdrBytes = toSend;
                    memcpy(buffer, hdr + index, hdrBytes);
                    sent += hdrBytes;
                }

                // Send from audio data
                if (sent < toSend) {
                    size_t audioOffset = (index > 44) ? index - 44 : 0;
                    if (index < 44) audioOffset = 0;
                    size_t dataStart = (index < 44) ? 0 : index - 44;
                    size_t dataBytes = toSend - sent;
                    if (dataStart + dataBytes > audioLen)
                        dataBytes = audioLen - dataStart;
                    memcpy(buffer + sent, audioPtr + dataStart, dataBytes);
                    sent += dataBytes;
                }

                return sent;
            }
        );
        response->addHeader("Content-Disposition", "attachment; filename=\"recording.wav\"");
        req->send(response);
    });

    // ─── Scan LAN for Sonos speakers ───
    // Returns a JSON array of {name, ip} objects.
    // The scan takes up to ~3 seconds; call from an async context.
    server.on("/api/sonos/scan", HTTP_GET, [](AsyncWebServerRequest* req) {
        SonosDevice devices[16];
        int count = sonosDiscover(devices, 16, 3000);

        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();
        for (int i = 0; i < count; i++) {
            JsonObject obj = arr.add<JsonObject>();
            obj["name"] = devices[i].name;
            obj["ip"]   = devices[i].ip;
        }

        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    server.begin();
    Serial.println("[Web] Server started on port 80");
}
