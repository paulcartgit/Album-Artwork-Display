#!/usr/bin/env python3
"""
Strategies for filling a 480x800 portrait panel with square album art.

The panel is much taller than the artwork is. Fitting the square to the width
covers 60% of the screen and leaves a 320px band; cover-cropping fills it but
discards 40% of the cover horizontally, which on album art usually means
cutting through the typography. Neither is obviously right, so build them all
and look.
"""

import numpy as np
from PIL import Image

import eink
from firmware_config import EPD_WIDTH, EPD_HEIGHT

FIT, COVER, SMART, BLEED, ADAPTIVE = range(5)
NAMES = {FIT:"Fit (current)", COVER:"Cover crop", SMART:"Smart crop",
         BLEED:"Bleed", ADAPTIVE:"Adaptive"}

# Fraction of the width a cover-crop would discard, and how much detail is
# allowed to sit in it before cropping is judged too destructive.
CROP_LOSS = 1.0 - EPD_WIDTH / EPD_HEIGHT          # 0.40 for this panel
CUT_LIMIT = 7.0


def _cover_scaled(src):
    """Scale the source so it covers the panel; returns the oversized image."""
    w, h = src.size
    s = max(EPD_WIDTH / w, EPD_HEIGHT / h)
    return src.resize((max(EPD_WIDTH, int(w * s + .5)),
                       max(EPD_HEIGHT, int(h * s + .5))), Image.LANCZOS)


def _saliency_columns(img):
    """
    Per-column interest, for choosing a crop window.

    Deliberately crude: local contrast plus colour saturation, summed down each
    column. It is not trying to find faces — it is trying to avoid slicing
    through the busiest part of a sleeve, which is usually where the type is.
    """
    a = np.asarray(img.convert("RGB"), dtype=np.float64)
    lum = 0.299*a[:,:,0] + 0.587*a[:,:,1] + 0.114*a[:,:,2]
    gx = np.abs(np.diff(lum, axis=1, prepend=lum[:, :1]))
    gy = np.abs(np.diff(lum, axis=0, prepend=lum[:1, :]))
    mx, mn = a.max(axis=2), a.min(axis=2)
    sat = np.where(mx > 0, (mx - mn) / np.maximum(mx, 1e-6), 0)
    return (gx + gy + sat * 40).sum(axis=0)


def _mirror_extend(art, out_h):
    """
    Extend a square vertically into the space above and below.

    Mirroring guarantees the colour matches exactly at the join — the reflected
    row next to the edge IS the edge row — so there is no seam to hide.

    But mirroring alone reflects *content*, and album covers put type near the
    edges: the first version produced legible ghost text smeared above "blond"
    and "MILES DAVIS", which reads as a rendering fault rather than a design.
    So the extension starts already blurred, hard enough that nothing is
    recognisable, and is pulled progressively toward a flat continuation of the
    artwork's own edge colour. What survives is the colour, not the picture.
    """
    aw, ah = art.size
    pad = (out_h - ah) // 2
    canvas = Image.new("RGB", (aw, out_h))
    canvas.paste(art, (0, pad))

    up = art.crop((0, 0, aw, min(pad, ah))).transpose(Image.FLIP_TOP_BOTTOM)
    canvas.paste(up, (0, pad - up.size[1]))
    dn = art.crop((0, max(0, ah - pad), aw, ah)).transpose(Image.FLIP_TOP_BOTTOM)
    canvas.paste(dn, (0, pad + ah))

    from PIL import ImageFilter
    arr = np.asarray(canvas, dtype=np.float64)

    # Flat continuation of the edge colour, per column, softened horizontally so
    # a busy edge does not become vertical stripes.
    def edge_wash(rows):
        band = np.asarray(Image.fromarray(rows.astype(np.uint8))
                          .filter(ImageFilter.GaussianBlur(9)), dtype=np.float64)
        return band.mean(axis=0)

    top_wash = edge_wash(arr[pad:pad + 24])
    bot_wash = edge_wash(arr[pad + ah - 24:pad + ah])

    bands = 8
    for i in range(bands):
        f0, f1 = i / bands, (i + 1) / bands
        radius = int(10 + 34 * f1 ** 1.2)      # already unreadable at the join
        for y0, y1, wash in (
                (int(pad * (1 - f1)), int(pad * (1 - f0)), top_wash),
                (pad + ah + int(pad * f0), pad + ah + int(pad * f1), bot_wash)):
            y0, y1 = max(0, y0), min(out_h, y1)
            if y1 - y0 < 2:
                continue
            ctx0, ctx1 = max(0, y0 - radius), min(out_h, y1 + radius)
            strip = Image.fromarray(arr[ctx0:ctx1].astype(np.uint8)) \
                         .filter(ImageFilter.GaussianBlur(radius))
            blurred = np.asarray(strip, dtype=np.float64)[y0-ctx0:y1-ctx0]
            # Further from the artwork, lean harder on the flat edge colour.
            t = f1 ** 0.8
            arr[y0:y1] = blurred * (1 - t) + wash[None, :, :] * t
    return Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8))


def adaptive_zoom(src):
    """
    Choose how far to enlarge a sleeve before its own detail starts being cut.

    Rather than one threshold deciding crop-or-don't, walk the zoom up and stop
    at the last step whose cut lines are not passing through anything strong.
    A photographic sleeve runs all the way to a full bleed; a sleeve with type
    across it stops early and keeps its type.
    """
    best = 1.0
    for z in (1.0, 1.1, 1.2, 1.3, 1.45, EPD_HEIGHT / EPD_WIDTH):
        if cut_severity(src, z) < CUT_LIMIT:
            best = z
        else:
            break
    return best


