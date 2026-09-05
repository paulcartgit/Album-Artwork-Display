#!/usr/bin/env python3
"""
Assert the simulator still behaves like the firmware.

The simulator's whole purpose is to let you iterate on the look of the display
without a 15-second panel refresh in the loop.  It had silently drifted into a
different program — a seven-colour palette including an Orange the panel does
not have, plain RGB matching instead of CIELAB, no virtual colours, a different
layout — while its docstring still claimed it "mirrors dither.cpp exactly".

This script fails the build if that starts happening again.  It checks two
things: that the constants on both sides agree (by reading the C++ source), and
that the Python dither exhibits the same behavioural properties the native C++
tests assert.

Run:  python parity_check.py
"""

import re
import sys
from pathlib import Path

import numpy as np
from PIL import Image

import eink
from firmware_config import (
    CONFIG_H, EPD_WIDTH, EPD_HEIGHT, EPD_COLORS, PALETTE_RGB, RENDER_PROFILES,
)

SRC = CONFIG_H.parent
FAILURES = []


def check(label, condition, detail=""):
    if condition:
        print(f"  ok   {label}")
    else:
        print(f"  FAIL {label}" + (f" — {detail}" if detail else ""))
        FAILURES.append(label)


def read(name):
    return (SRC / name).read_text()


# ═══════════════════════════════════════════════════════════
print("Constants shared with the firmware")
# ═══════════════════════════════════════════════════════════

check("palette size matches EPD_COLORS", len(PALETTE_RGB) == EPD_COLORS)
check("eink.PALETTE mirrors config.h",
      np.array_equal(eink.PALETTE, np.array(PALETTE_RGB, dtype=np.float64)))
check("canvas is 480x800 portrait", (EPD_WIDTH, EPD_HEIGHT) == (480, 800),
      f"got {EPD_WIDTH}x{EPD_HEIGHT}")
check("three render profiles", len(RENDER_PROFILES) == 3)

dither_cpp = read("dither.cpp")

# VIRTUAL_CELL in dither.cpp. Read the declared size rather than pinning it:
# the table went 2 pairs -> 9 pairs -> 11 2x2 cells, and each time a hardcoded
# size made this silently stop comparing instead of failing.
cells = re.search(r"VIRTUAL_CELL\[\d+\]\[4\]\s*=\s*\{(.*?)\n\};", dither_cpp, re.S)
firmware_cells = tuple(
    tuple(int(n) for n in m)
    for m in re.findall(r"\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}",
                        cells.group(1) if cells else "")
)
check("virtual colour cells match dither.cpp",
      firmware_cells == eink.VIRTUAL_CELL,
      f"firmware {firmware_cells} vs simulator {eink.VIRTUAL_CELL}")

nvirtual = len(eink.VIRTUAL_CELL)
check(f"matching palette has {EPD_COLORS} real + {nvirtual} virtual entries",
      eink.MATCH_COLORS == EPD_COLORS + nvirtual
      and eink.MATCH_PAL.shape[0] == EPD_COLORS + nvirtual)
check("firmware MATCH_COLORS agrees with the pair table",
      f"MATCH_COLORS = EPD_COLORS + {nvirtual}" in dither_cpp,
      "dither.cpp declares a different count from its VIRTUAL_PAIR table")
check("every virtual cell mixes at least two real pigments",
      all(len(set(c)) >= 2 and all(0 <= i < EPD_COLORS for i in c)
          for c in eink.VIRTUAL_CELL))
check("no two virtual cells have the same mix",
      len({tuple(sorted(c)) for c in eink.VIRTUAL_CELL}) == len(eink.VIRTUAL_CELL))
check("firmware tiles the cell the same way the simulator does",
      "cell[((y & 1) << 1) | (x & 1)]" in dither_cpp)

# The firmware must still derive its match palette from PALETTE, not from
# idealised RGB cube corners — that was the original bug.
check("firmware matches in calibrated space, not cube corners",
      "MATCH_PAL[i][0] = (float)PALETTE[i].r" in dither_cpp,
      "dither.cpp no longer derives MATCH_PAL from the calibrated PALETTE")

check("firmware diffuses error against the placed pigment",
      "cr - (float)PALETTE[displayIdx].r" in dither_cpp)

pipeline_cpp = read("image_pipeline.cpp")
for name, value in (("ARTIST_BAND_H", eink.ARTIST_BAND_H),
                    ("ALBUM_BAND_H", eink.ALBUM_BAND_H)):
    m = re.search(rf"const int {name}\s*=\s*(\d+)", pipeline_cpp)
    check(f"{name} matches image_pipeline.cpp",
          m is not None and int(m.group(1)) == value,
          f"firmware {m.group(1) if m else '?'} vs simulator {value}")

