#!/usr/bin/env python3
"""
Standalone rendering preview — push any image through the real e-ink pipeline.

Uses eink.py, which mirrors the firmware's image_pipeline.cpp and dither.cpp,
with the palette and render profiles read from firmware/src/config.h.  What you
see here is what the panel will show.

Usage:
    python dither_preview.py photo.jpg                     # opens a preview window
    python dither_preview.py photo.jpg -o dithered.png     # save to a file
    python dither_preview.py photo.jpg --compare           # side-by-side
    python dither_preview.py photo.jpg --profile Punchy    # try a render profile
    python dither_preview.py photo.jpg --all-profiles      # compare all three
    python dither_preview.py https://i.scdn.co/image/...   # URL input
"""

import argparse
import io
import sys
import time

import numpy as np
import requests
from PIL import Image, ImageDraw

import eink
from firmware_config import (
    EPD_WIDTH, EPD_HEIGHT, PALETTE_NAMES, PALETTE_HEX,
    RENDER_PROFILES, DEFAULT_PROFILE,
)


def color_stats(indices, label=""):
    h, w = indices.shape
    total = h * w
    print(f"\n  Colour usage {label}({w}x{h} = {total:,} pixels):")
    for i, name in enumerate(PALETTE_NAMES):
        count = int(np.sum(indices == i))
        pct = count / total * 100
        print(f"    {PALETTE_HEX[i]} {name:7s}  {pct:5.1f}%  {'#' * int(pct / 2)}")


def load_image(source):
    if source.startswith(("http://", "https://")):
        print(f"Downloading {source}...")
        r = requests.get(source, timeout=15)
        r.raise_for_status()
        return Image.open(io.BytesIO(r.content)).convert("RGB")
    return Image.open(source).convert("RGB")


def resolve_profile(name):
    if name is None:
        return DEFAULT_PROFILE
    for i, p in enumerate(RENDER_PROFILES):
        if p["name"].lower() == name.lower():
            return i
    names = ", ".join(p["name"] for p in RENDER_PROFILES)
    sys.exit(f"Unknown profile '{name}'. Available: {names}")


def label_strip(images, labels):
    """Lay images out side by side with captions underneath."""
    gap, caption_h = 20, 40
    w = sum(im.width for im in images) + gap * (len(images) - 1)
    combined = Image.new("RGB", (w, EPD_HEIGHT + caption_h), (0x22, 0x22, 0x22))
    draw = ImageDraw.Draw(combined)
    x = 0
    for im, label in zip(images, labels):
        combined.paste(im, (x, 0))
        draw.text((x + im.width // 2 - len(label) * 3, EPD_HEIGHT + 12),
                  label, fill=(0xAA, 0xAA, 0xAA))
        x += im.width + gap
    return combined


def main():
    parser = argparse.ArgumentParser(description="E-Ink rendering preview")
    parser.add_argument("input", help="Image file path or URL")
    parser.add_argument("-o", "--output", help="Save the rendered image to a file")
    parser.add_argument("--compare", action="store_true",
                        help="Show the source next to the rendered output")
    parser.add_argument("--all-profiles", action="store_true",
                        help="Render with every profile side by side")
    parser.add_argument("--profile", help="Render profile name (default: Natural)")
    parser.add_argument("--artist", default="", help="Artist name for the overlay")
    parser.add_argument("--album", default="", help="Album name for the overlay")
    parser.add_argument("--bg-mode", type=int, default=2, choices=[0, 1, 2],
                        help="0 = solid, 1 = blur, 2 = auto (default)")
    parser.add_argument("--bg-style", type=int, default=0, choices=[0, 1],
                        help="0 = darken, 1 = wash out")
    parser.add_argument("--no-show", action="store_true", help="Don't open a preview window")
    parser.add_argument("--stats", action="store_true", help="Print colour usage statistics")
    args = parser.parse_args()

    img = load_image(args.input)
    print(f"Input: {img.size[0]}x{img.size[1]}")

    def render(profile_index):
        t0 = time.time()
        out, indices = eink.render(
            img, args.artist, args.album,
            bg_mode=args.bg_mode, bg_style=args.bg_style,
            profile_index=profile_index,
            show_text=bool(args.artist and args.album))
        print(f"  {RENDER_PROFILES[profile_index]['name']:8s} rendered in {time.time()-t0:.1f}s")
        return out, indices

    print(f"Rendering at {EPD_WIDTH}x{EPD_HEIGHT}...")

    if args.all_profiles:
        results = [render(i) for i in range(len(RENDER_PROFILES))]
        if args.stats:
            for (_, idx), p in zip(results, RENDER_PROFILES):
                color_stats(idx, f"[{p['name']}] ")
        combined = label_strip([r[0] for r in results],
                               [p["name"] for p in RENDER_PROFILES])
        if args.output:
            combined.save(args.output)
            print(f"Saved to {args.output}")
        if not args.no_show:
            combined.show("E-Ink render profiles")
        return

    profile_index = resolve_profile(args.profile)
    rendered, indices = render(profile_index)

    if args.stats:
        color_stats(indices)

    if args.output:
        rendered.save(args.output)
        print(f"Saved to {args.output}")

    if not args.no_show:
        if args.compare:
            source = img.resize((EPD_WIDTH, EPD_HEIGHT), Image.LANCZOS)
            label_strip([source, rendered],
                        ["Source", RENDER_PROFILES[profile_index]["name"]]).show(
                            "E-Ink comparison")
        else:
            rendered.show(f"E-Ink preview ({EPD_WIDTH}x{EPD_HEIGHT}, 6-colour)")


if __name__ == "__main__":
    main()