def cut_severity(src, zoom):
    """
    Peak detail lying along the two vertical cut lines at this zoom, relative to
    the sleeve overall.

    Averaging down the whole cut was the mistake in the first two attempts: a
    band of type occupies a small fraction of the height, so its contribution
    washed out and covers that cropping visibly ruins scored as safe. Taking a
    high percentile instead asks the right question — is there ANY row where
    the cut goes through something strong.
    """
    side = int(EPD_WIDTH * zoom + .5)
    if side <= EPD_WIDTH:
        return 0.0
    img = src.convert("RGB").resize((side, side), Image.LANCZOS)
    a = np.asarray(img, dtype=np.float64)
    lum = 0.299*a[:,:,0] + 0.587*a[:,:,1] + 0.114*a[:,:,2]
    gx = np.abs(np.diff(lum, axis=1, prepend=lum[:, :1]))

    x = (side - EPD_WIDTH) // 2
    band = max(2, side // 200)
    cuts = np.concatenate([gx[:, max(0,x-band):x+band],
                           gx[:, side-x-band:min(side,side-x+band)]], axis=1)
    per_row = cuts.max(axis=1)
    return float(np.percentile(per_row, 96) / max(gx.mean(), 1e-6))


def crop_cost(src):
    """
    How badly a cover-crop would damage this sleeve.

    The first attempt measured how much *detail* the crop removes, and it did
    not discriminate at all — every cover scored near 1.0, so sleeves that
    cropping visibly ruins came out looking safe. Wrong question. What matters
    is not how much is discarded but whether the cut passes through something
    continuous: an artist name spanning the sleeve survives being darkened at
    the edges, and does not survive being sliced in half.

    So measure the detail lying along the two cut lines, relative to the sleeve
    as a whole. A cut through a wall or a sky is cheap; a cut through type is
    not.
    """
    big = _cover_scaled(src)
    a = np.asarray(big.convert("RGB"), dtype=np.float64)
    lum = 0.299*a[:,:,0] + 0.587*a[:,:,1] + 0.114*a[:,:,2]
    gx = np.abs(np.diff(lum, axis=1, prepend=lum[:, :1]))

    w = lum.shape[1]
    keep = int(w * EPD_WIDTH / EPD_HEIGHT)
    side = (w - keep) // 2
    if side < 4:
        return 0.0

    band = max(3, w // 100)          # a few pixels either side of each cut
    cuts = np.concatenate([gx[:, side-band:side+band],
                          gx[:, w-side-band:w-side+band]], axis=1)
    overall = gx.mean()
    return float(cuts.mean() / max(overall, 1e-6))


def build(src, mode, bg_style=1, zoom=1.0):
    """Return an EPD_WIDTH x EPD_HEIGHT RGB array, before enhancement/dither."""
    src = src.convert("RGB")

    if mode == FIT:
        canvas, _ = eink.compose(src, "", "", bg_mode=2, bg_style=bg_style,
                                 profile_index=1, show_text=False)
        return canvas

    if mode == ADAPTIVE:
        # Fill the whole panel when the cover can take it, and protect the
        # artwork when it cannot. Photographic sleeves that run to the edge
        # gain enormously from a full bleed; a sleeve with the artist's name
        # across the top is ruined by it.
        return build(src, BLEED, bg_style, zoom=adaptive_zoom(src))

    if mode in (COVER, SMART):
        big = _cover_scaled(src)
        if mode == COVER:
            x = (big.size[0] - EPD_WIDTH) // 2
        else:
            col = _saliency_columns(big)
            win = np.convolve(col, np.ones(EPD_WIDTH), mode="valid")
            x = int(np.argmax(win))
        y = (big.size[1] - EPD_HEIGHT) // 2
        return np.asarray(big.crop((x, y, x + EPD_WIDTH, y + EPD_HEIGHT)), dtype=np.uint8)

    # BLEED: the artwork enlarged by `zoom`, then extended to the panel edges.
    #
    # zoom is the whole design. At 1.0 nothing is cropped and a wide band is
    # filled by the extension; at 1.667 the artwork covers the panel outright
    # and there is no extension at all. Everything useful is in between, so the
    # choice stops being crop-or-don't and becomes how much to crop — which is
    # the question that actually has a good answer for a given sleeve.
    zoom = max(1.0, min(EPD_HEIGHT / EPD_WIDTH, zoom))
    side = int(EPD_WIDTH * zoom + .5)
    art = src.resize((side, side), Image.LANCZOS)
    if side > EPD_WIDTH:
        x = (side - EPD_WIDTH) // 2
        art = art.crop((x, 0, x + EPD_WIDTH, side))
    if side >= EPD_HEIGHT:
        y = (side - EPD_HEIGHT) // 2
        return np.asarray(art.crop((0, y, EPD_WIDTH, y + EPD_HEIGHT)), dtype=np.uint8)

    out = _mirror_extend(art, EPD_HEIGHT)
    arr = np.asarray(out, dtype=np.float64)

    # Feather the join so the eye cannot find where the sharp artwork stops.
    pad = (EPD_HEIGHT - side) // 2
    feather = 26
    from PIL import ImageFilter
    soft = np.asarray(Image.fromarray(arr.astype(np.uint8))
                      .filter(ImageFilter.GaussianBlur(3)), dtype=np.float64)
    for i in range(feather):
        t = i / feather
        for y in (pad + i, pad + side - 1 - i):
            if 0 <= y < EPD_HEIGHT:
                arr[y] = arr[y] * t + soft[y] * (1 - t)
    return np.clip(arr, 0, 255).astype(np.uint8)


def render(src, mode, profile_index=1, zoom=1.0):
    canvas = build(src, mode, zoom=zoom)
    canvas = eink.enhance_for_eink(canvas, profile_index)
    _, idx = eink.dither(canvas, profile_index)
    return idx