m = re.search(r"useBlur = var >= ([\d.]+)f", pipeline_cpp)
check("edge-variance blur threshold matches",
      m is not None and float(m.group(1)) == eink.EDGE_VARIANCE_BLUR_THRESHOLD,
      f"firmware {m.group(1) if m else '?'} vs simulator {eink.EDGE_VARIANCE_BLUR_THRESHOLD}")

m = re.search(r"const int shadowPad = (\d+)", pipeline_cpp)
check("drop-shadow padding matches",
      m is not None and int(m.group(1)) == eink.SHADOW_PAD)


# ═══════════════════════════════════════════════════════════
print("\nBehaviour (mirrors firmware/test/test_native)")
# ═══════════════════════════════════════════════════════════

def solid(w, h, rgb):
    a = np.empty((h, w, 3), dtype=np.uint8)
    a[:, :] = rgb
    return a


# Each pigment must round-trip to its own index with nothing to diffuse.
pigment_ok = True
bad = []
for i, rgb in enumerate(PALETTE_RGB):
    _, idx = eink.dither(solid(4, 4, rgb))
    if not (idx == i).all():
        pigment_ok = False
        bad.append((i, sorted(set(idx.ravel().tolist()))))
check("each calibrated pigment round-trips to its own index", pigment_ok, str(bad))

