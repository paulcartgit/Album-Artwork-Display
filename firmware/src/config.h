#pragma once
#include <Arduino.h>

// ─── EPD (E-ink) SPI Pins ───
#define EPD_MOSI  11
#define EPD_CLK   10
#define EPD_DC     8
#define EPD_CS     9
#define EPD_RST   12
#define EPD_BUSY  13

// ─── SD Card (SDMMC 4-bit) ───
#define SD_CLK    39
#define SD_CMD    41
#define SD_D0     40
#define SD_D1      1
#define SD_D2      2
#define SD_D3     38

// ─── I2C Bus ───
#define I2C_SDA   47
#define I2C_SCL   48

// ─── I2S Audio ───
#define I2S_MCLK  14
#define I2S_BCLK  15
#define I2S_WS    16
#define I2S_DIN   18   // Data from ES7210 (mic ADC)
#define I2S_DOUT  17   // Data to ES8311 (speaker DAC)

// ─── Audio Amplifier ───
#define PA_ENABLE  7

// ─── I2C Device Addresses ───
#define AXP2101_ADDR  0x34
#define ES8311_ADDR   0x18
#define ES7210_ADDR   0x40

// ─── Misc GPIO ───
#define AXP_IRQ_PIN   21
#define LED_RED       45
#define LED_GREEN     42
#define BTN_BOOT       0
#define BTN_PWR        5
#define BTN_KEY        4

// ─── Display (portrait orientation) ───
#define EPD_WIDTH   480
#define EPD_HEIGHT  800
#define EPD_COLORS    6

// ─── Audio Recording ───
#define AUDIO_SAMPLE_RATE  44100
#define AUDIO_BITS         16
#define AUDIO_CHANNELS     2       // stereo — ES7210 sends MIC1 on L, MIC2 on R
#define AUDIO_RECORD_SECS  12
#define LISTEN_RECORD_SECS 6
#define AUDIO_BUFFER_SIZE  (AUDIO_SAMPLE_RATE * (AUDIO_BITS / 8) * AUDIO_CHANNELS * AUDIO_RECORD_SECS)
#define LISTEN_BUFFER_SIZE (AUDIO_SAMPLE_RATE * (AUDIO_BITS / 8) * AUDIO_CHANNELS * LISTEN_RECORD_SECS)

// ─── Timing defaults ───
#define SONOS_POLL_INTERVAL_MS      10000       // 10s — how often to check Sonos
#define VINYL_RECHECK_INTERVAL_MS   600000      // 10 min — re-identify vinyl (~half LP side)
#define NO_MATCH_COOLDOWN_MS        300000      // 5 min — pause after 3 failed identifications
#define VINYL_MAX_COOLDOWN_MS       1800000     // 30 min — max cooldown after repeated escalation
#define VINYL_RETRY_DELAY_MS        15000       // 15s between no-match retries
#define VINYL_MAX_RETRIES           3           // retries before entering cooldown (first cycle)
#define IDLE_GALLERY_INTERVAL_MS    300000      // 5 min — rotate gallery images when idle

// ─── Panel care ───
// Spectra 6 panels have a finite refresh life and each full update takes 20-25s.
// Skipping quickly through a playlist would otherwise repaint on every track.
#define MIN_REFRESH_INTERVAL_MS     45000       // 45s floor between refreshes

// The main loop blocks for the whole panel refresh, so the watchdog has to
// tolerate that plus a slow artwork download.
#define WATCHDOG_TIMEOUT_S          90
#define DISPLAY_HOLD_MS             1800000     // 30 min — keep a test/calibration pattern on screen

// ─── 6-Color Palette (calibrated to GDEP073E01 actual pigment appearance) ───
// These RGB values represent what the e-ink pigments LOOK LIKE, not ideal RGB.
// The dither matches against these values and diffuses error against them, so
// their accuracy directly determines output quality — see DITHERING.md.
//
// Measured from a RAW capture of the calibration card (simulator/
// calibrate_from_photo.py --corners), anchored on the card's own black and
// white chips. Two captures in different lighting agreed with each other far
// more closely than either agreed with the previous hand-tuned values, which
// understated all three saturated pigments.
//
// Adopting the measurement makes the panel BOLDER, not tamer. That is
// counter-intuitive — a more saturated model should need less ink to hit a
// target — but measured across a spread of real covers the chromatic share of
// placed pigment rose on 8 of 10 (mean 32.9% -> 34.6%, Fitz and The Tantrums
// 43.1% -> 51.3%). Re-measure with simulator/palette_ab.py before assuming
// otherwise.
//
// Cross-checked since against two independently published measurements of this
// panel (epdoptimize, and quark-zju's converter gist). Anchoring each on its
// own black and white so exposure drops out, all three agree the chromatics
// sit at 95-100% saturation; the old hand-tuned values were the outlier at
// 78-83%. Ours lands closest to quark-zju's.
//
// Green's blue channel is the one place we disagreed with everyone: the
// measurement gave 0x45, while the other two read 0x00 and 0x1F. Pulled to
// 0x2E — still the greener of the published values, but no longer teal.
struct PaletteColor {
    uint8_t r, g, b;
    uint8_t index;
};

