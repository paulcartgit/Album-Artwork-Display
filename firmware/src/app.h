#pragma once
#include <Arduino.h>
#include "config.h"
#include "sonos_client.h"

// ═══════════════════════════════════════════════════════════
// Shared application state
//
// Owned and mutated by controller.cpp (the main-loop task).  The async web
// server task reads most of it and raises *requests* rather than acting
// directly, because ESPAsyncWebServer callbacks run on the AsyncTCP task,
// which has a small stack and must never block on network or flash I/O.
// ═══════════════════════════════════════════════════════════

static const int SONOS_SCAN_MAX = 16;

enum ScanState : uint8_t {
    SCAN_IDLE = 0,
    SCAN_REQUESTED,
    SCAN_RUNNING,
    SCAN_DONE
};

struct AppContext {
    Settings settings;
    AppState state;

    // Now playing
    String currentArtist;
    String currentTitle;
    String currentAlbum;
    String lastArtUrl;

    // Sonos poll timing
    unsigned long lastPollTime;

    // While set, the state machine leaves the panel alone.  Test and
    // calibration patterns are useless if the idle gallery paints over them a
    // second later — which is exactly what used to happen.
    unsigned long displayHoldUntil;

    // Vinyl identification back-off
    unsigned long lastNoMatchTime;
    unsigned long lastVinylMatchTime;
    int vinylNoMatchCount;
    int vinylCooldownLevel;

    // Last recording (mono), kept for the debug download endpoint.  Allocated
    // once and never freed — see stashDebugAudio() in identify.cpp.
    uint8_t* lastAudio;
    size_t   lastAudioLen;
    uint32_t lastAudioChannels;
    uint32_t lastAudioSampleRate;

    // Sonos discovery, run on the main loop on behalf of the web server
    SonosDevice  scanResults[SONOS_SCAN_MAX];
    volatile int scanCount;
    volatile ScanState scanState;
};

extern AppContext g_app;

// Work requested by the web server, actioned by the controller loop.
struct AppRequests {
    volatile bool forceRefresh;
    volatile bool testColors;
    volatile bool testDither;
    volatile bool testCalibration;

    // Show one specific history entry, for A/B testing render changes against
    // a fixed set of covers rather than whatever happens to be playing.
    volatile bool showHistory;
    char showHistoryFile[24];

    // A pre-dithered 4bpp frame pushed straight from a host, bypassing the
    // pipeline. Lets test composites (tiled artwork, reference patches) be
    // built off-device, and lets the simulator's own output be put on the panel
    // to check the port against the real thing.
    volatile bool showRaw;
    uint8_t* rawFrame;
    volatile size_t rawFrameLen;
    volatile bool forceListen;
    volatile bool reboot;
};

extern AppRequests g_req;
