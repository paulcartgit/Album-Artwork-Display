#!/usr/bin/env python3
"""
E-Ink Display Simulator — renders artwork exactly as the firmware does.

Polls a real Sonos speaker, fetches the album art, and puts it through the same
pipeline as the device (see eink.py, which mirrors image_pipeline.cpp and
dither.cpp with the palette read from firmware/src/config.h). Serves a preview
at http://localhost:5555.

It does NOT simulate vinyl identification. The firmware identifies records via
Shazam; this used to model ACRCloud and Spotify, which the firmware has never
used, and simulating the wrong services is worse than simulating none. Use the
device itself for that path.

Usage:
    python vinyl_sim.py                          # prompts for a Sonos IP
    python vinyl_sim.py --sonos 192.168.1.42     # direct
    python vinyl_sim.py --image path/to/art.jpg  # preview a local image
    python vinyl_sim.py --url https://...        # preview a remote image
"""

import argparse
import base64
import hashlib
import hmac
import io
import json
import os
import re
import struct
import sys
import threading
import time
import html
from pathlib import Path
from urllib.parse import quote

import numpy as np
import requests
from PIL import Image, ImageDraw, ImageFont
from flask import Flask, Response, jsonify, request, send_file

# Optional: audio capture for vinyl identification
try:
    import sounddevice as sd
    HAS_AUDIO = True
except ImportError:
    HAS_AUDIO = False

# ─── Constants ───
# Read straight from firmware/src/config.h — see firmware_config.py.  These
# used to be a hand-maintained copy that had drifted to a seven-colour palette
# (including an Orange the panel doesn't have) with completely different RGB
# values from the real pigments.
from firmware_config import (          # noqa: E402
    EPD_WIDTH, EPD_HEIGHT, EPD_COLORS,
    PALETTE_RGB, PALETTE_NAMES, PALETTE_HEX, RENDER_PROFILES, DEFAULT_PROFILE,
)
import eink                            # noqa: E402

PALETTE = eink.PALETTE

# ─── Paths ───
SIM_DIR = Path(__file__).parent
SETTINGS_PATH = SIM_DIR / "settings.json"
GALLERY_DIR = SIM_DIR / "gallery"
PREVIEW_PATH = SIM_DIR / "preview.png"
FIRMWARE_DIR = SIM_DIR.parent / "firmware"

# ─── Global State ───
app_state = {
    "state": 1,  # IDLE
    "artist": "",
    "title": "",
    "album": "",
    "art_url": "",
    "is_line_in": False,
    "uptime_start": time.time(),
    "last_track_key": "",
    "vinyl_art_found": False,   # True once vinyl art successfully identified
    "preview_image": None,      # PIL Image of dithered preview
    "original_image": None,     # PIL Image before dithering
    "poll_active": False,
}

settings = {}

# ═══════════════════════════════════════════════════════════════
# Sonos SOAP Client (mirrors sonos_client.cpp)
# ═══════════════════════════════════════════════════════════════

GETPOS_ENVELOPE = (
    '<?xml version="1.0" encoding="utf-8"?>'
    '<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"'
    ' s:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">'
    '<s:Body>'
    '<u:GetPositionInfo xmlns:u="urn:schemas-upnp-org:service:AVTransport:1">'
    '<InstanceID>0</InstanceID>'
    '</u:GetPositionInfo>'
    '</s:Body></s:Envelope>'
)

GETTRANS_ENVELOPE = (
    '<?xml version="1.0" encoding="utf-8"?>'
    '<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"'
    ' s:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">'
    '<s:Body>'
    '<u:GetTransportInfo xmlns:u="urn:schemas-upnp-org:service:AVTransport:1">'
    '<InstanceID>0</InstanceID>'
    '</u:GetTransportInfo>'
    '</s:Body></s:Envelope>'
)

def extract_tag(xml_str, tag):
    """Extract text content from an XML tag (mirrors xml_utils.h extractTag)."""
    pattern = f"<{tag}[^>]*>(.*?)</{tag}>"
    m = re.search(pattern, xml_str, re.DOTALL)
    return m.group(1) if m else ""

