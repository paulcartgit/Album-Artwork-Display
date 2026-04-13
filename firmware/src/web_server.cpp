#include "web_server.h"
#include "web_portal.h"
#include "config.h"
#include "sd_manager.h"
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

static AsyncWebServer server(80);

void webServerInit() {
    // ─── Serve HTML ───
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send_P(200, "text/html", INDEX_HTML);
    });

    // ─── Status API ───
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        doc["state"]   = (int)g_state;
        doc["artist"]  = g_currentArtist;
        doc["title"]   = g_currentTitle;
        doc["album"]   = g_currentAlbum;
        doc["ip"]      = WiFi.localIP().toString();
        doc["uptime"]  = millis() / 1000;

        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    // ─── Get settings (mask secrets) ───
    server.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        doc["sonos_ip"]                = g_settings.sonos_ip;
        doc["shazam_api_key_set"]      = strlen(g_settings.shazam_api_key) > 0;
        doc["spotify_client_id"]       = g_settings.spotify_client_id;
        doc["spotify_client_secret_set"] = strlen(g_settings.spotify_client_secret) > 0;
        doc["google_photos_url"]       = g_settings.google_photos_url;
        doc["poll_interval_ms"]        = g_settings.poll_interval_ms;
        doc["show_track_info"]         = g_settings.show_track_info;
        doc["use_dithering"]           = g_settings.use_dithering;

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
                if (doc["shazam_api_key"].is<const char*>())
                    strlcpy(g_settings.shazam_api_key, doc["shazam_api_key"], sizeof(g_settings.shazam_api_key));
                if (doc["spotify_client_id"].is<const char*>())
                    strlcpy(g_settings.spotify_client_id, doc["spotify_client_id"], sizeof(g_settings.spotify_client_id));
                if (doc["spotify_client_secret"].is<const char*>())
                    strlcpy(g_settings.spotify_client_secret, doc["spotify_client_secret"], sizeof(g_settings.spotify_client_secret));
                if (doc["google_photos_url"].is<const char*>())
                    strlcpy(g_settings.google_photos_url, doc["google_photos_url"], sizeof(g_settings.google_photos_url));
                if (doc["poll_interval_ms"].is<unsigned int>())
                    g_settings.poll_interval_ms = doc["poll_interval_ms"];
                if (doc["show_track_info"].is<bool>())
                    g_settings.show_track_info = doc["show_track_info"];
                if (doc["use_dithering"].is<bool>())
                    g_settings.use_dithering = doc["use_dithering"];

                sdWriteSettings(g_settings);
                req->send(200, "application/json", "{\"ok\":true}");
            }
        }
    );

    // ─── List gallery ───
    server.on("/api/gallery", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", sdListGallery());
    });

    // ─── Upload to gallery ───
    server.on("/api/upload", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            req->send(200, "application/json", "{\"ok\":true}");
        },
        [](AsyncWebServerRequest* req, const String& filename, size_t index,
           uint8_t* data, size_t len, bool final) {
            static File uploadFile;
            if (index == 0) {
                String path = "/gallery/" + filename;
                SD_MMC.mkdir("/gallery");
                uploadFile = SD_MMC.open(path, FILE_WRITE);
                Serial.printf("[Web] Upload start: %s\n", filename.c_str());
            }
            if (uploadFile) {
                uploadFile.write(data, len);
            }
            if (final && uploadFile) {
                uploadFile.close();
                Serial.printf("[Web] Upload done: %s (%u bytes)\n", filename.c_str(), index + len);
            }
        }
    );

    // ─── Delete gallery image ───
    server.on("/api/gallery/delete", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!req->hasParam("name", true)) {
            req->send(400, "application/json", "{\"error\":\"missing name\"}");
            return;
        }
        String name = req->getParam("name", true)->value();
        // Path traversal protection
        if (name.indexOf("..") >= 0 || name.indexOf("/") >= 0) {
            req->send(400, "application/json", "{\"error\":\"invalid name\"}");
            return;
        }
        String path = "/gallery/" + name;
        if (sdDeleteFile(path.c_str())) {
            req->send(200, "application/json", "{\"ok\":true}");
        } else {
            req->send(404, "application/json", "{\"error\":\"not found\"}");
        }
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

    server.begin();
    Serial.println("[Web] Server started on port 80");
}
