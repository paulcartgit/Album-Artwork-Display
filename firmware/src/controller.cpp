#include <Arduino.h>
#include <Wire.h>
#include <esp_heap_caps.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <ctime>

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
#include "upnp_events.h"
#include "metadata_client.h"
#include "power.h"

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

// ─── Sonos eventing ───
// A subscription turns track changes into a push instead of a wait, so the
// display follows within a second or two rather than up to a poll interval.
// Polling continues underneath at a relaxed rate: the subscription can lapse,
// the speaker can reboot, and events can simply be missed.
static const uint16_t UPNP_EVENT_PORT = 1401;
static char  g_sid[64] = {};
static unsigned long g_subRenewAt = 0;
static unsigned long g_subRetryAt = 0;
static char  g_subIp[40] = {};

static void sonosDropSubscription() {
    if (g_sid[0] && g_subIp[0]) sonosUnsubscribe(g_subIp, g_sid);
    g_sid[0] = 0; g_subIp[0] = 0; g_subRenewAt = 0;
}

// Keep the subscription pointed at whichever speaker we are polling.
static void maintainSubscription(const char* ip) {
    unsigned long now = millis();
    if (!ip || !ip[0]) return;

    if (g_sid[0] && strcmp(g_subIp, ip) != 0) {
        sonosDropSubscription();          // speaker changed
    }

    if (g_sid[0]) {
        if ((long)(now - g_subRenewAt) < 0) return;
        uint32_t secs = 1800;
        if (sonosRenewSubscription(ip, g_sid, &secs)) {
            g_subRenewAt = now + (secs / 2) * 1000UL;
        } else {
            activityLog("Sonos subscription lapsed — resubscribing");
            g_sid[0] = 0;
        }
        return;
    }

    if ((long)(now - g_subRetryAt) < 0) return;
    g_subRetryAt = now + 60000;

    String cb = String("http://") + WiFi.localIP().toString() + ":" +
                String(UPNP_EVENT_PORT) + "/notify";
    uint32_t secs = 1800;
    if (sonosSubscribe(ip, cb.c_str(), g_sid, sizeof(g_sid), &secs)) {
        strlcpy(g_subIp, ip, sizeof(g_subIp));
        g_subRenewAt = now + (secs / 2) * 1000UL;
        activityLogf("Subscribed to Sonos events (%us)", (unsigned)secs);
    }
}

// ─── Sonos re-discovery when IP changes ───
static int g_sonosUnreachableCount = 0;
static const int SONOS_UNREACHABLE_BEFORE_REDISCOVER = 3;

// ─── Physical button for vinyl re-identify ───
static volatile bool g_buttonReIdentify = false;
static void IRAM_ATTR onKeyPress() {
    g_buttonReIdentify = true;
}

// ─── Panel care ───

// Quiet hours: leave the panel alone overnight. E-ink holds its image with no
// power, so this costs nothing to look at and halves the refresh count over the
// life of the frame.
bool inQuietHours() {
    if (g_app.settings.quiet_start_hour == g_app.settings.quiet_end_hour) return false;
    time_t now = time(nullptr);
    if (now < 1600000000) return false;          // clock not set yet
    int hour = (int)(((now / 3600) + g_app.settings.utc_offset_hours) % 24 + 24) % 24;
    int a = g_app.settings.quiet_start_hour, b = g_app.settings.quiet_end_hour;
    return (a < b) ? (hour >= a && hour < b)     // e.g. 01:00-07:00
                   : (hour >= a || hour < b);    // wraps midnight, e.g. 23:00-07:00
}

// Set by an explicit refresh request and consumed by the next repaint. The
// floor exists to stop a playlist skipping through tracks wearing the panel
// out; someone pressing Redraw has asked for exactly one repaint and should
// get it. Before this, Force refresh logged "clearing caches" and was then
// silently swallowed by the floor, which made the button a lie.
static bool g_bypassRefreshFloor = false;
static bool g_restorePending = false;

