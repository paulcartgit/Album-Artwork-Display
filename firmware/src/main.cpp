#include <Arduino.h>
#include <Wire.h>
#include <esp_heap_caps.h>
#include <XPowersLib.h>

#include "config.h"
#include "sd_manager.h"
#include "wifi_manager.h"
#include "display.h"
#include "sonos_client.h"
#include "audio_capture.h"
#include "shazam_client.h"
#include "spotify_client.h"
#include "google_photos.h"
#include "image_pipeline.h"
#include "web_server.h"
#include "activity_log.h"

// ─── Globals (shared with web_server.cpp) ───
Settings g_settings;
AppState g_state = STATE_BOOT;
String   g_currentArtist;
String   g_currentTitle;
String   g_currentAlbum;

// ─── Track-change detection ───
static String g_lastTrackHash;
static String g_lastArtUrl;

static String trackHash(const String& artist, const String& title) {
    return artist + "|" + title;
}

// ─── Flags set from web API, handled in main loop ───
volatile bool g_forceRefresh = false;
volatile bool g_testColors = false;
volatile bool g_forceListen = false;

// ─── Last recorded audio (for web download/debug) ───
uint8_t* g_lastAudio = nullptr;
size_t   g_lastAudioLen = 0;
uint32_t g_lastAudioChannels = AUDIO_CHANNELS;
uint32_t g_lastAudioSampleRate = AUDIO_SAMPLE_RATE;

// ─── Idle state helpers ───
static unsigned long g_lastIdleSwap = 0;
static const unsigned long IDLE_SWAP_INTERVAL = 5 * 60 * 1000; // 5 min

