#!/usr/bin/env python3
"""
A/B a cover against the panel: source art, what the simulator predicts, and a
photograph of the panel actually showing it.

The point is to judge the render — dithering as much as colour — against real
artwork, which is the only thing that actually matters. Absolute colour from a
webcam is unreliable (see calibrate_from_photo.py), but structure, banding,
posterisation, noise in flat areas and detail retention all read fine.

Usage:
  python panel_compare.py <historyFile.jpg> [more.jpg ...]

Requires /tmp/panel_geom.json, written by capturing the calibration card, and
the frame not to have moved since.
"""

import json
import os
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
import requests
from PIL import Image, ImageDraw

import eink
import calibrate_from_photo as cal
from firmware_config import EPD_WIDTH, EPD_HEIGHT, RENDER_PROFILES

# Override with:  NOWPLAYING=http://192.168.1.50 python panel_probe.py
DEVICE = os.environ.get("NOWPLAYING", "http://nowplaying.local")
SNAP = Path(__file__).resolve().parent.parent / "tools" / "Snap.app"
GEOM = Path("/tmp/panel_geom.json")
OUT = Path("/tmp/compare")


def device_settings():
    return requests.get(f"{DEVICE}/api/settings", timeout=10).json()


def _log(n=6):
    try:
        return [e["m"] for e in requests.get(f"{DEVICE}/api/log", timeout=5).json()[:n]]
    except requests.RequestException:
        return []


def show_on_panel(fname):
    """
    Ask the device to display one cover and wait until the panel has settled.

    Waiting on "a hold is active" is not enough: a hold left over from the
    calibration card is already active, so the wait returns instantly and the
    photograph catches the previous image. Wait for this file's own log line.
    """
    marker = f"Showing {fname} on request"
    r = requests.post(f"{DEVICE}/api/history/show", data={"f": fname}, timeout=10)
    r.raise_for_status()

    for _ in range(50):
        time.sleep(3)
        lines = _log()
        if any(marker in line for line in lines):
            # The log line is written before the ~20s refresh begins; the hold
            # is set immediately after it finishes.
            for _ in range(30):
                time.sleep(3)
                try:
                    st = requests.get(f"{DEVICE}/api/status", timeout=5).json()
                except requests.RequestException:
                    continue  # the device stops answering mid-refresh
                if "display_hold_sec" in st and st["display_hold_sec"] > 1700:
                    time.sleep(2)
                    return True
            return False
    return False


def capture(path):
    path = Path(path)
    if path.exists():
        path.unlink()
    subprocess.run(["open", "-a", str(SNAP), "--args", str(path), "3"], check=True)
    for _ in range(60):
        if path.exists():
            time.sleep(1)
            return True
        time.sleep(1)
    return False


def normalise_white_balance(img, geom):
    """
    Undo the webcam's auto white balance using the scene around the panel.

    AWB is driven by scene content, so it swings whenever the artwork changes —
    two captures of the same cover came back with visibly different casts. The
    surroundings (desk, wall) don't change between shots and are near neutral,
    so grey-worlding on everything OUTSIDE the panel gives a per-shot reference
    that makes captures comparable with each other.
    """
    arr = np.asarray(img, dtype=np.float64)
    h, w = arr.shape[:2]

    xs = [p[0] for p in geom["src"]]
    ys = [p[1] for p in geom["src"]]
    mask = np.ones((h, w), dtype=bool)
    pad = 40
    y0 = max(0, int(min(ys)) - pad); y1 = min(h, int(max(ys)) + pad)
    x0 = max(0, int(min(xs)) - pad); x1 = min(w, int(max(xs)) + pad)
    mask[y0:y1, x0:x1] = False

    surround = arr[mask]
    # Mid-tones only: clipped highlights and deep shadows carry no colour info
    lum = surround.mean(axis=1)
    keep = (lum > 40) & (lum < 240)
    if keep.sum() < 1000:
        return img, None
    ref = surround[keep].mean(axis=0)
    gain = ref.mean() / np.maximum(ref, 1e-6)
    balanced = np.clip(arr * gain, 0, 255).astype(np.uint8)
    return Image.fromarray(balanced), gain


