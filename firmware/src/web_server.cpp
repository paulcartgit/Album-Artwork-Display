#include "web_server.h"
#include "web_portal.h"
#include "captive_portal.h"
#include "app.h"
#include "config.h"
#include "sd_manager.h"
#include "sonos_client.h"
#include "image_pipeline.h"
#include "activity_log.h"
#include "wav_utils.h"
#include "backoff.h"

#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <ArduinoJson.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <Update.h>

// ═══════════════════════════════════════════════════════════
// Request-body accumulation
//
// These callbacks fire once per TCP chunk and several requests can be in
// flight at once, so the buffer must belong to the request — not to the
// handler.  _tempObject is freed by the framework when the request completes.
// It is also capped: an unbounded String here is a trivial way to exhaust the
// heap from the LAN.
// ═══════════════════════════════════════════════════════════
static const size_t MAX_BODY_BYTES = 4096;

struct BodyBuffer {
    size_t len;
    size_t cap;
    char   data[MAX_BODY_BYTES + 1];
};

// Returns true once the whole body has arrived and is available in `out`.
static bool collectBody(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                        size_t index, size_t total, const char** out) {
    if (total > MAX_BODY_BYTES) {
        if (index == 0) req->send(413, "application/json", "{\"error\":\"body too large\"}");
        return false;
    }
    if (index == 0) {
        if (req->_tempObject) free(req->_tempObject);
        req->_tempObject = calloc(1, sizeof(BodyBuffer));
        if (!req->_tempObject) {
            req->send(500, "application/json", "{\"error\":\"out of memory\"}");
            return false;
        }
    }
    BodyBuffer* buf = (BodyBuffer*)req->_tempObject;
    if (!buf) return false;

    size_t room = MAX_BODY_BYTES - buf->len;
    size_t n = (len < room) ? len : room;
    memcpy(buf->data + buf->len, data, n);
    buf->len += n;
    buf->data[buf->len] = '\0';

    if (index + len < total) return false;
    *out = buf->data;
    return true;
}

// ═══════════════════════════════════════════════════════════
// Access control
//
// Anyone on the LAN could previously rewrite the Wi-Fi credentials and reboot
// the device.  A password is optional (empty = open, as before) but when set
// it guards every API route, not just the destructive ones — /api/settings
// alone leaks the configured SSID and speaker.
// ═══════════════════════════════════════════════════════════
static bool requireAuth(AsyncWebServerRequest* req) {
    const char* pwd = g_app.settings.portal_password;
    if (!pwd || pwd[0] == '\0') return true; // auth disabled
    if (req->authenticate("admin", pwd)) return true;
    req->requestAuthentication();
    return false;
}

// Reject history filenames that try to escape /history.
static bool safeHistoryName(const String& f) {
    return f.length() > 0 && f.length() < 64 &&
           f.indexOf("..") < 0 && f.indexOf('/') < 0 && f.indexOf('\\') < 0;
}

static void sendJson(AsyncWebServerRequest* req, int code, const JsonDocument& doc) {
    String out;
    serializeJson(doc, out);
    req->send(code, "application/json", out);
}

// Shared Wi-Fi scan handler: always uses the *async* scan API.  The blocking
// variant stalls the AsyncTCP task for seconds and takes the whole portal
// down with it.
static void handleWifiScan(AsyncWebServerRequest* req) {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
        req->send(202, "application/json", "[]"); // client retries shortly
        return;
    }
    if (n == WIFI_SCAN_FAILED || n == 0) {
        WiFi.scanDelete();
        WiFi.scanNetworks(true);
        req->send(200, "application/json", "[]");
        return;
    }
    // Deduplicate by SSID — keep the strongest signal for each
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.isEmpty()) continue;
        int rssi = WiFi.RSSI(i);
        bool isOpen = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
        bool found = false;
        for (JsonObject obj : arr) {
            if (obj["ssid"].as<String>() == ssid) {
                if (rssi > obj["rssi"].as<int>()) {
                    obj["rssi"] = rssi;
                    obj["open"] = isOpen;
                }
                found = true;
                break;
            }
        }
        if (!found) {
            JsonObject obj = arr.add<JsonObject>();
            obj["ssid"] = ssid;
            obj["rssi"] = rssi;
            obj["open"] = isOpen;
        }
    }
    WiFi.scanDelete();
    WiFi.scanNetworks(true); // start the next scan for future requests
    sendJson(req, 200, doc);
}