static const PaletteColor PALETTE[EPD_COLORS] = {
    {0x0D, 0x0A, 0x10, 0}, // Black  (near-black charcoal)
    {0xE0, 0xE0, 0xD9, 1}, // White  (light grey, slight warm tint)
    {0x1F, 0x6C, 0x2E, 2}, // Green  (deep leaf-green)
    {0x00, 0x5D, 0xAB, 3}, // Blue   (strong mid-blue)
    {0xBD, 0x0F, 0x05, 4}, // Red    (vivid scarlet)
    {0xFF, 0xDA, 0x1B, 5}, // Yellow (bright golden)
};

// ─── Render profiles ───
// The rendering pipeline is heavily parameterised.  Rather than bake the
// constants in, expose three named presets the user can pick in the portal.
// PROFILE_NATURAL reproduces the historical (pre-profile) behaviour exactly.
struct RenderProfile {
    const char* name;
    // Pre-dither enhancement (image_pipeline.cpp / enhanceForEink)
    float sharpen;        // unsharp-mask strength
    float contrast;       // contrast multiplier around mid-grey
    float gamma;          // < 1 lifts midtones
    // Dithering (dither.cpp)
    float chromaPenaltyK;      // strength of the achromatic penalty
    float chromaPenaltyOnset;  // chroma below which no penalty applies
    float edgeAttenuation;     // 0 = diffuse across edges, 1 = fully blocked
};

enum RenderProfileId {
    PROFILE_PUNCHY  = 0,
    PROFILE_NATURAL = 1,
    PROFILE_SOFT    = 2,
    PROFILE_COUNT   = 3
};

static const RenderProfile RENDER_PROFILES[PROFILE_COUNT] = {
    //  name        sharpen contrast gamma  chromaK onset  edgeAtten
    { "Punchy",     0.65f,  1.35f,   0.85f, 7.0f,   10.0f, 0.85f },
    { "Natural",    0.40f,  1.20f,   0.90f, 5.0f,   12.0f, 0.85f },
    { "Soft",       0.20f,  1.08f,   0.95f, 3.5f,   16.0f, 0.70f },
};

// ─── Artwork fill ───
// The panel is 480x800 but album art is square, so fitting it to the width
// covers only 60% of the screen. Cover-cropping fills it but discards 40% of
// the sleeve horizontally, which usually cuts straight through the type.
enum FillMode {
    FILL_FIT      = 0,  // square centred, blurred background (original behaviour)
    FILL_ADAPTIVE = 1,  // enlarge as far as the sleeve's own detail allows
    FILL_BLEED    = 2,  // never crop; extend the artwork to the edges
    FILL_COVER    = 3   // always fill completely, cropping whatever it takes
};

// Zoom limits and the crop-severity threshold live in fill_policy.h,
// which is unit-tested.

inline const RenderProfile& renderProfile(uint8_t id) {
    return RENDER_PROFILES[(id < PROFILE_COUNT) ? id : PROFILE_NATURAL];
}

// ─── App State ───
enum AppState {
    STATE_BOOT,
    STATE_IDLE,
    STATE_DIGITAL,
    STATE_VINYL,
    STATE_ERROR,
    STATE_SETUP   // captive-portal WiFi provisioning mode
};

// ─── Settings (stored in /settings.json on SD) ───
struct Settings {
    char sonos_ip[64];    // cached resolved IP — updated automatically on re-discovery
    char sonos_name[64];  // speaker room name — used to find it by name rather than IP
    char shazam_api_key[128];
    // Timing
    uint32_t sonos_poll_ms;
    uint32_t vinyl_recheck_ms;
    uint32_t no_match_cooldown_ms;
    uint32_t idle_gallery_ms;
    // Display
    bool show_track_info;
    uint8_t bg_mode;         // 0 = always solid, 1 = always blur, 2 = auto (default)
    uint8_t bg_style;        // 0 = darken background, 1 = wash out (lighten)
    uint8_t render_profile;  // RenderProfileId — 1 (Natural) by default
    uint8_t fill_mode;
    // Look for a better-rendering scan of the same sleeve on the Cover Art
    // Archive. Off by default: it costs several seconds and a handful of
    // downloads per new album, and the gate that keeps it honest (cover_match.h)
    // is worth understanding before turning it on.
    bool cover_variants;       // FillMode — how artwork fills the portrait panel
    uint32_t min_refresh_ms; // floor between panel refreshes (protects the panel)
    uint8_t quiet_start_hour;// local hour to stop refreshing (0-23)
    uint8_t quiet_end_hour;  // local hour to resume (equal values = never quiet)
    int8_t  utc_offset_hours;// for quiet hours; NTP gives us UTC
    // Web portal access control (empty password = no auth)
    char portal_password[64];
};

// ─── WiFi Config (stored in /config.json on SD) ───
struct WifiConfig {
    char ssid[64];
    char password[64];
};
