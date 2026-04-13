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
#define IDLE_GALLERY_INTERVAL_MS    300000      // 5 min — rotate gallery images when idle

// ─── 6-Color Palette (calibrated to GDEP073E01 actual pigment appearance) ───
// These RGB values represent what the e-ink pigments LOOK LIKE, not ideal RGB.
// Accurate values are critical for Floyd-Steinberg dithering quality.
struct PaletteColor {
    uint8_t r, g, b;
    uint8_t index;
};

static const PaletteColor PALETTE[EPD_COLORS] = {
    {0x00, 0x00, 0x00, 0}, // Black
    {0xFF, 0xFF, 0xFF, 1}, // White
    {0x67, 0xA0, 0x62, 2}, // Green  (muted olive-green)
    {0x3A, 0x6E, 0xB5, 3}, // Blue   (muted steel-blue)
    {0xB0, 0x26, 0x28, 4}, // Red    (deep crimson)
    {0xE8, 0xD0, 0x52, 5}, // Yellow (warm golden)
};

// ─── App State ───
enum AppState {
    STATE_BOOT,
    STATE_IDLE,
    STATE_DIGITAL,
    STATE_VINYL,
    STATE_ERROR
};

// ─── Settings (stored in /settings.json on SD) ───
struct Settings {
    char sonos_ip[64];
    char shazam_api_key[128];
    // Timing
    uint32_t sonos_poll_ms;
    uint32_t vinyl_recheck_ms;
    uint32_t no_match_cooldown_ms;
    uint32_t idle_gallery_ms;
    // Display
    bool show_track_info;
    bool use_dithering;
};

// ─── WiFi Config (stored in /config.json on SD) ───
struct WifiConfig {
    char ssid[64];
    char password[64];
};
