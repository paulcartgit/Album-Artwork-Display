#include "web_server.h"
#include "web_portal.h"
#include "config.h"
#include "sd_manager.h"
#include "image_pipeline.h"

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
        doc["acrcloud_host"]           = g_settings.acrcloud_host;
        doc["acrcloud_key"]            = g_settings.acrcloud_key;
        doc["acrcloud_secret_set"]     = strlen(g_settings.acrcloud_secret) > 0;
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
                if (doc["acrcloud_host"].is<const char*>())
                    strlcpy(g_settings.acrcloud_host, doc["acrcloud_host"], sizeof(g_settings.acrcloud_host));
                if (doc["acrcloud_key"].is<const char*>())
                    strlcpy(g_settings.acrcloud_key, doc["acrcloud_key"], sizeof(g_settings.acrcloud_key));
                if (doc["acrcloud_secret"].is<const char*>())
                    strlcpy(g_settings.acrcloud_secret, doc["acrcloud_secret"], sizeof(g_settings.acrcloud_secret));
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

    server.begin();
    Serial.println("[Web] Server started on port 80");
}
