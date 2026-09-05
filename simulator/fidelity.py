"""
Does the render actually look like the artwork?

Every earlier measure here was self-referential — chromatic share, dominant
pigment, how many pixels changed. None of them compare the output to the
SOURCE, so a total hue inversion scores clean: blue hair and pink hair are
both "chromatic". That is exactly how Help!'s blue capes and the KPop sleeve's
blue hair got through, twice, while the numbers said things were improving.

This scores the render against the source it came from:

  dE      mean CIE76 distance over blocks, after blurring both to the scale
          the eye integrates a dither at. The headline number.
  hue     mean hue-angle error in degrees, chroma-weighted. This is the one
          that catches pink rendered as blue; dE alone can be dragged down by
          a lightness shift and hide it.
  worst   the blocks that are furthest off, so a local failure in a face or a
          head of hair cannot average away against a large flat background.
"""
import numpy as np
from PIL import Image

import eink

BLOCK = 8   # dither integrates well below this on a 480x800 panel


def _blocks(a, n=BLOCK):
    h, w = a.shape[:2]
    h, w = h - h % n, w - w % n
    a = a[:h, :w]
    return a.reshape(h // n, n, w // n, n, 3).mean(axis=(1, 3))


def score(source_rgb, render_rgb):
    """source_rgb and render_rgb must already be the same size."""
    s = _blocks(np.asarray(source_rgb, dtype=np.float64))
    r = _blocks(np.asarray(render_rgb, dtype=np.float64))

    ls = eink.rgb_to_lab(s.reshape(-1, 3))
    lr = eink.rgb_to_lab(r.reshape(-1, 3))

    dE = np.sqrt(((ls - lr) ** 2).sum(axis=1))

    # Hue angle, weighted by how colourful the SOURCE is: a hue error in a
    # near-grey block is meaningless, one in saturated hair is the whole story.
    cs = np.sqrt(ls[:, 1] ** 2 + ls[:, 2] ** 2)
    cr = np.sqrt(lr[:, 1] ** 2 + lr[:, 2] ** 2)
    hs = np.degrees(np.arctan2(ls[:, 2], ls[:, 1]))
    hr = np.degrees(np.arctan2(lr[:, 2], lr[:, 1]))
    dh = np.abs((hs - hr + 180.0) % 360.0 - 180.0)
    w = np.clip(cs, 0, None)
    hue = float((dh * w).sum() / max(w.sum(), 1e-6))

    return dict(dE=float(dE.mean()),
                dE_p95=float(np.percentile(dE, 95)),
                hue=hue,
                chroma_loss=float((cs.mean() - cr.mean())))


def report(name, source_rgb, render_rgb):
    m = score(source_rgb, render_rgb)
    print(f"  {name:24s} dE {m['dE']:5.1f}  worst5% {m['dE_p95']:5.1f}  "
          f"hue {m['hue']:5.1f}deg  chroma lost {m['chroma_loss']:+5.1f}")
    return m