def rectified_photo(path, report=False):
    geom = json.loads(GEOM.read_text())
    img = Image.open(path).convert("RGB")
    img, gain = normalise_white_balance(img, geom)
    if report and gain is not None:
        print(f"  white-balance gain applied: "
              f"R{gain[0]:.3f} G{gain[1]:.3f} B{gain[2]:.3f}")
    return cal.rectify_panel(img, [tuple(p) for p in geom["src"]],
                             [tuple(p) for p in geom["dst"]])


def label(img, text, height=28):
    out = Image.new("RGB", (img.width, img.height + height), (24, 24, 24))
    out.paste(img, (0, height))
    ImageDraw.Draw(out).text((6, 8), text, fill=(230, 230, 230))
    return out


def main():
    if not GEOM.exists():
        sys.exit("No panel geometry. Show the calibration card and capture it first.")
    files = sys.argv[1:]
    if not files:
        sys.exit(__doc__)

    OUT.mkdir(exist_ok=True)
    settings = device_settings()
    prof = int(settings.get("render_profile", 1))
    print(f"Device: profile={RENDER_PROFILES[prof]['name']}, "
          f"bg_mode={settings['bg_mode']}, bg_style={settings['bg_style']}, "
          f"track_info={settings['show_track_info']}")

    history = {e["f"]: e for e in requests.get(f"{DEVICE}/api/history", timeout=10).json()}

    for fname in files:
        meta = history.get(fname, {})
        title = f"{meta.get('a','?')} - {meta.get('al','') or meta.get('t','')}"
        print(f"\n=== {fname}  {title} ===")

        src_path = OUT / f"src_{fname}"
        if not src_path.exists():
            data = requests.get(f"{DEVICE}/api/history/image", params={"f": fname},
                                timeout=20).content
            src_path.write_bytes(data)
        source = Image.open(src_path).convert("RGB")
        print(f"  source {source.size[0]}x{source.size[1]}")

        print("  rendering in simulator...")
        t0 = time.time()
        predicted, _ = eink.render(
            source, "", "",
            bg_mode=int(settings["bg_mode"]), bg_style=int(settings["bg_style"]),
            profile_index=prof, show_text=bool(settings["show_track_info"]))
        print(f"  rendered in {time.time()-t0:.1f}s")

        print("  displaying on panel...")
        if not show_on_panel(fname):
            print("  ! panel did not settle; skipping")
            continue

        photo_path = OUT / f"photo_{fname}"
        if not capture(photo_path):
            print("  ! capture failed; skipping")
            continue
        actual = rectified_photo(photo_path, report=True)

        # The camera resolves the panel at roughly half its native resolution,
        # so it optically integrates the dither pattern — which is also what the
        # eye does at normal viewing distance. Comparing a pixel-exact
        # prediction against an integrated photograph makes the dither look far
        # noisier than it is, so show the prediction both ways.
        geom = json.loads(GEOM.read_text())
        xs = [p[0] for p in geom["src"]]
        ys = [p[1] for p in geom["src"]]
        eff_w = max(8, int(max(xs) - min(xs)))
        eff_h = max(8, int(max(ys) - min(ys)))
        integrated = (predicted
                      .resize((eff_w, eff_h), Image.BOX)
                      .resize((EPD_WIDTH, EPD_HEIGHT), Image.LANCZOS))

        src_fit = source.resize((EPD_WIDTH, EPD_HEIGHT), Image.LANCZOS)
        panels = [label(src_fit, "SOURCE (scaled)"),
                  label(predicted, "PREDICTION (pixel-exact)"),
                  label(integrated, f"PREDICTION (at {eff_w}x{eff_h} viewing res)"),
                  label(actual, "PANEL (photographed)")]
        gap = 12
        combo = Image.new("RGB",
                          (sum(p.width for p in panels) + gap * (len(panels) - 1),
                           panels[0].height), (24, 24, 24))
        x = 0
        for p in panels:
            combo.paste(p, (x, 0))
            x += p.width + gap
        combo_path = OUT / f"compare_{Path(fname).stem}.png"
        combo.save(combo_path)
        print(f"  -> {combo_path}")


if __name__ == "__main__":
    main()
