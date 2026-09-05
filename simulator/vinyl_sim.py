#!/usr/bin/env python3
"""
Vinyl Now-Playing E-Ink Display — Full Local Simulator

Connects to a real Sonos speaker, fetches album art, applies the same
Floyd-Steinberg dithering as the ESP32 firmware, and shows a 480×800
portrait preview with album art on top and track info below.

Also serves the web portal at http://localhost:5555 with live Sonos data.

Usage:
    python vinyl_sim.py                          # interactive — prompts for Sonos IP
    python vinyl_sim.py --sonos 192.168.1.42     # direct
    python vinyl_sim.py --image path/to/art.jpg  # preview a local image (no Sonos)
    python vinyl_sim.py --url https://...         # preview a remote image
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
# Spotify Client (mirrors spotify_client.cpp)
# ═══════════════════════════════════════════════════════════════

_spotify_token = ""
_spotify_token_expiry = 0


def spotify_get_token():
    """Get/refresh Spotify Client Credentials token."""
    global _spotify_token, _spotify_token_expiry
    if time.time() < _spotify_token_expiry and _spotify_token:
        return _spotify_token

    cid = settings.get("spotify_client_id", "")
    csec = settings.get("spotify_client_secret", "")
    if not cid or not csec:
        return ""

    try:
        r = requests.post(
            "https://accounts.spotify.com/api/token",
            data={"grant_type": "client_credentials"},
            auth=(cid, csec),
            timeout=10,
        )
        r.raise_for_status()
        data = r.json()
        _spotify_token = data["access_token"]
        _spotify_token_expiry = time.time() + data.get("expires_in", 3600) - 60
        print("[Spotify] Token acquired")
        return _spotify_token
    except requests.RequestException as e:
        print(f"[Spotify] Token error: {e}")
        return ""


def _spotify_search(query, search_type="track"):
    """Run a single Spotify search, return art URL or empty string."""
    token = spotify_get_token()
    if not token:
        return ""
    try:
        r = requests.get(
            "https://api.spotify.com/v1/search",
            params={"q": query, "type": search_type, "limit": 1},
            headers={"Authorization": f"Bearer {token}"},
            timeout=10,
        )
        r.raise_for_status()
        data = r.json()
        if search_type == "track":
            images = data["tracks"]["items"][0]["album"]["images"]
        else:
            images = data["albums"]["items"][0]["images"]
        art_url = images[0]["url"] if images else ""
        if art_url:
            print(f"[Spotify] Art: {art_url}")
        return art_url
    except (requests.RequestException, KeyError, IndexError):
        return ""


def spotify_get_album_art_url(artist, title, album=""):
    """Search Spotify with multiple fallback strategies."""
    # Clean parenthetical noise from ACRCloud titles e.g. "Revolver (Documentary)"
    import re as _re
    clean_title = _re.sub(r"\s*\(.*?\)\s*", " ", title).strip()

    # Strategy 1: exact artist + track
    queries = [f"artist:{artist} track:{title}"]

    # Strategy 2: cleaned title (without parenthetical)
    if clean_title != title:
        queries.append(f"artist:{artist} track:{clean_title}")

    # Strategy 3: album search if we have an album name
    if album:
        clean_album = _re.sub(r"\s*\(.*?\)\s*", " ", album).strip()
        queries.append(f"artist:{artist} album:{clean_album}")

    # Strategy 4: just artist + title as free text
    queries.append(f"{artist} {clean_title}")

    for i, q in enumerate(queries):
        search_type = "album" if "album:" in q else "track"
        result = _spotify_search(q, search_type)
        if result:
            if i > 0:
                print(f"[Spotify] Found on attempt {i+1}: {q}")
            return result
        print(f"[Spotify] No result for: {q}")

    return ""


# ═══════════════════════════════════════════════════════════════
# Audio Capture + ACRCloud (mirrors audio_capture + acrcloud_client)
# ═══════════════════════════════════════════════════════════════

AUDIO_SAMPLE_RATE = 44100
AUDIO_CHANNELS = 1
AUDIO_RECORD_SECS = 12


def record_audio():
    """Record audio from Mac microphone. Returns WAV bytes (16-bit signed LE mono).
    Raw audio is sent without normalization — ACRCloud works better on unprocessed signal."""
    if not HAS_AUDIO:
        print("[Audio] sounddevice not installed — run: pip3 install sounddevice")
        return None

    print(f"[Audio] Recording {AUDIO_RECORD_SECS}s from microphone...")
    try:
        audio = sd.rec(
            int(AUDIO_SAMPLE_RATE * AUDIO_RECORD_SECS),
            samplerate=AUDIO_SAMPLE_RATE,
            channels=AUDIO_CHANNELS,
            dtype="int16",
            blocking=True,
        )
        # Check it's not silent
        peak = int(np.max(np.abs(audio)))
        rms = np.sqrt(np.mean(audio.astype(np.float64) ** 2))
        print(f"[Audio] Recorded {len(audio)} samples, RMS={rms:.0f}, peak={peak}")

        if peak < 30:
            print("[Audio] Warning: audio is essentially silent — is music playing?")
            return None

        # Wrap in WAV format so ACRCloud can parse it — send raw, unnormalized audio
        wav_buf = io.BytesIO()
        pcm_bytes = audio.tobytes()
        data_size = len(pcm_bytes)
        bits_per_sample = 16
        byte_rate = AUDIO_SAMPLE_RATE * AUDIO_CHANNELS * bits_per_sample // 8
        block_align = AUDIO_CHANNELS * bits_per_sample // 8

        wav_buf.write(b"RIFF")
        wav_buf.write(struct.pack("<I", 36 + data_size))
        wav_buf.write(b"WAVE")
        wav_buf.write(b"fmt ")
        wav_buf.write(struct.pack("<I", 16))  # fmt chunk size
        wav_buf.write(struct.pack("<H", 1))   # PCM
        wav_buf.write(struct.pack("<H", AUDIO_CHANNELS))
        wav_buf.write(struct.pack("<I", AUDIO_SAMPLE_RATE))
        wav_buf.write(struct.pack("<I", byte_rate))
        wav_buf.write(struct.pack("<H", block_align))
        wav_buf.write(struct.pack("<H", bits_per_sample))
        wav_buf.write(b"data")
        wav_buf.write(struct.pack("<I", data_size))
        wav_buf.write(pcm_bytes)

        wav_bytes = wav_buf.getvalue()
        # Save to disk for debugging
        debug_path = SIM_DIR / "last_recording.wav"
        with open(debug_path, "wb") as f:
            f.write(wav_bytes)
        print(f"[Audio] WAV: {len(wav_bytes)} bytes — saved to {debug_path}")
        return wav_bytes
    except Exception as e:
        print(f"[Audio] Recording error: {e}")
        return None


def acrcloud_identify(audio_data):
    """Submit audio to ACRCloud for identification. Returns dict or None."""
    host = settings.get("acrcloud_host", "")
    access_key = settings.get("acrcloud_key", "")
    access_secret = settings.get("acrcloud_secret", "")

    if not host or not access_key or not access_secret:
        print("[ACRCloud] Credentials not configured — set them in Settings tab")
        return None

    timestamp = str(int(time.time()))
    string_to_sign = f"POST\n/v1/identify\n{access_key}\naudio\n1\n{timestamp}"
    signature = base64.b64encode(
        hmac.new(access_secret.encode(), string_to_sign.encode(), hashlib.sha1).digest()
    ).decode()

    url = f"https://{host}/v1/identify"

    try:
        r = requests.post(
            url,
            data={
                "access_key": access_key,
                "data_type": "audio",
                "signature_version": "1",
                "signature": signature,
                "timestamp": timestamp,
                "sample_bytes": str(len(audio_data)),
            },
            files={"sample": ("audio.wav", audio_data, "audio/wav")},
            timeout=15,
        )
        r.raise_for_status()
        data = r.json()
    except requests.RequestException as e:
        print(f"[ACRCloud] Request error: {e}")
        return None

    status_code = data.get("status", {}).get("code", -1)
    if status_code != 0:
        msg = data.get("status", {}).get("msg", "unknown")
        print(f"[ACRCloud] Status {status_code}: {msg}")
        return None

    try:
        music = data["metadata"]["music"][0]
        artist = music["artists"][0]["name"]
        title = music["title"]
        album = music.get("album", {}).get("name", "")
        print(f"[ACRCloud] Found: {artist} — {title} ({album})")

        # Extract Spotify ID if ACRCloud returned it (3rd Party ID Integration)
        spotify_album_id = ""
        spotify_track_id = ""
        for ext in music.get("external_metadata", {}).get("spotify", {}).get("album", {}).get("id", ""), :
            spotify_album_id = ext if ext else ""
        # Try structured path first
        sp = music.get("external_metadata", {}).get("spotify", {})
        if isinstance(sp, dict):
            if "album" in sp and isinstance(sp["album"], dict):
                spotify_album_id = sp["album"].get("id", "")
            if "track" in sp and isinstance(sp["track"], dict):
                spotify_track_id = sp["track"].get("id", "")

        if spotify_album_id:
            print(f"[ACRCloud] Spotify album ID: {spotify_album_id}")
        if spotify_track_id:
            print(f"[ACRCloud] Spotify track ID: {spotify_track_id}")

        return {
            "artist": artist, "title": title, "album": album,
            "spotify_album_id": spotify_album_id,
            "spotify_track_id": spotify_track_id,
        }
    except (KeyError, IndexError) as e:
        print(f"[ACRCloud] Parse error: {e}")
        return None


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


def vinyl_identify_and_display():
    """Full vinyl pipeline: record → ACRCloud → Spotify → dither → preview.
    Returns True on success, False on failure."""
    audio = record_audio()
    if not audio:
        return False

    result = acrcloud_identify(audio)
    if not result:
        print("[Vinyl] Could not identify track")
        return False

    app_state["artist"] = result["artist"]
    app_state["title"] = result["title"]
    app_state["album"] = result["album"]

    art_url = ""

    # Try 1: Use Spotify ID directly from ACRCloud (fastest, most accurate)
    if result.get("spotify_album_id") and settings.get("spotify_client_id"):
        token = spotify_get_token()
        if token:
            try:
                r = requests.get(
                    f"https://api.spotify.com/v1/albums/{result['spotify_album_id']}",
                    headers={"Authorization": f"Bearer {token}"},
                    timeout=10,
                )
                r.raise_for_status()
                images = r.json().get("images", [])
                if images:
                    art_url = images[0]["url"]
                    print(f"[Spotify] Art from album ID: {art_url}")
            except Exception as e:
                print(f"[Spotify] Album ID lookup failed: {e}")

    elif result.get("spotify_track_id") and settings.get("spotify_client_id"):
        token = spotify_get_token()
        if token:
            try:
                r = requests.get(
                    f"https://api.spotify.com/v1/tracks/{result['spotify_track_id']}",
                    headers={"Authorization": f"Bearer {token}"},
                    timeout=10,
                )
                r.raise_for_status()
                images = r.json().get("album", {}).get("images", [])
                if images:
                    art_url = images[0]["url"]
                    print(f"[Spotify] Art from track ID: {art_url}")
            except Exception as e:
                print(f"[Spotify] Track ID lookup failed: {e}")

    # Try 2: Fall back to search if no ID or lookup failed
    if not art_url and settings.get("spotify_client_id"):
        art_url = spotify_get_album_art_url(result["artist"], result["title"], result.get("album", ""))

    if art_url:
        app_state["art_url"] = art_url
        if not process_art_url(art_url):
            return False
        return True
    else:
        print("[Vinyl] No album art found")
        return False


def vinyl_recheck():
    """Re-identify vinyl to detect record changes. Only updates display if
    a *different* album is detected. Never downgrades on failure."""
    audio = record_audio()
    if not audio:
        return

    result = acrcloud_identify(audio)
    if not result:
        print("[Vinyl] Re-check: could not identify — keeping current art")
        return

    new_key = f"{result['artist']}|{result['album']}"
    current_key = f"{app_state['artist']}|{app_state['album']}"
    if new_key == current_key:
        print(f"[Vinyl] Re-check: same album ({result['album']}), no change")
        return

    print(f"[Vinyl] Re-check: NEW album detected! {result['artist']} — {result['album']}")
    # Run full identification pipeline to update display
    app_state["artist"] = result["artist"]
    app_state["title"] = result["title"]
    app_state["album"] = result["album"]

    art_url = ""

    if result.get("spotify_album_id") and settings.get("spotify_client_id"):
        token = spotify_get_token()
        if token:
            try:
                r = requests.get(
                    f"https://api.spotify.com/v1/albums/{result['spotify_album_id']}",
                    headers={"Authorization": f"Bearer {token}"},
                    timeout=10,
                )
                r.raise_for_status()
                images = r.json().get("images", [])
                if images:
                    art_url = images[0]["url"]
            except Exception as e:
                print(f"[Spotify] Album ID lookup failed: {e}")

    elif result.get("spotify_track_id") and settings.get("spotify_client_id"):
        token = spotify_get_token()
        if token:
            try:
                r = requests.get(
                    f"https://api.spotify.com/v1/tracks/{result['spotify_track_id']}",
                    headers={"Authorization": f"Bearer {token}"},
                    timeout=10,
                )
                r.raise_for_status()
                images = r.json().get("album", {}).get("images", [])
                if images:
                    art_url = images[0]["url"]
            except Exception as e:
                print(f"[Spotify] Track ID lookup failed: {e}")

    if not art_url and settings.get("spotify_client_id"):
        art_url = spotify_get_album_art_url(result["artist"], result["title"], result.get("album", ""))

    if art_url:
        app_state["art_url"] = art_url
        if process_art_url(art_url):
            print(f"[Vinyl] Re-check: display updated to {result['album']}")
        else:
            print("[Vinyl] Re-check: art download failed — keeping previous art")
    else:
        print("[Vinyl] Re-check: no art found for new album — keeping previous art")


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

        # If no art URL from Sonos, try Spotify
        if not art_url and settings.get("spotify_client_id"):
            print("[Poll] No Sonos art URL, trying Spotify...")
            art_url = spotify_get_album_art_url(info["artist"], info["title"])

        if art_url:
            app_state["art_url"] = art_url
            success = process_art_url(art_url)
            # If Sonos art failed, fall back to Spotify
            if not success and settings.get("spotify_client_id"):
                print("[Poll] Sonos art download failed, trying Spotify...")
                spotify_url = spotify_get_album_art_url(info["artist"], info["title"])
                if spotify_url:
                    app_state["art_url"] = spotify_url
                    process_art_url(spotify_url)
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
        "acrcloud_host": settings.get("acrcloud_host", ""),
        "acrcloud_key": settings.get("acrcloud_key", ""),
        "acrcloud_secret_set": bool(settings.get("acrcloud_secret", "")),
        "spotify_client_id": settings.get("spotify_client_id", ""),
        "spotify_client_secret_set": bool(settings.get("spotify_client_secret", "")),
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
    for key in ["sonos_ip", "acrcloud_host", "acrcloud_key", "spotify_client_id",
                 "google_photos_url"]:
        if key in data:
            settings[key] = data[key]
    if data.get("acrcloud_secret"):
        settings["acrcloud_secret"] = data["acrcloud_secret"]
    if data.get("spotify_client_secret"):
        settings["spotify_client_secret"] = data["spotify_client_secret"]
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
        if not art_url and settings.get("spotify_client_id"):
            art_url = spotify_get_album_art_url(info["artist"], info["title"])
        if art_url:
            app_state["art_url"] = art_url
            success = process_art_url(art_url)
            if not success and settings.get("spotify_client_id"):
                print("[Trigger] Sonos art failed, trying Spotify...")
                spotify_url = spotify_get_album_art_url(info["artist"], info["title"])
                if spotify_url:
                    app_state["art_url"] = spotify_url
                    process_art_url(spotify_url)

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
            "spotify_client_id": "",
            "spotify_client_secret": "",
            "acrcloud_host": "",
            "acrcloud_key": "",
            "acrcloud_secret": "",
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
