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
4. The device reboots. Once connected it shows its address on screen, and is reachable at
   **http://nowplaying.local** — or at that IP directly.

   > **On Android, use the IP.** Android has no system-wide mDNS resolver, so `.local`
   > names do not resolve in Chrome and you will get `DNS_PROBE_FINISHED_NXDOMAIN`.
   > macOS, iOS and most desktop browsers resolve `nowplaying.local` fine. The frame
   > displays its IP address after connecting for exactly this reason.
5. Open the web portal → **Settings** tab → scan for Sonos speakers and select yours
6. (Optional) Add a [Shazam RapidAPI](https://rapidapi.com/apidojo/api/shazam/) key for vinyl identification
7. Save settings — the display will start showing album art automatically

If the WiFi connection fails (e.g. wrong password), the device falls back to the captive portal automatically. The e-ink display shows "Wi-Fi Failed" so you know to reconnect to `NowPlaying-Setup` and try again.

## Rendering Pipeline

Album art goes through a multi-stage image processing pipeline optimised for the 6-color e-ink panel:

1. **JPEG decode** — Downloaded image decoded to RGB888 in PSRAM via TJpg_Decoder
2. **Background fill** — Auto-detected per image:
   - **Blurred** — Source image scaled to fill, 4-pass box blur (radius 12), then darkened (55%) or washed out (45% toward white)
   - **Solid colour** — Saturation-weighted average of edge pixels (vibrant pixels dominate, prevents muddy brown), slight saturation boost
   - Configurable: always solid, always blur, or auto (smart edge-variance threshold)
3. **Scaling** — Fit to display with correct aspect ratio, centred with margin for drop shadow
4. **Drop shadow** — Top and bottom gradient bands (20px, quadratic falloff) in full-art mode
5. **Text overlay** — 2× supersampled anti-aliased rendering (FreeSansBold 24pt artist, FreeSans 18pt album), auto-scaling and ellipsis truncation for long names, contrast-adaptive text colour (white on dark, black on light), frosted overlay on blurred backgrounds for legibility
6. **Enhancement** — Unsharp mask sharpening, contrast boost, gamma correction with shadow protection (strength set by the render profile)
7. **Dithering** — CIELAB matching against the *calibrated* pigment values, Floyd-Steinberg error diffusion, serpentine scanning, virtual Cyan/Magenta entries for out-of-gamut colours, chroma-aware penalty so White doesn't absorb pastels, shadow chroma suppression, edge-aware attenuation. See [DITHERING.md](DITHERING.md).
8. **Placeholder fallback** — When artwork can't be decoded (unsupported JPEG format), a text-only display shows artist and album name on a dark background

Artwork is cached to the SD card only after it successfully decodes, so an unreadable image can't wedge the idle gallery.

## Web Portal

Once connected, visit **http://nowplaying.local** — or the IP shown on the frame,
which is what you need on Android:

### Now Playing
Current track info, artwork preview, and activity log. Buttons to force a Sonos check or trigger a manual listen (Shazam identify). Auto-refreshes every 3 seconds.

### Settings
- **Speaker** — Scan and select a Sonos speaker by room name
- **Network** — Change Wi-Fi credentials (reboots to reconnect)
- **The picture** — Fill mode, render profile, track-info overlay, background treatment
- **Panel care** — Minimum interval between redraws, quiet hours, UTC offset
- **Timing** — Sonos poll interval, vinyl re-identify interval, no-match cooldown, idle rotation
- **Vinyl** — RapidAPI Shazam key
- **Security** — Optional portal password (username `admin`). Off by default; with no password, anyone on your network can change the Wi-Fi settings
- **Frame / Diagnostics / Firmware** — Device facts, test patterns, and over-the-air update

### History
Gallery grid of all saved album covers (up to 100), split into **Pinned** and **History** sections:
- Toggle covers on/off for idle gallery rotation
- **Pin** a cover to keep it permanently (exempt from the 100-entry cap)
- **Delete** unwanted entries

### What the frame is showing
The main screen shows the **actual bitmap on the panel**, read back from the
device — not the source artwork. That is the only way to see how something
rendered: the dithering, the crop, the fill decision. Tap it to compare against
the original. It is also available directly:

```
curl -s http://nowplaying.local/api/display/current.bmp -o panel.bmp
```

## Filling the Screen

Album art is square; the panel is 480×800. Fitting the square to the width
covers 60% of the screen, and cover-cropping to fill it discards 40% of the
sleeve horizontally — which usually slices through the artist's name.

**Adaptive** (the default) enlarges each sleeve as far as it can before the crop
lines start cutting into the artwork's own detail, then blends whatever is left
out to the edges by mirroring and blurring past recognition. Photographic
sleeves reach a full bleed; sleeves with type across them are left uncropped.
The alternatives are **Never crop**, **Always fill**, and the original
**Centred square**, which is also used whenever the artist/album overlay is on.

## Panel Care

A full refresh takes 20–25 seconds and e-ink panels have a finite refresh life.
The frame enforces a minimum interval between redraws (45 s by default) so
skipping through a playlist does not repaint on every track, counts its redraws,
and can be silenced overnight with **quiet hours** — e-ink holds its image with
no power, so a paused frame still looks like a picture.

## Release Details

For anything with an album name, the frame looks up the pressing on MusicBrainz
— year, label and catalogue number — and shows it under the track. Results are
cached per album in the history index, so a record is looked up once and never
again. No API key is needed.

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
- **Group coordinator** — When the speaker is grouped, the coordinator carries the group's transport state, so that is what gets polled
- **Push updates** — Subscribes to Sonos UPnP events, so track changes appear within a second or two; polling continues underneath as the fallback
- **Watchdog** — A hung I2C bus or SD write reboots the frame rather than leaving it dead until it is unplugged
- **Blocking refresh** — A panel refresh takes ~15s and blocks the state machine for its duration, yielding throughout so Wi-Fi and the web portal stay responsive
- **Physical button** — BTN_KEY triggers immediate re-identification (resets all cooldowns)

## 6-Color Palette

The Spectra 6 e-ink display uses these calibrated pigment colors:

| Index | Color | Calibrated RGB |
|-------|-------|----------------|
| 0 | Black | `#101012` (near-black charcoal) |
| 1 | White | `#D8DAD4` (light grey, slight cool tint) |
| 2 | Green | `#306658` (dark teal-green) |
| 3 | Blue | `#3868C0` (medium-bright, saturated) |
| 4 | Red | `#9C302C` (dark brick-crimson) |
| 5 | Yellow | `#C8B830` (warm golden) |

These are the single source of truth: the dither matches and diffuses error against them, and the simulator parses them straight out of `config.h`. If you recalibrate, change them here and nowhere else.

To re-derive them from your own panel: **Debug → Palette Calibration Card**, photograph it, then

```bash
cd simulator && python calibrate_from_photo.py photo.jpg --preview check.png
```

See [DITHERING.md](DITHERING.md#verifying-the-palette-against-the-real-panel) for the full loop.

## Project Structure

```
firmware/               ESP32-S3 PlatformIO firmware
├── src/
│   ├── main.cpp            setup()/loop() shim
│   ├── controller.cpp/h    State machine, Sonos polling, request servicing
│   ├── app.h               Shared application state and web-server requests
│   ├── config.h            Pin definitions, constants, palette, render profiles
│   ├── display.cpp/h       GxEPD2 6-color e-ink driver (non-blocking refresh)
│   ├── image_pipeline.cpp/h    JPEG → scale → enhance → dither → display
│   ├── dither.cpp/h        CIELAB + Floyd-Steinberg + serpentine dithering
│   ├── web_server.cpp/h    HTTP API + captive portal + settings portal
│   ├── web_portal.h        Embedded HTML/CSS/JS (status, settings, history, debug)
│   ├── captive_portal.h    Embedded HTML/CSS/JS for WiFi setup wizard
│   ├── sonos_client.cpp/h  UPnP/SOAP topology discovery + now-playing queries
│   ├── shazam_client.cpp/h Shazam audio fingerprinting (RapidAPI)
│   ├── audio_capture.cpp/h I2S microphone recording (ES7210)
│   ├── sd_manager.cpp/h    SD card: settings, wifi config, art history
│   ├── wifi_manager.cpp/h  WiFi STA connection + AP mode for setup
│   ├── identify.cpp/h      Record → mono → auto-gain → Shazam → display
│   ├── metadata_client.cpp/h   MusicBrainz release lookup (year, label, cat no.)
│   ├── power.cpp/h         AXP2101 battery reporting
│   ├── upnp_events.cpp/h   GENA NOTIFY receiver (raw listener; NOTIFY is not
│   │                       a method ESPAsyncWebServer parses)
│   ├── fill_policy.h       How far to enlarge artwork before the crop bites
│   ├── activity_log.h      Circular activity log for web UI
│   ├── backoff.h           Vinyl retry/cooldown escalation policy
│   ├── history_policy.h    History eviction and timestamp rules
│   ├── wav_utils.h         WAV header construction
│   ├── certs.h             Pinned root CA for the Shazam API
│   └── xml_utils.h         XML tag extraction (UPnP/SOAP)
├── test/
│   ├── test_native/test_main.cpp   Native unit tests
│   └── mocks/          Arduino/ESP stubs for native tests
└── platformio.ini

simulator/              Python simulator (runs without hardware)
├── eink.py             Port of the firmware rendering pipeline
├── firmware_config.py  Reads the palette + profiles from firmware/src/config.h
├── parity_check.py     Fails CI if the simulator drifts from the firmware
├── fill_modes.py       Fill strategies, for comparing them on real covers
├── panel_probe.py      Push test frames to the panel and read them back
├── panel_compare.py    Source vs prediction vs photograph, side by side
├── calibrate_from_photo.py  Regenerate the palette from a photo of the card
├── vinyl_sim.py        Full simulator with web UI
├── dither_preview.py   Standalone render preview tool
├── requirements.txt    Python dependencies
└── settings.example.json   Template for credentials
```

## Running Tests

Native unit tests — dithering, history policy, back-off escalation, XML parsing.
No hardware needed:

```bash
cd firmware && pio test -e native
```

Verify the simulator still matches the firmware:

```bash
cd simulator && python parity_check.py
```

Preview how a cover will actually render, without flashing anything:

```bash
cd simulator && python dither_preview.py cover.jpg --all-profiles
```

CI runs all three on every push.

## Updating Firmware Over the Air

After the first USB flash, subsequent updates go over the network:

1. `cd firmware && pio run -e esp32-s3-photopainter`
2. Open the portal → **Debug** → **Firmware Update**
3. Upload `.pio/build/esp32-s3-photopainter/firmware.bin`

The device verifies the image, writes it to the inactive OTA slot and reboots.
If the upload fails, the running firmware is untouched.

## License

See [LICENSE.md](LICENSE.md).