// ═══════════════════════════════════════════════════════════
// Captive portal (setup mode)
// ═══════════════════════════════════════════════════════════

static DNSServer      s_dns;
static AsyncWebServer s_setupServer(80);
static const uint8_t  DNS_PORT = 53;

void captivePortalInit() {
    s_dns.start(DNS_PORT, "*", WiFi.softAPIP());

    // Kick off an async WiFi scan immediately so results are ready on first load
    WiFi.scanNetworks(true);

    // Serve the setup page for any path (handles OS captive-portal probes too)
    s_setupServer.onNotFound([](AsyncWebServerRequest* req) {
        req->send_P(200, "text/html", CAPTIVE_PORTAL_HTML);
    });

    s_setupServer.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send_P(200, "text/html", CAPTIVE_PORTAL_HTML);
    });

    s_setupServer.on("/api/wifi/scan", HTTP_GET, handleWifiScan);

    // Save credentials and reboot
    s_setupServer.on("/api/wifi/save", HTTP_POST,
        [](AsyncWebServerRequest* req) { /* handled in body callback */ },
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            const char* body = nullptr;
            if (!collectBody(req, data, len, index, total, &body)) return;

            JsonDocument doc;
            if (deserializeJson(doc, body)) {
                req->send(400, "application/json", "{\"error\":\"bad json\"}");
                return;
            }
            const char* ssid = doc["ssid"] | "";
            const char* pwd  = doc["password"] | "";
            if (strlen(ssid) == 0) {
                req->send(400, "application/json", "{\"error\":\"ssid required\"}");
                return;
            }
            WifiConfig cfg;
            strlcpy(cfg.ssid,     ssid, sizeof(cfg.ssid));
            strlcpy(cfg.password, pwd,  sizeof(cfg.password));
            if (!sdWriteWifiConfig(cfg)) {
                req->send(500, "application/json", "{\"error\":\"sd write failed\"}");
                return;
            }
            req->send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
            // Reboot from the main loop — never block the AsyncTCP task
            g_req.reboot = true;
        }
    );

    s_setupServer.begin();
    Serial.println("[CaptivePortal] Setup server started");
}

void captivePortalLoop() {
    s_dns.processNextRequest();
}

// ═══════════════════════════════════════════════════════════
// Main portal
// ═══════════════════════════════════════════════════════════

static AsyncWebServer server(80);

