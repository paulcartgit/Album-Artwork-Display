# Now Playing

An ESP32-S3 powered e-ink display that shows the album art of whatever's currently playing on your Sonos system — including vinyl records identified via Shazam.

## How It Works

1. **Digital streaming** — Polls your Sonos speaker for now-playing metadata and album art URL, processes the image through an advanced dithering pipeline, and renders it on a 6-color e-ink display.
2. **Vinyl records** — When Sonos reports a line-in source, records audio from an onboard microphone, identifies the track via Shazam (RapidAPI), and displays the album art.
3. **Listen mode** — Tap the "Listen" button in the web portal to manually identify whatever's playing in the room.
4. **Idle mode** — When nothing is playing, cycles through a history of previously displayed album covers.

## Hardware

- **Board**: [Waveshare ESP32-S3-PhotoPainter](https://www.waveshare.com/wiki/ESP32-S3-PhotoPainter) (ESP32-S3-WROOM-1-N16R8, 16MB flash, 8MB PSRAM)
- **Display**: 7.3" GDEP073E01 e-ink (Spectra 6), 800×480, driven in portrait mode (480×800)
- **Audio**: ES7210 quad-ADC for microphone input (44.1 kHz, 16-bit stereo)
- **Storage**: SD card (4-bit SDMMC) for settings, WiFi config, and art history
- **Power**: AXP2101 PMIC with battery support

## Getting Started

### Prerequisites

- [PlatformIO](https://platformio.org/install) (VS Code extension or CLI)
- Waveshare ESP32-S3-PhotoPainter board
- FAT32-formatted SD card inserted into the board

### Build & Flash

```bash
cd firmware
pio run -e esp32-s3-photopainter -t upload
```

Or use the convenience script:

```bash
./firmware/flash.sh
```

### First-Time Setup

1. Power on the device — it will create a WiFi network called **NowPlaying-Setup**
2. Connect to it from your phone or laptop
3. A captive portal will appear — select your home WiFi network and enter the password
4. The device reboots. Once connected, it's accessible at **http://nowplaying.local**
5. Open the web portal → **Settings** tab → scan for Sonos speakers and select yours
6. (Optional) Add a [Shazam RapidAPI](https://rapidapi.com/apidojo/api/shazam/) key for vinyl identification
7. Save settings — the display will start showing album art automatically

If the WiFi connection fails (e.g. wrong password), the device falls back to the captive portal automatically. The e-ink display shows "Wi-Fi Failed" so you know to reconnect to `NowPlaying-Setup` and try again.

## Rendering Pipeline

Album art goes through a multi-stage image processing pipeline optimised for the 6-color e-ink panel:

1. **JPEG decode** — Downloaded image decoded to RGB888 in PSRAM via TJpg_Decoder
2. **Background fill** — Auto-detected per image:
   - **Blurred** — Source image scaled to fill, 4-pass box blur (radius 12), then darkened (55%) or washed out (45% toward white)
   - **Patterned** — Palette-derived decorative backing pattern (stripes/waves/geometric bands) generated from dominant image colours, then darkened/washed out
   - **Solid colour** — Saturation-weighted average of edge pixels (vibrant pixels dominate, prevents muddy brown), slight saturation boost
   - Configurable: always solid, always blur, always patterned, or auto (smart edge-variance threshold)
3. **Scaling** — Fit to display with correct aspect ratio, centred with margin for drop shadow
4. **Drop shadow** — Top and bottom gradient bands (20px, quadratic falloff) in full-art mode
5. **Text overlay** — 2× supersampled anti-aliased rendering (FreeSansBold 24pt artist, FreeSans 18pt album), auto-scaling and ellipsis truncation for long names, contrast-adaptive text colour (white on dark, black on light), frosted overlay on blurred backgrounds for legibility
6. **Enhancement** — Unsharp mask sharpening, contrast boost, gamma correction with shadow protection
7. **Dithering** — CIELAB color space, Stucki error diffusion kernel, serpentine scanning, lightness-weighted matching, error cap with dampening for stability
8. **Placeholder fallback** — When artwork can't be decoded (unsupported JPEG format), a text-only display shows artist and album name on a dark background

## Web Portal

Once connected, visit **http://nowplaying.local** (or the device IP):

### Now Playing
Current track info, artwork preview, and activity log. Buttons to force a Sonos check or trigger a manual listen (Shazam identify). Auto-refreshes every 3 seconds.

### Settings
- **Wi-Fi** — Scan for networks, change WiFi credentials (reboots to reconnect)
- **Sonos** — Scan and select a speaker by room name
- **Shazam** — RapidAPI key for vinyl identification
- **Timing** — Sonos poll interval (5–60s), vinyl re-identify interval (1–30 min), no-match cooldown (1–15 min), idle gallery rotation (1–30 min)
- **Display** — Track info overlay toggle, background fill mode (auto/blur/patterned/solid), background style (darken/wash out)

### History
Gallery grid of all saved album covers (up to 100), split into **Pinned** and **History** sections:
- Toggle covers on/off for idle gallery rotation
- **Pin** a cover to keep it permanently (exempt from the 100-entry cap)
- **Delete** unwanted entries

### Debug
Device IP, uptime, force display refresh, test color pattern, download last audio recording.

## Album Art History

Album covers are automatically saved to the SD card as they're displayed. When idle, the device cycles through enabled covers using shuffle-bag randomisation. Pinned covers are never pruned. The oldest unpinned entries are automatically removed when the 100-entry limit is reached.

## State Machine

| State | Description |
|-------|-------------|
| **BOOT** | Hardware init, WiFi connect, setup |
| **IDLE** | Nothing playing — rotates gallery covers |
| **DIGITAL** | Sonos streaming — displays album art from Sonos metadata |
| **VINYL** | Sonos line-in — records audio, identifies via Shazam |
| **SETUP** | Captive portal for WiFi provisioning |
| **ERROR** | Halted (e.g. SD card failure) |

Additional behaviours:
- **Idle debounce** — Requires 2 consecutive idle polls before transitioning from playing to idle (prevents false transitions during track changes)
- **Escalating cooldown** — After Shazam retries are exhausted, cooldown duration escalates progressively, capped at 30 minutes
- **Speaker rediscovery** — After 3 consecutive Sonos failures, re-discovers the speaker by room name via UPnP/SOAP topology API
- **Display queue** — Artwork arriving while the e-ink is still refreshing (~15s) is queued and processed when the panel finishes
- **Physical button** — BTN_KEY triggers immediate re-identification (resets all cooldowns)

## 6-Color Palette

The Spectra 6 e-ink display uses these calibrated pigment colors:

| Index | Color | Calibrated RGB |
|-------|-------|----------------|
| 0 | Black | `#000000` |
| 1 | White | `#FFFFFF` |
| 2 | Green | `#3A6B35` (muted olive) |
| 3 | Blue | `#4A6B8A` (muted steel) |
| 4 | Red | `#8B2500` (deep crimson) |
| 5 | Yellow | `#C8A000` (warm golden) |

## Project Structure

```
firmware/               ESP32-S3 PlatformIO firmware
├── src/
│   ├── main.cpp            Entry point, state machine, Sonos polling
│   ├── config.h            Pin definitions, constants, settings struct
│   ├── display.cpp/h       GxEPD2 6-color e-ink driver (non-blocking refresh)
│   ├── image_pipeline.cpp/h    JPEG → scale → enhance → dither → display
│   ├── dither.cpp/h        CIELAB + Stucki + serpentine dithering
│   ├── web_server.cpp/h    HTTP API + captive portal + settings portal
│   ├── web_portal.h        Embedded HTML/CSS/JS (status, settings, history, debug)
│   ├── captive_portal.h    Embedded HTML/CSS/JS for WiFi setup wizard
│   ├── sonos_client.cpp/h  UPnP/SOAP topology discovery + now-playing queries
│   ├── shazam_client.cpp/h Shazam audio fingerprinting (RapidAPI)
│   ├── audio_capture.cpp/h I2S microphone recording (ES7210)
│   ├── sd_manager.cpp/h    SD card: settings, wifi config, art history
│   ├── wifi_manager.cpp/h  WiFi STA connection + AP mode for setup
│   ├── activity_log.h      Circular activity log for web UI
│   ├── url_utils.h         URL encoding helpers
│   └── xml_utils.h         XML tag extraction (UPnP/SOAP)
├── test/
│   ├── test_native/test_main.cpp   Native unit tests
│   └── mocks/          Arduino/ESP stubs for native tests
└── platformio.ini

simulator/              Python simulator (runs without hardware)
├── vinyl_sim.py        Full simulator with web UI
├── dither_preview.py   Standalone dither preview tool
├── requirements.txt    Python dependencies
└── settings.example.json   Template for credentials
```

## Running Tests

```bash
cd firmware
pio test -e native
```

## License

See [LICENSE.md](LICENSE.md).
