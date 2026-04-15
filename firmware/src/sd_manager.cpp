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

    strlcpy(settings.sonos_ip,   doc["sonos_ip"]   | "", sizeof(settings.sonos_ip));
    strlcpy(settings.sonos_name, doc["sonos_name"] | "", sizeof(settings.sonos_name));
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
    doc["sonos_ip"]   = settings.sonos_ip;
    doc["sonos_name"] = settings.sonos_name;
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

// Forward declaration — defined with shuffle-bag state below
static bool g_shuffleDirty = true;

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

    // Deterministic filename from artist+album (not track title) so different
    // tracks on the same album share one history entry and artwork file.
    // Falls back to artist+title when album is empty (e.g. singles).
    String key;
    if (album && album[0]) {
        key = String(artist) + "|" + String(album);
    } else {
        key = String(artist) + "|" + String(title);
    }
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

    // Prune oldest non-pinned entry if at capacity
    while (arr.size() >= HISTORY_MAX) {
        // Find oldest non-pinned entry by timestamp
        int oldest = -1;
        unsigned long oldestTs = ULONG_MAX;
        int i = 0;
        for (JsonObject obj : arr) {
            bool pinned = obj["pin"] | false;
            if (!pinned) {
                unsigned long ts = obj["ts"] | 0UL;
                if (ts < oldestTs) { oldestTs = ts; oldest = i; }
            }
            i++;
        }
        if (oldest < 0) {
            // All entries are pinned — cannot prune
            Serial.println("[History] All entries pinned, cannot prune");
            break;
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
            g_shuffleDirty = true;
            return true;
        }
    }
    return false;
}

bool sdHistorySetPinned(const char* file, bool pinned) {
    JsonDocument doc;
    if (!readIndex(doc)) return false;
    JsonArray arr = doc.as<JsonArray>();
    for (JsonObject obj : arr) {
        if (strcmp(obj["f"] | "", file) == 0) {
            obj["pin"] = pinned;
            writeIndex(doc);
            g_shuffleDirty = true;
            return true;
        }
    }
    return false;
}

bool sdHistoryDelete(const char* file) {
    if (!file || !file[0]) return false;
    JsonDocument doc;
    if (!readIndex(doc)) return false;
    JsonArray arr = doc.as<JsonArray>();
    int found = -1;
    int i = 0;
    for (JsonObject obj : arr) {
        if (strcmp(obj["f"] | "", file) == 0) { found = i; break; }
        i++;
    }
    if (found < 0) return false;
    // Delete JPEG file
    String fpath = String("/history/") + file;
    SD_MMC.remove(fpath);
    arr.remove(found);
    writeIndex(doc);
    g_shuffleDirty = true;
    Serial.printf("[History] Deleted: %s\n", file);
    return true;
}

// ─── Shuffle-bag state ───
// Implements iPod-shuffle-style randomness: cycle through all enabled items in
// a random order, then re-shuffle for the next cycle.  Avoids the clustering
// and starvation that pure random produces.
static char g_shuffleBag[HISTORY_MAX][HISTORY_FNAME_LEN];
static int  g_shuffleCount = 0;
static int  g_shufflePos   = 0;
// g_shuffleDirty forward-declared above

static void rebuildShuffleBag() {
    JsonDocument doc;
    if (!readIndex(doc)) { g_shuffleCount = 0; g_shufflePos = 0; return; }
    JsonArray arr = doc.as<JsonArray>();

    g_shuffleCount = 0;
    for (JsonObject obj : arr) {
        if ((obj["on"] | true) && g_shuffleCount < HISTORY_MAX) {
            strlcpy(g_shuffleBag[g_shuffleCount], obj["f"] | "", HISTORY_FNAME_LEN);
            g_shuffleCount++;
        }
    }

    // Fisher-Yates shuffle
    for (int i = g_shuffleCount - 1; i > 0; i--) {
        int j = random(i + 1);
        char tmp[HISTORY_FNAME_LEN];
        strlcpy(tmp, g_shuffleBag[i], HISTORY_FNAME_LEN);
        strlcpy(g_shuffleBag[i], g_shuffleBag[j], HISTORY_FNAME_LEN);
        strlcpy(g_shuffleBag[j], tmp, HISTORY_FNAME_LEN);
    }
    g_shufflePos = 0;
}

String sdHistoryRandomFile() {
    if (g_shuffleDirty || g_shufflePos >= g_shuffleCount) {
        rebuildShuffleBag();
        g_shuffleDirty = false;
        if (g_shuffleCount == 0) return "";
    }
    return String("/history/") + g_shuffleBag[g_shufflePos++];
}
