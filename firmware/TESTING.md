# Vinyl Now-Playing Display — Setup & Testing Guide

## Prerequisites

### Software
- **Python 3.8+** — macOS ships with it, or `brew install python3`
- **PlatformIO Core (CLI)** — install once:
  ```bash
  pip3 install platformio
  ```
- A code editor (VS Code recommended — install the PlatformIO IDE extension for a nicer experience)

### Hardware (when ready to flash)
- Waveshare ESP32-S3-PhotoPainter board
- USB-C cable (data-capable, not charge-only)
- MicroSD card (FAT32 formatted, any size)

### Accounts (for full functionality)
| Service | Sign up | What you need |
|---------|---------|---------------|
| **ACRCloud** | [acrcloud.com](https://www.acrcloud.com/) | Create a "Music Recognition" project → Host, Access Key, Access Secret |
| **Spotify** | [developer.spotify.com/dashboard](https://developer.spotify.com/dashboard) | Create an app → Client ID, Client Secret |
| **Google Photos Bridge** | Optional — requires deploying a Google Apps Script | The deployed web app URL |

---

## Part 1 — Test Locally Without the Device

The dithering algorithm, Sonos XML parsing, and URL encoding logic are all tested natively on your Mac — no board needed.

### Run the tests

```bash
cd firmware
python3 -m platformio test -e native -v
```

You should see:

```
19 Tests 0 Failures 0 Ignored
OK
```

### What's tested

| Module | Tests | What it verifies |
|--------|-------|------------------|
| **Dithering** | 5 tests | Solid black/white/red map to correct palette index, pixel packing (high/low nibble), all output indices are valid (0–6) |
| **XML parsing** | 7 tests | Tag extraction from Sonos SOAP responses, DIDL-Lite entity decoding, line-in URI detection, missing/empty tag handling |
| **URL encoding** | 6 tests | Spaces → `%20`, colons → `%3A`, safe chars pass through, UTF-8 bytes encoded, empty string |

### Build the ESP32 firmware (without flashing)

To verify the full firmware compiles:

```bash
python3 -m platformio run -e esp32-s3-photopainter
```

This downloads all dependencies and cross-compiles for ESP32-S3. You'll see `SUCCESS` and memory usage.

---

## Part 2 — Local Simulator (No Hardware Required)

A full Python simulator connects to your real Sonos, fetches real album art, applies the same Floyd-Steinberg dithering, and renders an 800×480 preview of what the e-ink display would show. It also serves the actual web portal UI with live data.

### Install

```bash
cd simulator
pip3 install -r requirements.txt
```

### Run the full simulator

```bash
# Interactive — prompts for Sonos IP on first run
python3 vinyl_sim.py

# Or specify Sonos IP directly
python3 vinyl_sim.py --sonos 192.168.1.42
```

This starts a local web server on port 5555 (configurable with `--port`). Open these in your browser:

| URL | What it shows |
|-----|---------------|
| `http://localhost:5555/` | The device's web portal — same HTML/CSS/JS as the real firmware |
| `http://localhost:5555/preview` | 800×480 e-ink display preview (dithered 7-color) |
| `http://localhost:5555/compare` | Side-by-side: original image vs dithered output |

### What you can do

- **Status tab** — shows current Sonos state, what's playing, uptime
- **Settings tab** — configure Sonos IP, Spotify credentials, ACRCloud credentials, poll interval. Settings persist in `simulator/settings.json`
- **Gallery tab** — upload/delete images (stored in `simulator/gallery/`)
- **Preview page** — auto-updates when a new track is detected. Click "Re-poll Sonos" to force an immediate poll
- **Upload any image** via `POST /api/dither_upload` to see how it would look on the e-ink display

### How it works

The simulator mirrors the firmware's logic:
1. Polls Sonos via the same SOAP/UPnP envelopes as `sonos_client.cpp`
2. If the Sonos art URL is missing and Spotify credentials are configured, searches Spotify (same Client Credentials OAuth flow as `spotify_client.cpp`)
3. Downloads the album art, scales/crops to 800×480 (same algorithm as `image_pipeline.cpp`)
4. Applies Floyd-Steinberg dithering with the same 7-color palette and error diffusion (same as `dither.cpp`)
5. Serves the web portal HTML extracted directly from `firmware/src/web_portal.h`

### Standalone dithering preview

Test any image through the rendering pipeline without running the full simulator:

```bash
# Open a preview window
python3 dither_preview.py photo.jpg

# Side-by-side comparison
python3 dither_preview.py photo.jpg --compare

# With color usage statistics
python3 dither_preview.py photo.jpg --compare --stats

# Save to file (no window)
python3 dither_preview.py photo.jpg -o dithered.png --no-show

# Test a URL directly
python3 dither_preview.py "https://i.scdn.co/image/ab67616d0000b273..."
```

The `--stats` flag prints a breakdown of how many pixels use each palette colour — useful for evaluating whether an image works well with the 7-colour palette.

### Run without Sonos

If you don't have a Sonos on the network yet, you can still preview the web portal and test dithering:

```bash
# Web portal only — no polling
python3 vinyl_sim.py --no-poll

# Preview a specific image through the full pipeline
python3 vinyl_sim.py --image path/to/album_cover.jpg

# Preview a remote image URL
python3 vinyl_sim.py --url "https://i.scdn.co/image/ab67616d0000b273..."
```

### Simulator files

```
simulator/
├── vinyl_sim.py          # Full simulator (Sonos, Spotify, dithering, web portal)
├── dither_preview.py     # Standalone dithering preview tool
├── requirements.txt      # Python dependencies
├── settings.json         # Saved settings (created on first run)
├── gallery/              # Uploaded gallery images
└── preview.png           # Last rendered e-ink preview
```

---

## Part 3 — Prepare the SD Card

Format a MicroSD card as **FAT32**. Create this file in the root:

### `/config.json` — WiFi credentials

```json
{
  "ssid": "YourWiFiName",
  "password": "YourWiFiPassword"
}
```

### `/gallery/` — Fallback images (optional)

Create a `gallery` folder and drop in a few `.jpg` files (any resolution — they'll be scaled to 800×480 and dithered to 7 colours). These show when nothing is playing.

### What the device will create

After first boot and web portal configuration, you'll also find:

- `/settings.json` — Sonos IP, API keys, poll interval (written by the web portal)

---

## Part 4 — Flash the Device

### 1. Connect the board

Plug the ESP32-S3-PhotoPainter into your Mac via USB-C.

**If the port isn't detected:** Hold the **BOOT** button on the board while plugging in, then release after 1 second. This forces the ESP32-S3 into download mode.

### 2. Flash

```bash
cd firmware
python3 -m platformio run -e esp32-s3-photopainter --target upload
```

PlatformIO auto-detects the serial port. If it can't find it, specify manually:

```bash
python3 -m platformio run -e esp32-s3-photopainter --target upload --upload-port /dev/cu.usbmodem*
```

### 3. Open serial monitor

```bash
python3 -m platformio device monitor
```

Baud rate is 115200 (configured in `platformio.ini`). You should see boot output like:

```
=== Vinyl Now-Playing Display ===
Free heap: 275432 | PSRAM: 8388608
[SD] Card size: 14832MB
[WiFi] Connecting to YourWiFiName.......
[WiFi] Connected — IP: 192.168.1.42
[WiFi] mDNS: vinyl.local
[Display] Initialized 7.3" 7-color ACeP
[Web] Server started on port 80
[BOOT] Ready — entering main loop
```

**If you see errors:**

| Serial output | Fix |
|---------------|-----|
| `[SD] Mount failed` | Check SD card is FAT32, fully inserted |
| `No WiFi config` | Check `/config.json` exists on SD root, JSON is valid |
| `WiFi Failed` | Check SSID/password, ensure 2.4GHz network (ESP32 doesn't support 5GHz) |
| `[Display] Initialized` doesn't appear | SPI issue — check board revision matches pin config |

---

## Part 5 — Configure via Web Portal

### 1. Open the portal

On your phone or laptop (same WiFi network), go to:

```
http://vinyl.local
```

If mDNS isn't working on your network, use the IP address shown in the serial output (e.g. `http://192.168.1.42`).

### 2. Enter settings

Go to the **Settings** tab and fill in:

| Field | Value |
|-------|-------|
| **Sonos speaker IP** | The local IP of your Sonos speaker (find it in the Sonos app → Settings → About My System) |
| **ACRCloud Host** | e.g. `identify-eu-west-1.acrcloud.com` |
| **ACRCloud Access Key** | From your ACRCloud project dashboard |
| **ACRCloud Access Secret** | From your ACRCloud project dashboard |
| **Spotify Client ID** | From your Spotify developer app |
| **Spotify Client Secret** | From your Spotify developer app |
| **Google Photos Bridge URL** | (Optional) Your deployed GAS web app URL |
| **Poll interval** | `45000` (45 seconds, default) |

Hit **Save Settings**. Settings are stored on the SD card and survive reboots.

### 3. Upload gallery images

Go to the **Gallery** tab. Tap the upload area and pick JPEGs from your phone's camera roll. These are used as fallback/idle images.

---

## Part 6 — Verify It Works (Step by Step)

Work through these checks in order. Each one builds on the previous.

### Check 1 — Board boots clean
Serial shows `[BOOT] Ready` with no errors.

### Check 2 — Web portal loads
`http://vinyl.local` shows the status page on your phone.

### Check 3 — Settings save
Enter and save settings → serial shows no errors → refresh Settings page and values persist.

### Check 4 — Gallery upload
Upload a JPEG via the Gallery tab → serial shows `[Web] Upload done` → file appears in gallery list.

### Check 5 — Idle display
Wait up to 5 minutes (or reboot with a gallery image on SD). The e-ink should show a dithered version of a gallery image.

### Check 6 — Digital playback
Play a track on Sonos (Spotify, Apple Music, etc.). Within ~45 seconds:

```
[Sonos] Queen — Bohemian Rhapsody
[Pipeline] JPEG 640x640 (87234 bytes)
[Dither] Done — 800x480 → 192000 bytes packed
[Display] Refresh complete
```

The e-ink updates with the album art.

### Check 7 — Vinyl identification (requires ACRCloud)
Play a vinyl record through Sonos Line-In. Within ~45 seconds:

```
[Main] Line-In detected → VINYL
[Audio] Recording 10 seconds...
[Audio] Recorded 320000 bytes
[ACRCloud] Found: Pink Floyd — Comfortably Numb (The Wall)
[Spotify] Art: https://i.scdn.co/image/ab67616d0000b273...
[Pipeline] JPEG 640x640 (...)
[Display] Refresh complete
```

---

## Quick Reference

### Common commands

```bash
# ── Simulator (no hardware) ──

# Install simulator deps (one-time)
cd simulator && pip3 install -r requirements.txt

# Full simulator with live Sonos
python3 vinyl_sim.py --sonos 192.168.1.42

# Preview a local image through the dithering pipeline
python3 dither_preview.py photo.jpg --compare --stats

# Web portal only (no Sonos)
python3 vinyl_sim.py --no-poll

# ── Firmware ──

# Build only (no flash)
cd firmware
python3 -m platformio run -e esp32-s3-photopainter

# Build + flash
python3 -m platformio run -e esp32-s3-photopainter --target upload

# Serial monitor (Ctrl+C to exit)
python3 -m platformio device monitor

# Run native tests (no hardware)
python3 -m platformio test -e native -v

# Clean build artifacts
python3 -m platformio run --target clean
```

### Project structure

```
Album Art/
├── Specs/                          # Design specs & project overview
├── simulator/                      # Local simulator (no hardware needed)
│   ├── vinyl_sim.py                # Full simulator (Sonos + Spotify + dithering + web portal)
│   ├── dither_preview.py           # Standalone dithering preview tool
│   ├── requirements.txt            # Python deps (pip install -r requirements.txt)
│   ├── settings.json               # Saved settings (auto-created)
│   ├── gallery/                    # Uploaded gallery images
│   └── preview.png                 # Last rendered e-ink preview
├── firmware/
│   ├── platformio.ini              # Build config (ESP32 + native test envs)
│   ├── TESTING.md                  # This file
│   ├── src/
│   │   ├── main.cpp                # Entry point, state machine
│   │   ├── config.h                # Pin definitions, palette, structs
│   │   ├── sd_manager.cpp/.h       # SD card (config, settings, gallery)
│   │   ├── wifi_manager.cpp/.h     # WiFi + mDNS
│   │   ├── display.cpp/.h          # GxEPD2 7-color e-ink wrapper
│   │   ├── dither.cpp/.h           # Floyd-Steinberg → 7-color palette
│   │   ├── image_pipeline.cpp/.h   # JPEG download → decode → scale → dither → display
│   │   ├── sonos_client.cpp/.h     # Sonos SOAP/UPnP polling
│   │   ├── audio_capture.cpp/.h    # I2S mic recording (ES7210)
│   │   ├── acrcloud_client.cpp/.h  # Audio fingerprinting API
│   │   ├── spotify_client.cpp/.h   # Album art search + OAuth
│   │   ├── google_photos.cpp/.h    # GAS bridge for idle photos
│   │   ├── web_server.cpp/.h       # REST API routes
│   │   ├── web_portal.h            # Embedded HTML/CSS/JS (PROGMEM)
│   │   ├── xml_utils.h             # XML tag parsing (tested natively)
│   │   └── url_utils.h             # URL percent-encoding (tested natively)
│   ├── test/
│   │   ├── mocks/
│   │   │   ├── Arduino.h           # Minimal Arduino String shim for desktop
│   │   │   └── esp_heap_caps.h     # malloc/free wrapper for desktop
│   │   └── test_native/
│   │       └── test_main.cpp       # 19 unit tests (dither, XML, URL encoding)
│   └── lib/                        # Vendored libraries (future: codec_board)
```

### SD card layout

```
(SD root)/
├── config.json       # WiFi credentials (you create this)
├── settings.json     # API keys, Sonos IP (created by web portal)
└── gallery/          # Fallback JPEG images (you upload via web portal or copy here)
```

---

## Troubleshooting

| Problem | Solution |
|---------|----------|
| `pio: command not found` | Use `python3 -m platformio` instead, or add `~/.local/bin` to your PATH |
| Upload fails with "no serial port" | Hold BOOT button while plugging in USB-C, then retry |
| E-ink shows nothing after boot | Wait 30+ seconds for first refresh. Check serial for `[Display]` messages |
| `vinyl.local` doesn't resolve | Use the IP address from serial output instead. Some routers block mDNS |
| Album art looks wrong / colours off | This is expected with 7-colour dithering. Results vary by image. Dark/high-contrast art works best |
| ACRCloud returns no match | Ensure the mic is picking up audio. Check serial for `[Audio] Recorded X bytes` — if X is very low, there's a hardware issue |
| Spotify returns no art | Check Client ID/Secret are correct. The Spotify search may not find a match for every track ACRCloud identifies |
| WiFi disconnects periodically | The ESP32 will keep polling. It doesn't auto-reconnect yet — a reboot fixes it. This can be improved in a future update |