// Refuse to repaint too soon. Without this, skipping through a playlist
// repaints on every track, and each full refresh is 20-25s of panel wear.
static bool refreshTooSoon() {
    if (g_bypassRefreshFloor) return false;
    unsigned long last = displayLastRefreshMs();
    if (last == 0) return false;
    return (millis() - last) < g_app.settings.min_refresh_ms;
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

// Fill in the pressing details for whatever is playing, once per album.
// Runs after the artwork is on screen so it never delays the display.
static void enrichRelease(const String& artist, const String& album) {
    if (!artist.length() || !album.length()) return;
    String cached;
    if (sdHistoryGetRelease(artist.c_str(), album.c_str(), cached)) {
        g_app.releaseInfo = cached;      // already known, including a known miss
        return;
    }
    ReleaseInfo info;
    if (metadataLookup(artist.c_str(), album.c_str(), info)) {
        g_app.releaseInfo = metadataSummary(info);
        activityLogf("Release: %s", g_app.releaseInfo.c_str());
    } else {
        g_app.releaseInfo = "";
        activityLogf("No release details for %s", album.c_str());
    }
    sdHistorySetRelease(artist.c_str(), album.c_str(), g_app.releaseInfo.c_str());
}
static void serviceRequests();

// Keep whatever is on the panel there, so a test or calibration pattern
// survives long enough to be photographed.  Released by a forced refresh.
static void holdDisplay() {
    g_app.displayHoldUntil = millis() + DISPLAY_HOLD_MS;
    activityLogf("Display held for %lu min — Force Display Refresh to release",
                 (unsigned long)DISPLAY_HOLD_MS / 60000);
}

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
    powerBegin();

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
    upnpEventsBegin(UPNP_EVENT_PORT);

    // ── Ready ──
    // ── Watchdog ──
    // The loop blocks for the whole panel refresh, so the timeout has to clear
    // that comfortably. Without one, a wedged I2C bus or a hung SD write leaves
    // the frame dead until someone unplugs it.
    esp_task_wdt_config_t wdt = {
        .timeout_ms = WATCHDOG_TIMEOUT_S * 1000,
        .idle_core_mask = 0,
        .trigger_panic = true
    };
    if (esp_task_wdt_reconfigure(&wdt) == ESP_OK || esp_task_wdt_init(&wdt) == ESP_OK) {
        esp_task_wdt_add(NULL);
        Serial.printf("[BOOT] Watchdog armed (%ds)\n", WATCHDOG_TIMEOUT_S);
    } else {
        Serial.println("[BOOT] Watchdog init failed");
    }

    g_app.state = STATE_IDLE;
    digitalWrite(LED_GREEN, HIGH);
    Serial.printf("[BOOT] Reset reason: %d\n", (int)esp_reset_reason());
    Serial.println("[BOOT] Ready — entering main loop");

    // Show the IP as well as the mDNS name. Android has no system mDNS
    // resolver, so nowplaying.local simply does not resolve in Chrome there —
    // and a phone is the most likely thing anyone sets this up from. The frame
    // is sitting right in front of them, so it may as well say its address.
    {
        String ready = String("Ready\n\nnowplaying.local\nor\n") +
                       WiFi.localIP().toString();
        displayShowMessage(ready.c_str());
    }
    delay(3000);

    // Ask the loop to put the last cover back, rather than doing it here.
    // Doing it inline was a 20-25s panel refresh inside setup, after the
    // watchdog is armed and before the loop that feeds it exists — the frame
    // reset-looped on the watchdog. The loop already feeds it every pass, so
    // the restore belongs there.
    g_restorePending = true;
}

