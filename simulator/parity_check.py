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

# VIRTUAL_PAIR in dither.cpp: { {2,3}, {4,3} }
pairs = re.search(r"VIRTUAL_PAIR\[2\]\[2\]\s*=\s*\{(.*?)\};", dither_cpp, re.S)
firmware_pairs = tuple(
    tuple(int(n) for n in m)
    for m in re.findall(r"\{\s*(\d+)\s*,\s*(\d+)\s*\}", pairs.group(1) if pairs else "")
)
check("virtual colour pairs match dither.cpp",
      firmware_pairs == eink.VIRTUAL_PAIR,
      f"firmware {firmware_pairs} vs simulator {eink.VIRTUAL_PAIR}")

check("matching palette has 6 real + 2 virtual entries",
      eink.MATCH_COLORS == EPD_COLORS + 2 and eink.MATCH_PAL.shape[0] == EPD_COLORS + 2)

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

# Profiles must actually change the output.
grad = solid(16, 16, (150, 110, 170))
_, punchy = eink.dither(grad, 0)
_, soft = eink.dither(grad, 2)
check("render profiles produce different output", not np.array_equal(punchy, soft))

# Full render produces a correctly-sized canvas.
out, idx = eink.render(Image.fromarray(solid(200, 200, (180, 90, 60))),
                       "Artist Name", "Album Name")
check("render() produces a full 480x800 canvas", out.size == (EPD_WIDTH, EPD_HEIGHT),
      str(out.size))


# ═══════════════════════════════════════════════════════════
print()
if FAILURES:
    print(f"PARITY CHECK FAILED — {len(FAILURES)} problem(s):")
    for f in FAILURES:
        print(f"  - {f}")
    sys.exit(1)
print("Parity check passed — the simulator matches the firmware.")
