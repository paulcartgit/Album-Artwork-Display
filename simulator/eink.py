"""
Python port of the firmware rendering pipeline.

This is a deliberate line-by-line mirror of firmware/src/dither.cpp and
firmware/src/image_pipeline.cpp — matching in CIELAB against the *calibrated*
pigment values, virtual Cyan/Magenta entries, the chroma penalty, shadow chroma
suppression, source-side edge attenuation, serpentine Floyd-Steinberg, and the
same enhancement LUT and background treatment.

Palette and profile constants are read from firmware/src/config.h at import
time (see firmware_config.py) so they cannot drift.  parity_check.py asserts
the rest.
"""

import os
import warnings

import numpy as np
from PIL import Image, ImageDraw, ImageFont

from firmware_config import (
    EPD_WIDTH, EPD_HEIGHT, EPD_COLORS,
    PALETTE_RGB, RENDER_PROFILES, DEFAULT_PROFILE, profile,
)

PALETTE = np.array(PALETTE_RGB, dtype=np.float64)

# Virtual entries: the 2x2 cell each one tiles. Mirrors VIRTUAL_CELL in
# dither.cpp. Repeating a pigment weights it, so a cell can express 25% steps
# and three-pigment mixes, not just a 50/50 pair.
VIRTUAL_CELL = (
    (2, 3, 3, 2),   # Cyan          Green / Blue
    (4, 3, 3, 4),   # Magenta       Red   / Blue
    (4, 5, 5, 4),   # Orange        Red   / Yellow
    (2, 5, 5, 2),   # Lime          Green / Yellow
    (4, 2, 2, 4),   # Brown         Red   / Green
    (4, 1, 1, 4),   # Light Pink    Red   / White
    (5, 1, 1, 5),   # Light Yellow  Yellow/ White
    (3, 1, 1, 3),   # Light Blue    Blue  / White
    (2, 1, 1, 2),   # Light Green   Green / White
    (4, 3, 1, 1),   # Light Purple  quarter Red, quarter Blue, half White
    (4, 1, 1, 1),   # Pale Pink     quarter Red, three-quarter White
)
MATCH_COLORS = EPD_COLORS + len(VIRTUAL_CELL)

MATCH_PAL = np.vstack([
    PALETTE,
    np.array([np.mean([PALETTE[i] for i in cell], axis=0) for cell in VIRTUAL_CELL]),
])

# Layout constants — must match processJpegBuffer() in image_pipeline.cpp
ARTIST_BAND_H = 100
ALBUM_BAND_H = 80
SHADOW_PAD = 12
SHADOW_MARGIN_V = 22
EDGE_VARIANCE_BLUR_THRESHOLD = 800.0


# ═══════════════════════════════════════════════════════════
# sRGB -> CIELAB
# ═══════════════════════════════════════════════════════════

def _srgb_to_linear(v):
    v = np.clip(v, 0.0, 255.0) / 255.0
    return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4)


def rgb_to_lab(rgb):
    """rgb: (..., 3) float array in 0-255. Returns (..., 3) Lab."""
    rgb = np.asarray(rgb, dtype=np.float64)
    lin = _srgb_to_linear(rgb)
    r, g, b = lin[..., 0], lin[..., 1], lin[..., 2]

    x = r * 0.4124564 + g * 0.3575761 + b * 0.1804375
    y = r * 0.2126729 + g * 0.7151522 + b * 0.0721750
    z = r * 0.0193339 + g * 0.1191920 + b * 0.9503041
    x = x / 0.95047
    z = z / 1.08883

    def f(t):
        return np.where(t > 0.008856, np.cbrt(t), 7.787 * t + 16.0 / 116.0)

    fx, fy, fz = f(x), f(y), f(z)
    return np.stack([116.0 * fy - 16.0,
                     500.0 * (fx - fy),
                     200.0 * (fy - fz)], axis=-1)


MATCH_PAL_LAB = rgb_to_lab(MATCH_PAL)
MATCH_PAL_CHROMA = np.sqrt(MATCH_PAL_LAB[:, 1] ** 2 + MATCH_PAL_LAB[:, 2] ** 2)
ACHROMATIC = MATCH_PAL_CHROMA < 5.0


