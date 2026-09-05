#!/usr/bin/env python3
"""
Build and push test frames straight to the panel, bypassing the pipeline.

Two problems this solves.

First, colour. E-ink is reflective, so what a camera records is pigment
reflectance times whatever light is falling on the panel. The panel and the
desk around it are lit differently (the panel faces a window; the room is
warmer), so white-balancing a photo against the surroundings corrects for the
wrong illuminant. Every frame here therefore carries black and white reference
patches ON the panel, beside the artwork, under the same light.

Second, spatial variation. Tiling the same image at several positions shows
whether the panel or the lighting varies across its surface, and whether the
dither behaves differently in different places — invisible with one image
filling the frame.

Frames are pushed as packed 4bpp indices to /api/display/raw, so what the panel
shows is exactly what was computed here, with no firmware pipeline in between.
That also makes this a direct test of the simulator's port: identical input,
identical expected output.
"""

import json
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
import requests
from PIL import Image

import eink
import calibrate_from_photo as cal
from firmware_config import EPD_WIDTH, EPD_HEIGHT, PALETTE_RGB

DEVICE = "http://192.168.86.67"
SNAP = Path(__file__).resolve().parent.parent / "tools" / "Snap.app"
GEOM = Path("/tmp/panel_geom.json")

# Reference strip down each side, mirrored K/W over W/K as on the calibration
# card, so the two means land on the artwork's own centroid.
REF_W = 40
REF_GAP = 6


def pack(indices):
    """HxW palette indices -> packed 4bpp, high nibble first."""
    flat = np.asarray(indices, dtype=np.uint8).ravel()
    if flat.size % 2:
        raise ValueError("odd pixel count")
    return ((flat[0::2] << 4) | (flat[1::2] & 0x0F)).astype(np.uint8).tobytes()


def push(indices):
    body = pack(indices)
    r = requests.post(f"{DEVICE}/api/display/raw", data=body,
                      headers={"Content-Type": "application/octet-stream"},
                      timeout=60)
    r.raise_for_status()
    return r.json()


def hold_remaining():
    try:
        return requests.get(f"{DEVICE}/api/status", timeout=5).json().get("display_hold_sec", 0)
    except requests.RequestException:
        return None      # the device stops answering while the panel refreshes


def push_and_wait(indices, timeout=240):
    """
    Push a frame and wait until THAT frame is actually on the panel.

    A hold left over from the previous push is still counting down, so "a hold
    is active" says nothing. Watch for the hold to jump back up, which only
    happens when the new refresh completes. Getting this wrong photographs the
    previous frame, or catches the panel mid-cycle with black and white
    reference patches reading the same value.
    """
    before = hold_remaining() or 0
    push(indices)

    deadline = time.time() + timeout
    while time.time() < deadline:
        time.sleep(3)
        now = hold_remaining()
        if now is None:
            continue
        if now > before + 30:        # reset, so this frame has landed
            time.sleep(2)
            return True
    return False


def capture(path, warmup=3):
    path = Path(path)
    if path.exists():
        path.unlink()
    subprocess.run(["open", "-a", str(SNAP), "--args", str(path), str(warmup)], check=True)
    for _ in range(60):
        if path.exists():
            time.sleep(1)
            return True
        time.sleep(1)
    return False


def rectify(path):
    geom = json.loads(GEOM.read_text())
    return cal.rectify_panel(Image.open(path).convert("RGB"),
                             [tuple(p) for p in geom["src"]],
                             [tuple(p) for p in geom["dst"]])


def with_references(indices):
    """Stamp mirrored black/white reference columns down both edges."""
    out = np.array(indices, dtype=np.uint8)
    h = EPD_HEIGHT
    half = h // 2
    out[:half, :REF_W] = 0            # left top: black
    out[half:, :REF_W] = 1            # left bottom: white
    out[:half, -REF_W:] = 1           # right top: white
    out[half:, -REF_W:] = 0           # right bottom: black
    return out


