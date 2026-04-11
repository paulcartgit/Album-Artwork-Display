#include <Arduino.h>
#include <Wire.h>
#include <esp_heap_caps.h>

#include "config.h"
#include "sd_manager.h"
#include "wifi_manager.h"
#include "display.h"
#include "sonos_client.h"
#include "audio_capture.h"
#include "acrcloud_client.h"
#include "spotify_client.h"
#include "google_photos.h"
#include "image_pipeline.h"
#include "web_server.h"

// ─── Globals (shared with web_server.cpp) ───
Settings g_settings;
AppState g_state = STATE_BOOT;
String   g_currentArtist;
String   g_currentTitle;
String   g_currentAlbum;

// ─── Track-change detection ───
static String g_lastTrackHash;

static String trackHash(const String& artist, const String& title) {
    return artist + "|" + title;
}

// ─── Idle state helpers ───
static unsigned long g_lastIdleSwap = 0;
static const unsigned long IDLE_SWAP_INTERVAL = 5 * 60 * 1000; // 5 min

// ─── Forward declarations ───
static void handlePlaying();
static void handleIdle();
static void showFallbackImage();

// ═══════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== Vinyl Now-Playing Display ===");
    Serial.printf("Free heap: %u | PSRAM: %u\n",
                  ESP.getFreeHeap(), ESP.getFreePsram());

    // I2C bus (shared: AXP2101, ES7210, ES8311)
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000);

    // LED indicators
    pinMode(LED_RED, OUTPUT);
    pinMode(LED_GREEN, OUTPUT);
    digitalWrite(LED_RED, LOW);
    digitalWrite(LED_GREEN, LOW);

    // ── SD Card ──
    if (!sdInit()) {
        Serial.println("[BOOT] SD init failed — halting");
        displayInit();
        displayShowMessage("SD Card Error");
        g_state = STATE_ERROR;
        return;
    }

    // ── WiFi ──
    WifiConfig wifiCfg;
    if (!sdReadWifiConfig(wifiCfg)) {
        Serial.println("[BOOT] No WiFi config on SD");
        displayInit();
        displayShowMessage("No WiFi config\nPut config.json on SD");
        g_state = STATE_ERROR;
        return;
    }

    // ── Display ──
    displayInit();
    displayShowMessage("Connecting...");

    if (!wifiConnect(wifiCfg)) {
        displayShowMessage("WiFi Failed\nCheck config.json");
        g_state = STATE_ERROR;
        return;
    }

    // ── NTP time sync (needed for ACRCloud timestamp) ──
    configTime(0, 0, "pool.ntp.org");

    // ── Load settings ──
    sdReadSettings(g_settings);

    // ── Init subsystems ──
    if (strlen(g_settings.spotify_client_id) > 0) {
        spotifyInit(g_settings.spotify_client_id, g_settings.spotify_client_secret);
    }

    // ── Web server ──
    webServerInit();

    // ── Ready ──
    g_state = STATE_IDLE;
    digitalWrite(LED_GREEN, HIGH);
    Serial.println("[BOOT] Ready — entering main loop");
    displayShowMessage("Ready\nvinyl.local");
    delay(3000);
}

// ═══════════════════════════════════════════════════════════
void loop() {
    if (g_state == STATE_ERROR) {
        delay(10000);
        return;
    }

    static unsigned long lastPoll = 0;
    unsigned long now = millis();

    if (now - lastPoll < g_settings.poll_interval_ms) {
        delay(100);
        return;
    }
    lastPoll = now;

    // ── Check Sonos ──
    if (strlen(g_settings.sonos_ip) == 0) {
        // No Sonos configured — stay idle
        handleIdle();
        return;
    }

    bool playing = sonosIsPlaying(g_settings.sonos_ip);
    if (!playing) {
        if (g_state != STATE_IDLE) {
            Serial.println("[Main] Sonos stopped → IDLE");
            g_state = STATE_IDLE;
            g_currentArtist = "";
            g_currentTitle  = "";
            g_currentAlbum  = "";
            g_lastTrackHash = "";
        }
        handleIdle();
        return;
    }

    // Sonos is playing — get track info
    handlePlaying();
}

