#!/usr/bin/env python3
"""
Regenerate the calibrated palette from a photograph of the device.

Why this exists
---------------
The dither matches and diffuses error against PALETTE[] in firmware/src/config.h
— what the pigments actually LOOK like, not what they are called. Those numbers
are the single most important input to output quality, and the only honest way
to obtain them is to photograph the panel.

Doing that by eye, one hex value at a time, is slow and hard to converge. This
turns it into: show the calibration card, take one photo, run this.

Usage
-----
  1. Web portal -> Debug -> "Palette Calibration Card", wait ~15s for the refresh
  2. Photograph the panel square-on in even, indirect light.
     Crop roughly to the edges of the visible panel area. Avoid flash,
     avoid glare, avoid coloured light.
  3. python calibrate_from_photo.py photo.jpg

     --check          compare against the current palette, don't emit new values
     --preview out.png  write an image showing exactly where it sampled
     --anchor keep|photo
                      keep  (default) preserve the existing black and white
                            points and correct the four chromatic pigments
                            relative to them
                      photo use the photographed black/white directly

About the anchoring
-------------------
A photo cannot recover absolute pigment RGB — exposure and white balance are
unknown. What it CAN recover reliably is the pigments *relative to each other*.
The default maps the photographed black and white patches onto the black and
white values already in config.h, then applies that same per-channel affine
transform to the other four. That removes most of the camera's exposure and
white-balance influence while preserving the black and white points you already
settled on.

This is an approximation, not a colorimeter: a two-point affine correction
cannot undo a camera's tone curve, so expect a residual of roughly +/-10 levels
per channel. Against a synthetic photo with a strong warm cast, lifted blacks
and gamma 0.85, it recovers the four chromatic pigments to within 6-9 levels.
That is well inside useful, but it means the workflow is iterative: apply the
values, re-flash, re-shoot the card, and confirm the reported delta has shrunk.
Two rounds is normally enough.
"""

import argparse
import re
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

from firmware_config import CONFIG_H, PALETTE_RGB, PALETTE_NAMES, EPD_WIDTH, EPD_HEIGHT

# Must match the CAL_* constants in firmware/src/image_pipeline.h
CAL = dict(margin_x=30, margin_y=40, patch_w=190, patch_h=220,
           gutter_x=40, gutter_y=30, cols=2, rows=3)

# Fraction of each patch to average over (centre crop, so modest misalignment
# or a slightly off-square photo doesn't pull in gutter white).
SAMPLE_FRACTION = 0.5


def verify_geometry_matches_firmware():
    """Fail loudly if the firmware card layout changed but this script didn't."""
    header = (CONFIG_H.parent / "image_pipeline.h").read_text()
    expected = {
        "CAL_MARGIN_X": CAL["margin_x"], "CAL_MARGIN_Y": CAL["margin_y"],
        "CAL_PATCH_W": CAL["patch_w"], "CAL_PATCH_H": CAL["patch_h"],
        "CAL_GUTTER_X": CAL["gutter_x"], "CAL_GUTTER_Y": CAL["gutter_y"],
        "CAL_COLS": CAL["cols"], "CAL_ROWS": CAL["rows"],
    }
    for name, value in expected.items():
        m = re.search(rf"#define\s+{name}\s+(\d+)", header)
        if not m or int(m.group(1)) != value:
            sys.exit(f"Calibration card geometry has changed in the firmware "
                     f"({name}={m.group(1) if m else '?'}, this script expects {value}). "
                     f"Update CAL in {Path(__file__).name}.")


def patch_rect_normalised(index):
    """Patch bounding box as fractions of the panel, for palette index 0..5."""
    col = index % CAL["cols"]
    row = index // CAL["cols"]
    x0 = CAL["margin_x"] + col * (CAL["patch_w"] + CAL["gutter_x"])
    y0 = CAL["margin_y"] + row * (CAL["patch_h"] + CAL["gutter_y"])
    return (x0 / EPD_WIDTH, y0 / EPD_HEIGHT,
            (x0 + CAL["patch_w"]) / EPD_WIDTH, (y0 + CAL["patch_h"]) / EPD_HEIGHT)


def sample(img, index):
    """Median colour of the centre of one patch. Median resists glare specks."""
    w, h = img.size
    fx0, fy0, fx1, fy1 = patch_rect_normalised(index)
    cx, cy = (fx0 + fx1) / 2, (fy0 + fy1) / 2
    half_w = (fx1 - fx0) * SAMPLE_FRACTION / 2
    half_h = (fy1 - fy0) * SAMPLE_FRACTION / 2

    box = (int((cx - half_w) * w), int((cy - half_h) * h),
           int((cx + half_w) * w), int((cy + half_h) * h))
    region = np.asarray(img.crop(box), dtype=np.float64).reshape(-1, 3)
    return np.median(region, axis=0), box