void webServerInit() {
    // ─── Serve HTML ───
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send_P(200, "text/html", INDEX_HTML);
    });

    // ─── Status API ───
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!requireAuth(req)) return;
        JsonDocument doc;
        const char* stateNames[] = {"BOOT","IDLE","DIGITAL","VINYL","ERROR","SETUP"};
        int stateIdx = (int)g_app.state;
        doc["state"]      = stateIdx;
        doc["state_name"] = (stateIdx >= 0 && stateIdx < 6) ? stateNames[stateIdx] : "UNKNOWN";
        doc["artist"]     = g_app.currentArtist;
        doc["title"]      = g_app.currentTitle;
        doc["album"]      = g_app.currentAlbum;
        doc["art_url"]    = g_app.lastArtUrl;
        doc["ip"]         = WiFi.localIP().toString();
        doc["uptime"]     = millis() / 1000;
        if (g_app.displayHoldUntil != 0) {
            long remaining = (long)(g_app.displayHoldUntil - millis());
            doc["display_hold_sec"] = (remaining > 0) ? remaining / 1000 : 0;
        }

        // Timing: next Sonos poll
        unsigned long now = millis();
        unsigned long elapsed = now - g_app.lastPollTime;
        unsigned long pollInterval = g_app.settings.sonos_poll_ms;
        doc["next_poll_sec"] = (elapsed < pollInterval) ? (pollInterval - elapsed) / 1000 : 0;

        // Timing: vinyl recheck
        if (g_app.state == STATE_VINYL && g_app.lastVinylMatchTime != 0) {
            unsigned long since = now - g_app.lastVinylMatchTime;
            doc["next_vinyl_check_sec"] =
                (since < g_app.settings.vinyl_recheck_ms)
                    ? (g_app.settings.vinyl_recheck_ms - since) / 1000 : 0;
            doc["vinyl_recheck_min"] = g_app.settings.vinyl_recheck_ms / 60000;
        }

        // Timing: no-match retry / cooldown (mirrors controller.cpp)
        if (g_app.lastNoMatchTime != 0) {
            unsigned long since = now - g_app.lastNoMatchTime;
            doc["no_match_retries"] = g_app.vinylNoMatchCount;
            doc["cooldown_level"]   = g_app.vinylCooldownLevel;
            int maxRetries = vinylMaxRetriesFor(g_app.vinylCooldownLevel, VINYL_MAX_RETRIES);
            if (g_app.vinylNoMatchCount >= maxRetries) {
                uint32_t cooldown = vinylCooldownMsFor(g_app.settings.no_match_cooldown_ms,
                                                       g_app.vinylCooldownLevel,
                                                       VINYL_MAX_COOLDOWN_MS);
                if (since < cooldown) doc["cooldown_remaining_sec"] = (cooldown - since) / 1000;
            } else if (since < VINYL_RETRY_DELAY_MS) {
                doc["retry_in_sec"] = (VINYL_RETRY_DELAY_MS - since) / 1000;
            }
        }
        sendJson(req, 200, doc);
    });

    // ─── Render profiles (for the settings dropdown) ───
    server.on("/api/profiles", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!requireAuth(req)) return;
        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();
        for (int i = 0; i < PROFILE_COUNT; i++) {
            JsonObject obj = arr.add<JsonObject>();
            obj["id"]   = i;
            obj["name"] = RENDER_PROFILES[i].name;
        }
        sendJson(req, 200, doc);
    });

    // ─── Get settings (mask secrets) ───
    server.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!requireAuth(req)) return;
        JsonDocument doc;
        doc["sonos_ip"]             = g_app.settings.sonos_ip;
        doc["sonos_name"]           = g_app.settings.sonos_name;
        doc["shazam_api_key_set"]   = strlen(g_app.settings.shazam_api_key) > 0;
        doc["sonos_poll_ms"]        = g_app.settings.sonos_poll_ms;
        doc["vinyl_recheck_ms"]     = g_app.settings.vinyl_recheck_ms;
        doc["no_match_cooldown_ms"] = g_app.settings.no_match_cooldown_ms;
        doc["idle_gallery_ms"]      = g_app.settings.idle_gallery_ms;
        doc["show_track_info"]      = g_app.settings.show_track_info;
        doc["bg_mode"]              = g_app.settings.bg_mode;
        doc["bg_style"]             = g_app.settings.bg_style;
        doc["render_profile"]       = g_app.settings.render_profile;
        doc["portal_password_set"]  = strlen(g_app.settings.portal_password) > 0;
        sendJson(req, 200, doc);
    });

    // ─── Save settings (JSON body) ───
    server.on("/api/settings", HTTP_POST,
        [](AsyncWebServerRequest* req) { /* handled in body callback */ },
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            if (!requireAuth(req)) return;
            const char* body = nullptr;
            if (!collectBody(req, data, len, index, total, &body)) return;

            JsonDocument doc;
            if (deserializeJson(doc, body)) {
                req->send(400, "application/json", "{\"error\":\"bad json\"}");
                return;
            }

            if (doc["sonos_ip"].is<const char*>())
                strlcpy(g_app.settings.sonos_ip, doc["sonos_ip"], sizeof(g_app.settings.sonos_ip));
            if (doc["sonos_name"].is<const char*>())
                strlcpy(g_app.settings.sonos_name, doc["sonos_name"], sizeof(g_app.settings.sonos_name));
            if (doc["shazam_api_key"].is<const char*>())
                strlcpy(g_app.settings.shazam_api_key, doc["shazam_api_key"], sizeof(g_app.settings.shazam_api_key));
            if (doc["portal_password"].is<const char*>())
                strlcpy(g_app.settings.portal_password, doc["portal_password"], sizeof(g_app.settings.portal_password));
            if (doc["sonos_poll_ms"].is<unsigned int>())
                g_app.settings.sonos_poll_ms = doc["sonos_poll_ms"];
            if (doc["vinyl_recheck_ms"].is<unsigned int>())
                g_app.settings.vinyl_recheck_ms = doc["vinyl_recheck_ms"];
            if (doc["no_match_cooldown_ms"].is<unsigned int>())
                g_app.settings.no_match_cooldown_ms = doc["no_match_cooldown_ms"];
            if (doc["idle_gallery_ms"].is<unsigned int>())
                g_app.settings.idle_gallery_ms = doc["idle_gallery_ms"];
            if (doc["show_track_info"].is<bool>())
                g_app.settings.show_track_info = doc["show_track_info"];
            if (doc["bg_mode"].is<unsigned int>())
                g_app.settings.bg_mode = doc["bg_mode"];
            if (doc["bg_style"].is<unsigned int>())
                g_app.settings.bg_style = doc["bg_style"];
            if (doc["render_profile"].is<unsigned int>()) {
                uint8_t p = doc["render_profile"];
                g_app.settings.render_profile = (p < PROFILE_COUNT) ? p : PROFILE_NATURAL;
            }

            sdWriteSettings(g_app.settings);
            req->send(200, "application/json", "{\"ok\":true}");
        }
    );

    // ─── Scan WiFi networks ───
    server.on("/api/wifi/scan", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!requireAuth(req)) return;
        handleWifiScan(req);
    });

    // ─── Get current WiFi SSID ───
    server.on("/api/wifi", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!requireAuth(req)) return;
        WifiConfig cfg;
        JsonDocument doc;
        doc["ssid"] = sdReadWifiConfig(cfg) ? cfg.ssid : "";
        sendJson(req, 200, doc);
    });

    // ─── Update WiFi credentials ───
    server.on("/api/wifi", HTTP_POST,
        [](AsyncWebServerRequest* req) { /* handled in body callback */ },
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            if (!requireAuth(req)) return;
            const char* body = nullptr;
            if (!collectBody(req, data, len, index, total, &body)) return;

            JsonDocument doc;
            if (deserializeJson(doc, body)) {
                req->send(400, "application/json", "{\"error\":\"bad json\"}");
                return;
            }
            const char* ssid = doc["ssid"] | "";
            const char* pwd  = doc["password"] | "";
            if (strlen(ssid) == 0) {
                req->send(400, "application/json", "{\"error\":\"ssid required\"}");
                return;
            }
            WifiConfig cfg;
            strlcpy(cfg.ssid,     ssid, sizeof(cfg.ssid));
            strlcpy(cfg.password, pwd,  sizeof(cfg.password));
            if (!sdWriteWifiConfig(cfg)) {
                req->send(500, "application/json", "{\"error\":\"sd write failed\"}");
                return;
            }
            req->send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
            g_req.reboot = true;
        }
    );

    // ─── Serve history image (must register before /api/history) ───
    server.on("/api/history/image", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!requireAuth(req)) return;
        if (!req->hasParam("f")) {
            req->send(400, "text/plain", "missing f");
            return;
        }
        String file = req->getParam("f")->value();
        if (!safeHistoryName(file)) {
            req->send(400, "text/plain", "invalid");
            return;
        }
        String path = "/history/" + file;
        if (!SD_MMC.exists(path)) {
            req->send(404, "text/plain", "not found");
            return;
        }
        AsyncWebServerResponse* response = req->beginResponse(SD_MMC, path, "image/jpeg");
        response->addHeader("Cache-Control", "public, max-age=604800, immutable");
        req->send(response);
    });

    // ─── Pin/unpin history entry ───
    server.on("/api/history/pin", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!requireAuth(req)) return;
        if (!req->hasParam("f", true) || !req->hasParam("pin", true)) {
            req->send(400, "application/json", "{\"error\":\"missing f or pin\"}");
            return;
        }
        String file = req->getParam("f", true)->value();
        if (!safeHistoryName(file)) {
            req->send(400, "application/json", "{\"error\":\"invalid name\"}");
            return;
        }
        bool pin = req->getParam("pin", true)->value() == "1";
        if (sdHistorySetPinned(file.c_str(), pin)) {
            req->send(200, "application/json", "{\"ok\":true}");
        } else {
            req->send(404, "application/json", "{\"error\":\"not found\"}");
        }
    });

    // ─── Toggle history entry on/off ───
    server.on("/api/history/toggle", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!requireAuth(req)) return;
        if (!req->hasParam("f", true) || !req->hasParam("on", true)) {
            req->send(400, "application/json", "{\"error\":\"missing f or on\"}");
            return;
        }
        String file = req->getParam("f", true)->value();
        if (!safeHistoryName(file)) {
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

    // ─── Delete history entry ───
    server.on("/api/history/delete", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!requireAuth(req)) return;
        if (!req->hasParam("f", true)) {
            req->send(400, "application/json", "{\"error\":\"missing f\"}");
            return;
        }
        String file = req->getParam("f", true)->value();
        if (!safeHistoryName(file)) {
            req->send(400, "application/json", "{\"error\":\"invalid name\"}");
            return;
        }
        if (sdHistoryDelete(file.c_str())) {
            req->send(200, "application/json", "{\"ok\":true}");
        } else {
            req->send(404, "application/json", "{\"error\":\"not found\"}");
        }
    });

    // ─── List album art history ───
    server.on("/api/history", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!requireAuth(req)) return;
        req->send(200, "application/json", sdHistoryList());
    });

    // ─── Actions: all deferred to the controller loop ───
    server.on("/api/refresh", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!requireAuth(req)) return;
        g_req.forceRefresh = true;
        req->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/api/test-colors", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!requireAuth(req)) return;
        g_req.testColors = true;
        req->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/api/test-dither", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!requireAuth(req)) return;
        g_req.testDither = true;
        req->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/api/test-calibration", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!requireAuth(req)) return;
        g_req.testCalibration = true;
        req->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/api/listen", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!requireAuth(req)) return;
        g_req.forceListen = true;
        req->send(200, "application/json", "{\"ok\":true}");
    });

    // ─── Activity log ───
    server.on("/api/log", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!requireAuth(req)) return;
        LogEntry entries[LOG_MAX_ENTRIES];
        int count = activityLogGet(entries, LOG_MAX_ENTRIES);

        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();
        for (int i = 0; i < count; i++) {
            JsonObject obj = arr.add<JsonObject>();
            obj["t"] = entries[i].timestamp / 1000; // seconds
            obj["m"] = entries[i].message;
        }
        sendJson(req, 200, doc);
    });

    // ─── Download last audio recording as WAV ───
    // The audio buffer is allocated once and never freed (see identify.cpp), so
    // the pointer stays valid for the life of the chunked response.  Worst case
    // a new recording overwrites it mid-download and you get a spliced WAV —
    // which beats the use-after-free this used to be.
    server.on("/api/last-audio", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!requireAuth(req)) return;
        uint8_t* audioPtr = g_app.lastAudio;
        size_t   audioLen = g_app.lastAudioLen;
        if (!audioPtr || audioLen == 0) {
            req->send(404, "text/plain", "No audio recorded yet");
            return;
        }

        uint8_t hdr[WAV_HEADER_SIZE];
        wavWriteHeader(hdr, audioLen, g_app.lastAudioChannels, g_app.lastAudioSampleRate);

        AsyncWebServerResponse* response = req->beginChunkedResponse("audio/wav",
            [hdr, audioPtr, audioLen](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
                size_t totalLen = WAV_HEADER_SIZE + audioLen;
                if (index >= totalLen) return 0;

                size_t remaining = totalLen - index;
                size_t toSend = (remaining < maxLen) ? remaining : maxLen;
                size_t sent = 0;

                if (index < WAV_HEADER_SIZE) {
                    size_t hdrBytes = WAV_HEADER_SIZE - index;
                    if (hdrBytes > toSend) hdrBytes = toSend;
                    memcpy(buffer, hdr + index, hdrBytes);
                    sent += hdrBytes;
                }
                if (sent < toSend) {
                    size_t dataStart = (index < WAV_HEADER_SIZE) ? 0 : index - WAV_HEADER_SIZE;
                    size_t dataBytes = toSend - sent;
                    if (dataStart + dataBytes > audioLen) dataBytes = audioLen - dataStart;
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
    // SSDP plus a SOAP round-trip takes seconds; running it here would block
    // the AsyncTCP task (and blow its stack via HTTPClient).  Ask the
    // controller loop to do it and poll for the result.
    server.on("/api/sonos/scan", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!requireAuth(req)) return;

        if (g_app.scanState == SCAN_DONE) {
            JsonDocument doc;
            JsonArray arr = doc.to<JsonArray>();
            int n = g_app.scanCount;
            for (int i = 0; i < n && i < SONOS_SCAN_MAX; i++) {
                JsonObject obj = arr.add<JsonObject>();
                obj["name"] = g_app.scanResults[i].name;
                obj["ip"]   = g_app.scanResults[i].ip;
            }
            g_app.scanState = SCAN_IDLE; // consumed
            sendJson(req, 200, doc);
            return;
        }

        if (g_app.scanState == SCAN_IDLE) g_app.scanState = SCAN_REQUESTED;
        req->send(202, "application/json", "[]"); // client retries shortly
    });

    // ═══ OTA firmware update ═══
    // POST the firmware.bin produced by `pio run` to /api/update.
    server.on("/api/update", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            bool ok = !Update.hasError();
            AsyncWebServerResponse* res = req->beginResponse(
                ok ? 200 : 500, "application/json",
                ok ? "{\"ok\":true,\"rebooting\":true}" : "{\"error\":\"update failed\"}");
            res->addHeader("Connection", "close");
            req->send(res);
            if (ok) g_req.reboot = true;
        },
        [](AsyncWebServerRequest* req, const String& filename, size_t index,
           uint8_t* data, size_t len, bool final) {
            if (!requireAuth(req)) return;
            if (index == 0) {
                activityLogf("OTA: starting update from %s", filename.c_str());
                if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                    Update.printError(Serial);
                    activityLog("OTA: begin failed");
                    return;
                }
            }
            if (Update.write(data, len) != len) {
                Update.printError(Serial);
                activityLog("OTA: write failed");
                return;
            }
            if (final) {
                if (Update.end(true)) {
                    activityLogf("OTA: complete (%u bytes) — rebooting", (unsigned)(index + len));
                } else {
                    Update.printError(Serial);
                    activityLog("OTA: finalise failed");
                }
            }
        }
    );

    server.begin();
    Serial.println("[Web] Server started on port 80");
}