class _MatchCache:
    """
    Lazily-filled 256^3 lookup of rounded-RGB -> match index.

    The firmware matches on floats; rounding to integers changes the result
    only for pixels sitting exactly on a decision boundary, and buys a ~100x
    speed-up that makes the simulator actually usable for iterating.
    """

    def __init__(self, profile_dict):
        self.k = profile_dict["chroma_penalty_k"]
        self.onset = profile_dict["chroma_penalty_onset"]
        self._table = np.full(256 * 256 * 256, 255, dtype=np.uint8)

    def _compute(self, keys):
        rgb = np.stack([(keys >> 16) & 0xFF, (keys >> 8) & 0xFF, keys & 0xFF], axis=-1)
        lab = rgb_to_lab(rgb.astype(np.float64))
        chroma = np.sqrt(lab[:, 1] ** 2 + lab[:, 2] ** 2)
        excess = np.maximum(0.0, chroma - self.onset)
        penalty = excess * excess * self.k

        d = ((lab[:, None, :] - MATCH_PAL_LAB[None, :, :]) ** 2).sum(axis=2)
        d[:, ACHROMATIC] += penalty[:, None]
        return np.argmin(d, axis=1).astype(np.uint8)

    def lookup(self, r, g, b):
        keys = (np.rint(np.clip(r, 0, 255)).astype(np.int64) << 16) \
             | (np.rint(np.clip(g, 0, 255)).astype(np.int64) << 8) \
             |  np.rint(np.clip(b, 0, 255)).astype(np.int64)
        vals = self._table[keys]
        missing = vals == 255
        if missing.any():
            need = np.unique(keys[missing])
            self._table[need] = self._compute(need)
            vals = self._table[keys]
        return vals


# ═══════════════════════════════════════════════════════════
# Edge map (mirrors buildEdgeMap)
# ═══════════════════════════════════════════════════════════

def build_edge_map(rgb):
    lum = 0.299 * rgb[:, :, 0] + 0.587 * rgb[:, :, 1] + 0.114 * rgb[:, :, 2]
    left = np.roll(lum, 1, axis=1);  left[:, 0] = lum[:, 0]
    right = np.roll(lum, -1, axis=1); right[:, -1] = lum[:, -1]
    up = np.roll(lum, 1, axis=0);    up[0, :] = lum[0, :]
    down = np.roll(lum, -1, axis=0); down[-1, :] = lum[-1, :]

    gx = right - left
    gy = down - up
    mag = np.sqrt(gx * gx + gy * gy)
    return np.minimum(mag / 150.0, 1.0)


# ═══════════════════════════════════════════════════════════
# Dithering (mirrors ditherFloydSteinberg)
# ═══════════════════════════════════════════════════════════

def dither(rgb_img, profile_index=DEFAULT_PROFILE):
    """
    Dither to palette indices. Runs the FIRMWARE's dither.cpp via
    native_dither, so the simulator cannot drift from the device — the Python
    version below was never exactly equal to it (float32 against float64
    through the error diffusion put them ~13% of pixels apart, about the same
    as a +/-1 level change to the input) and it is ~16x slower.

    Set EINK_PYTHON_DITHER=1 to force the reference implementation.
    """
    if not os.environ.get("EINK_PYTHON_DITHER"):
        try:
            import native_dither
            idx = native_dither.dither(rgb_img, profile_index)
            return index_to_image(idx), idx
        except Exception as exc:                      # toolchain missing, etc.
            warnings.warn(f"native dither unavailable ({exc}); "
                          f"falling back to the Python reference")
    return dither_python(rgb_img, profile_index)