def anchor_to_existing(samples):
    """
    Per-channel affine map sending the photographed black/white onto the black
    and white already in config.h, applied to every patch.
    """
    target_black = np.array(PALETTE_RGB[0], dtype=np.float64)
    target_white = np.array(PALETTE_RGB[1], dtype=np.float64)
    photo_black, photo_white = samples[0], samples[1]

    span = photo_white - photo_black
    if np.any(np.abs(span) < 8):
        sys.exit("The black and white patches look nearly identical in this photo.\n"
                 "That usually means the crop is wrong, the photo is badly "
                 "over/under-exposed, or the panel hadn't finished refreshing.\n"
                 "Re-shoot with even lighting and try again (use --preview to check "
                 "where it sampled).")

    scale = (target_white - target_black) / span
    return [np.clip(target_black + (s - photo_black) * scale, 0, 255) for s in samples]


def sanity_check(values):
    """Catch an obviously wrong crop before the user pastes nonsense into config.h."""
    warnings = []
    lum = [0.299 * v[0] + 0.587 * v[1] + 0.114 * v[2] for v in values]
    if lum[0] > lum[1]:
        warnings.append("the 'black' patch is brighter than the 'white' patch — "
                        "the photo may be rotated or cropped wrongly")
    if lum[5] < lum[4]:
        warnings.append("yellow is darker than red — unusual; check the patch order")
    if values[3][2] < values[3][0]:
        warnings.append("the 'blue' patch has more red than blue — check the crop")
    if values[4][0] < values[4][2]:
        warnings.append("the 'red' patch has more blue than red — check the crop")
    return warnings


def format_palette_block(values):
    comments = ["Black", "White", "Green", "Blue", "Red", "Yellow"]
    lines = ["static const PaletteColor PALETTE[EPD_COLORS] = {"]
    for i, v in enumerate(values):
        r, g, b = (int(round(c)) for c in v)
        lines.append(f"    {{0x{r:02X}, 0x{g:02X}, 0x{b:02X}, {i}}}, "
                     f"// {comments[i]}")
    lines.append("};")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("photo", help="Photo of the calibration card, cropped to the panel")
    ap.add_argument("--check", action="store_true",
                    help="Report the difference from the current palette, don't emit new values")
    ap.add_argument("--preview", help="Write an image showing where it sampled")
    ap.add_argument("--anchor", choices=["keep", "photo"], default="keep",
                    help="keep (default): preserve the existing black/white points")
    args = ap.parse_args()

    verify_geometry_matches_firmware()

    img = Image.open(args.photo).convert("RGB")
    print(f"Photo: {img.size[0]}x{img.size[1]}")

    raw, boxes = [], []
    for i in range(len(PALETTE_RGB)):
        colour, box = sample(img, i)
        raw.append(colour)
        boxes.append(box)

    if args.preview:
        annotated = img.copy()
        draw = ImageDraw.Draw(annotated)
        for i, box in enumerate(boxes):
            draw.rectangle(box, outline=(255, 0, 255), width=4)
            draw.text((box[0] + 8, box[1] + 8), f"{i} {PALETTE_NAMES[i]}",
                      fill=(255, 0, 255))
        annotated.save(args.preview)
        print(f"Sampling preview written to {args.preview} — "
              f"check every box sits inside its patch")

    values = anchor_to_existing(raw) if args.anchor == "keep" else raw

    print("\n  idx  name     photographed      ->  calibrated     current      delta")
    print("  " + "-" * 72)
    total_delta = 0
    for i, name in enumerate(PALETTE_NAMES):
        r0, g0, b0 = (int(round(c)) for c in raw[i])
        r1, g1, b1 = (int(round(c)) for c in values[i])
        cr, cg, cb = PALETTE_RGB[i]
        delta = max(abs(r1 - cr), abs(g1 - cg), abs(b1 - cb))
        total_delta += delta
        flag = "  <-- large" if delta > 24 else ""
        print(f"  {i:3d}  {name:7s}  #{r0:02X}{g0:02X}{b0:02X}"
              f"          ->  #{r1:02X}{g1:02X}{b1:02X}      "
              f"#{cr:02X}{cg:02X}{cb:02X}     {delta:3d}{flag}")

    for w in sanity_check(values):
        print(f"\n  WARNING: {w}")

    print(f"\n  Total absolute difference from the current palette: {total_delta}")
    if total_delta <= 6 * 8:
        print("  The current palette already matches this photo closely — "
              "no change needed.")
    elif args.check:
        print("  Re-run without --check to emit a replacement PALETTE[] block.")

    if args.check:
        return

    print(f"\nPaste this over the PALETTE[] block in {CONFIG_H}:\n")
    print(format_palette_block(values))
    print("\nThen rebuild and re-flash (or upload firmware.bin via Debug -> "
          "Firmware Update), and re-shoot the card to confirm it converged.")
    print("The simulator picks the new values up automatically — "
          "it parses config.h.")


if __name__ == "__main__":
    main()
