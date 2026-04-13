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

    // Ensure gallery directory exists
    if (!SD_MMC.exists("/gallery")) {
        SD_MMC.mkdir("/gallery");
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

String sdListGallery() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    File dir = SD_MMC.open("/gallery");
    if (!dir || !dir.isDirectory()) {
        String out;
        serializeJson(doc, out);
        return out;
    }

    File entry;
    while ((entry = dir.openNextFile())) {
        if (!entry.isDirectory()) {
            JsonObject obj = arr.add<JsonObject>();
            obj["name"] = String(entry.name());
            obj["size"] = entry.size();
        }
        entry.close();
    }
    dir.close();

    String out;
    serializeJson(doc, out);
    return out;
}

bool sdDeleteFile(const char* path) {
    return SD_MMC.remove(path);
}

String sdRandomGalleryFile() {
    File dir = SD_MMC.open("/gallery");
    if (!dir || !dir.isDirectory()) return "";

    int count = 0;
    File entry;
    while ((entry = dir.openNextFile())) {
        if (!entry.isDirectory()) count++;
        entry.close();
    }
    if (count == 0) return "";

    int pick = random(count);
    dir.rewindDirectory();
    int idx = 0;
    String result;
    while ((entry = dir.openNextFile())) {
        if (!entry.isDirectory()) {
            if (idx == pick) {
                result = String("/gallery/") + entry.name();
                entry.close();
                break;
            }
            idx++;
        }
        entry.close();
    }
    dir.close();
    return result;
}
