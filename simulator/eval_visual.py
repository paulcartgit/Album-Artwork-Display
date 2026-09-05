"""Side-by-side auto-levelling comparison over a tonal spread of real covers."""
import json, sys
import numpy as np
from PIL import Image, ImageDraw
import eink, fill_modes
from firmware_config import PALETTE_RGB

CHROMATIC = [i for i in range(len(PALETTE_RGB)) if i not in (0, 1)]


def render(art, strength):
    canvas = fill_modes.build(art, 1, bg_style=1)
    prof = dict(eink.profile(1))
    prof["auto_level"] = strength
    lo, hi = eink.auto_level_window(canvas, strength)
    lut = eink._enhance_lut(prof, lo, hi)
    a = np.asarray(canvas, dtype=np.float64)
    p = np.pad(a, ((1, 1), (1, 1), (0, 0)), mode="edge")
    blur = np.floor(sum(p[dy:dy + a.shape[0], dx:dx + a.shape[1], :]
                        for dy in range(3) for dx in range(3)) / 9.0)
    sharp = np.clip(a + prof["sharpen"] * (a - blur), 0, 255).astype(np.uint8)
    _, idx = eink.dither(lut[sharp], 1)
    return idx, (lo, hi)


def stats(idx):
    c = np.bincount(np.asarray(idx).ravel(), minlength=len(PALETTE_RGB))
    return c[CHROMATIC].sum() / idx.size * 100, c.max() / idx.size * 100


# Covers that must appear in every comparison, whatever the tonal sampling
# picks. KPOP_DEMON_HUNTERS is the household favourite and the sleeve this
# whole washed-out investigation started from, so a change that quietly ruins
# it must never pass unnoticed. HELP is the blue-capes-going-green regression.
KEY_COVERS = [
    "995beeb2.jpg",   # HUNTR/X - KPop Demon Hunters
    "bb8149c0.jpg",   # The Beatles - Help!
]

tone = json.load(open("/tmp/tone.json"))
picks = [t["file"] for t in tone[:5]]                       # palest
picks += [t["file"] for t in tone[len(tone)//2 - 2:len(tone)//2 + 2]]  # middle
picks += [t["file"] for t in tone[-4:]]                     # darkest
picks = [k for k in KEY_COVERS if k not in picks] + picks
label = {t["file"]: t["label"] for t in tone}
strength = float(sys.argv[1]) if len(sys.argv) > 1 else 0.75

tiles, lines = [], []
for f in picks:
    art = Image.open(f"gallery/{f}").convert("RGB")
    off, _ = render(art, 0.0)
    on, win = render(art, strength)
    co, do = stats(off)
    cn, dn = stats(on)
    lines.append(f"{label[f]:46s} win {win[0]:5.0f}-{win[1]:3.0f}  "
                 f"chroma {co:5.1f}->{cn:5.1f}  dominant {do:5.1f}->{dn:5.1f}")
    print(lines[-1], flush=True)
    tiles.append((label[f], eink.index_to_image(off), eink.index_to_image(on)))

# Contact sheet: source-off-on triplets, small enough to read at a glance.
TW, TH = 150, 250
cols = 4
rows = (len(tiles) + cols - 1) // cols
sheet = Image.new("RGB", (cols * (TW * 2 + 14), rows * (TH + 26)), "white")
d = ImageDraw.Draw(sheet)
for i, (name, a, b) in enumerate(tiles):
    x = (i % cols) * (TW * 2 + 14)
    y = (i // cols) * (TH + 26)
    sheet.paste(a.resize((TW, TH), Image.LANCZOS), (x, y + 20))
    sheet.paste(b.resize((TW, TH), Image.LANCZOS), (x + TW + 4, y + 20))
    d.text((x + 2, y + 6), name[:44], fill="black")
sheet.save("/tmp/autolevel_sheet.png")
print("\nwrote /tmp/autolevel_sheet.png  (left = off, right = on)")
