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
    settings.poll_interval_ms = SONOS_POLL_INTERVAL_MS;
    settings.show_track_info = true;
    settings.use_dithering = true;

    File f = SD_MMC.open("/settings.json", FILE_READ);
    if (!f) return false;

    JsonDocument doc;
    if (deserializeJson(doc, f)) {
        f.close();
        return false;
    }
    f.close();

    strlcpy(settings.sonos_ip, doc["sonos_ip"] | "", sizeof(settings.sonos_ip));
    strlcpy(settings.acrcloud_host, doc["acrcloud_host"] | "", sizeof(settings.acrcloud_host));
    strlcpy(settings.acrcloud_key, doc["acrcloud_key"] | "", sizeof(settings.acrcloud_key));
    strlcpy(settings.acrcloud_secret, doc["acrcloud_secret"] | "", sizeof(settings.acrcloud_secret));
    strlcpy(settings.spotify_client_id, doc["spotify_client_id"] | "", sizeof(settings.spotify_client_id));
    strlcpy(settings.spotify_client_secret, doc["spotify_client_secret"] | "", sizeof(settings.spotify_client_secret));
    strlcpy(settings.google_photos_url, doc["google_photos_url"] | "", sizeof(settings.google_photos_url));
    settings.poll_interval_ms = doc["poll_interval_ms"] | SONOS_POLL_INTERVAL_MS;
    settings.show_track_info = doc["show_track_info"] | true;
    settings.use_dithering = doc["use_dithering"] | true;
    return true;
}

bool sdWriteSettings(const Settings& settings) {
    JsonDocument doc;
    doc["sonos_ip"] = settings.sonos_ip;
    doc["acrcloud_host"] = settings.acrcloud_host;
    doc["acrcloud_key"] = settings.acrcloud_key;
    doc["acrcloud_secret"] = settings.acrcloud_secret;
    doc["spotify_client_id"] = settings.spotify_client_id;
    doc["spotify_client_secret"] = settings.spotify_client_secret;
    doc["google_photos_url"] = settings.google_photos_url;
    doc["poll_interval_ms"] = settings.poll_interval_ms;
    doc["show_track_info"] = settings.show_track_info;
    doc["use_dithering"] = settings.use_dithering;

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
