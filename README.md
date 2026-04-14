# Album Artwork Display

An ESP32-S3 powered e-ink display that shows the album art of whatever's currently playing on your Sonos system — including vinyl records identified via Shazam.

## How It Works

1. **Digital streaming** — Polls your Sonos speaker for now-playing metadata and album art URL, processes the image through an advanced dithering pipeline, and renders it on a 6-color e-ink display.
2. **Vinyl records** — When Sonos reports a line-in source, records audio from an onboard microphone, identifies the track via Shazam (RapidAPI), and displays the album art.
3. **Listen mode** — Tap the "Listen" button in the web portal to manually identify whatever's playing in the room.
4. **Idle mode** — When nothing is playing, cycles through a history of previously displayed album covers.

## Hardware

- **Board**: [Waveshare ESP32-S3-PhotoPainter](https://www.waveshare.com/wiki/ESP32-S3-PhotoPainter) (ESP32-S3-WROOM-1-N16R8)
- **Display**: 7.3" GDEP073E01 e-ink (Spectra 6), 800×480 landscape
- **Audio**: ES7210 quad-ADC for microphone input
- **Storage**: SD card (4-bit SDMMC) for settings, WiFi config, and art history
- **Power**: AXP2101 PMIC with battery support

## Rendering Pipeline

Album art goes through a sophisticated image processing pipeline optimised for the 6-color e-ink panel:

1. **JPEG decode** — Downloaded image decoded to RGB888 in PSRAM
2. **Background fill** — Auto-detected per image:
   - **Blurred pillarbox** — for photographic covers (1.3× zoom, box blur, 70% dim)
   - **Solid colour** — for clean/minimalist covers (saturation-weighted edge average)
   - Configurable: always solid, always blur, or auto (smart edge-variance detection)
3. **Scaling** — Fit to display with correct aspect ratio
4. **Enhancement** — Unsharp mask sharpening, contrast boost, gamma correction with shadow protection
5. **Dithering** — CIELAB color space conversion, Stucki error diffusion kernel, serpentine scanning, edge-aware error attenuation, shadow chroma suppression, chroma-gated error boost for saturated colours

## Album Art History

Album covers are automatically saved to the SD card as they're displayed (up to 100). When idle, the device cycles through these saved covers. In the web portal's **History** tab you can:

- See thumbnails of all previously displayed covers
- Toggle individual covers on/off (only enabled covers appear during idle rotation)
- Oldest entries are automatically pruned when the 100-entry limit is reached

## Project Structure

```
firmware/               ESP32-S3 PlatformIO firmware
├── src/
│   ├── main.cpp            Entry point, state machine, Sonos polling
│   ├── config.h            Pin definitions, constants, settings struct
│   ├── display.cpp/h       GxEPD2 6-color e-ink driver (non-blocking refresh)
│   ├── image_pipeline.cpp/h    JPEG → scale → enhance → dither → display
│   ├── dither.cpp/h        CIELAB + Stucki + serpentine + edge-aware dithering
│   ├── web_server.cpp/h    HTTP API + settings portal
│   ├── web_portal.h        Embedded HTML/CSS/JS (status, settings, history, debug)
│   ├── sonos_client.cpp/h  UPnP/SOAP now-playing queries
│   ├── shazam_client.cpp/h Shazam audio fingerprinting (RapidAPI)
│   ├── audio_capture.cpp/h I2S microphone recording (ES7210)
│   ├── sd_manager.cpp/h    SD card: settings, wifi config, art history
│   ├── wifi_manager.cpp/h  WiFi connection from SD config
│   ├── activity_log.h      Circular activity log for web UI
│   ├── url_utils.h         URL encoding helpers
│   └── xml_utils.h         XML tag extraction (UPnP)
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

## Firmware

### Prerequisites

- [PlatformIO](https://platformio.org/install) (VS Code extension or CLI)
- Waveshare ESP32-S3-PhotoPainter board

### Build & Flash

```bash
cd firmware
pio run -e esp32-s3-photopainter -t upload
```

### SD Card Setup

Create a JSON file on a FAT32 SD card:

**config.json** (WiFi — required):
```json
{
    "ssid": "YourNetwork",
    "password": "YourPassword"
}
```

Settings (Sonos IP, Shazam API key, timing, display options) are configured via the web portal and saved to `/settings.json` automatically.

### Running Tests

```bash
cd firmware
pio test -e native
```

## Web Portal

Once running, visit `http://vinyl.local` (or the device IP) to:

- **Now Playing** — View current track, artwork, and activity log. Buttons to force a Sonos check or listen to identify what's playing.
- **Settings** — Configure Sonos IP, Shazam API key, timing intervals, display options (track info overlay, background fill mode).
- **History** — Browse thumbnails of all saved album covers. Toggle covers on/off for idle rotation.
- **Debug** — Device info, force display refresh, test color pattern, download last audio recording.

## 6-Color Palette

The Spectra 6 e-ink display supports these physical pigment colors:

| Index | Color | RGB |
|-------|-------|-----|
| 0 | Black | `#000000` |
| 1 | White | `#FFFFFF` |
| 2 | Green | `#00A000` |
| 3 | Blue | `#0000FF` |
| 4 | Red | `#FF0000` |
| 5 | Yellow | `#FFFF00` |

The dithering pipeline operates in CIELAB color space with a Stucki error diffusion kernel for optimal color reproduction on this limited palette.