def sonos_get_track_info(sonos_ip):
    """Poll Sonos GetPositionInfo — returns dict with artist, title, album, art_url, is_line_in."""
    url = f"http://{sonos_ip}:1400/MediaRenderer/AVTransport/Control"
    headers = {
        "Content-Type": 'text/xml; charset="utf-8"',
        "SOAPAction": '"urn:schemas-upnp-org:service:AVTransport:1#GetPositionInfo"',
    }
    try:
        r = requests.post(url, data=GETPOS_ENVELOPE, headers=headers, timeout=5)
        r.raise_for_status()
    except requests.RequestException as e:
        print(f"[Sonos] Error: {e}")
        return None

    body = r.text
    track_uri = extract_tag(body, "TrackURI")
    is_line_in = track_uri.startswith("x-rincon-stream:")

    meta_raw = extract_tag(body, "TrackMetaData")
    if not meta_raw:
        return {"artist": "", "title": "", "album": "", "art_url": "", "is_line_in": is_line_in}

    meta = html.unescape(meta_raw)
    artist = extract_tag(meta, "dc:creator")
    title = extract_tag(meta, "dc:title")
    album = extract_tag(meta, "upnp:album")
    art_path = html.unescape(extract_tag(meta, "upnp:albumArtURI"))

    if art_path:
        if art_path.startswith("http"):
            art_url = art_path
        else:
            art_url = f"http://{sonos_ip}:1400{art_path}"
    else:
        art_url = ""

    return {
        "artist": artist,
        "title": title,
        "album": album,
        "art_url": art_url,
        "is_line_in": is_line_in,
    }

def sonos_is_playing(sonos_ip):
    """Check Sonos transport state — returns True if PLAYING."""
    url = f"http://{sonos_ip}:1400/MediaRenderer/AVTransport/Control"
    headers = {
        "Content-Type": 'text/xml; charset="utf-8"',
        "SOAPAction": '"urn:schemas-upnp-org:service:AVTransport:1#GetTransportInfo"',
    }
    try:
        r = requests.post(url, data=GETTRANS_ENVELOPE, headers=headers, timeout=5)
        r.raise_for_status()
    except requests.RequestException:
        return False

    state = extract_tag(r.text, "CurrentTransportState")
    return state == "PLAYING"

# ═══════════════════════════════════════════════════════════════
# ═══════════════════════════════════════════════════════════════

# ═══════════════════════════════════════════════════════════════
# Audio capture (the firmware identifies via Shazam; not simulated here)
# ═══════════════════════════════════════════════════════════════

AUDIO_SAMPLE_RATE = 44100
AUDIO_CHANNELS = 1
AUDIO_RECORD_SECS = 12

def show_gallery_fallback():
    """Pick a random gallery image and display it as fallback."""
    GALLERY_DIR.mkdir(exist_ok=True)
    images = [f for f in GALLERY_DIR.iterdir()
              if f.is_file() and f.suffix.lower() in (".jpg", ".jpeg", ".png", ".bmp")]
    if not images:
        print("[Gallery] No fallback images in gallery/")
        return
    import random
    pick = random.choice(images)
    print(f"[Gallery] Showing fallback: {pick.name}")
    img = Image.open(str(pick)).convert("RGB")
    app_state["original_image"] = img
    dithered, _ = process_image(img)
    app_state["preview_image"] = dithered
    dithered.save(str(PREVIEW_PATH))
    print(f"[Preview] Saved to {PREVIEW_PATH}")

# ═══════════════════════════════════════════════════════════════
# Floyd-Steinberg Dithering (mirrors dither.cpp exactly)
# ═══════════════════════════════════════════════════════════════

def download_image(url):
    """Download image from URL, return PIL Image."""
    try:
        r = requests.get(url, timeout=15, stream=True)
        r.raise_for_status()
        return Image.open(io.BytesIO(r.content)).convert("RGB")
    except Exception as e:
        print(f"[Pipeline] Download error: {e}")
        return None

