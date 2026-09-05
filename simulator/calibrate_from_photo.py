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
           chip_w=70, chip_gap=10, patch_w=300, keyline=2, halves=2)

# Left column runs black over white; the right column mirrors it, white over
# black. Must match pipelineShowCalibrationCard(). The mirroring is what puts
# both reference means on the pigment's centroid; changing it silently degrades
# the correction.
LEFT_HALVES = (0, 1)
RIGHT_HALVES = (1, 0)

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
        "CAL_CHIP_HALVES": CAL["halves"],
    }
    for name, value in expected.items():
        m = re.search(rf"#define\s+{name}\s+(\d+)", header)
        if not m or int(m.group(1)) != value:
            sys.exit(f"Calibration card geometry has changed in the firmware "
                     f"({name}={m.group(1) if m else '?'}, this script expects {value}). "
                     f"Update CAL in {Path(__file__).name}.")



# ═══════════════════════════════════════════════════════════
# Locating the panel in a photograph
# ═══════════════════════════════════════════════════════════

def _saturation_mask(arr):
    mx = arr.max(axis=2).astype(np.float64)
    mn = arr.min(axis=2).astype(np.float64)
    sat = np.divide(mx - mn, np.maximum(mx, 1e-6))
    return (sat > 0.28) & (mx > 45)


