#include "sd_manager.h"
#include <SD_MMC.h>
#include <ArduinoJson.h>

bool sdInit() {
    SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0, SD_D1, SD_D2, SD_D3);
    if (!SD_MMC.begin("/sd", false, false)) {
        Serial.println("[SD] Mount failed");
        return false;
    }
    Serial.printf("[SD] Card size: %lluMB\n", SD_MMC.cardSize() / (1024 * 1024));

    // Ensure history directory exists
    if (!SD_MMC.exists("/history")) {
        SD_MMC.mkdir("/history");
    }
    return true;
}

bool sdReadWifiConfig(WifiConfig& cfg) {
    File f = SD_MMC.open("/config.json", FILE_READ);
    if (!f) {
        Serial.println("[SD] config.json not found");
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, f)) {
        f.close();
        Serial.println("[SD] config.json parse error");
        return false;
    }
    f.close();

    strlcpy(cfg.ssid, doc["ssid"] | "", sizeof(cfg.ssid));
    strlcpy(cfg.password, doc["password"] | "", sizeof(cfg.password));
    return strlen(cfg.ssid) > 0;
}

bool sdReadSettings(Settings& settings) {
    memset(&settings, 0, sizeof(settings));
    settings.sonos_poll_ms = SONOS_POLL_INTERVAL_MS;
    settings.vinyl_recheck_ms = VINYL_RECHECK_INTERVAL_MS;
    settings.no_match_cooldown_ms = NO_MATCH_COOLDOWN_MS;
    settings.idle_gallery_ms = IDLE_GALLERY_INTERVAL_MS;
    settings.show_track_info = true;
    settings.bg_mode = 2;  // auto

    File f = SD_MMC.open("/settings.json", FILE_READ);
    if (!f) return false;

    JsonDocument doc;
    if (deserializeJson(doc, f)) {
        f.close();
        return false;
    }
    f.close();

    strlcpy(settings.sonos_ip, doc["sonos_ip"] | "", sizeof(settings.sonos_ip));
    strlcpy(settings.shazam_api_key, doc["shazam_api_key"] | "", sizeof(settings.shazam_api_key));
    settings.sonos_poll_ms = doc["sonos_poll_ms"] | SONOS_POLL_INTERVAL_MS;
    settings.vinyl_recheck_ms = doc["vinyl_recheck_ms"] | VINYL_RECHECK_INTERVAL_MS;
    settings.no_match_cooldown_ms = doc["no_match_cooldown_ms"] | NO_MATCH_COOLDOWN_MS;
    settings.idle_gallery_ms = doc["idle_gallery_ms"] | IDLE_GALLERY_INTERVAL_MS;
    settings.show_track_info = doc["show_track_info"] | true;
    // Migrate old bool blur_background → new bg_mode
    if (doc["bg_mode"].is<unsigned int>()) {
        settings.bg_mode = doc["bg_mode"] | 2;
    } else if (doc["blur_background"].is<bool>()) {
        settings.bg_mode = doc["blur_background"].as<bool>() ? 1 : 0;
    } else {
        settings.bg_mode = 2;
    }
    return true;
}

bool sdWriteSettings(const Settings& settings) {
    JsonDocument doc;
    doc["sonos_ip"] = settings.sonos_ip;
    doc["shazam_api_key"] = settings.shazam_api_key;
    doc["sonos_poll_ms"] = settings.sonos_poll_ms;
    doc["vinyl_recheck_ms"] = settings.vinyl_recheck_ms;
    doc["no_match_cooldown_ms"] = settings.no_match_cooldown_ms;
    doc["idle_gallery_ms"] = settings.idle_gallery_ms;
    doc["show_track_info"] = settings.show_track_info;
    doc["bg_mode"] = settings.bg_mode;

    File f = SD_MMC.open("/settings.json", FILE_WRITE);
    if (!f) return false;
    serializeJson(doc, f);
    f.close();
    return true;
}

bool sdFileExists(const char* path) {
    return SD_MMC.exists(path);
}

// ─── History helpers ───

static const char* HISTORY_INDEX = "/history/index.json";
static const int   HISTORY_MAX   = 100;