def process_image(img):
    """
    Run the artwork through the same pipeline the firmware uses.

    Everything here lives in eink.py, which mirrors image_pipeline.cpp and
    dither.cpp.  parity_check.py fails the build if the two drift apart.
    """
    print(f"[Pipeline] Input: {img.size[0]}x{img.size[1]}")

    artist = app_state.get("artist", "")
    album = app_state.get("album", "")
    # Sonos sometimes puts a URL path in the album field
    if album and ("/" in album or "getaa" in album or album.startswith("x-")):
        album = ""

    prof = int(settings.get("render_profile", DEFAULT_PROFILE))
    bg_mode = int(settings.get("bg_mode", 2))
    bg_style = int(settings.get("bg_style", 0))
    show_text = bool(settings.get("show_track_info", True))

    t0 = time.time()
    if settings.get("use_dithering", True):
        out, indices = eink.render(img, artist, album,
                                   bg_mode=bg_mode, bg_style=bg_style,
                                   profile_index=prof, show_text=show_text)
        print(f"[Pipeline] {EPD_WIDTH}x{EPD_HEIGHT} "
              f"'{RENDER_PROFILES[prof]['name']}' profile in {time.time()-t0:.1f}s")
        return out, indices

    canvas, _ = eink.compose(img, artist, album, bg_mode, bg_style, prof, show_text)
    print(f"[Pipeline] Composed {EPD_WIDTH}x{EPD_HEIGHT} in {time.time()-t0:.1f}s "
          f"(dithering OFF)")
    return Image.fromarray(canvas, "RGB"), None

def process_art_url(art_url):
    """Download + process art URL, update global preview. Returns True on success."""
    img = download_image(art_url)
    if not img:
        return False
    app_state["original_image"] = img
    dithered, _ = process_image(img)
    app_state["preview_image"] = dithered
    dithered.save(str(PREVIEW_PATH))
    print(f"[Preview] Saved to {PREVIEW_PATH}")
    return True

# ═══════════════════════════════════════════════════════════════
# Sonos Polling Loop (mirrors main.cpp loop)
# ═══════════════════════════════════════════════════════════════

def poll_loop():
    """Background thread: poll Sonos, update state, trigger pipeline."""
    sonos_ip = settings.get("sonos_ip", "")
    if not sonos_ip:
        print("[Poll] No Sonos IP configured — polling disabled")
        return

    poll_ms = settings.get("poll_interval_ms", 45000)
    poll_sec = poll_ms / 1000.0
    print(f"[Poll] Starting — Sonos {sonos_ip}, interval {poll_sec:.0f}s")
    app_state["poll_active"] = True

    while app_state["poll_active"]:
        try:
            playing = sonos_is_playing(sonos_ip)

            if not playing:
                if app_state["state"] != 1:  # not already IDLE
                    print("[Poll] Sonos stopped → IDLE")
                    app_state["state"] = 1
                    app_state["artist"] = ""
                    app_state["title"] = ""
                    app_state["album"] = ""
                    app_state["art_url"] = ""
                    app_state["vinyl_art_found"] = False
                    app_state["last_track_key"] = ""
            else:
                info = sonos_get_track_info(sonos_ip)
                if info:
                    _handle_track_info(info)

        except Exception as e:
            print(f"[Poll] Error: {e}")
            app_state["state"] = 4  # ERROR

        time.sleep(poll_sec)