def locate_panel(img, debug=False):
    """
    Find the panel in a wider photo, using the card's own geometry.

    Rows 2-5 (green, blue, red, yellow) are the only strongly saturated things
    on the card, they share an x-range, and they are evenly spaced. Looking for
    exactly that signature is what stops it locking onto something else
    colourful in the room — which, photographing a frame on a desk, there
    invariably is.

    Returns a crop box in pixels, or None if it can't find a confident match.
    """
    from scipy import ndimage

    arr = np.asarray(img.convert("RGB"))
    h, w = arr.shape[:2]
    mask = _saturation_mask(arr)

    labels, n = ndimage.label(mask)
    if n == 0:
        return None

    min_area = (w * h) * 0.0008
    blobs = []
    for sl, idx in zip(ndimage.find_objects(labels), range(1, n + 1)):
        ys, xs = sl
        area = int((labels[sl] == idx).sum())
        if area < min_area:
            continue
        blobs.append(dict(x0=xs.start, x1=xs.stop, y0=ys.start, y1=ys.stop,
                          area=area, label=idx))
    if len(blobs) < 3:
        return None

    # Group blobs that share an x-range AND a similar width. The x-overlap
    # test alone is not enough: a picture frame around the panel is often
    # saturated too, and it fully contains the bands in x, so it happily joins
    # the group and blows the estimate up. Requiring a comparable width rejects
    # it, because the bands are a known fraction of the panel and the frame is
    # much wider.
    def x_overlap(a, b):
        lo = max(a["x0"], b["x0"]); hi = min(a["x1"], b["x1"])
        inter = max(0, hi - lo)
        return inter / max(1, min(a["x1"] - a["x0"], b["x1"] - b["x0"]))

    def similar_width(a, b):
        # Asymmetric on purpose. A band can come out NARROWER than its
        # neighbours when part of it dips below the saturation threshold —
        # measured on a RAW frame, one band read 429px against 633px for the
        # others, and a symmetric 0.7 bound dropped it, leaving too few blobs
        # to group. A band never comes out wider, so the upper bound stays
        # tight and still rejects a picture frame spanning the whole image.
        wa = a["x1"] - a["x0"]; wb = b["x1"] - b["x0"]
        return 0.5 <= (wa / max(1, wb)) <= 1.35

    row_pitch = CAL["row_h"] + CAL["row_gap"]
    first_sat_row = 2                      # green is the first saturated row
    last_sat_row = len(PALETTE_RGB) - 1    # yellow is the last

    fx0 = (CAL["margin_x"] + CAL["chip_w"] + CAL["chip_gap"]) / EPD_WIDTH
    fx1 = fx0 + CAL["patch_w"] / EPD_WIDTH
    fy0 = (CAL["margin_y"] + first_sat_row * row_pitch) / EPD_HEIGHT
    fy1 = (CAL["margin_y"] + last_sat_row * row_pitch + CAL["row_h"]) / EPD_HEIGHT
    expected_aspect = EPD_WIDTH / EPD_HEIGHT

    candidates = []
    for seed in blobs:
        group = [b for b in blobs
                 if x_overlap(seed, b) > 0.75 and similar_width(seed, b)]
        # Adjacent bands sometimes merge (red into yellow), so accept 3 as well
        # as the nominal 4.
        if len(group) < 3:
            continue

        group.sort(key=lambda b: b["y0"])
        gx0 = min(b["x0"] for b in group); gx1 = max(b["x1"] for b in group)
        gy0 = min(b["y0"] for b in group); gy1 = max(b["y1"] for b in group)

        # Work out which rows these blobs are BEFORE judging the aspect. Not
        # every band is always detected — a dim one can fall below the
        # saturation threshold — and assuming the group always spans rows 2..5
        # made the implied panel too short, so a correct match was rejected on
        # aspect. This has to agree with the span used for the homography
        # below; when it did not, valid photos were silently refused.
        heights = [b["y1"] - b["y0"] for b in group]
        unit = min(heights)
        spans = [max(1, int(round(hh / unit))) for hh in heights]
        rowi = first_sat_row
        for sp in spans[:-1]:
            rowi += sp
        bottom = rowi + spans[-1] - 1
        if bottom > last_sat_row:
            continue

        gfy0 = (CAL["margin_y"] + first_sat_row * row_pitch) / EPD_HEIGHT
        gfy1 = (CAL["margin_y"] + bottom * row_pitch + CAL["row_h"]) / EPD_HEIGHT

        panel_w = (gx1 - gx0) / (fx1 - fx0)
        panel_h = (gy1 - gy0) / (gfy1 - gfy0)
        if panel_w <= 0 or panel_h <= 0:
            continue
        aspect = panel_w / panel_h

        # The panel is 480x800. Selecting on aspect rather than raw area is what
        # makes this robust: a wrong grouping almost always gets the shape wrong.
        if not (0.75 * expected_aspect < aspect < 1.35 * expected_aspect):
            continue

        total = sum(b["area"] for b in group)
        candidates.append((total, group, gx0, gy0, panel_w, panel_h, aspect, bottom))

    if not candidates:
        if debug:
            print("  locate_panel: no blob group matched the card's geometry")
        return None

    total, group, gx0, gy0, panel_w, panel_h, aspect, bottom_row = max(
        candidates, key=lambda c: c[0])

    # Corner points of the topmost and bottommost saturated bands. Using the
    # extremes of the actual mask (not the bounding box) means a tilted or
    # perspective-distorted panel still yields true corners.
    def blob_corners(b):
        sub = labels[b["y0"]:b["y1"], b["x0"]:b["x1"]] == b["label"]
        ys, xs = np.nonzero(sub)
        xs = xs + b["x0"]; ys = ys + b["y0"]
        ssum = xs + ys
        sdif = xs - ys
        return dict(tl=(xs[np.argmin(ssum)], ys[np.argmin(ssum)]),
                    br=(xs[np.argmax(ssum)], ys[np.argmax(ssum)]),
                    tr=(xs[np.argmax(sdif)], ys[np.argmax(sdif)]),
                    bl=(xs[np.argmin(sdif)], ys[np.argmin(sdif)]))

    top = blob_corners(group[0])
    bottom = blob_corners(group[-1])

    # Work out which rows these blobs actually are, rather than assuming the
    # last one is yellow. Adjacent bands merge when the keyline between them is
    # not resolved, so a photo may show three blobs, not four — and assuming
    # the bottom blob is the last row then stretches the vertical mapping,
    # which drifts the sampling progressively down the card until the black and
    # white references swap over. That failure is silent and produces confident
    # nonsense, so it is worth the arithmetic.
    heights = [b["y1"] - b["y0"] for b in group]
    unit = min(heights)                       # a single, unmerged band
    spans = [max(1, int(round(h / unit))) for h in heights]

    row_index = first_sat_row
    for span_rows in spans[:-1]:
        row_index += span_rows
    bottom_row = row_index + spans[-1] - 1

    if bottom_row > last_sat_row:
        if debug:
            print(f"  locate_panel: inferred bottom row {bottom_row} beyond the "
                  f"card's {last_sat_row}; rejecting")
        return None

    # Their positions in panel coordinates are known exactly.
    x_patch_l = CAL["margin_x"] + CAL["chip_w"] + CAL["chip_gap"]
    x_patch_r = x_patch_l + CAL["patch_w"]
    y_top = CAL["margin_y"] + first_sat_row * row_pitch
    y_bot = CAL["margin_y"] + bottom_row * row_pitch + CAL["row_h"]

    if debug and bottom_row != last_sat_row:
        print(f"  locate_panel: {len(group)} blobs spanning rows "
              f"{first_sat_row}-{bottom_row} (some bands merged)")

    src = [top["tl"], top["tr"], bottom["br"], bottom["bl"]]
    dst = [(x_patch_l, y_top), (x_patch_r, y_top),
           (x_patch_r, y_bot), (x_patch_l, y_bot)]

    if debug:
        print(f"  locate_panel: {len(group)} saturated bands, aspect {aspect:.2f}, "
              f"corners {[(int(a), int(b)) for a, b in src]}")

    return src, dst


