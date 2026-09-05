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
  noise   grain that survives being looked at, from surviving_noise() below.
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


def _adapt(ls, lr):
    """
    Best global lightness fit of the render to the source.

    A viewer adapts to how bright the panel is. Its white is a dull grey, so
    every render is dimmer than the artwork, and a metric that charges for
    that charges the same amount however good the picture is — it made Help!,
    which is almost purely a uniform dimming and looks fine, score worse than
    covers with visible hue faults. Fit and remove one global gain and offset
    on L*, the way adaptation does, and what is left is the error the eye
    actually keeps seeing.
    """
    x, y = lr[:, 0], ls[:, 0]
    n = len(x)
    if n < 2 or np.allclose(x, x[0]):
        return lr
    gain, offset = np.polyfit(x, y, 1)
    gain = float(np.clip(gain, 0.5, 2.0))       # a fit, not a rescue
    out = lr.copy()
    out[:, 0] = np.clip(x * gain + offset, 0, 100)
    return out


def score(source_rgb, render_rgb, adapt=True):
    """source_rgb and render_rgb must already be the same size."""
    s = _blocks(np.asarray(source_rgb, dtype=np.float64))
    r = _blocks(np.asarray(render_rgb, dtype=np.float64))

    ls = eink.rgb_to_lab(s.reshape(-1, 3))
    lr = eink.rgb_to_lab(r.reshape(-1, 3))
    lr_raw = lr
    if adapt:
        lr = _adapt(ls, lr)

    dE = np.sqrt(((ls - lr) ** 2).sum(axis=1))

    # Hue angle, weighted by how colourful the SOURCE is: a hue error in a
    # near-grey block is meaningless, one in saturated hair is the whole story.
    cs = np.sqrt(ls[:, 1] ** 2 + ls[:, 2] ** 2)
    cr = np.sqrt(lr_raw[:, 1] ** 2 + lr_raw[:, 2] ** 2)
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


def surviving_noise(render_rgb, target_rgb, sigma=1.6):
    """
    How much dither grain a viewer actually sees.

    Counting pixels that differ from all four neighbours seems like the
    obvious measure and is badly wrong: a perfect checkerboard scores 100% and
    looks perfectly smooth. On this panel that matters more than it sounds,
    because purple can only be made by alternating red and blue — there is no
    magenta pigment — so the most regular, best-looking rendering of a purple
    region is also the one where every pixel differs from every neighbour.
    Judged that way, gamut mapping looked like it had tripled the noise when
    it had in fact cut it by more than half.

    So blur both images and measure what is left. A regular pattern averages
    away; clumped error diffusion leaves low-frequency blotches, and those are
    what reads as grain. On a synthetic control, a checkerboard scores 0.25
    and random dots at the same density score 13.5.
    """
    r = np.arange(-4, 5)
    k = np.exp(-r ** 2 / (2 * sigma ** 2))
    k /= k.sum()

    def blur(a):
        p = np.pad(a, ((4, 4), (4, 4), (0, 0)), mode="edge")
        o = sum(w * p[i:i + a.shape[0], 4:4 + a.shape[1]] for i, w in enumerate(k))
        p = np.pad(o, ((0, 0), (4, 4), (0, 0)), mode="edge")
        return sum(w * p[:, i:i + a.shape[1]] for i, w in enumerate(k))

    return float(np.std(blur(np.asarray(render_rgb, float)) -
                        blur(np.asarray(target_rgb, float))))