def _handle_track_info(info):
    """Process a Sonos track info result within the poll loop."""
    # For vinyl (line-in), Sonos returns empty artist/title so use
    # a stable key to avoid re-triggering the full initial detection.
    if info["is_line_in"]:
        track_key = "__VINYL_LINE_IN__"
    else:
        track_key = f"{info['artist']}|{info['title']}"

    is_new_track = (track_key != app_state["last_track_key"])

    # ── Continuing vinyl: retry if no art yet, or re-check for record change ──
    if not is_new_track and info["is_line_in"]:
        if not app_state["vinyl_art_found"]:
            print(f"[Poll] VINYL — retrying identification...")
            max_retries = 3
            retry_delay = 10  # seconds
            for attempt in range(1, max_retries + 1):
                # Re-check Sonos before each attempt — bail if source changed
                sonos_ip = settings.get("sonos_ip", "")
                if sonos_ip:
                    fresh = sonos_get_track_info(sonos_ip)
                    if fresh and not fresh["is_line_in"]:
                        print("[Vinyl] Sonos no longer on line-in — aborting retries")
                        break
                print(f"[Vinyl] Retry attempt {attempt}/{max_retries}")
                if vinyl_identify_and_display():
                    app_state["vinyl_art_found"] = True
                    break
                if attempt < max_retries:
                    print(f"[Vinyl] No result — retrying in {retry_delay}s...")
                    time.sleep(retry_delay)
        else:
            # Already have art — re-check in case the record changed
            vinyl_recheck()
        return

    if not is_new_track:
        return

    # ── New track / source detected ──
    app_state["last_track_key"] = track_key
    app_state["artist"] = info["artist"]
    app_state["title"] = info["title"]
    app_state["album"] = info["album"]
    app_state["art_url"] = info["art_url"]
    app_state["is_line_in"] = info["is_line_in"]
    app_state["vinyl_art_found"] = False

    if info["is_line_in"]:
        app_state["state"] = 3  # VINYL
        print(f"[Poll] VINYL — Line-In detected")
        # Show gallery immediately so screen isn't blank
        show_gallery_fallback()
        # Rapid-fire attempts for initial vinyl detection
        max_retries = 3
        retry_delay = 10  # seconds
        for attempt in range(1, max_retries + 1):
            # Re-check Sonos before each attempt — bail if source changed
            if attempt > 1:
                sonos_ip = settings.get("sonos_ip", "")
                if sonos_ip:
                    fresh = sonos_get_track_info(sonos_ip)
                    if fresh and not fresh["is_line_in"]:
                        print("[Vinyl] Sonos no longer on line-in — aborting retries")
                        break
            print(f"[Vinyl] Identification attempt {attempt}/{max_retries}")
            if vinyl_identify_and_display():
                app_state["vinyl_art_found"] = True
                break
            if attempt < max_retries:
                print(f"[Vinyl] Retrying in {retry_delay}s...")
                time.sleep(retry_delay)
        else:
            print(f"[Vinyl] All {max_retries} attempts failed — will retry next poll cycle")
    else:
        app_state["state"] = 2  # DIGITAL
        print(f"[Poll] DIGITAL — {info['artist']} — {info['title']}")

        art_url = info["art_url"]

        if art_url:
            app_state["art_url"] = art_url
            success = process_art_url(art_url)
        else:
            print("[Poll] No album art found")

# ═══════════════════════════════════════════════════════════════
# Flask Web Server (mirrors web_server.cpp + web_portal.h)
# ═══════════════════════════════════════════════════════════════

flask_app = Flask(__name__)
flask_app.logger.disabled = True

import logging
log = logging.getLogger("werkzeug")
log.setLevel(logging.WARNING)

def load_web_portal_html():
    """Extract HTML from firmware's web_portal.h PROGMEM literal."""
    portal_path = FIRMWARE_DIR / "src" / "web_portal.h"
    if not portal_path.exists():
        return "<h1>web_portal.h not found</h1><p>Expected at: " + str(portal_path) + "</p>"
    text = portal_path.read_text()
    # Extract between R"rawliteral( and )rawliteral"
    m = re.search(r'R"rawliteral\((.*?)\)rawliteral"', text, re.DOTALL)
    if not m:
        return "<h1>Could not extract HTML from web_portal.h</h1>"
    return m.group(1)

PORTAL_HTML = None  # lazy-loaded

@flask_app.route("/")
def index():
    global PORTAL_HTML
    if PORTAL_HTML is None:
        PORTAL_HTML = load_web_portal_html()
    return PORTAL_HTML

@flask_app.route("/api/status")
def api_status():
    return jsonify({
        "state": app_state["state"],
        "artist": app_state["artist"],
        "title": app_state["title"],
        "album": app_state["album"],
        "art_url": app_state["art_url"],
        "is_line_in": app_state["is_line_in"],
        "ip": "127.0.0.1 (simulator)",
        "uptime": int(time.time() - app_state["uptime_start"]),
    })

@flask_app.route("/api/settings", methods=["GET"])
def api_settings_get():
    return jsonify({
        "sonos_ip": settings.get("sonos_ip", ""),
        "google_photos_url": settings.get("google_photos_url", ""),
        "poll_interval_ms": settings.get("poll_interval_ms", 45000),
        "show_track_info": settings.get("show_track_info", True),
        "use_dithering": settings.get("use_dithering", True),
        "render_profile": settings.get("render_profile", DEFAULT_PROFILE),
        "bg_mode": settings.get("bg_mode", 2),
        "bg_style": settings.get("bg_style", 0),
        "profiles": [{"id": i, "name": p["name"]}
                     for i, p in enumerate(RENDER_PROFILES)],
    })