// ─── Forward declarations ───
static void handlePlaying();
static void handleIdle();
static void showFallbackImage();
static void handleListen();

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

    // Power management (AXP2101) — must be before display/SD init
    XPowersAXP2101 pmu;
    if (pmu.begin(Wire, AXP2101_ADDR, I2C_SDA, I2C_SCL)) {
        pmu.setDC1Voltage(3300);
        pmu.enableDC1();
        pmu.setALDO1Voltage(3300);
        pmu.enableALDO1();
        pmu.setALDO2Voltage(3300);
        pmu.enableALDO2();
        pmu.setALDO3Voltage(3300);
        pmu.enableALDO3();
        pmu.setALDO4Voltage(3300);
        pmu.enableALDO4();
        Serial.println("[BOOT] PMIC initialized — power rails enabled");
    } else {
        Serial.println("[BOOT] PMIC init failed!");
    }
    delay(100); // let rails stabilize

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

    // ── NTP time sync ──
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

    // Test color pattern (must run in main loop, not web handler)
    if (g_testColors) {
        g_testColors = false;
        activityLog("Test color pattern requested");
        pipelineShowTestPattern();
        return;
    }

    // Force listen: record audio and identify regardless of Sonos
    if (g_forceListen) {
        g_forceListen = false;
        handleListen();
        return;
    }

    // Force refresh: clear caches so next poll re-renders
    if (g_forceRefresh) {
        g_forceRefresh = false;
        g_lastTrackHash = "";
        g_lastArtUrl    = "";
        g_lastIdleSwap  = 0;
        lastPoll = 0; // force immediate poll
        activityLog("Force refresh — clearing caches");
    }

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
            activityLog("Sonos stopped → idle");
            g_state = STATE_IDLE;
            g_currentArtist = "";
            g_currentTitle  = "";
            g_currentAlbum  = "";
            g_lastTrackHash = "";
            g_lastArtUrl    = "";
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
        activityLog("Sonos poll failed");
        return;
    }

    if (track.isLineIn) {
        // ── VINYL mode ──
        String hash = trackHash("vinyl", String(millis() / 60000)); // re-identify every minute
        if (hash == g_lastTrackHash) return;

        g_state = STATE_VINYL;
        activityLog("Line-In detected → recording audio...");
        digitalWrite(LED_RED, HIGH);

        // Record audio
        uint8_t* audioBuf = (uint8_t*)heap_caps_malloc(AUDIO_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
        if (!audioBuf) {
            activityLog("Audio buffer alloc failed");
            showFallbackImage();
            return;
        }

        if (!audioInit()) {
            activityLog("Audio init failed");
            heap_caps_free(audioBuf);
            showFallbackImage();
            return;
        }

        size_t recorded = 0;
        bool ok = audioRecord(audioBuf, AUDIO_BUFFER_SIZE, recorded);
        audioDeinit();

        if (!ok || recorded == 0) {
            activityLog("Audio recording failed");
            heap_caps_free(audioBuf);
            showFallbackImage();
            return;
        }

        activityLogf("Recorded %u bytes — identifying via Shazam...", (unsigned)recorded);

        // Identify via Shazam
        ShazamResult shazam;
        bool identified = shazamIdentify(
            g_settings.shazam_api_key,
            audioBuf, recorded, shazam
        );
        heap_caps_free(audioBuf);

        if (!identified) {
            activityLog("Shazam — no match");
            showFallbackImage();
            return;
        }

        activityLogf("Identified: %s — %s", shazam.artist.c_str(), shazam.title.c_str());

        g_currentArtist = shazam.artist;
        g_currentTitle  = shazam.title;
        g_currentAlbum  = shazam.album;
        g_lastTrackHash = trackHash(shazam.artist, shazam.title);

        // Fetch album art — prefer Shazam cover art, fall back to Spotify
        activityLog("Fetching album art...");
        String artUrl = shazam.coverArtUrl;
        if (artUrl.length() == 0)
            artUrl = spotifyGetAlbumArtUrl(shazam.artist.c_str(), shazam.title.c_str());
        if (artUrl.length() > 0) {
            const char* overlayArtist = g_settings.show_track_info ? shazam.artist.c_str() : nullptr;
            const char* overlayAlbum  = g_settings.show_track_info ? shazam.album.c_str() : nullptr;
            pipelineProcessUrl(artUrl.c_str(), overlayArtist, overlayAlbum);
            activityLog("Display updated");
        } else {
            activityLog("No album art found — showing fallback");
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

        activityLogf("Track: %s — %s (%s)",
                      track.artist.c_str(), track.title.c_str(), track.album.c_str());

        // Prefer Sonos-provided art URL, fall back to Spotify search
        String artUrl = track.artUrl;
        if (artUrl.length() == 0) {
            activityLog("No Sonos art URL — searching Spotify...");
            artUrl = spotifyGetAlbumArtUrl(track.artist.c_str(), track.title.c_str());
        }

        // Skip display refresh if the artwork hasn't changed (same album)
        if (artUrl.length() > 0 && artUrl == g_lastArtUrl) {
            activityLog("Same artwork — skipping refresh");
            return;
        }

        if (artUrl.length() > 0) {
            activityLog("Downloading artwork...");
            const char* overlayArtist = g_settings.show_track_info ? track.artist.c_str() : nullptr;
            const char* overlayAlbum  = g_settings.show_track_info ? track.album.c_str() : nullptr;
            if (pipelineProcessUrl(artUrl.c_str(), overlayArtist, overlayAlbum)) {
                g_lastArtUrl = artUrl;
                activityLog("Display updated");
            }
        } else {
            activityLog("No album art found — showing fallback");
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
        activityLogf("Showing gallery: %s", path.c_str());
        pipelineProcessFile(path.c_str());
    } else {
        displayShowMessage("No images\nUpload at vinyl.local");
    }
}

// ═══════════════════════════════════════════════════════════
static void handleListen() {
    activityLog("Listen: initializing microphone...");
    g_state = STATE_VINYL;
    digitalWrite(LED_RED, HIGH);

    uint8_t* audioBuf = (uint8_t*)heap_caps_malloc(LISTEN_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    if (!audioBuf) {
        activityLog("Listen: audio buffer alloc failed");
        digitalWrite(LED_RED, LOW);
        return;
    }

    if (!audioInit()) {
        activityLog("Listen: audio init failed — check hardware");
        heap_caps_free(audioBuf);
        digitalWrite(LED_RED, LOW);
        return;
    }

    activityLogf("Listen: recording %ds of audio...", LISTEN_RECORD_SECS);
    size_t recorded = 0;
    bool ok = audioRecord(audioBuf, LISTEN_BUFFER_SIZE, recorded);
    audioDeinit();

    if (!ok || recorded == 0) {
        activityLogf("Listen: recording failed (got %u bytes)", (unsigned)recorded);
        heap_caps_free(audioBuf);
        digitalWrite(LED_RED, LOW);
        return;
    }

    activityLogf("Listen: recorded %uKB — sending to Shazam...", (unsigned)(recorded / 1024));

    // Stash a copy of raw stereo data for web download/debug
    if (g_lastAudio) { heap_caps_free(g_lastAudio); g_lastAudio = nullptr; }
    g_lastAudio = (uint8_t*)heap_caps_malloc(recorded, MALLOC_CAP_SPIRAM);
    if (g_lastAudio) {
        memcpy(g_lastAudio, audioBuf, recorded);
        g_lastAudioLen = recorded;
    }

    // Convert stereo to mono (take L channel = every other sample)
    // and apply software gain to boost quiet mic signal
    size_t monoBytes = recorded;
    if (AUDIO_CHANNELS > 1) {
        int16_t* samples = (int16_t*)audioBuf;
        size_t totalSamples = recorded / 2;
        size_t monoSamples = totalSamples / AUDIO_CHANNELS;

        // Find peak to calculate safe gain multiplier
        int32_t peak = 0;
        for (size_t i = 0; i < totalSamples; i += AUDIO_CHANNELS) {
            int32_t a = abs((int32_t)samples[i]);
            if (a > peak) peak = a;
        }
        // Auto-gain: normalize to ~80% of full scale, capped at 32x
        int32_t gain = (peak > 0) ? (26000 / peak) : 1;
        if (gain < 1) gain = 1;
        if (gain > 32) gain = 32;
        activityLogf("Listen: peak=%d gain=%dx", (int)peak, (int)gain);

        for (size_t i = 0; i < monoSamples; i++) {
            int32_t s = (int32_t)samples[i * AUDIO_CHANNELS] * gain;
            // Clamp to 16-bit range
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            samples[i] = (int16_t)s;
        }
        monoBytes = monoSamples * 2;
        activityLogf("Listen: mono %uKB (gain %dx)", (unsigned)(monoBytes/1024), (int)gain);
    }

    // Shazam wants raw PCM as base64 — send mono audio directly
    ShazamResult shazam;
    bool identified = shazamIdentify(
        g_settings.shazam_api_key,
        audioBuf, monoBytes, shazam
    );
    heap_caps_free(audioBuf);

    if (!identified) {
        activityLog("Listen: Shazam — no match");
        digitalWrite(LED_RED, LOW);
        return;
    }

    activityLogf("Listen: %s — %s (%s)", shazam.artist.c_str(), shazam.title.c_str(), shazam.album.c_str());

    g_currentArtist = shazam.artist;
    g_currentTitle  = shazam.title;
    g_currentAlbum  = shazam.album;
    g_lastTrackHash = trackHash(shazam.artist, shazam.title);

    activityLog("Listen: fetching album art...");
    // Use Shazam's cover art if available, otherwise fall back to Spotify
    String artUrl = shazam.coverArtUrl;
    if (artUrl.length() == 0)
        artUrl = spotifyGetAlbumArtUrl(shazam.artist.c_str(), shazam.title.c_str());
    if (artUrl.length() > 0) {
        const char* overlayArtist = g_settings.show_track_info ? shazam.artist.c_str() : nullptr;
        const char* overlayAlbum  = g_settings.show_track_info ? shazam.album.c_str() : nullptr;
        if (pipelineProcessUrl(artUrl.c_str(), overlayArtist, overlayAlbum)) {
            g_lastArtUrl = artUrl;
            activityLog("Listen: display updated");
        }
    } else {
        activityLog("Listen: no album art found");
    }

    digitalWrite(LED_RED, LOW);
}
