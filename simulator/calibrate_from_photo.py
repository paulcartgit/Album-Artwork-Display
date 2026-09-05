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
CAL = dict(rows=6, margin_x=10, margin_y=20, row_h=120, row_gap=8,
           chip_w=70, chip_gap=10, patch_w=300, keyline=2, quarters=4)

# K / W / W / K down each reference column — must match QUARTER_IDX in
# image_pipeline.cpp. The ordering is what puts both reference means on the
# pigment's centroid; changing it silently degrades the correction.
QUARTER_IDX = (0, 1, 1, 0)

# Fraction of each region to average over (centre crop), so a slightly off
# crop or a non-square photo doesn't pull in the keyline or the white field.
SAMPLE_FRACTION = 0.55


def verify_geometry_matches_firmware():
    """Fail loudly if the firmware card layout changed but this script didn't."""
    header = (CONFIG_H.parent / "image_pipeline.h").read_text()
    expected = {
        "CAL_ROWS": CAL["rows"], "CAL_MARGIN_X": CAL["margin_x"],
        "CAL_MARGIN_Y": CAL["margin_y"], "CAL_ROW_H": CAL["row_h"],
        "CAL_ROW_GAP": CAL["row_gap"], "CAL_CHIP_W": CAL["chip_w"],
        "CAL_CHIP_GAP": CAL["chip_gap"], "CAL_PATCH_W": CAL["patch_w"],
        "CAL_CHIP_QUARTERS": CAL["quarters"],
    }
    for name, value in expected.items():
        m = re.search(rf"#define\s+{name}\s+(\d+)", header)
        if not m or int(m.group(1)) != value:
            sys.exit(f"Calibration card geometry has changed in the firmware "
                     f"({name}={m.group(1) if m else '?'}, this script expects {value}). "
                     f"Update CAL in {Path(__file__).name}.")


def row_regions_normalised(index):
    """
    Boxes for one row, as panel fractions. Reference columns are quartered
    K/W/W/K on each side; the pigment spans the full row height between them.
    Returns (reference boxes with their true index, pigment box).
    """
    x_left  = CAL["margin_x"]
    x_patch = x_left + CAL["chip_w"] + CAL["chip_gap"]
    x_right = x_patch + CAL["patch_w"] + CAL["chip_gap"]
    y0 = CAL["margin_y"] + index * (CAL["row_h"] + CAL["row_gap"])
    quarter = CAL["row_h"] // CAL["quarters"]

    def box(x0, w, yy0, hh):
        return (x0 / EPD_WIDTH, yy0 / EPD_HEIGHT,
                (x0 + w) / EPD_WIDTH, (yy0 + hh) / EPD_HEIGHT)

    refs = []
    for x in (x_left, x_right):
        for q, idx in enumerate(QUARTER_IDX):
            refs.append((idx, box(x, CAL["chip_w"], y0 + q * quarter, quarter)))

    pigment = box(x_patch, CAL["patch_w"], y0, CAL["row_h"])
    return refs, pigment


def sample_box(img, fbox):
    """Median colour of the centre of a normalised box. Median resists glare."""
    w, h = img.size
    fx0, fy0, fx1, fy1 = fbox
    cx, cy = (fx0 + fx1) / 2, (fy0 + fy1) / 2
    hw = (fx1 - fx0) * SAMPLE_FRACTION / 2
    hh = (fy1 - fy0) * SAMPLE_FRACTION / 2
    box = (int((cx - hw) * w), int((cy - hh) * h),
           int((cx + hw) * w), int((cy + hh) * h))
    region = np.asarray(img.crop(box), dtype=np.float64).reshape(-1, 3)
    return np.median(region, axis=0), box


def sample_row(img, index):
    """
    Returns ((black, pigment, white), boxes).

    Black is the mean of the four black quarters and white the mean of the four
    white quarters. Both means land on the pigment's own centroid in x and y, so
    a smooth illumination gradient or lens vignetting affects references and
    pigment identically and drops out of the correction.
    """
    refs_n, pigment_n = row_regions_normalised(index)

    blacks, whites, boxes = [], [], []
    for idx, fbox in refs_n:
        v, px = sample_box(img, fbox)
        boxes.append(px)
        (blacks if idx == 0 else whites).append(v)

    pigment, pig_box = sample_box(img, pigment_n)
    boxes.append(pig_box)

    black = np.mean(blacks, axis=0)
    white = np.mean(whites, axis=0)
    return (black, pigment, white), boxes


