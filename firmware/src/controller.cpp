#include <Arduino.h>
#include <Wire.h>
#include <esp_heap_caps.h>
#include <XPowersLib.h>
#include <WiFi.h>

#include "controller.h"
#include "app.h"
#include "config.h"
#include "sd_manager.h"
#include "wifi_manager.h"
#include "display.h"
#include "sonos_client.h"
#include "identify.h"
#include "image_pipeline.h"
#include "web_server.h"
#include "activity_log.h"
#include "backoff.h"

// ─── Shared state (declared in app.h) ───
AppContext  g_app = {};
AppRequests g_req = {};

// ─── Track-change detection ───
static String g_lastTrackHash;
static String g_lastVinylAlbumKey; // artist|album — skip re-display if same album

static String trackHash(const String& artist, const String& title) {
    return artist + "|" + title;
}

// ─── Idle state helpers ───
static unsigned long g_lastIdleSwap = 0;
static int g_consecutiveIdlePolls = 0;
static const int IDLE_DEBOUNCE_COUNT = 2; // consecutive idle polls before transitioning

// ─── Sonos re-discovery when IP changes ───
static int g_sonosUnreachableCount = 0;
static const int SONOS_UNREACHABLE_BEFORE_REDISCOVER = 3;

// ─── Physical button for vinyl re-identify ───
static volatile bool g_buttonReIdentify = false;
static void IRAM_ATTR onKeyPress() {
    g_buttonReIdentify = true;
}

// Escalating cooldown duration based on the current level
static unsigned long vinylCooldownMs() {
    return vinylCooldownMsFor(g_app.settings.no_match_cooldown_ms,
                              g_app.vinylCooldownLevel, VINYL_MAX_COOLDOWN_MS);
}

// Retries before entering cooldown (fewer after the first escalation)
static int vinylMaxRetries() {
    return vinylMaxRetriesFor(g_app.vinylCooldownLevel, VINYL_MAX_RETRIES);
}

// ─── Forward declarations ───
static void handlePlaying();
static void handleIdle();
static void showFallbackImage();
static void resetVinylBackoff();
static void serviceRequests();

// ═══════════════════════════════════════════════════════════
void controllerSetup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== Vinyl Now-Playing Display ===");
    Serial.printf("Free heap: %u | PSRAM: %u\n",
                  ESP.getFreeHeap(), ESP.getFreePsram());

    g_app.state = STATE_BOOT;
    g_app.lastAudioChannels   = 1;
    g_app.lastAudioSampleRate = AUDIO_SAMPLE_RATE;

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
        g_app.state = STATE_ERROR;
        return;
    }

    // ── Display ──
    displayInit();

    // ── WiFi ──
    WifiConfig wifiCfg;
    bool hasConfig = sdReadWifiConfig(wifiCfg);
    bool wifiOk = false;
    if (hasConfig) {
        displayShowMessage("Connecting...");
        wifiOk = wifiConnect(wifiCfg);
    }

    if (!wifiOk) {
        // No credentials on SD, or connection failed → start setup AP
        const char* AP_NAME = "NowPlaying-Setup";
        Serial.println("[BOOT] Entering setup mode (captive portal)");
        if (!hasConfig) {
            Serial.println("[BOOT] No WiFi config on SD");
            displayShowMessage("Setup Mode\nJoin Wi-Fi:\nNowPlaying-Setup\nthen visit\n192.168.4.1");
        } else {
            Serial.println("[BOOT] WiFi connection failed");
            displayShowMessage("Wi-Fi Failed\nJoin Wi-Fi:\nNowPlaying-Setup\nto reconfigure\n192.168.4.1");
        }
        if (!wifiStartAP(AP_NAME)) {
            displayShowMessage("AP start failed");
            g_app.state = STATE_ERROR;
            return;
        }
        captivePortalInit();
        g_app.state = STATE_SETUP;
        return;
    }

    // ── NTP time sync (history timestamps depend on this) ──
    configTime(0, 0, "pool.ntp.org");

    // ── Load settings ──
    sdReadSettings(g_app.settings);

    // ── Resolve Sonos speaker IP from room name ──
    // Migration path: if a name is not yet stored but an IP is, fetch the name
    // from the device and cache it so future re-discovery can use it.
    if (strlen(g_app.settings.sonos_name) == 0 && strlen(g_app.settings.sonos_ip) > 0) {
        char name[64] = {};
        if (sonosGetDeviceName(g_app.settings.sonos_ip, name, sizeof(name))) {
            strlcpy(g_app.settings.sonos_name, name, sizeof(g_app.settings.sonos_name));
            sdWriteSettings(g_app.settings);
            Serial.printf("[BOOT] Cached speaker name: %s\n", g_app.settings.sonos_name);
        }
    }
    // If we have a name but the IP cache is empty, run discovery now.
    if (strlen(g_app.settings.sonos_name) > 0 && strlen(g_app.settings.sonos_ip) == 0) {
        Serial.printf("[BOOT] Resolving IP for speaker: %s\n", g_app.settings.sonos_name);
        char ip[40] = {};
        if (sonosResolveByName(g_app.settings.sonos_name, ip, sizeof(ip))) {
            strlcpy(g_app.settings.sonos_ip, ip, sizeof(g_app.settings.sonos_ip));
            sdWriteSettings(g_app.settings);
            Serial.printf("[BOOT] Resolved IP: %s\n", g_app.settings.sonos_ip);
        } else {
            Serial.println("[BOOT] Speaker not found on network — will retry during polling");
        }
    }

    // ── Web server ──
    webServerInit();

    // ── Ready ──
    g_app.state = STATE_IDLE;
    digitalWrite(LED_GREEN, HIGH);
    Serial.println("[BOOT] Ready — entering main loop");
    displayShowMessage("Ready\nnowplaying.local");
    delay(3000);
}