@flask_app.route("/api/settings", methods=["POST"])
def api_settings_post():
    data = request.get_json(force=True)
    for key in ["sonos_ip", "google_photos_url"]:
        if key in data:
            settings[key] = data[key]
    if "poll_interval_ms" in data:
        settings["poll_interval_ms"] = int(data["poll_interval_ms"])
    if "show_track_info" in data:
        settings["show_track_info"] = bool(data["show_track_info"])
    if "use_dithering" in data:
        settings["use_dithering"] = bool(data["use_dithering"])
    for key in ["render_profile", "bg_mode", "bg_style"]:
        if key in data:
            settings[key] = int(data[key])

    save_settings()
    return jsonify({"ok": True})

@flask_app.route("/api/gallery")
def api_gallery():
    GALLERY_DIR.mkdir(exist_ok=True)
    files = []
    for f in sorted(GALLERY_DIR.iterdir()):
        if f.is_file() and f.suffix.lower() in (".jpg", ".jpeg", ".png", ".bmp"):
            files.append({"name": f.name, "size": f.stat().st_size})
    return jsonify(files)

@flask_app.route("/api/upload", methods=["POST"])
def api_upload():
    GALLERY_DIR.mkdir(exist_ok=True)
    if "file" not in request.files:
        return jsonify({"error": "no file"}), 400
    f = request.files["file"]
    if not f.filename:
        return jsonify({"error": "no filename"}), 400
    # Sanitise filename
    safe_name = re.sub(r"[^\w.\-]", "_", f.filename)
    dest = GALLERY_DIR / safe_name
    f.save(str(dest))
    return jsonify({"ok": True, "name": safe_name})

@flask_app.route("/api/gallery/delete", methods=["POST"])
def api_gallery_delete():
    name = request.form.get("name") or (request.get_json(silent=True) or {}).get("name", "")
    if not name:
        return jsonify({"error": "no name"}), 400
    safe_name = re.sub(r"[^\w.\-]", "_", name)
    path = GALLERY_DIR / safe_name
    if path.exists() and path.parent == GALLERY_DIR:
        path.unlink()
        return jsonify({"ok": True})
    return jsonify({"error": "not found"}), 404

# ─── Extra simulator-only routes ───

@flask_app.route("/preview")
def preview_page():
    """Show the dithered e-ink preview in browser."""
    return f"""<!DOCTYPE html>
<html><head><title>E-Ink Preview</title>
<style>
body {{ background: #222; color: #eee; font-family: sans-serif; text-align: center; padding: 20px; }}
h1 {{ font-size: 1.2rem; margin-bottom: 8px; }}
.info {{ color: #aaa; font-size: 0.85rem; margin-bottom: 16px; }}
img {{ border: 2px solid #444; max-width: 100%; image-rendering: pixelated; }}
.controls {{ margin: 16px 0; }}
button {{ padding: 8px 16px; margin: 0 4px; background: #2a6; border: none; border-radius: 6px;
         color: #fff; cursor: pointer; font-size: .9rem; }}
button:hover {{ background: #185; }}
</style></head>
<body>
<h1>&#127912; E-Ink Display Preview (480&times;800 portrait, 7-color)</h1>
<div class="info" id="info">
    {f'{app_state["artist"]} — {app_state["title"]}' if app_state["artist"] else 'No track'}
</div>
<div class="controls">
    <button onclick="location.reload()">Refresh</button>
    <button onclick="fetch('/api/trigger').then(()=>setTimeout(()=>location.reload(),2000))">Re-poll Sonos</button>
</div>
<img src="/preview.png?t={time.time()}" alt="E-Ink Preview" width="480" height="800">
<div class="info" style="margin-top:12px">
    Palette: Black, White, Green, Blue, Red, Yellow, Orange<br>
    Floyd-Steinberg dithering &bull; Euclidean RGB distance
</div>
</body></html>"""