def _perspective_coeffs(dst_quad, src_quad):
    """
    Coefficients for PIL's PERSPECTIVE transform, which maps each OUTPUT pixel
    back to a source pixel. dst_quad is in output (panel) space, src_quad the
    matching points in the photo.
    """
    A, B = [], []
    for (xd, yd), (xs_, ys_) in zip(dst_quad, src_quad):
        A.append([xd, yd, 1, 0, 0, 0, -xs_ * xd, -xs_ * yd])
        B.append(xs_)
        A.append([0, 0, 0, xd, yd, 1, -ys_ * xd, -ys_ * yd])
        B.append(ys_)
    coeffs, *_ = np.linalg.lstsq(np.array(A, dtype=np.float64),
                                 np.array(B, dtype=np.float64), rcond=None)
    return coeffs


def rectify_panel(img, src_quad, dst_quad):
    """
    Warp the photographed panel onto a flat EPD_WIDTH x EPD_HEIGHT canvas.

    A plain rectangular crop cannot do this: a frame standing on a desk leans
    back, so the panel photographs foreshortened (measured 0.72 against the
    true 0.60 aspect) and every sampling box drifts progressively down the card.
    """
    coeffs = _perspective_coeffs(dst_quad, src_quad)
    return img.transform((EPD_WIDTH, EPD_HEIGHT), Image.PERSPECTIVE,
                         coeffs, Image.BICUBIC)