// ═══════════════════════════════════════════════════════════
void controllerLoop() {
    if (g_app.state == STATE_ERROR) {
        delay(10000);
        return;
    }
    if (g_app.state == STATE_SETUP) {
        captivePortalLoop();
        if (g_req.reboot) {
            g_req.reboot = false;
            delay(500); // let the HTTP response reach the browser
            ESP.restart();
        }
        delay(10);
        return;
    }

    unsigned long now = millis();

    // ── Periodic heap health check (every 60s) ──
    static unsigned long s_lastHeapLog = 0;
    if (now - s_lastHeapLog > 60000) {
        s_lastHeapLog = now;
        Serial.printf("[Health] Heap: %u free, PSRAM: %u free (largest: %u)\n",
                      ESP.getFreeHeap(),
                      heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                      heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    }

    // ── WiFi reconnection ──
    if (!wifiIsConnected()) {
        static unsigned long s_lastReconnect = 0;
        if (now - s_lastReconnect > 30000) {
            s_lastReconnect = now;
            activityLog("WiFi disconnected — attempting reconnect...");
            WiFi.reconnect();
        }
        return;
    }

    // ── Work requested by the web server ──
    serviceRequests();
    if (g_app.state == STATE_ERROR) return;

    // Physical button: immediately re-identify vinyl
    if (g_buttonReIdentify) {
        g_buttonReIdentify = false;
        resetVinylBackoff();
        g_app.lastPollTime = 0; // force immediate poll
        activityLog("Button pressed → re-identifying vinyl");
    }

    if (now - g_app.lastPollTime < g_app.settings.sonos_poll_ms) {
        delay(100);
        return;
    }
    g_app.lastPollTime = now;

    // ── Check Sonos ──
    if (strlen(g_app.settings.sonos_ip) == 0 && strlen(g_app.settings.sonos_name) == 0) {
        handleIdle(); // no Sonos configured — stay idle
        return;
    }

    // If we have a name but no cached IP (e.g. boot resolution failed), resolve now
    if (strlen(g_app.settings.sonos_ip) == 0 && strlen(g_app.settings.sonos_name) > 0) {
        activityLogf("Resolving speaker '%s'...", g_app.settings.sonos_name);
        char ip[40] = {};
        if (sonosResolveByName(g_app.settings.sonos_name, ip, sizeof(ip))) {
            strlcpy(g_app.settings.sonos_ip, ip, sizeof(g_app.settings.sonos_ip));
            sdWriteSettings(g_app.settings);
            activityLogf("Sonos resolved: %s", ip);
        } else {
            activityLog("Sonos resolve failed — will retry");
            handleIdle();
            return;
        }
    }

    bool reachable = false;
    bool playing = sonosIsPlaying(g_app.settings.sonos_ip, &reachable);

    if (!reachable) {
        // Connection error — device may have changed IP
        g_sonosUnreachableCount++;
        activityLogf("Sonos unreachable (%d/%d)", g_sonosUnreachableCount,
                     SONOS_UNREACHABLE_BEFORE_REDISCOVER);

        if (g_sonosUnreachableCount >= SONOS_UNREACHABLE_BEFORE_REDISCOVER &&
            strlen(g_app.settings.sonos_name) > 0) {
            activityLogf("Re-discovering '%s'...", g_app.settings.sonos_name);
            char newIp[40] = {};
            if (sonosResolveByName(g_app.settings.sonos_name, newIp, sizeof(newIp))) {
                strlcpy(g_app.settings.sonos_ip, newIp, sizeof(g_app.settings.sonos_ip));
                sdWriteSettings(g_app.settings);
                activityLogf("Sonos re-discovered at %s", newIp);
                g_sonosUnreachableCount = 0;
                // Retry immediately with the new IP
                playing = sonosIsPlaying(g_app.settings.sonos_ip, &reachable);
                if (!reachable) { handleIdle(); return; }
            } else {
                activityLog("Re-discovery failed — will retry next poll");
                handleIdle();
                return;
            }
        } else {
            handleIdle();
            return;
        }
    } else {
        g_sonosUnreachableCount = 0; // reset on any successful contact
    }

    if (!playing) {
        g_consecutiveIdlePolls++;
        if (g_app.state != STATE_IDLE) {
            if (g_consecutiveIdlePolls < IDLE_DEBOUNCE_COUNT) {
                activityLogf("Sonos idle (%d/%d) — waiting to confirm",
                             g_consecutiveIdlePolls, IDLE_DEBOUNCE_COUNT);
                return; // don't transition yet, could be a track change
            }
            activityLog("Sonos stopped → idle");
            g_app.state = STATE_IDLE;
            g_app.currentArtist = "";
            g_app.currentTitle  = "";
            g_app.currentAlbum  = "";
            g_lastTrackHash = "";
            g_app.lastArtUrl = "";
            resetVinylBackoff();
        }
        handleIdle();
        return;
    }

    // Sonos is playing — reset idle debounce counter
    g_consecutiveIdlePolls = 0;
    handlePlaying();
}

// ═══════════════════════════════════════════════════════════
// Requests raised by the web server task
// ═══════════════════════════════════════════════════════════
static void serviceRequests() {
    if (g_req.reboot) {
        g_req.reboot = false;
        activityLog("Reboot requested");
        delay(200);
        ESP.restart();
    }

    if (g_req.testColors) {
        g_req.testColors = false;
        activityLog("Test color pattern requested");
        pipelineShowTestPattern();
        return;
    }

    if (g_req.testDither) {
        g_req.testDither = false;
        activityLog("Dither test pattern requested");
        pipelineShowDitherTest();
        return;
    }

    // Sonos discovery — SSDP + SOAP, far too slow to run on the AsyncTCP task
    if (g_app.scanState == SCAN_REQUESTED) {
        g_app.scanState = SCAN_RUNNING;
        activityLog("Scanning for Sonos speakers...");
        int n = sonosDiscover(g_app.scanResults, SONOS_SCAN_MAX, 3000);
        g_app.scanCount = n;
        g_app.scanState = SCAN_DONE;
        activityLogf("Sonos scan complete: %d speaker(s)", n);
        return;
    }

    if (g_req.forceListen) {
        g_req.forceListen = false;
        g_app.state = STATE_VINYL;
        digitalWrite(LED_RED, HIGH);
        String artist, title, album;
        if (identifyNowPlaying(IDENTIFY_MANUAL, artist, title, album) == IDENTIFY_OK) {
            g_app.currentArtist = artist;
            g_app.currentTitle  = title;
            g_app.currentAlbum  = album;
            g_lastTrackHash = trackHash(artist, title);
        }
        digitalWrite(LED_RED, LOW);
        return;
    }

    if (g_req.forceRefresh) {
        g_req.forceRefresh = false;
        g_lastTrackHash = "";
        g_app.lastArtUrl = "";
        g_lastIdleSwap = 0;
        g_app.lastPollTime = 0; // force immediate poll
        resetVinylBackoff();
        activityLog("Force refresh — clearing caches");
    }
}

static void resetVinylBackoff() {
    g_app.lastVinylMatchTime = 0;
    g_app.lastNoMatchTime    = 0;
    g_app.vinylNoMatchCount  = 0;
    g_app.vinylCooldownLevel = 0;
    g_lastVinylAlbumKey = "";
}

// ═══════════════════════════════════════════════════════════
static void handleVinyl() {
    // Skip if we identified recently and no retries are pending
    if (g_app.lastVinylMatchTime != 0 && g_app.vinylNoMatchCount == 0 &&
        (millis() - g_app.lastVinylMatchTime) < g_app.settings.vinyl_recheck_ms) {
        return;
    }

    // Cool down after repeated no-matches to save API calls
    if (g_app.lastNoMatchTime != 0) {
        unsigned long sinceNoMatch = millis() - g_app.lastNoMatchTime;
        if (g_app.vinylNoMatchCount >= vinylMaxRetries()) {
            if (sinceNoMatch < vinylCooldownMs()) return;
            // Cooldown expired — escalate and try again
            g_app.vinylCooldownLevel++;
            g_app.vinylNoMatchCount = 0;
            g_app.lastNoMatchTime = 0;
            activityLogf("Cooldown expired — escalation level %d, next cooldown %lum",
                         g_app.vinylCooldownLevel, vinylCooldownMs() / 60000);
        } else {
            if (sinceNoMatch < VINYL_RETRY_DELAY_MS) return; // still retrying
        }
    }

    g_app.state = STATE_VINYL;
    activityLog("Line-In detected → recording audio...");
    digitalWrite(LED_RED, HIGH);

    String artist, title, album;
    IdentifyResult res = identifyNowPlaying(IDENTIFY_VINYL, artist, title, album);
    digitalWrite(LED_RED, LOW);

    if (res == IDENTIFY_SILENT) {
        g_app.lastNoMatchTime = millis(); // cool down before retrying
        return;
    }
    if (res == IDENTIFY_ERROR) {
        showFallbackImage();
        return;
    }
    if (res == IDENTIFY_NO_MATCH) {
        g_app.vinylNoMatchCount++;
        g_app.lastNoMatchTime = millis();
        if (g_app.vinylNoMatchCount >= vinylMaxRetries()) {
            activityLogf("No match (%d/%d) — cooling down %lum (level %d)",
                         g_app.vinylNoMatchCount, vinylMaxRetries(),
                         vinylCooldownMs() / 60000, g_app.vinylCooldownLevel);
        } else {
            activityLogf("No match (%d/%d) — retrying in %lus",
                         g_app.vinylNoMatchCount, vinylMaxRetries(),
                         (unsigned long)VINYL_RETRY_DELAY_MS / 1000);
        }
        return;
    }

    // Matched — reset retry counters and start the recheck timer
    g_app.vinylNoMatchCount  = 0;
    g_app.lastNoMatchTime    = 0;
    g_app.vinylCooldownLevel = 0;
    g_app.lastVinylMatchTime = millis();

    g_app.currentArtist = artist;
    g_app.currentTitle  = title;
    g_app.currentAlbum  = album;

    // Remember the album so an unchanged side doesn't re-render the panel.
    // Singles with no album name are always shown.
    g_lastVinylAlbumKey = (album.length() > 0) ? (artist + "|" + album) : String("");
}

static void handleDigital(const SonosTrackInfo& track) {
    String hash = trackHash(track.artist, track.title);
    if (hash == g_lastTrackHash) return; // same track still playing

    g_app.state = STATE_DIGITAL;
    g_app.currentArtist = track.artist;
    g_app.currentTitle  = track.title;
    g_app.currentAlbum  = track.album;
    g_lastTrackHash = hash;

    activityLogf("Track: %s — %s (%s)",
                 track.artist.c_str(), track.title.c_str(), track.album.c_str());

    const String& artUrl = track.artUrl;
    if (artUrl.length() == 0) {
        activityLog("No album art found — showing fallback");
        showFallbackImage();
        return;
    }

    // Skip the refresh if the artwork hasn't changed (same album)
    if (artUrl == g_app.lastArtUrl) {
        activityLog("Same artwork — skipping refresh");
        return;
    }

    activityLog("Downloading artwork...");
    const char* overlayArtist = g_app.settings.show_track_info ? track.artist.c_str() : nullptr;
    const char* overlayAlbum  = g_app.settings.show_track_info ? track.album.c_str()  : nullptr;
    if (pipelineProcessUrl(artUrl.c_str(), overlayArtist, overlayAlbum,
                           track.artist.c_str(), track.title.c_str(), track.album.c_str())) {
        g_app.lastArtUrl = artUrl;
        activityLog("Display updated");
    } else {
        activityLog("Artwork pipeline failed");
    }
}

static void handlePlaying() {
    SonosTrackInfo track;
    if (!sonosGetTrackInfo(g_app.settings.sonos_ip, track)) {
        activityLog("Sonos poll failed");
        return;
    }

    if (track.isLineIn) {
        handleVinyl();
    } else {
        handleDigital(track);
    }
}

// ═══════════════════════════════════════════════════════════
static void handleIdle() {
    unsigned long now = millis();
    if (g_lastIdleSwap != 0 && (now - g_lastIdleSwap) < g_app.settings.idle_gallery_ms) return;
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
