"""
Re-run the day's two headline comparisons with the framing the device
actually uses (ADAPTIVE), and score them against the SOURCE with fidelity.py
rather than with the self-referential pigment counts used earlier.
"""
import json
import numpy as np
from PIL import Image
import eink, fill_modes, fidelity, tone_select
from tone_map import compress

CURRENT  = [(0x10,0x10,0x12),(0xD8,0xDA,0xD4),(0x30,0x66,0x58),
            (0x38,0x68,0xC0),(0x9C,0x30,0x2C),(0xC8,0xB8,0x30)]
CALIBRATED = [tuple(c) for c in eink.PALETTE_RGB]

KEY = ["995beeb2.jpg", "bb8149c0.jpg"]
tone = json.load(open('/tmp/tone.json'))
label = {t['file']: t['label'] for t in tone}
FILES = KEY + [tone[i]['file'] for i in (0, 8, 20, 35, 50, 75, 97)]


def canvas(f):
    return np.asarray(fill_modes.build(Image.open(f'gallery/{f}').convert('RGB'),
                                       fill_modes.ADAPTIVE, bg_style=1))


def render(src, palette=None, keep=1.0):
    # A palette swap has to go through the Python reference: the native path
    # runs the firmware, which compiles PALETTE in from config.h and cannot be
    # told to use another one. Both sides of the A/B use the same
    # implementation, so the comparison stays fair.
    if palette is not None:
        old = eink.PALETTE, eink.MATCH_PAL, eink.MATCH_PAL_LAB, eink.ACHROMATIC
        eink.PALETTE = np.array(palette, float)
        eink.MATCH_PAL = np.vstack([eink.PALETTE,
            np.array([np.mean([eink.PALETTE[i] for i in c], axis=0)
                      for c in eink.VIRTUAL_CELL])])
        eink.MATCH_PAL_LAB = eink.rgb_to_lab(eink.MATCH_PAL)
        eink.ACHROMATIC = np.sqrt(eink.MATCH_PAL_LAB[:,1]**2 +
                                  eink.MATCH_PAL_LAB[:,2]**2) < 5.0
    try:
        enhanced = eink.enhance_for_eink(compress(src, keep), 1)
        if palette is not None:
            idx = np.asarray(eink.dither_python(enhanced, 1)[1])
        else:
            idx = np.asarray(eink.dither(enhanced, 1)[1])
    finally:
        if palette is not None:
            eink.PALETTE, eink.MATCH_PAL, eink.MATCH_PAL_LAB, eink.ACHROMATIC = old
    # Always paint with the CALIBRATED pigments: that is what the panel shows,
    # whatever the dither believed while choosing.
    return np.array(CALIBRATED, dtype=np.uint8)[idx]


print("PALETTE  (old hand-tuned -> RAW-calibrated), ADAPTIVE framing")
print(f"{'cover':40s} {'dE':>14s} {'hue (deg)':>16s}")
da = db = ha = hb = 0.0
for f in FILES:
    src = canvas(f)
    a = fidelity.score(src, render(src, CURRENT))
    b = fidelity.score(src, render(src, CALIBRATED))
    da += a['dE']; db += b['dE']; ha += a['hue']; hb += b['hue']
    print(f"{label[f][:40]:40s} {a['dE']:5.1f} -> {b['dE']:5.1f}  {a['hue']:6.1f} -> {b['hue']:6.1f}")
n = len(FILES)
print(f"{'MEAN':40s} {da/n:5.1f} -> {db/n:5.1f}  {ha/n:6.1f} -> {hb/n:6.1f}")

print("\n\nTONE MAP  (off -> adaptive), ADAPTIVE framing")
print(f"{'cover':40s} {'pick':>6s} {'dE':>14s} {'hue (deg)':>16s}")
da = db = ha = hb = 0.0
for f in FILES:
    src = canvas(f)
    pick = tone_select.choose(src)
    a = fidelity.score(src, render(src, None, 1.0))
    b = fidelity.score(src, render(src, None, pick))
    da += a['dE']; db += b['dE']; ha += a['hue']; hb += b['hue']
    flag = "" if pick == 1.0 else "  <-- darkened"
    print(f"{label[f][:40]:40s} x{pick:.2f} {a['dE']:5.1f} -> {b['dE']:5.1f}  "
          f"{a['hue']:6.1f} -> {b['hue']:6.1f}{flag}")
print(f"{'MEAN':40s}       {da/n:5.1f} -> {db/n:5.1f}  {ha/n:6.1f} -> {hb/n:6.1f}")