def row_regions_normalised(index):
    """
    Boxes for one row, as panel fractions. The left reference column is black
    over white and the right mirrors it; the pigment spans the full row height
    between them.
    Returns (reference boxes with their true index, pigment box).
    """
    x_left  = CAL["margin_x"]
    x_patch = x_left + CAL["chip_w"] + CAL["chip_gap"]
    x_right = x_patch + CAL["patch_w"] + CAL["chip_gap"]
    y0 = CAL["margin_y"] + index * (CAL["row_h"] + CAL["row_gap"])
    half = CAL["row_h"] // CAL["halves"]

    def box(x0, w, yy0, hh):
        return (x0 / EPD_WIDTH, yy0 / EPD_HEIGHT,
                (x0 + w) / EPD_WIDTH, (yy0 + hh) / EPD_HEIGHT)

    refs = []
    for x, order in ((x_left, LEFT_HALVES), (x_right, RIGHT_HALVES)):
        for q, idx in enumerate(order):
            refs.append((idx, box(x, CAL["chip_w"], y0 + q * half, half)))

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

    Black is the mean of the two black halves (top-left and bottom-right) and
    white the mean of the two white halves (bottom-left and top-right). Both
    means land on the pigment's own centroid in x and y, so a LINEAR
    illumination gradient affects references and pigment identically and drops
    out of the correction.

    That only holds while the gradient is linear. A lamp off to one side casts
    a shadow across one edge, and then the two-sided mean sits below the level
    the centre is actually lit at — which reads as the pigment reflecting more
    light than white, and looks exactly like a camera boosting saturation. So
    the per-side white levels come back too, for the caller to compare.
    """
    refs_n, pigment_n = row_regions_normalised(index)

    blacks, whites, boxes = [], [], []
    side = {"left": [], "right": []}
    for idx, fbox in refs_n:
        v, px = sample_box(img, fbox)
        boxes.append(px)
        (blacks if idx == 0 else whites).append(v)
        if idx == 1:
            side["left" if fbox[0] < 0.5 else "right"].append(v)

    pigment, pig_box = sample_box(img, pigment_n)
    boxes.append(pig_box)

    black = np.mean(blacks, axis=0)
    white = np.mean(whites, axis=0)
    sides = (float(np.mean(side["left"])), float(np.mean(side["right"])))
    return (black, pigment, white, sides), boxes


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
    ap.add_argument("--crop", help="Manual crop as x0,y0,x1,y1 (skips auto-detection)")
    ap.add_argument("--corners",
                    help="Manual panel corners as x,y of top-left, top-right, "
                         "bottom-right, bottom-left (8 numbers). Skips detection "
                         "and still corrects perspective, so it handles a frame "
                         "leaning back where --crop cannot.")
    ap.add_argument("--no-auto-crop", action="store_true",
                    help="Assume the photo is already cropped to the panel")
    ap.add_argument("--force", action="store_true",
                    help="Emit a palette even when the readings fail the "
                         "physical-plausibility check")
    ap.add_argument("--anchor", choices=["row", "none"], default="row",
                    help="row (default): correct each pigment against its own "
                         "black/white chips. none: report raw photographed values")
    args = ap.parse_args()

    verify_geometry_matches_firmware()

    img = Image.open(args.photo).convert("RGB")
    print(f"Photo: {img.size[0]}x{img.size[1]}")
    full = img

    if args.corners:
        nums = [float(v) for v in args.corners.replace(" ", "").split(",")]
        if len(nums) != 8:
            sys.exit("--corners needs 8 numbers: x,y for top-left, top-right, "
                     "bottom-right, bottom-left")
        src_quad = [(nums[i], nums[i + 1]) for i in range(0, 8, 2)]
        dst_quad = [(0, 0), (EPD_WIDTH, 0), (EPD_WIDTH, EPD_HEIGHT), (0, EPD_HEIGHT)]
        img = rectify_panel(img, src_quad, dst_quad)
        print(f"  rectified from given corners -> {img.size[0]}x{img.size[1]}")
    elif args.crop:
        box = tuple(int(v) for v in args.crop.split(","))
        img = img.crop(box)
        print(f"  cropped to {box} -> {img.size[0]}x{img.size[1]}")
    elif not args.no_auto_crop:
        found = locate_panel(img, debug=True)
        if found is None:
            sys.exit("Could not locate the calibration card in this photo.\n"
                     "Check the card is actually on screen (Debug -> Palette "
                     "Calibration Card) and that the whole panel is visible, "
                     "including the yellow row at the bottom.\n"
                     "You can also pass --crop x0,y0,x1,y1 manually.")
        src_quad, dst_quad = found
        img = rectify_panel(img, src_quad, dst_quad)
        print(f"  rectified to {img.size[0]}x{img.size[1]} "
              f"(perspective corrected)")

    raw, values, all_boxes, refs, side_levels = [], [], [], [], []
    for i in range(len(PALETTE_RGB)):
        (black, pigment, white, sides), boxes = sample_row(img, i)
        side_levels.append(sides)
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

    # ── Shot quality ──
    # These two numbers decide whether the result is worth acting on, so print
    # them before the palette rather than after.
    black_levels = [float(np.mean(k)) for k, _ in refs]
    white_levels = [float(np.mean(w)) for _, w in refs]
    spans = [w - k for k, w in zip(black_levels, white_levels)]
    min_span = min(spans)
    spread = max(white_levels) - min(white_levels)
    black_lift = max(black_levels) - min(black_levels)

    print(f"\n  Shot quality")
    print(f"    black->white span : {min_span:.0f}-{max(spans):.0f} levels")
    print(f"    white uniformity  : {min(white_levels):.0f}-{max(white_levels):.0f} "
          f"(spread {spread:.0f})")
    print(f"    black uniformity  : {min(black_levels):.0f}-{max(black_levels):.0f} "
          f"(lift {black_lift:.0f})")

    # ── Geometry check ──
    # The black reference must be darker than the white one in every row. If it
    # is not, the rectification is misaligned and every number below is
    # meaningless — better to stop than to explain away inverted readings.
    inverted = [PALETTE_NAMES[i] for i in range(len(PALETTE_RGB))
                if np.mean(refs[i][0]) >= np.mean(refs[i][1])]
    if inverted:
        sys.exit(
            f"Reference chips are inverted in row(s): {', '.join(inverted)}.\n"
            f"The black reference is reading brighter than the white one, which "
            f"means the panel was not located correctly — the sampling grid is "
            f"offset or stretched.\n"
            f"Check the --preview image. A photo taken from a steep angle, or "
            f"one where some colour bands are not clearly separated, can do this.")

    # ── Validity check the references cannot see ──
    # A reflective panel cannot bounce more light in any channel than its own
    # white pigment does. If a photographed pigment exceeds its row's white
    # reference, the camera is boosting saturation or contrast — and because
    # black and white are ACHROMATIC, the correction above is completely blind
    # to that. It will happily produce confident, wrong, over-saturated values.
    impossible = []
    for i, name in enumerate(PALETTE_NAMES):
        pig = raw[i]
        white_ref = refs[i][1]
        excess = pig - white_ref
        # White's own row compares a surface against itself; allow the noise floor.
        limit = 12 if i == 1 else 6
        over = [c for c, e in zip("RGB", excess) if e > limit]
        if over:
            impossible.append((name, "".join(over), int(max(excess))))

    # Left-vs-right white imbalance. The whole per-row correction assumes the
    # flanking chips bracket the centre; a side-lit panel breaks that, and it
    # is invisible to the row-to-row "white uniformity" figure above because it
    # runs ACROSS the card, not down it.
    imbalance = max(abs(l - r) / max(l, r, 1.0) for l, r in side_levels)
    side_lit = imbalance > 0.15

    problems = []
    if impossible:
        detail = ", ".join(f"{n} ({ch} by {e})" for n, ch, e in impossible)
        if side_lit:
            problems.append(
                f"Physically impossible readings: {detail}. The likely cause is "
                f"the lighting, not the camera: the white chips differ by "
                f"{imbalance * 100:.0f}% between the left and right of the card, "
                f"so one edge is in shadow. The correction assumes the two "
                f"flanking chips bracket the level the centre is lit at, and "
                f"when one is shadowed their mean sits too low — every pigment "
                f"then reads too bright. Re-shoot with the light square on "
                f"(diffuse light, or an overcast window in front of the panel, "
                f"not a lamp off to one side).")
        else:
            problems.append(
                f"Physically impossible readings: {detail}. The lighting is even "
                f"({imbalance * 100:.0f}% left-right), so this is the camera "
                f"enhancing saturation or contrast. The black and white "
                f"references are neutral, so this correction CANNOT detect or "
                f"undo it — the chromatic values below are biased and should not "
                f"be used. Shoot RAW (phone ProRAW or a manual camera app) to "
                f"get a frame without that processing.")
    elif side_lit:
        problems.append(
            f"The white chips differ by {imbalance * 100:.0f}% between the left "
            f"and right of the card, so the panel is lit from one side. The "
            f"readings survived the plausibility check, but light the panel more "
            f"evenly before trusting them to a few levels.")

    if min_span < 90:
        problems.append(
            f"Low contrast (span {min_span:.0f}). The correction has to scale by "
            f"{(216 - 16) / max(min_span, 1):.1f}x, which amplifies noise by the "
            f"same factor. Get more light on the panel, or move it closer so the "
            f"camera exposes for it rather than the background.")
    if black_lift > 20:
        problems.append(
            f"The blacks lift by {black_lift:.0f} across the card while the whites "
            f"barely move. That is veiling glare — light bouncing off the glass — "
            f"not a lighting gradient. Angle the panel away from any bright window "
            f"or lamp until the black patches look genuinely black.")
    if spread > 25:
        problems.append(
            f"Uneven lighting (white spread {spread:.0f}). Per-row anchoring "
            f"corrects for this, but evener light gives a better result.")

    for p_ in problems:
        print(f"\n  ! {p_}")
    if not problems:
        print("    -> good shot; these numbers are worth acting on")

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

    if impossible:
        print("\nNot emitting a palette: the readings above are physically "
              "impossible, so they would make the display worse, not better.\n"
              "Re-shoot with a camera whose colour processing can be disabled, "
              "or pass --force if you know what you are doing.")
        if not args.force:
            return

    print(f"\nPaste this over the PALETTE[] block in {CONFIG_H}:\n")
    print(format_palette_block(values))
    print("\nThen rebuild and update over the air (Debug -> Firmware Update), and "
          "re-shoot the card to confirm it converged.")
    print("The simulator picks the new values up automatically — it parses config.h.")


if __name__ == "__main__":
    main()