@flask_app.route("/preview.png")
def preview_png():
    """Serve the current dithered preview as PNG."""
    img = app_state.get("preview_image")
    if img:
        buf = io.BytesIO()
        img.save(buf, format="PNG")
        buf.seek(0)
        return send_file(buf, mimetype="image/png")
    # No preview yet — generate a placeholder
    placeholder = Image.new("RGB", (EPD_WIDTH, EPD_HEIGHT), (0x11, 0x11, 0x11))
    draw = ImageDraw.Draw(placeholder)
    draw.text((EPD_WIDTH // 2 - 100, EPD_HEIGHT // 2 - 10),
              "No image yet — play something on Sonos!",
              fill=(0x88, 0x88, 0x88))
    buf = io.BytesIO()
    placeholder.save(buf, format="PNG")
    buf.seek(0)
    return send_file(buf, mimetype="image/png")

@flask_app.route("/original.png")
def original_png():
    """Serve the original (pre-dither) image with layout for comparison."""
    img = app_state.get("original_image")
    if not img:
        return "No image", 404
    album = app_state.get("album", "")
    if album and ("/" in album or "getaa" in album or album.startswith("x-")):
        album = ""
    canvas, _ = eink.compose(
        img, app_state.get("artist", ""), album,
        bg_mode=int(settings.get("bg_mode", 2)),
        bg_style=int(settings.get("bg_style", 0)),
        profile_index=int(settings.get("render_profile", DEFAULT_PROFILE)),
        show_text=bool(settings.get("show_track_info", True)))
    buf = io.BytesIO()
    Image.fromarray(canvas, "RGB").save(buf, format="PNG")
    buf.seek(0)
    return send_file(buf, mimetype="image/png")

@flask_app.route("/compare")
def compare_page():
    """Side-by-side comparison: original vs dithered."""
    return f"""<!DOCTYPE html>
<html><head><title>Dither Comparison</title>
<style>
body {{ background: #222; color: #eee; font-family: sans-serif; text-align: center; padding: 20px; }}
h1 {{ font-size: 1.2rem; margin-bottom: 16px; }}
.pair {{ display: flex; gap: 16px; justify-content: center; flex-wrap: wrap; }}
.panel {{ text-align: center; }}
.panel img {{ border: 2px solid #444; max-width: 100%; }}
.label {{ color: #aaa; font-size: 0.85rem; margin-top: 4px; }}
</style></head>
<body>
<h1>Original vs E-Ink Dithered</h1>
<div class="pair">
    <div class="panel">
        <img src="/original.png?t={time.time()}" width="300"><br>
        <span class="label">Original (480&times;800)</span>
    </div>
    <div class="panel">
        <img src="/preview.png?t={time.time()}" width="300"><br>
        <span class="label">Floyd-Steinberg dithered (7 colors)</span>
    </div>
</div>
</body></html>"""

@flask_app.route("/api/trigger")
def api_trigger():
    """Manually trigger a Sonos poll (simulator-only)."""
    sonos_ip = settings.get("sonos_ip", "")
    if not sonos_ip:
        return jsonify({"error": "no sonos_ip"}), 400

    def do_poll():
        info = sonos_get_track_info(sonos_ip)
        if not info:
            return
        app_state["artist"] = info["artist"]
        app_state["title"] = info["title"]
        app_state["album"] = info["album"]
        app_state["is_line_in"] = info["is_line_in"]
        app_state["state"] = 3 if info["is_line_in"] else 2
        app_state["last_track_key"] = f"{info['artist']}|{info['title']}"

        art_url = info["art_url"]
        if art_url:
            app_state["art_url"] = art_url
            success = process_art_url(art_url)

    threading.Thread(target=do_poll, daemon=True).start()
    return jsonify({"ok": True, "message": "poll triggered"})

@flask_app.route("/api/dither_upload", methods=["POST"])
def api_dither_upload():
    """Upload an image and get back the dithered preview (simulator-only)."""
    if "file" not in request.files:
        return jsonify({"error": "no file"}), 400
    f = request.files["file"]
    img = Image.open(f.stream).convert("RGB")
    app_state["original_image"] = img
    dithered, _ = process_image(img)
    app_state["preview_image"] = dithered
    dithered.save(str(PREVIEW_PATH))
    return jsonify({"ok": True})

# ═══════════════════════════════════════════════════════════════
# Settings persistence
# ═══════════════════════════════════════════════════════════════

def load_settings():
    global settings
    if SETTINGS_PATH.exists():
        with open(SETTINGS_PATH) as f:
            settings = json.load(f)
    else:
        settings = {
            "sonos_ip": "",
            "google_photos_url": "",
            "poll_interval_ms": 45000,
            "show_track_info": True,
            "use_dithering": True,
            "render_profile": DEFAULT_PROFILE,
            "bg_mode": 2,
            "bg_style": 0,
        }
        save_settings()

def save_settings():
    with open(SETTINGS_PATH, "w") as f:
        json.dump(settings, f, indent=4)

# ═══════════════════════════════════════════════════════════════
# Main
# ═══════════════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(description="Vinyl E-Ink Display Simulator")
    parser.add_argument("--sonos", type=str, help="Sonos speaker IP address")
    parser.add_argument("--image", type=str, help="Local image file to preview (no Sonos)")
    parser.add_argument("--url", type=str, help="Remote image URL to preview (no Sonos)")
    parser.add_argument("--port", type=int, default=5555, help="Web server port (default: 5555)")
    parser.add_argument("--no-poll", action="store_true", help="Disable Sonos polling (web portal only)")
    args = parser.parse_args()

    load_settings()

    # Override Sonos IP from CLI
    if args.sonos:
        settings["sonos_ip"] = args.sonos
        save_settings()

    GALLERY_DIR.mkdir(exist_ok=True)

    # ─── Static image mode ───
    if args.image or args.url:
        if args.image:
            print(f"[Preview] Loading {args.image}")
            img = Image.open(args.image).convert("RGB")
        else:
            print(f"[Preview] Downloading {args.url}")
            img = download_image(args.url)
            if not img:
                sys.exit(1)

        app_state["original_image"] = img
        dithered, _ = process_image(img)
        app_state["preview_image"] = dithered
        dithered.save(str(PREVIEW_PATH))
        print(f"[Preview] Saved to {PREVIEW_PATH}")
        print(f"[Web] Starting on http://localhost:{args.port}")
        print(f"       Preview:  http://localhost:{args.port}/preview")
        print(f"       Compare:  http://localhost:{args.port}/compare")
        print(f"       Portal:   http://localhost:{args.port}/")
        flask_app.run(host="0.0.0.0", port=args.port, debug=False)
        return

    # ─── Full simulation mode ───
    print("=" * 60)
    print("  Vinyl Now-Playing E-Ink Display — Simulator")
    print("=" * 60)

    sonos_ip = settings.get("sonos_ip", "")
    if not sonos_ip and not args.no_poll:
        sonos_ip = input("\nEnter Sonos speaker IP (or press Enter to skip): ").strip()
        if sonos_ip:
            settings["sonos_ip"] = sonos_ip
            save_settings()

    if sonos_ip and not args.no_poll:
        # Quick connectivity test
        print(f"\n[Sonos] Testing connection to {sonos_ip}...")
        if sonos_is_playing(sonos_ip) is not None:
            info = sonos_get_track_info(sonos_ip)
            if info:
                playing = sonos_is_playing(sonos_ip)
                if playing and info["artist"]:
                    print(f"[Sonos] Connected! Now playing: {info['artist']} — {info['title']}")
                elif playing:
                    print(f"[Sonos] Connected! Playing (no metadata)")
                else:
                    print(f"[Sonos] Connected! (not currently playing)")
            else:
                print(f"[Sonos] Connected but got no response — check IP")
        else:
            print(f"[Sonos] Could not reach {sonos_ip}")

        # Start polling in background
        poll_thread = threading.Thread(target=poll_loop, daemon=True)
        poll_thread.start()
    else:
        print("\n[Poll] No Sonos IP — running web portal only")
        print("       Configure Sonos IP in the Settings tab or restart with --sonos IP")

    print(f"\n[Web] Starting on http://localhost:{args.port}")
    print(f"       Portal:   http://localhost:{args.port}/")
    print(f"       Preview:  http://localhost:{args.port}/preview")
    print(f"       Compare:  http://localhost:{args.port}/compare")
    print()
    flask_app.run(host="0.0.0.0", port=args.port, debug=False)

if __name__ == "__main__":
    main()
