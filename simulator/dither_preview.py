#!/usr/bin/env python3
"""
Standalone dithering preview — test any image through the e-ink pipeline.

Usage:
    python dither_preview.py photo.jpg                     # opens preview window
    python dither_preview.py photo.jpg -o dithered.png     # save to file
    python dither_preview.py photo.jpg --compare           # side-by-side comparison
    python dither_preview.py https://i.scdn.co/image/...   # URL input
"""

import argparse
import io
import sys
import time
from pathlib import Path

import numpy as np
import requests
from PIL import Image, ImageDraw, ImageFont

# ─── Constants (must match firmware config.h) ───
EPD_WIDTH = 800
EPD_HEIGHT = 480

PALETTE = np.array([
    [0x00, 0x00, 0x00],  # 0 Black
    [0xFF, 0xFF, 0xFF],  # 1 White
    [0x60, 0x80, 0x50],  # 2 Green
    [0x50, 0x80, 0xB8],  # 3 Blue
    [0xA0, 0x20, 0x20],  # 4 Red
    [0xF0, 0xE0, 0x50],  # 5 Yellow
    [0xE0, 0x80, 0x30],  # 6 Orange
], dtype=np.float64)

PALETTE_NAMES = ["Black", "White", "Green", "Blue", "Red", "Yellow", "Orange"]
PALETTE_HEX = ["#000000", "#FFFFFF", "#608050", "#5080B8", "#A02020", "#F0E050", "#E08030"]


def nearest_palette_color(r, g, b):
    diff = PALETTE - np.array([r, g, b])
    dists = np.sum(diff ** 2, axis=1)
    return int(np.argmin(dists))


def dither_floyd_steinberg(img):
    """Floyd-Steinberg dithering to 7-color palette. Returns (dithered_image, index_array)."""
    w, h = img.size
    buf = np.array(img, dtype=np.float64)
    out = np.zeros((h, w), dtype=np.uint8)

    for y in range(h):
        for x in range(w):
            r = max(0.0, min(255.0, buf[y, x, 0]))
            g = max(0.0, min(255.0, buf[y, x, 1]))
            b = max(0.0, min(255.0, buf[y, x, 2]))

            ci = nearest_palette_color(r, g, b)
            out[y, x] = ci

            er = r - PALETTE[ci, 0]
            eg = g - PALETTE[ci, 1]
            eb = b - PALETTE[ci, 2]

            if x + 1 < w:
                buf[y, x + 1] += np.array([er, eg, eb]) * 7.0 / 16.0
            if y + 1 < h:
                if x - 1 >= 0:
                    buf[y + 1, x - 1] += np.array([er, eg, eb]) * 3.0 / 16.0
                buf[y + 1, x] += np.array([er, eg, eb]) * 5.0 / 16.0
                if x + 1 < w:
                    buf[y + 1, x + 1] += np.array([er, eg, eb]) * 1.0 / 16.0

    result = np.zeros((h, w, 3), dtype=np.uint8)
    for ci in range(len(PALETTE)):
        mask = out == ci
        result[mask] = PALETTE[ci].astype(np.uint8)

    return Image.fromarray(result, "RGB"), out


def scale_and_crop(img, target_w=EPD_WIDTH, target_h=EPD_HEIGHT):
    src_w, src_h = img.size
    scale_x = target_w / src_w
    scale_y = target_h / src_h
    scale = max(scale_x, scale_y)

    scaled_w = int(src_w * scale)
    scaled_h = int(src_h * scale)
    img = img.resize((scaled_w, scaled_h), Image.LANCZOS)

    left = (scaled_w - target_w) // 2
    top = (scaled_h - target_h) // 2
    return img.crop((left, top, left + target_w, top + target_h))


def color_stats(indices):
    """Print color usage statistics."""
    h, w = indices.shape
    total = h * w
    print(f"\n  Color usage ({w}×{h} = {total:,} pixels):")
    for i, name in enumerate(PALETTE_NAMES):
        count = int(np.sum(indices == i))
        pct = count / total * 100
        bar = "█" * int(pct / 2)
        print(f"    {PALETTE_HEX[i]} {name:7s}  {pct:5.1f}%  {bar}")


def load_image(source):
    """Load from file path or URL."""
    if source.startswith("http://") or source.startswith("https://"):
        print(f"Downloading {source}...")
        r = requests.get(source, timeout=15)
        r.raise_for_status()
        return Image.open(io.BytesIO(r.content)).convert("RGB")
    else:
        return Image.open(source).convert("RGB")


def main():
    parser = argparse.ArgumentParser(description="E-Ink Dithering Preview")
    parser.add_argument("input", help="Image file path or URL")
    parser.add_argument("-o", "--output", help="Save dithered image to file")
    parser.add_argument("--compare", action="store_true", help="Show side-by-side comparison")
    parser.add_argument("--no-show", action="store_true", help="Don't open preview window")
    parser.add_argument("--stats", action="store_true", help="Print color usage statistics")
    args = parser.parse_args()

    img = load_image(args.input)
    print(f"Input: {img.size[0]}×{img.size[1]}")

    scaled = scale_and_crop(img)
    print(f"Scaled/cropped to {EPD_WIDTH}×{EPD_HEIGHT}")

    print("Dithering (this takes a moment for 800×480)...")
    t0 = time.time()
    dithered, indices = dither_floyd_steinberg(scaled)
    dt = time.time() - t0
    print(f"Done in {dt:.1f}s")

    if args.stats:
        color_stats(indices)

    if args.output:
        dithered.save(args.output)
        print(f"Saved to {args.output}")

    if not args.no_show:
        if args.compare:
            # Side-by-side
            combined = Image.new("RGB", (EPD_WIDTH * 2 + 20, EPD_HEIGHT + 40), (0x22, 0x22, 0x22))
            combined.paste(scaled, (0, 0))
            combined.paste(dithered, (EPD_WIDTH + 20, 0))
            draw = ImageDraw.Draw(combined)
            draw.text((EPD_WIDTH // 2 - 30, EPD_HEIGHT + 8), "Original", fill=(0xAA, 0xAA, 0xAA))
            draw.text((EPD_WIDTH + 20 + EPD_WIDTH // 2 - 30, EPD_HEIGHT + 8),
                       "Dithered (7-color)", fill=(0xAA, 0xAA, 0xAA))
            combined.show("E-Ink Dither Comparison")
        else:
            dithered.show("E-Ink Preview (800×480, 7-color)")


if __name__ == "__main__":
    main()