// ═══════════════════════════════════════════════════════════
void controllerLoop() {
    esp_task_wdt_reset();

    // One-shot: put back whatever was last on the panel. E-ink keeps showing
    // it through a restart, but the firmware has no copy, so the portal has
    // nothing to serve until the next track change.
    if (g_restorePending) {
        g_restorePending = false;
        String last = sdHistoryNewestFile();
        if (!last.length()) {
            activityLog("No history to restore after restart");
        } else if (pipelineProcessFile(last.c_str())) {
            String artist, album;
            if (sdHistoryLookup(last.substring(9).c_str(), artist, album)) {
                g_app.currentArtist = artist;
                g_app.currentAlbum  = album;
                enrichRelease(artist, album);
            }
            activityLog("Restored the last cover after restart");
        } else {
            activityLogf("Could not restore %s after restart", last.c_str());
        }
        esp_task_wdt_reset();
        return;                    // one long job per pass
    }

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

    // Overnight, leave the panel alone entirely.
    if (inQuietHours()) {
        static bool announced = false;
        if (!announced) {
            activityLogf("Quiet hours (%02d:00-%02d:00) — display paused",
                         g_app.settings.quiet_start_hour, g_app.settings.quiet_end_hour);
            announced = true;
        }
        delay(1000);
        return;
    } else {
        static bool wasQuiet = false;
        if (wasQuiet) { activityLog("Quiet hours ended"); wasQuiet = false; }
    }

    // A test or calibration pattern is on screen — leave it alone.
    if (g_app.displayHoldUntil != 0) {
        if ((long)(now - g_app.displayHoldUntil) < 0) {
            delay(100);
            return;
        }
        g_app.displayHoldUntil = 0;
        activityLog("Display hold expired — resuming normal operation");
    }

    // A pushed event means something changed; poll immediately rather than
    // waiting out the interval.
    bool pushed = upnpEventsPoll();
    if (pushed) {
        g_app.lastPollTime = 0;
        g_app.eventCount++;
        activityLog("Sonos event — checking now");
    }

    if (!pushed && now - g_app.lastPollTime < g_app.settings.sonos_poll_ms) {
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

    // Grouped speakers report their own transport, not the group's, so always
    // talk to the coordinator.
    char pollIp[40];
    strlcpy(pollIp, g_app.settings.sonos_ip, sizeof(pollIp));
    if (strlen(g_app.settings.sonos_name) > 0) {
        static unsigned long s_lastCoordCheck = 0;
        static char s_coord[40] = {};
        if (s_coord[0] == 0 || now - s_lastCoordCheck > 30000) {
            s_lastCoordCheck = now;
            sonosResolveCoordinator(g_app.settings.sonos_ip,
                                    g_app.settings.sonos_name, s_coord, sizeof(s_coord));
        }
        if (s_coord[0]) strlcpy(pollIp, s_coord, sizeof(pollIp));
    }
    strlcpy(g_app.pollIp, pollIp, sizeof(g_app.pollIp));
    maintainSubscription(pollIp);

    bool reachable = false;
    bool playing = sonosIsPlaying(pollIp, &reachable);

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
                sonosDropSubscription();
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
            g_app.releaseInfo   = "";
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
        holdDisplay();
        return;
    }

    if (g_req.testDither) {
        g_req.testDither = false;
        activityLog("Dither test pattern requested");
        pipelineShowDitherTest();
        holdDisplay();
        return;
    }

    if (g_req.testCalibration) {
        g_req.testCalibration = false;
        pipelineShowCalibrationCard();
        holdDisplay();
        return;
    }

    if (g_req.showRaw) {
        g_req.showRaw = false;
        if (g_req.rawFrame && g_req.rawFrameLen == (EPD_WIDTH * EPD_HEIGHT) / 2) {
            activityLog("Displaying pushed frame");
            displayShowImage(g_req.rawFrame);
            holdDisplay();
        } else {
            activityLogf("Pushed frame wrong size (%u)", (unsigned)g_req.rawFrameLen);
        }
        return;
    }

    if (g_req.showHistory) {
        g_req.showHistory = false;
        String path = String("/history/") + g_req.showHistoryFile;
        activityLogf("Showing %s on request", g_req.showHistoryFile);
        if (pipelineProcessFile(path.c_str())) {
            holdDisplay();
            // Pick up the artist and album for this entry so the portal shows
            // what it is, and its pressing details, not just a picture.
            String a, al;
            if (sdHistoryLookup(g_req.showHistoryFile, a, al)) {
                g_app.currentArtist = a;
                g_app.currentAlbum  = al;
                g_app.currentTitle  = "";
                enrichRelease(a, al);
            }
        } else {
            activityLogf("Could not display %s", g_req.showHistoryFile);
        }
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
            enrichRelease(artist, album);
        }
        digitalWrite(LED_RED, LOW);
        return;
    }

    if (g_req.forceRefresh) {
        g_req.forceRefresh = false;
        g_app.displayHoldUntil = 0;   // an explicit refresh releases the hold
        g_lastTrackHash = "";
        g_app.lastArtUrl = "";
        g_lastIdleSwap = 0;
        g_app.lastPollTime = 0; // force immediate poll
        resetVinylBackoff();
        g_bypassRefreshFloor = true;
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
    enrichRelease(artist, album);

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

    if (refreshTooSoon()) {
        activityLogf("Skipping repaint — panel refreshed %lus ago (floor %lus)",
                     (millis() - displayLastRefreshMs()) / 1000,
                     (unsigned long)g_app.settings.min_refresh_ms / 1000);
        return;
    }

    g_bypassRefreshFloor = false;   // one repaint per request, not a mode
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
    enrichRelease(track.artist, track.album);
}

static void handlePlaying() {
    SonosTrackInfo track;
    const char* ip = g_app.pollIp[0] ? g_app.pollIp : g_app.settings.sonos_ip;
    if (!sonosGetTrackInfo(ip, track)) {
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
    if (refreshTooSoon()) return;
    g_lastIdleSwap = now;
    g_bypassRefreshFloor = false;   // consumed here too, if the gallery got there first

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