def tiled(source, rows=3, cols=2, profile_index=1, references=True):
    """
    The same cover dithered into a grid of tiles.

    Each tile is dithered independently at its own size, so this also shows how
    the dither behaves at different scales.
    """
    inner_x = REF_W + REF_GAP
    avail_w = EPD_WIDTH - 2 * inner_x
    tile_w = avail_w // cols
    tile_h = EPD_HEIGHT // rows

    canvas = np.ones((EPD_HEIGHT, EPD_WIDTH), dtype=np.uint8)  # white field
    for r in range(rows):
        for c in range(cols):
            tile = source.resize((tile_w, tile_h), Image.LANCZOS)
            _, idx = eink.dither(np.asarray(tile), profile_index)
            y0 = r * tile_h
            x0 = inner_x + c * tile_w
            canvas[y0:y0 + tile_h, x0:x0 + tile_w] = idx
    return with_references(canvas) if references else canvas


def full(source, profile_index=1, references=True):
    canvas, _ = eink.compose(source, "", "", bg_mode=2, bg_style=1,
                             profile_index=profile_index, show_text=False)
    _, idx = eink.dither(canvas, profile_index)
    return with_references(idx) if references else idx


def measure_references(photo):
    """
    Per-half black/white references, then the affine that maps them onto the
    known pigment values. Returns (gain, offset) per channel, or None.
    """
    a = np.asarray(photo, dtype=np.float64)
    h = EPD_HEIGHT
    half = h // 2
    inset = slice(6, REF_W - 6)

    blacks = np.concatenate([a[:half, inset].reshape(-1, 3),
                             a[half:, EPD_WIDTH - REF_W + 6:EPD_WIDTH - 6].reshape(-1, 3)])
    whites = np.concatenate([a[half:, inset].reshape(-1, 3),
                             a[:half, EPD_WIDTH - REF_W + 6:EPD_WIDTH - 6].reshape(-1, 3)])
    k = np.median(blacks, axis=0)
    w = np.median(whites, axis=0)
    span = w - k
    if np.any(span < 20):
        return None, k, w
    tb = np.array(PALETTE_RGB[0], float)
    tw = np.array(PALETTE_RGB[1], float)
    gain = (tw - tb) / span
    return (gain, tb - k * gain), k, w


def corrected(photo):
    res, k, w = measure_references(photo)
    if res is None:
        return None, k, w
    gain, offset = res
    a = np.asarray(photo, dtype=np.float64)
    return Image.fromarray(np.clip(a * gain + offset, 0, 255).astype(np.uint8)), k, w


def variants(source, settings, rows=3, cols=2, references=True):
    """
    The same image dithered several different ways, tiled onto one frame.

    This is the whole reason for pushing raw frames. Comparing dither changes by
    flashing, displaying and photographing them one at a time takes ~40s per
    variant and compares shots taken under different light. Here every variant
    lands on the same panel, in the same photograph, under the same illuminant —
    so differences between tiles are differences in the algorithm and nothing
    else.

    settings: list of (label, kwargs) applied to eink module globals.
    """
    inner_x = REF_W + REF_GAP
    avail_w = EPD_WIDTH - 2 * inner_x
    tile_w = avail_w // cols
    tile_h = EPD_HEIGHT // rows

    canvas = np.ones((EPD_HEIGHT, EPD_WIDTH), dtype=np.uint8)
    tile = source.resize((tile_w, tile_h), Image.LANCZOS)
    arr = np.asarray(tile)

    for n, (label, opts) in enumerate(settings[:rows * cols]):
        saved = {k: getattr(eink, k) for k in opts}
        for k, v in opts.items():
            setattr(eink, k, v)
        try:
            _, idx = eink.dither(arr, opts.get("profile_index", 1))
        finally:
            for k, v in saved.items():
                setattr(eink, k, v)

        r, c = divmod(n, cols)
        y0 = r * tile_h
        x0 = inner_x + c * tile_w
        canvas[y0:y0 + tile_h, x0:x0 + tile_w] = idx
        print(f"  tile {n} ({r},{c}): {label}")

    return with_references(canvas) if references else canvas