def dither_python(rgb_img, profile_index=DEFAULT_PROFILE):
    """
    rgb_img: PIL Image or HxWx3 array.
    Returns (PIL Image rendered in pigment colours, HxW index array).
    """
    if isinstance(rgb_img, Image.Image):
        rgb = np.asarray(rgb_img.convert("RGB"), dtype=np.float64)
    else:
        rgb = np.asarray(rgb_img, dtype=np.float64)

    h, w = rgb.shape[:2]
    if h == 0 or w == 0:
        return Image.new("RGB", (max(w, 1), max(h, 1))), np.zeros((h, w), np.uint8)

    prof = profile(profile_index)
    edge_atten = prof["edge_attenuation"]
    cache = _MatchCache(prof)

    edge = build_edge_map(rgb)
    out = np.zeros((h, w), dtype=np.uint8)

    # Two rolling error rows, exactly as the firmware does
    row = [np.zeros((w, 3), dtype=np.float64), np.zeros((w, 3), dtype=np.float64)]

    for y in range(h):
        row[0] += rgb[y]
        ltr = (y & 1) == 0
        xs = range(w) if ltr else range(w - 1, -1, -1)

        for x in xs:
            c = np.clip(row[0][x], 0.0, 255.0)

            ci = int(cache.lookup(c[0:1], c[1:2], c[2:3])[0])

            if ci >= EPD_COLORS:
                cell = VIRTUAL_CELL[ci - EPD_COLORS]
                display_idx = cell[((y & 1) << 1) | (x & 1)]
            else:
                display_idx = ci
            out[y, x] = display_idx

            # Error against the pigment physically placed
            err = c - PALETTE[display_idx]

            # Shadow chroma suppression
            lum = 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]
            if lum < 8.0:
                scale = (lum / 8.0) ** 2
                e_lum = 0.299 * err[0] + 0.587 * err[1] + 0.114 * err[2]
                err = e_lum + (err - e_lum) * scale

            # Edge-aware attenuation, source side only
            err = err * (1.0 - edge[y, x] * edge_atten)

            step = 1 if ltr else -1
            nx = x + step
            if 0 <= nx < w:
                row[0][nx] += err * (7.0 / 16.0)
            if y + 1 < h:
                for dx, wt in ((-step, 3.0), (0, 5.0), (step, 1.0)):
                    tx = x + dx
                    if 0 <= tx < w:
                        row[1][tx] += err * (wt / 16.0)

        row[0], row[1] = row[1], row[0]
        row[1][:] = 0.0

    return index_to_image(out), out


def index_to_image(indices):
    return Image.fromarray(PALETTE[indices].astype(np.uint8), "RGB")


# ═══════════════════════════════════════════════════════════
# Pre-dither enhancement (mirrors enhanceForEink)
# ═══════════════════════════════════════════════════════════

SHADOW_THRESH = 50


def _enhance_lut(prof):
    i = np.arange(256, dtype=np.float64)
    v = np.clip(128.0 + (i - 128.0) * prof["contrast"], 0.0, 255.0)
    v = 255.0 * (v / 255.0) ** prof["gamma"]
    enhanced = np.clip(np.rint(v), 0, 255)

    # Shadow protection: below the threshold, blend back toward identity
    t = i / SHADOW_THRESH
    blended = np.rint(i + t * (enhanced - i))
    lut = np.where(i < SHADOW_THRESH, blended, enhanced)
    return np.clip(lut, 0, 255).astype(np.uint8)


def enhance_for_eink(rgb, profile_index=DEFAULT_PROFILE):
    prof = profile(profile_index)
    lut = _enhance_lut(prof)
    src = np.asarray(rgb, dtype=np.float64)

    # 3x3 box blur with edge clamping, then unsharp mask
    padded = np.pad(src, ((1, 1), (1, 1), (0, 0)), mode="edge")
    blur = sum(padded[dy:dy + src.shape[0], dx:dx + src.shape[1], :]
               for dy in range(3) for dx in range(3)) / 9.0
    # Integer division to match the firmware's `sum / 9` on ints
    blur = np.floor(blur)

    sharp = np.clip(src + prof["sharpen"] * (src - blur), 0, 255).astype(np.uint8)
    return lut[sharp]


# ═══════════════════════════════════════════════════════════
# Background treatment (mirrors averageEdgeColor / edgeVariance /
# fillBlurredBackground)
# ═══════════════════════════════════════════════════════════

def _edge_pixels(rgb):
    h, w = rgb.shape[:2]
    return np.concatenate([rgb[0, :, :], rgb[h - 1, :, :],
                           rgb[1:h - 1, 0, :], rgb[1:h - 1, w - 1, :]], axis=0)