# Purple is out of gamut: the panel can only make it by interleaving red+blue.
mid = tuple((np.array(PALETTE_RGB[4]) + np.array(PALETTE_RGB[3])) // 2)
_, idx = eink.dither(solid(16, 16, mid))
reds = int((idx == 4).sum())
blues = int((idx == 3).sum())
check("out-of-gamut purple interleaves red and blue",
      reds > 16 * 16 / 8 and blues > 16 * 16 / 8,
      f"red={reds} blue={blues}")

# Virtual entries must never reach the panel.
rng = np.random.default_rng(7)
_, idx = eink.dither(rng.integers(0, 256, (16, 16, 3), dtype=np.uint8))
check("only real pigment indices are emitted", int(idx.max()) < EPD_COLORS,
      f"max index {int(idx.max())}")

# Solid black and solid white must be flat.
_, idx = eink.dither(solid(4, 4, (0, 0, 0)))
check("solid black stays flat black", (idx == 0).all())
_, idx = eink.dither(solid(4, 4, (255, 255, 255)))
check("solid white stays flat white", (idx == 1).all())

# Profiles must actually change the output. This has to go through the whole
# pipeline, not just dither(): once the white-paired blends were added the
# chroma penalty stopped mattering for most colours — it only ever penalised
# the achromatic entries, and a light tint now has a chromatic target of its
# own — so matching alone no longer separates the profiles. They still differ,
# via sharpen, contrast and gamma.
grad = np.tile(np.linspace(0, 255, 32, dtype=np.uint8)[None, :, None], (32, 1, 3))
grad[..., 1] = (grad[..., 1] * 0.7).astype(np.uint8)
punchy = eink.enhance_for_eink(grad, 0)
soft = eink.enhance_for_eink(grad, 2)
check("render profiles produce different output", not np.array_equal(punchy, soft))

# The penalty is now near-redundant, which is the point of the blends, but it
# must not have become a no-op that silently stops protecting saturated colour.
_m0 = eink._MatchCache(eink.profile(0))
_m2 = eink._MatchCache(eink.profile(2))
_rng = np.random.default_rng(4)
_c = _rng.integers(0, 256, (400, 3))
_d = (_m0.lookup(_c[:, 0], _c[:, 1], _c[:, 2])
      != _m2.lookup(_c[:, 0], _c[:, 1], _c[:, 2])).sum()
check("chroma penalty still separates profiles somewhere", _d > 0,
      "profiles now match identically on 400 random colours — the penalty is dead")

# Full render produces a correctly-sized canvas.
out, idx = eink.render(Image.fromarray(solid(200, 200, (180, 90, 60))),
                       "Artist Name", "Album Name")
check("render() produces a full 480x800 canvas", out.size == (EPD_WIDTH, EPD_HEIGHT),
      str(out.size))


# ═══════════════════════════════════════════════════════════
print("\nArtwork fill")
# ═══════════════════════════════════════════════════════════

import fill_modes as fmod

fill_h = (SRC / "fill_policy.h").read_text()
check("fill thresholds come from fill_policy.h",
      fmod.CUT_LIMIT == float(re.search(r"FILL_CUT_LIMIT\s+([0-9.]+)f", fill_h).group(1)))

m = re.search(r"VIRTUAL_PAIR|fillAdaptiveZoom", fill_h)
check("firmware still walks the zoom steps",
      "static const float STEPS[]" in fill_h and "1.45f" in fill_h)

# The row stride must round UP, or a sleeve between 128 and 256 rows tall is
# scanned only to row 128 and type below that is invisible. That shipped once.
check("firmware scans the full height (stride rounds up)",
      "(h + MAX_ROWS - 1) / MAX_ROWS" in fill_h)

# The percentile is taken from a DESCENDING sort, so it indexes near the front.
# Indexing at 0.96*n returned a near-minimum and cropped through type.
check("firmware takes the percentile from the correct end",
      "(n - 1) * 0.04f" in fill_h)

# Behaviour, mirroring firmware/test/test_native
def _noise(n, seed):
    rng = np.random.default_rng(seed)
    v = 90 + rng.integers(0, 64, (n, n))
    return np.dstack([v, v, v ^ 0x10]).astype(np.uint8)

def _band(a, y0, y1, seed=5):
    rng = np.random.default_rng(seed)
    a = a.copy()
    bar = rng.integers(0, 2, (y1 - y0, a.shape[1])) * 255
    for c in range(3):
        a[y0:y1, :, c] = bar
    return a

photo = Image.fromarray(_noise(300, 7))
typed = Image.fromarray(_band(_noise(300, 7), 40, 70))
check("photographic sleeve reaches a full bleed",
      fmod.adaptive_zoom(photo) == fmod.FILL_MAX_ZOOM,
      f"got {fmod.adaptive_zoom(photo)}")
check("sleeve with type across it is not cropped",
      fmod.adaptive_zoom(typed) == 1.0,
      f"got {fmod.adaptive_zoom(typed)}")
check("severity separates the two",
      fmod.cut_severity(typed, 1.3) > fmod.CUT_LIMIT > fmod.cut_severity(photo, 1.3),
      f"typed={fmod.cut_severity(typed,1.3):.1f} photo={fmod.cut_severity(photo,1.3):.1f}")

# Same artwork at different sizes must score alike — the device and the
# simulator disagreed precisely because this was not true.
small = Image.fromarray(_band(_noise(200, 11), 30, 45))
big   = Image.fromarray(_band(_noise(700, 11), 105, 157))
sa, sb = fmod.cut_severity(small, 1.3), fmod.cut_severity(big, 1.3)
check("severity is resolution independent",
      sa > fmod.CUT_LIMIT and sb > fmod.CUT_LIMIT and abs(sa - sb) < 0.65 * max(sa, sb),
      f"200px={sa:.1f} 700px={sb:.1f}")


# ═══════════════════════════════════════════════════════════
print("\nTone mapping")
# ═══════════════════════════════════════════════════════════
import tone_select
from tone_map import compress

tm_h = read("tone_map.h")
check("tone-map constants come from tone_map.h",
      tone_select.SCALES == (1.0, 0.9, 0.8) and tone_select.HUE_WEIGHT == 0.15,
      f"scales={tone_select.SCALES} weight={tone_select.HUE_WEIGHT}")
check("firmware measures rather than predicts",
      "ditherFloydSteinberg(cand, packed, dw, dh)" in read("image_pipeline.cpp"),
      "the trial render is gone — a predictor has crept back in")
check("firmware scores against the source",
      "toneMapScore(small, shown, dw, dh" in read("image_pipeline.cpp"))

# Compression must move lightness and leave hue alone. That is the whole
# reason it is done in Lab: an RGB white-point pull produced dither speckle
# instead of colour.
probe = np.full((8, 8, 3), (233, 163, 197), dtype=np.uint8)   # the KPop skin
lab_a = eink.rgb_to_lab(probe.reshape(-1, 3))[0]
lab_b = eink.rgb_to_lab(compress(probe, 0.80).reshape(-1, 3))[0]
h_a = np.degrees(np.arctan2(lab_a[2], lab_a[1])) % 360
h_b = np.degrees(np.arctan2(lab_b[2], lab_b[1])) % 360
check("compression darkens", lab_b[0] < lab_a[0] - 5,
      f"L* {lab_a[0]:.1f} -> {lab_b[0]:.1f}")
check("compression preserves hue", abs((h_a - h_b + 180) % 360 - 180) < 3.0,
      f"hue {h_a:.1f} -> {h_b:.1f} deg")
check("scale 1.0 is a no-op",
      np.array_equal(compress(probe, 1.0), probe))


# ═══════════════════════════════════════════════════════════
print()
if FAILURES:
    print(f"PARITY CHECK FAILED — {len(FAILURES)} problem(s):")
    for f in FAILURES:
        print(f"  - {f}")
    sys.exit(1)
print("Parity check passed — the simulator matches the firmware.")