def anchor_row(pigment, photo_black, photo_white):
    """
    Per-channel affine sending this row's own black and white references onto
    the black and white already in config.h, applied to the pigment between
    them.

    Because the references sit millimetres away on both sides, this cancels
    exposure, white balance, illumination gradient and lens vignetting in one
    step — the things that otherwise make a photograph useless as a colour
    reference.
    """
    target_black = np.array(PALETTE_RGB[0], dtype=np.float64)
    target_white = np.array(PALETTE_RGB[1], dtype=np.float64)
    span = photo_white - photo_black
    if np.any(np.abs(span) < 8):
        return None
    scale = (target_white - target_black) / span
    return np.clip(target_black + (pigment - photo_black) * scale, 0, 255)


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
    ap.add_argument("--anchor", choices=["row", "none"], default="row",
                    help="row (default): correct each pigment against its own "
                         "black/white chips. none: report raw photographed values")
    args = ap.parse_args()

    verify_geometry_matches_firmware()

    img = Image.open(args.photo).convert("RGB")
    print(f"Photo: {img.size[0]}x{img.size[1]}")

    raw, values, all_boxes, refs = [], [], [], []
    for i in range(len(PALETTE_RGB)):
        (black, pigment, white), boxes = sample_row(img, i)
        raw.append(pigment)
        refs.append((black, white))
        all_boxes.append(boxes)

        if args.anchor == "none":
            values.append(pigment)
            continue
        corrected = anchor_row(pigment, black, white)
        if corrected is None:
            sys.exit(f"Row {i}: the black and white reference chips look nearly "
                     f"identical (black={black.round()}, white={white.round()}).\n"
                     f"That usually means the crop is wrong, the photo is badly "
                     f"exposed, or the panel hadn't finished refreshing.\n"
                     f"Re-shoot and use --preview to check where it sampled.")
        values.append(corrected)

    if args.preview:
        annotated = img.copy()
        draw = ImageDraw.Draw(annotated)
        for i, boxes in enumerate(all_boxes):
            for j, box in enumerate(boxes):
                colour = (255, 0, 255) if j == len(boxes) - 1 else (0, 200, 255)
                draw.rectangle(box, outline=colour, width=3)
            draw.text((boxes[-1][0] + 6, boxes[-1][1] + 6),
                      f"{i} {PALETTE_NAMES[i]}", fill=(255, 0, 255))
        annotated.save(args.preview)
        print(f"Sampling preview written to {args.preview} — check every box sits "
              f"inside its patch (magenta = pigment, cyan = reference chips)")

    print("\n  idx  name     photographed   refs K/W          calibrated   current     delta")
    print("  " + "-" * 78)
    total_delta = 0
    for i, name in enumerate(PALETTE_NAMES):
        r0, g0, b0 = (int(round(c)) for c in raw[i])
        r1, g1, b1 = (int(round(c)) for c in values[i])
        cr, cg, cb = PALETTE_RGB[i]
        kl = int(round(np.mean(refs[i][0])))
        wl = int(round(np.mean(refs[i][1])))
        delta = max(abs(r1 - cr), abs(g1 - cg), abs(b1 - cb))
        total_delta += delta
        flag = "  <-- large" if delta > 24 else ""
        print(f"  {i:3d}  {name:7s}  #{r0:02X}{g0:02X}{b0:02X}         "
              f"{kl:3d}/{wl:3d}           #{r1:02X}{g1:02X}{b1:02X}      "
              f"#{cr:02X}{cg:02X}{cb:02X}    {delta:3d}{flag}")

    # Illumination uniformity: how much the white references vary down the card.
    white_levels = [float(np.mean(w)) for _, w in refs]
    spread = max(white_levels) - min(white_levels)
    print(f"\n  Illumination across the card: white refs {min(white_levels):.0f}"
          f"-{max(white_levels):.0f} (spread {spread:.0f})")
    if spread > 25:
        print("  That is a large gradient. Per-row anchoring corrects for it, but "
              "more even lighting will give a better result.")

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
    print("\nThen rebuild and update over the air (Debug -> Firmware Update), and "
          "re-shoot the card to confirm it converged.")
    print("The simulator picks the new values up automatically — it parses config.h.")


if __name__ == "__main__":
    main()