def average_edge_color(rgb):
    px = _edge_pixels(np.asarray(rgb, dtype=np.float64))
    mx = px.max(axis=1)
    mn = px.min(axis=1)
    sat = np.where(mx > 0, (mx - mn) / np.maximum(mx, 1e-9), 0.0)
    weight = 0.1 + sat * sat

    avg = (px * weight[:, None]).sum(axis=0) / weight.sum()
    gray = 0.299 * avg[0] + 0.587 * avg[1] + 0.114 * avg[2]
    boosted = gray + (avg - gray) * 1.3
    return tuple(int(v) for v in np.clip(np.rint(boosted), 0, 255))


def edge_variance(rgb):
    px = _edge_pixels(np.asarray(rgb, dtype=np.float64))
    return float(px.var(axis=0).mean())


def _box_blur_1d(a, radius, axis):
    """Running box blur matching the firmware's clamped-edge accumulator."""
    pad = [(0, 0)] * a.ndim
    pad[axis] = (radius, radius)
    padded = np.pad(a, pad, mode="edge")
    kernel_len = 2 * radius + 1
    cumsum = np.cumsum(padded, axis=axis)
    zero_shape = list(a.shape)
    zero_shape[axis] = 1
    cumsum = np.concatenate([np.zeros(zero_shape), cumsum], axis=axis)

    hi = np.take(cumsum, range(kernel_len, kernel_len + a.shape[axis]), axis=axis)
    lo = np.take(cumsum, range(0, a.shape[axis]), axis=axis)
    return np.floor((hi - lo) / kernel_len)


def fill_blurred_background(src_rgb, canvas_w, canvas_h, bg_style=0):
    src = Image.fromarray(np.asarray(src_rgb, dtype=np.uint8), "RGB")
    sw, sh = src.size

    scale = max(canvas_w / sw, canvas_h / sh) * 1.3   # extra zoom into the centre
    scaled = src.resize((max(1, int(sw * scale)), max(1, int(sh * scale))),
                        Image.NEAREST)
    crop_x = (scaled.width - canvas_w) // 2
    crop_y = (scaled.height - canvas_h) // 2
    cropped = scaled.crop((crop_x, crop_y, crop_x + canvas_w, crop_y + canvas_h))

    buf = np.asarray(cropped, dtype=np.float64)
    for _ in range(4):                    # 4 passes, radius 12
        buf = _box_blur_1d(buf, 12, axis=1)
        buf = _box_blur_1d(buf, 12, axis=0)

    if bg_style == 1:                     # wash out toward white
        buf = buf + (255 - buf) * 45 / 100
    else:                                 # darken to 55%
        buf = buf * 55 / 100
    return np.clip(buf, 0, 255).astype(np.uint8)


# ═══════════════════════════════════════════════════════════
# Composition (mirrors processJpegBuffer)
# ═══════════════════════════════════════════════════════════

def _load_font(path_candidates, size):
    for p in path_candidates:
        try:
            return ImageFont.truetype(p, size)
        except (OSError, IOError):
            continue
    return ImageFont.load_default()


_BOLD_FONTS = ["/System/Library/Fonts/Supplemental/Arial Bold.ttf",
               "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
               "DejaVuSans-Bold.ttf"]
_REG_FONTS = ["/System/Library/Fonts/Supplemental/Arial.ttf",
              "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
              "DejaVuSans.ttf"]


def _fit_text(draw, text, font_paths, start_size, min_size, max_width):
    """Mirror of fitTextToWidth(): shrink, then ellipsise."""
    size = start_size
    font = _load_font(font_paths, size)
    while size > min_size and draw.textlength(text, font=font) > max_width:
        size -= 4
        font = _load_font(font_paths, size)
    while len(text) > 4 and draw.textlength(text, font=font) > max_width:
        text = text[:-2]
        if draw.textlength(text + "...", font=font) <= max_width:
            text = text + "..."
            break
    return text, font


