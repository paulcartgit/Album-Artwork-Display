"""
Which palette makes the PANEL bolder?

The palette does two jobs and it is easy to confuse them. It is what the
preview draws — so a more saturated palette always makes the simulator's
picture look more vivid — and it is the set of match targets the dither aims
at, which is the only one that changes what the hardware actually does.

The panel's output is entirely determined by which pigment index lands on which
pixel. So the honest measure of "bolder" is the share of pixels carrying a
chromatic pigment in the INDEX MAP, scored with one fixed palette so the two
runs are comparable.
"""
import importlib, json, sys
import numpy as np
from PIL import Image, ImageDraw

CURRENT = [(0x10,0x10,0x12),(0xD8,0xDA,0xD4),(0x30,0x66,0x58),
           (0x38,0x68,0xC0),(0x9C,0x30,0x2C),(0xC8,0xB8,0x30)]
MEASURED = [(0x0D,0x0A,0x10),(0xE0,0xE0,0xD9),(0x1F,0x6C,0x2E),
            (0x00,0x5D,0xAB),(0xBD,0x0F,0x05),(0xFF,0xDA,0x1B)]


def with_palette(pal):
    """Reload eink with PALETTE swapped, so every derived table is rebuilt."""
    import firmware_config as fc
    fc.PALETTE_RGB = [tuple(int(v) for v in c) for c in pal]
    import eink
    importlib.reload(eink)
    return eink


def run(pal, files):
    eink = with_palette(pal)
    import fill_modes
    importlib.reload(fill_modes)
    out = {}
    for f in files:
        art = Image.open(f"gallery/{f}").convert("RGB")
        canvas = fill_modes.build(art, 1, bg_style=1)
        canvas = eink.enhance_for_eink(canvas, 1)
        _, idx = eink.dither(canvas, 1)
        out[f] = np.asarray(idx).copy()
    return out


# Covers that must appear in every comparison, whatever the tonal sampling
# picks. KPOP_DEMON_HUNTERS is the household favourite and the sleeve this
# whole washed-out investigation started from, so a change that quietly ruins
# it must never pass unnoticed. HELP is the blue-capes-going-green regression.
KEY_COVERS = [
    "995beeb2.jpg",   # HUNTR/X - KPop Demon Hunters
    "bb8149c0.jpg",   # The Beatles - Help!
]


def score(idx):
    c = np.bincount(idx.ravel(), minlength=6)
    return c[2:].sum() / idx.size * 100, c[1] / idx.size * 100


if __name__ == "__main__":
    tone = json.load(open("/tmp/tone.json"))
    label = {t["file"]: t["label"] for t in tone}
    files = [tone[i]["file"] for i in (0, 3, 8, 20, 35, 50, 60, 75, 88, 97)]
    files = [k for k in KEY_COVERS if k not in files] + files

    a = run(CURRENT, files)
    b = run(MEASURED, files)
    print(f"{'cover':44s} {'chromatic %':>18s} {'white %':>18s}")
    da, db = [], []
    for f in files:
        ca, wa = score(a[f]); cb, wb = score(b[f])
        da.append(ca); db.append(cb)
        diff = (b[f] != a[f]).mean() * 100
        print(f"{label[f][:44]:44s}  {ca:5.1f} -> {cb:5.1f}   "
              f"{wa:5.1f} -> {wb:5.1f}   ({diff:.1f}% of pixels differ)")
    print(f"\n  mean chromatic share  {np.mean(da):5.1f} -> {np.mean(db):5.1f}")