static uint32_t djb2(const char* s) {
    uint32_t h = 5381;
    while (*s) h = ((h << 5) + h) + (uint8_t)*s++;
    return h;
}

// Read the index file into a JsonDocument.  Returns false if missing/corrupt.
static bool readIndex(JsonDocument& doc) {
    File f = SD_MMC.open(HISTORY_INDEX, FILE_READ);
    if (!f) return false;
    bool ok = !deserializeJson(doc, f);
    f.close();
    return ok;
}

static bool writeIndex(const JsonDocument& doc) {
    File f = SD_MMC.open(HISTORY_INDEX, FILE_WRITE);
    if (!f) return false;
    serializeJson(doc, f);
    f.close();
    return true;
}

bool sdHistorySave(const char* artist, const char* title, const char* album,
                   const uint8_t* jpegBuf, size_t jpegSize)
{
    if (!artist || !title || !artist[0] || !title[0]) return false;

    // Deterministic filename from artist+title
    String key = String(artist) + "|" + String(title);
    char fname[20];
    snprintf(fname, sizeof(fname), "%08x.jpg", djb2(key.c_str()));
    String fpath = String("/history/") + fname;

    // Load existing index
    JsonDocument doc;
    readIndex(doc);
    JsonArray arr = doc.is<JsonArray>() ? doc.as<JsonArray>() : doc.to<JsonArray>();

    // Check if entry already exists — just bump timestamp
    for (JsonObject obj : arr) {
        if (strcmp(obj["f"] | "", fname) == 0) {
            obj["ts"] = (unsigned long)millis();
            writeIndex(doc);
            Serial.printf("[History] Already cached: %s\n", fname);
            return true;
        }
    }

    // Write JPEG to SD
    File f = SD_MMC.open(fpath, FILE_WRITE);
    if (!f) {
        Serial.printf("[History] Failed to write %s\n", fpath.c_str());
        return false;
    }
    f.write(jpegBuf, jpegSize);
    f.close();

    // Prune oldest if at capacity
    while (arr.size() >= HISTORY_MAX) {
        // Find oldest by timestamp
        int oldest = 0;
        unsigned long oldestTs = ULONG_MAX;
        int i = 0;
        for (JsonObject obj : arr) {
            unsigned long ts = obj["ts"] | 0UL;
            if (ts < oldestTs) { oldestTs = ts; oldest = i; }
            i++;
        }
        // Delete file and remove entry
        String delPath = String("/history/") + (arr[oldest]["f"] | "?.jpg");
        SD_MMC.remove(delPath);
        Serial.printf("[History] Pruned: %s\n", delPath.c_str());
        arr.remove(oldest);
    }

    // Add new entry
    JsonObject obj = arr.add<JsonObject>();
    obj["f"]  = fname;
    obj["a"]  = artist;
    obj["t"]  = title;
    obj["al"] = album ? album : "";
    obj["ts"] = (unsigned long)millis();
    obj["on"] = true;

    writeIndex(doc);
    Serial.printf("[History] Saved: %s (%s — %s)\n", fname, artist, title);
    return true;
}

String sdHistoryList() {
    JsonDocument doc;
    if (!readIndex(doc)) {
        return "[]";
    }
    String out;
    serializeJson(doc, out);
    return out;
}

bool sdHistorySetEnabled(const char* file, bool on) {
    JsonDocument doc;
    if (!readIndex(doc)) return false;
    JsonArray arr = doc.as<JsonArray>();
    for (JsonObject obj : arr) {
        if (strcmp(obj["f"] | "", file) == 0) {
            obj["on"] = on;
            writeIndex(doc);
            return true;
        }
    }
    return false;
}

String sdHistoryRandomFile() {
    JsonDocument doc;
    if (!readIndex(doc)) return "";
    JsonArray arr = doc.as<JsonArray>();

    // Count enabled entries
    int enabled = 0;
    for (JsonObject obj : arr) {
        if (obj["on"] | true) enabled++;
    }
    if (enabled == 0) return "";

    int pick = random(enabled);
    int idx = 0;
    for (JsonObject obj : arr) {
        if (obj["on"] | true) {
            if (idx == pick) {
                return String("/history/") + (obj["f"] | "?.jpg");
            }
            idx++;
        }
    }
    return "";
}