def compose(art_img, artist="", album="", bg_mode=2, bg_style=0,
            profile_index=DEFAULT_PROFILE, show_text=True):
    """
    Build the full EPD_WIDTH x EPD_HEIGHT canvas the firmware would build.
    Returns an RGB numpy array ready for dither().
    """
    art = art_img.convert("RGB")
    src = np.asarray(art, dtype=np.uint8)
    img_w, img_h = art.size

    show_text = bool(show_text and artist and album)
    art_area_h = EPD_HEIGHT - ARTIST_BAND_H - ALBUM_BAND_H if show_text else EPD_HEIGHT

    if bg_mode == 0:
        use_blur = False
    elif bg_mode == 1:
        use_blur = True
    else:
        use_blur = edge_variance(src) >= EDGE_VARIANCE_BLUR_THRESHOLD

    if use_blur:
        canvas = fill_blurred_background(src, EPD_WIDTH, EPD_HEIGHT, bg_style)
    else:
        canvas = np.empty((EPD_HEIGHT, EPD_WIDTH, 3), dtype=np.uint8)
        canvas[:, :] = average_edge_color(src)

    # Scale artwork to fit, leaving room for the drop shadow when blurred
    shadow_margin = SHADOW_MARGIN_V if use_blur else 0
    fit_w = EPD_WIDTH
    fit_h = max(1, art_area_h - shadow_margin * 2)
    scale = min(fit_w / img_w, fit_h / img_h)
    scaled_w = max(1, int(img_w * scale))
    scaled_h = max(1, int(img_h * scale))
    offset_x = (EPD_WIDTH - scaled_w) // 2
    offset_y = (ARTIST_BAND_H + (art_area_h - scaled_h) // 2) if show_text \
        else (EPD_HEIGHT - scaled_h) // 2

    # Drop shadow bands above and below the artwork
    if use_blur:
        for dy in range(1, SHADOW_PAD + 1):
            t = dy / SHADOW_PAD
            alpha = 0.35 * (1.0 - t) ** 2
            for py in (offset_y - dy, offset_y + scaled_h - 1 + dy):
                if 0 <= py < EPD_HEIGHT:
                    band = canvas[py, offset_x:offset_x + scaled_w].astype(np.float64)
                    canvas[py, offset_x:offset_x + scaled_w] = \
                        (band * (1.0 - alpha)).astype(np.uint8)

    resized = np.asarray(art.resize((scaled_w, scaled_h), Image.NEAREST), dtype=np.uint8)
    canvas[offset_y:offset_y + scaled_h, offset_x:offset_x + scaled_w] = resized

    canvas = enhance_for_eink(canvas, profile_index)
    return canvas, (show_text, artist, album, art_area_h)


def draw_text_bands(indices, canvas_rgb, artist, album, art_area_h):
    """
    Mirror of renderTextBandPacked(): text is drawn onto the *index* buffer
    after dithering, in flat black or white, so it stays crisp.
    """
    for text, y0, band_h, fonts, start in (
        (artist, 0, ARTIST_BAND_H, _BOLD_FONTS, 64),
        (album, ARTIST_BAND_H + art_area_h, ALBUM_BAND_H, _REG_FONTS, 40),
    ):
        if not text:
            continue
        band = canvas_rgb[y0:y0 + band_h]
        if band.size == 0:
            continue
        avg = band.reshape(-1, 3).mean(axis=0)
        brightness = (avg[0] * 299 + avg[1] * 587 + avg[2] * 114) / 1000
        text_idx = 1 if brightness < 128 else 0

        mask = Image.new("L", (EPD_WIDTH, band_h), 0)
        draw = ImageDraw.Draw(mask)
        fitted, font = _fit_text(draw, text, fonts, start, 16, EPD_WIDTH - 30)
        bbox = draw.textbbox((0, 0), fitted, font=font)
        draw.text(((EPD_WIDTH - (bbox[2] - bbox[0])) // 2 - bbox[0],
                   (band_h - (bbox[3] - bbox[1])) // 2 - bbox[1]),
                  fitted, font=font, fill=255)

        m = np.asarray(mask) > 127
        indices[y0:y0 + band_h][m] = text_idx
    return indices


def render(art_img, artist="", album="", bg_mode=2, bg_style=0,
           profile_index=DEFAULT_PROFILE, show_text=True):
    """Full pipeline: compose -> enhance -> dither -> text. Returns (PIL, indices)."""
    canvas, (show_text, artist, album, art_area_h) = compose(
        art_img, artist, album, bg_mode, bg_style, profile_index, show_text)
    _, indices = dither(canvas, profile_index)
    if show_text:
        indices = draw_text_bands(indices, canvas, artist, album, art_area_h)
    return index_to_image(indices), indices
