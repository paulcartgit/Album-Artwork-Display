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
String g_lastArtUrl;

// ─── Pending display update (queued while display is refreshing) ───
static String g_pendingArtUrl;
static String g_pendingArtist;
static String g_pendingTitle;
static String g_pendingAlbum;

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

// ─── Vinyl API-call protection ───
unsigned long g_lastNoMatchTime = 0;
static const unsigned long VINYL_RETRY_DELAY = 15 * 1000; // 15 sec between no-match retries
static const int VINYL_MAX_RETRIES = 3;
int g_vinylNoMatchCount = 0;
static const float SILENCE_RMS_THRESHOLD = 150.0f; // 16-bit PCM; typical noise floor ~50-100
unsigned long g_lastVinylMatchTime = 0; // when last successful vinyl identify happened

// ─── Sonos poll timing (accessible from web_server) ───
unsigned long g_lastPollTime = 0;

// ─── Physical button for vinyl re-identify ───
volatile bool g_buttonReIdentify = false;
static void IRAM_ATTR onKeyPress() {
    g_buttonReIdentify = true;
}

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

    // Physical button (BTN_KEY on back of frame) — press to re-identify vinyl
    pinMode(BTN_KEY, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BTN_KEY), onKeyPress, FALLING);

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

    unsigned long now = millis();

    // Process pending display update once panel finishes refreshing
    if (g_pendingArtUrl.length() > 0 && !displayIsBusy()) {
        String url = g_pendingArtUrl;
        String artist = g_pendingArtist;
        String title  = g_pendingTitle;
        String album  = g_pendingAlbum;
        g_pendingArtUrl = "";
        g_pendingArtist = "";
        g_pendingTitle  = "";
        g_pendingAlbum  = "";
        activityLog("Processing queued artwork...");
        const char* overlayArtist = g_settings.show_track_info ? artist.c_str() : nullptr;
        const char* overlayAlbum  = g_settings.show_track_info ? album.c_str()  : nullptr;
        if (pipelineProcessUrl(url.c_str(), overlayArtist, overlayAlbum,
                               artist.c_str(), title.c_str(), album.c_str())) {
            g_lastArtUrl = url;
            activityLog("Queued artwork displayed");
        }
    }

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

    // Physical button: immediately re-identify vinyl
    if (g_buttonReIdentify) {
        g_buttonReIdentify = false;
        g_lastVinylMatchTime = 0;
        g_lastNoMatchTime = 0;
        g_vinylNoMatchCount = 0;
        g_lastPollTime = 0; // force immediate poll
        activityLog("Button pressed → re-identifying vinyl");
    }

    // Force refresh: clear caches so next poll re-renders
    if (g_forceRefresh) {
        g_forceRefresh = false;
        g_lastTrackHash = "";
        g_lastArtUrl    = "";
        g_lastIdleSwap  = 0;
        g_lastPollTime = 0; // force immediate poll
        activityLog("Force refresh — clearing caches");
    }

    if (now - g_lastPollTime < g_settings.sonos_poll_ms) {
        delay(100);
        return;
    }
    g_lastPollTime = now;

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
            g_lastVinylMatchTime = 0;
            g_vinylNoMatchCount = 0;
            g_lastNoMatchTime = 0;
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

        // Skip if we identified recently and no retries pending
        if (g_lastVinylMatchTime != 0 && g_vinylNoMatchCount == 0 &&
            (millis() - g_lastVinylMatchTime) < g_settings.vinyl_recheck_ms) {
            return;
        }

        // Cool down after repeated no-matches to save API calls
        if (g_lastNoMatchTime != 0) {
            unsigned long sinceNoMatch = millis() - g_lastNoMatchTime;
            if (g_vinylNoMatchCount >= VINYL_MAX_RETRIES) {
                // 3 strikes — full cooldown
                if (sinceNoMatch < g_settings.no_match_cooldown_ms) return;
                // Cooldown expired — reset and try again
                g_vinylNoMatchCount = 0;
                g_lastNoMatchTime = 0;
            } else {
                // Still retrying — wait 15s between attempts
                if (sinceNoMatch < VINYL_RETRY_DELAY) return;
            }
        }

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

        activityLogf("Recorded %u bytes", (unsigned)recorded);

        // ── Silence detection: skip Shazam if audio is too quiet ──
        {
            size_t numSamples = recorded / 2; // 16-bit samples
            const int16_t* samples = (const int16_t*)audioBuf;
            double sumSq = 0;
            for (size_t i = 0; i < numSamples; i++) {
                double s = samples[i];
                sumSq += s * s;
            }
            float rms = sqrtf((float)(sumSq / numSamples));
            activityLogf("Audio RMS: %.0f (threshold: %.0f)", rms, SILENCE_RMS_THRESHOLD);
            if (rms < SILENCE_RMS_THRESHOLD) {
                activityLog("Audio too quiet — skipping Shazam (turntable silent?)");
                heap_caps_free(audioBuf);
                g_lastNoMatchTime = millis(); // cool down before retrying
                digitalWrite(LED_RED, LOW);
                return;
            }
        }

        activityLog("Identifying via Shazam...");

        // Wrap in WAV container for Shazam file upload
        size_t wavLen = 44 + recorded;
        uint8_t* wavBuf = (uint8_t*)heap_caps_malloc(wavLen, MALLOC_CAP_SPIRAM);
        if (!wavBuf) {
            activityLog("WAV buffer alloc failed");
            heap_caps_free(audioBuf);
            showFallbackImage();
            return;
        }
        {
            uint8_t* h = wavBuf;
            uint16_t ch = AUDIO_CHANNELS;
            uint32_t sr = AUDIO_SAMPLE_RATE;
            uint16_t bps = 16;
            uint32_t byteRate = sr * ch * bps / 8;
            uint16_t blockAlign = ch * bps / 8;
            uint32_t riffSize = wavLen - 8;
            uint32_t fmtSize = 16;
            uint16_t audioFmt = 1;
            memcpy(h, "RIFF", 4);      memcpy(h+4, &riffSize, 4);
            memcpy(h+8, "WAVEfmt ", 8); memcpy(h+16, &fmtSize, 4);
            memcpy(h+20, &audioFmt, 2); memcpy(h+22, &ch, 2);
            memcpy(h+24, &sr, 4);       memcpy(h+28, &byteRate, 4);
            memcpy(h+32, &blockAlign, 2); memcpy(h+34, &bps, 2);
            memcpy(h+36, "data", 4);    memcpy(h+40, &recorded, 4);
            memcpy(h+44, audioBuf, recorded);
        }
        heap_caps_free(audioBuf);

        // Identify via Shazam
        ShazamResult shazam;
        bool identified = shazamIdentify(
            g_settings.shazam_api_key,
            wavBuf, wavLen, shazam
        );
        heap_caps_free(wavBuf);

        if (!identified) {
            g_vinylNoMatchCount++;
            g_lastNoMatchTime = millis();
            if (g_vinylNoMatchCount >= VINYL_MAX_RETRIES) {
                activityLogf("Shazam — no match (%d/%d) — cooling down %lum",
                             g_vinylNoMatchCount, VINYL_MAX_RETRIES, g_settings.no_match_cooldown_ms / 60000);
            } else {
                activityLogf("Shazam — no match (%d/%d) — retrying in %lus",
                             g_vinylNoMatchCount, VINYL_MAX_RETRIES, VINYL_RETRY_DELAY / 1000);
            }
            digitalWrite(LED_RED, LOW);
            return;
        }

        // Got a match — reset retry counter and cooldown, start recheck timer
        g_vinylNoMatchCount = 0;
        g_lastNoMatchTime = 0;
        g_lastVinylMatchTime = millis();

        activityLogf("Identified: %s — %s", shazam.artist.c_str(), shazam.title.c_str());

        g_currentArtist = shazam.artist;
        g_currentTitle  = shazam.title;
        g_currentAlbum  = shazam.album;

        // Fetch album art from Shazam
        activityLog("Fetching album art...");
        String artUrl = shazam.coverArtUrl;
        if (artUrl.length() > 0) {
            const char* overlayArtist = g_settings.show_track_info ? shazam.artist.c_str() : nullptr;
            const char* overlayAlbum  = g_settings.show_track_info ? shazam.album.c_str() : nullptr;
            pipelineProcessUrl(artUrl.c_str(), overlayArtist, overlayAlbum,
                               shazam.artist.c_str(), shazam.title.c_str(), shazam.album.c_str());
            g_lastArtUrl = artUrl;
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

        String artUrl = track.artUrl;

        // Skip display refresh if the artwork hasn't changed (same album)
        if (artUrl.length() > 0 && artUrl == g_lastArtUrl) {
            activityLog("Same artwork — skipping refresh");
            return;
        }

        if (artUrl.length() > 0) {
            if (displayIsBusy()) {
                // Display still refreshing — queue for later instead of blocking
                activityLog("Display busy — queuing artwork");
                g_pendingArtUrl   = artUrl;
                g_pendingArtist   = track.artist;
                g_pendingTitle    = track.title;
                g_pendingAlbum    = track.album;
            } else {
                activityLog("Downloading artwork...");
                const char* overlayArtist = g_settings.show_track_info ? track.artist.c_str() : nullptr;
                const char* overlayAlbum  = g_settings.show_track_info ? track.album.c_str() : nullptr;
                if (pipelineProcessUrl(artUrl.c_str(), overlayArtist, overlayAlbum,
                                       track.artist.c_str(), track.title.c_str(), track.album.c_str())) {
                    g_lastArtUrl = artUrl;
                    activityLog("Display updated");
                }
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
    if (g_lastIdleSwap != 0 && (now - g_lastIdleSwap) < g_settings.idle_gallery_ms) return;
    g_lastIdleSwap = now;

    showFallbackImage();
}

static void showFallbackImage() {
    String path = sdHistoryRandomFile();
    if (path.length() > 0) {
        activityLogf("Showing history: %s", path.c_str());
        pipelineProcessFile(path.c_str());
    } else {
        displayShowMessage("No images\nPlay some music!");
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

    // Wrap mono PCM in WAV container for Shazam file upload
    size_t wavLen = 44 + monoBytes;
    uint8_t* wavBuf = (uint8_t*)heap_caps_malloc(wavLen, MALLOC_CAP_SPIRAM);
    if (!wavBuf) {
        activityLog("Listen: WAV buffer alloc failed");
        heap_caps_free(audioBuf);
        digitalWrite(LED_RED, LOW);
        return;
    }
    {
        uint8_t* h = wavBuf;
        uint16_t ch = 1;
        uint32_t sr = AUDIO_SAMPLE_RATE;
        uint16_t bps = 16;
        uint32_t byteRate = sr * ch * bps / 8;
        uint16_t blockAlign = ch * bps / 8;
        uint32_t riffSize = wavLen - 8;
        uint32_t fmtSize = 16;
        uint16_t audioFmt = 1;
        memcpy(h, "RIFF", 4);      memcpy(h+4, &riffSize, 4);
        memcpy(h+8, "WAVEfmt ", 8); memcpy(h+16, &fmtSize, 4);
        memcpy(h+20, &audioFmt, 2); memcpy(h+22, &ch, 2);
        memcpy(h+24, &sr, 4);       memcpy(h+28, &byteRate, 4);
        memcpy(h+32, &blockAlign, 2); memcpy(h+34, &bps, 2);
        memcpy(h+36, "data", 4);    memcpy(h+40, &monoBytes, 4);
        memcpy(h+44, audioBuf, monoBytes);
    }
    heap_caps_free(audioBuf);

    ShazamResult shazam;
    bool identified = shazamIdentify(
        g_settings.shazam_api_key,
        wavBuf, wavLen, shazam
    );
    heap_caps_free(wavBuf);

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
    String artUrl = shazam.coverArtUrl;
    if (artUrl.length() > 0) {
        const char* overlayArtist = g_settings.show_track_info ? shazam.artist.c_str() : nullptr;
        const char* overlayAlbum  = g_settings.show_track_info ? shazam.album.c_str() : nullptr;
        if (pipelineProcessUrl(artUrl.c_str(), overlayArtist, overlayAlbum,
                               shazam.artist.c_str(), shazam.title.c_str(), shazam.album.c_str())) {
            g_lastArtUrl = artUrl;
            activityLog("Listen: display updated");
        }
    } else {
        activityLog("Listen: no album art found");
    }

    digitalWrite(LED_RED, LOW);
}
