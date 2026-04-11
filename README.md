# Album Artwork Display

An ESP32-S3 powered e-ink display that shows the album art of whatever's currently playing on your Sonos system — including vinyl records played through a Sonos line-in.

## How It Works

1. **Digital streaming** — Polls your Sonos speaker for now-playing metadata, downloads the album art, dithers it to a 7-color palette, and pushes it to the e-ink display.
2. **Vinyl records** — When Sonos reports a line-in source, records audio from an onboard microphone, fingerprints it via ACRCloud, looks up the album art on Spotify, and displays it.
3. **Idle mode** — When nothing is playing, cycles through images from a user-uploaded gallery.

## Hardware

- **Board**: [Waveshare ESP32-S3-PhotoPainter](https://www.waveshare.com/wiki/ESP32-S3-PhotoPainter) (ESP32-S3-WROOM-1-N16R8)
- **Display**: 7.3" ACeP 7-color e-ink (Spectra 6), 480×800 portrait
- **Audio**: ES7210 quad-ADC for microphone input, ES8311 DAC
- **Storage**: SD card (4-bit SDMMC) for settings, WiFi config, and gallery images
- **Power**: AXP2101 PMIC with battery support

## Display Layout (Portrait 480×800)

```
┌──────────────────┐
│                  │
│    480×480       │
│    album art     │
│                  │
├──────────────────┤
│  Song Title      │
│  Artist          │  320px info panel
│  Album           │  (bg color from art)
│                  │
└──────────────────┘
```

When track info is disabled, the album art is centered vertically on the full display.

## Project Structure

```
firmware/               ESP32-S3 PlatformIO firmware
├── src/
│   ├── main.cpp        Entry point, Sonos polling loop
│   ├── config.h        Pin definitions, display/audio constants, palette
│   ├── display.cpp/h   GxEPD2 7-color e-ink driver
│   ├── image_pipeline.cpp/h   JPEG decode → scale → dither → display
│   ├── dither.cpp/h    Floyd-Steinberg error diffusion (7-color)
│   ├── web_server.cpp/h    HTTP API + settings portal
│   ├── web_portal.h    Embedded HTML (status, settings, gallery)
│   ├── sonos_client.cpp/h   UPnP/SOAP now-playing queries
│   ├── spotify_client.cpp/h  Spotify album art lookup (OAuth)
│   ├── acrcloud_client.cpp/h  Audio fingerprinting API
│   ├── audio_capture.cpp/h   I2S microphone recording
│   ├── sd_manager.cpp/h   SD card settings & gallery I/O
│   ├── wifi_manager.cpp/h   WiFi connection from SD config
│   ├── google_photos.cpp/h   Optional Google Photos fallback
│   ├── url_utils.h     URL encoding helpers
│   └── xml_utils.h     XML tag extraction (UPnP)
├── test/
│   ├── test_native/test_main.cpp   19 native unit tests
│   └── mocks/          Arduino/ESP stubs for native tests
└── platformio.ini

simulator/              Python simulator (runs without hardware)
├── vinyl_sim.py        Full simulator: Sonos, Spotify, ACRCloud, dithering, web UI
├── dither_preview.py   Standalone dither preview tool
├── requirements.txt    Python dependencies
└── settings.example.json   Template for API credentials
```

## Simulator

The Python simulator replicates the full firmware pipeline on your dev machine — no hardware needed.

### Setup

```bash
cd simulator
pip install -r requirements.txt
cp settings.example.json settings.json
# Edit settings.json with your credentials
```

### Required Credentials

| Setting | Source |
|---------|--------|
| `sonos_ip` | Your Sonos speaker's IP address |
| `spotify_client_id` / `secret` | [Spotify Developer Dashboard](https://developer.spotify.com/dashboard) |
| `acrcloud_host` / `key` / `secret` | [ACRCloud Console](https://console.acrcloud.com/) (for vinyl identification) |

### Running

```bash
python3 vinyl_sim.py
```

Opens a web UI at `http://localhost:5555` with:
- **Status tab** — current track, display preview (480×800 portrait)
- **Settings tab** — configure Sonos IP, API credentials, toggle track info & dithering
- **Gallery tab** — upload fallback images for idle mode

### Settings

| Setting | Default | Description |
|---------|---------|-------------|
| `poll_interval_ms` | 45000 | Sonos polling interval (ms) |
| `show_track_info` | true | Show song/artist/album panel below artwork |
| `use_dithering` | true | Apply Floyd-Steinberg dithering to 7-color palette |

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

Create two JSON files on a FAT32 SD card:

**config.json** (WiFi):
```json
{
    "ssid": "YourNetwork",
    "password": "YourPassword"
}
```

**settings.json** (API credentials):
```json
{
    "sonos_ip": "192.168.1.100",
    "spotify_client_id": "...",
    "spotify_client_secret": "...",
    "acrcloud_host": "identify-eu-west-1.acrcloud.com",
    "acrcloud_key": "...",
    "acrcloud_secret": "...",
    "poll_interval_ms": 45000,
    "show_track_info": true,
    "use_dithering": true
}
```

### Running Tests

```bash
cd firmware
pio test -e native
```

19 native unit tests covering dithering, XML parsing, and URL encoding.

## Web Portal

Once running (firmware or simulator), the built-in web portal lets you:

- View current playback status and display preview
- Edit all settings (Sonos IP, API keys, polling interval, display options)
- Upload and manage gallery images for idle-mode display

## 7-Color Palette

The Spectra 6 e-ink display supports these physical pigment colors:

| Index | Color | RGB |
|-------|-------|-----|
| 0 | Black | `#000000` |
| 1 | White | `#FFFFFF` |
| 2 | Green | `#608050` |
| 3 | Blue | `#5080B8` |
| 4 | Red | `#A02020` |
| 5 | Yellow | `#F0E050` |
| 6 | Orange | `#E08030` |

Floyd-Steinberg error diffusion dithering maps full-color album art to this palette. The algorithm is identical in both firmware (C++) and simulator (Python/NumPy).
