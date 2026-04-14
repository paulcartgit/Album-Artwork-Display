# Album Artwork Display — Setup & Testing Guide

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
| **Shazam (RapidAPI)** | [rapidapi.com/apidojo/api/shazam](https://rapidapi.com/apidojo/api/shazam) | Subscribe → copy your RapidAPI key |

Shazam is only needed for vinyl record identification. Digital Sonos playback (Spotify, Apple Music, etc.) gets album art directly from Sonos metadata — no external API needed.

---

## Part 1 — Test Locally Without the Device

The dithering algorithm, Sonos XML parsing, and URL encoding logic are all tested natively on your Mac — no board needed.

### Run the tests

```bash
cd firmware
python3 -m platformio test -e native -v
```

### What's tested

| Module | Tests | What it verifies |
|--------|-------|------------------|
| **Dithering** | Palette mapping | Solid black/white/red map to correct palette index, pixel packing (high/low nibble), all output indices are valid (0–5) |
| **XML parsing** | Tag extraction | Sonos SOAP responses, DIDL-Lite entity decoding, line-in URI detection, missing/empty tag handling |
| **URL encoding** | Percent encoding | Spaces → `%20`, colons → `%3A`, safe chars pass through, UTF-8 bytes encoded, empty string |

### Build the ESP32 firmware (without flashing)

To verify the full firmware compiles:

```bash
python3 -m platformio run -e esp32-s3-photopainter
```

This downloads all dependencies and cross-compiles for ESP32-S3. You'll see `SUCCESS` and memory usage.

---

## Part 2 — Prepare the SD Card

Format a MicroSD card as **FAT32**. Create this file in the root:

### `/config.json` — WiFi credentials

```json
{
  "ssid": "YourWiFiName",
  "password": "YourWiFiPassword"
}
```

That's all you need on the SD card. Settings (Sonos IP, Shazam key, timing, display options) are configured through the web portal after first boot.

### What the device creates automatically

- `/settings.json` — all settings (written by the web portal)
- `/history/` — cached album art JPEGs + `index.json` (auto-populated as you play music)

---

## Part 3 — Flash the Device

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
[Display] Initialized 7.3" Spectra 6
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

## Part 4 — Configure via Web Portal

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
| **Shazam API Key** | Your RapidAPI key (only needed for vinyl identification) |
| **Sonos check interval** | How often to poll Sonos (default: 10 seconds) |
| **Vinyl re-identify** | How often to re-identify vinyl after a match (default: 10 minutes) |
| **No-match cooldown** | Wait time after 3 failed Shazam attempts (default: 5 minutes) |
| **Idle gallery rotation** | How often to rotate album art when idle (default: 5 minutes) |
| **Show track info** | Toggle artist/album text overlay below artwork |
| **Background fill** | Auto (smart detection), Always blurred, or Always solid colour |

Hit **Save Settings**. Settings are stored on the SD card and survive reboots.

---

## Part 5 — Verify It Works (Step by Step)

Work through these checks in order. Each one builds on the previous.

### Check 1 — Board boots clean
Serial shows `[BOOT] Ready` with no errors.

### Check 2 — Web portal loads
`http://vinyl.local` shows the Now Playing page on your phone.

### Check 3 — Settings save
Enter and save settings → refresh Settings page and values persist.

### Check 4 — Digital playback
Play a track on Sonos (Spotify, Apple Music, etc.). Within the Sonos check interval:

```
[Sonos] Queen — Bohemian Rhapsody
[Pipeline] JPEG 640x640 (87234 bytes)
[Pipeline] Edge variance: 2340 → blur fill
[History] Saved: a1b2c3d4.jpg (Queen — Bohemian Rhapsody)
[Display] Refresh complete
```

The e-ink updates with the album art, and the cover is saved to history.

### Check 5 — History tab
Open the web portal → History tab. You should see a thumbnail of the album art you just played. The green checkmark means it's enabled for idle rotation.

### Check 6 — Idle display
Stop playback on Sonos. After the idle gallery rotation interval, the device will show a random cover from your enabled history entries.

### Check 7 — Vinyl identification (requires Shazam API key)
Play a vinyl record through Sonos Line-In. The device detects line-in and records audio:

```
[Main] Line-In detected → VINYL
[Audio] Recording 10 seconds...
[Shazam] Found: Pink Floyd — Comfortably Numb (The Wall)
[Pipeline] Fetching album art...
[Display] Refresh complete
```

### Check 8 — Listen mode
Tap the **Listen** button in the web portal to manually identify whatever's playing in the room (works without Sonos line-in).

---

## Quick Reference

### Common commands

```bash
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

### SD card layout

```
(SD root)/
├── config.json           # WiFi credentials (you create this)
├── settings.json         # All settings (created by web portal)
└── history/              # Album art cache (auto-managed)
    ├── index.json        # Metadata index (artist, title, album, enabled state)
    ├── a1b2c3d4.jpg      # Cached album cover (hash-named)
    └── ...               # Up to 100 entries, oldest pruned automatically
```

### Web portal tabs

| Tab | What it does |
|-----|-------------|
| **Now Playing** | Current track info, artwork, activity log, Listen & Check Sonos buttons |
| **Settings** | Sonos IP, Shazam key, timing sliders, display options |
| **History** | Thumbnail grid of saved covers with on/off toggles for idle rotation |
| **Debug** | Device IP, uptime, force refresh, test color pattern, download last audio |

---

## Troubleshooting

| Problem | Solution |
|---------|----------|
| `pio: command not found` | Use `python3 -m platformio` instead, or add `~/.local/bin` to your PATH |
| Upload fails with "no serial port" | Hold BOOT button while plugging in USB-C, then retry |
| E-ink shows nothing after boot | Wait 30+ seconds for first refresh. Check serial for `[Display]` messages |
| `vinyl.local` doesn't resolve | Use the IP address from serial output instead. Some routers block mDNS |
| Album art colours look limited | Expected — 6 physical pigment colours. High-contrast art works best |
| Shazam returns no match | Ensure the mic is picking up audio. Check serial for `[Audio] Recorded X bytes` |
| History thumbnails don't load | Check serial for errors. Images are served from SD — ensure card is working |
| Display shows "No images / Play some music!" | No history yet — play some tracks to build up the cover art cache |