// ═══════════════════════════════════════════════════════════
static void handlePlaying() {
    SonosTrackInfo track;
    if (!sonosGetTrackInfo(g_settings.sonos_ip, track)) {
        Serial.println("[Main] Sonos poll failed");
        return;
    }

    if (track.isLineIn) {
        // ── VINYL mode ──
        String hash = trackHash("vinyl", String(millis() / 60000)); // re-identify every minute
        if (hash == g_lastTrackHash) return;

        g_state = STATE_VINYL;
        Serial.println("[Main] Line-In detected → VINYL");
        digitalWrite(LED_RED, HIGH);

        // Record audio
        uint8_t* audioBuf = (uint8_t*)heap_caps_malloc(AUDIO_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
        if (!audioBuf) {
            Serial.println("[Main] Audio buffer alloc failed");
            showFallbackImage();
            return;
        }

        if (!audioInit()) {
            heap_caps_free(audioBuf);
            showFallbackImage();
            return;
        }

        size_t recorded = 0;
        bool ok = audioRecord(audioBuf, AUDIO_BUFFER_SIZE, recorded);
        audioDeinit();

        if (!ok || recorded == 0) {
            heap_caps_free(audioBuf);
            showFallbackImage();
            return;
        }

        // Identify via ACRCloud
        AcrResult acr;
        bool identified = acrcloudIdentify(
            g_settings.acrcloud_host,
            g_settings.acrcloud_key,
            g_settings.acrcloud_secret,
            audioBuf, recorded, acr
        );
        heap_caps_free(audioBuf);

        if (!identified) {
            Serial.println("[Main] ACRCloud — no match");
            showFallbackImage();
            return;
        }

        g_currentArtist = acr.artist;
        g_currentTitle  = acr.title;
        g_currentAlbum  = acr.album;
        g_lastTrackHash = trackHash(acr.artist, acr.title);

        // Fetch album art via Spotify
        String artUrl = spotifyGetAlbumArtUrl(acr.artist.c_str(), acr.title.c_str());
        if (artUrl.length() > 0) {
            pipelineProcessUrl(artUrl.c_str());
        } else {
            showFallbackImage();
        }

        digitalWrite(LED_RED, LOW);

    } else {
        // ── DIGITAL mode ──
        String hash = trackHash(track.artist, track.title);
        if (hash == g_lastTrackHash) return; // same track still playing

        g_state = STATE_DIGITAL;
        g_currentArtist = track.artist;
        g_currentTitle  = track.title;
        g_currentAlbum  = track.album;
        g_lastTrackHash = hash;

        Serial.printf("[Main] New track: %s — %s\n",
                      track.artist.c_str(), track.title.c_str());

        // Prefer Sonos-provided art URL, fall back to Spotify search
        String artUrl = track.artUrl;
        if (artUrl.length() == 0) {
            artUrl = spotifyGetAlbumArtUrl(track.artist.c_str(), track.title.c_str());
        }

        if (artUrl.length() > 0) {
            pipelineProcessUrl(artUrl.c_str());
        } else {
            showFallbackImage();
        }
    }
}

// ═══════════════════════════════════════════════════════════
static void handleIdle() {
    unsigned long now = millis();
    if (g_lastIdleSwap != 0 && (now - g_lastIdleSwap) < IDLE_SWAP_INTERVAL) return;
    g_lastIdleSwap = now;

    // Try Google Photos bridge first
    if (strlen(g_settings.google_photos_url) > 0) {
        String url = googlePhotosGetUrl(g_settings.google_photos_url);
        if (url.length() > 0 && pipelineProcessUrl(url.c_str())) {
            return;
        }
    }

    // Fall back to local gallery
    showFallbackImage();
}

static void showFallbackImage() {
    String path = sdRandomGalleryFile();
    if (path.length() > 0) {
        Serial.printf("[Main] Showing gallery: %s\n", path.c_str());
        pipelineProcessFile(path.c_str());
    } else {
        displayShowMessage("No images\nUpload at vinyl.local");
    }
}
