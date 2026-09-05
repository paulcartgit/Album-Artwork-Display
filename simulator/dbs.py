"""
Direct multitone search: stop dithering greedily.

Floyd-Steinberg (1976) walks the image once, picks the nearest pigment for
each pixel, and pushes the leftover error onto neighbours it has not reached
yet. Every decision is final and made with no knowledge of what comes after.
That is why flat pale areas come out speckled: the algorithm has no way to
notice that a different arrangement of the SAME pigments would look better.

Direct Binary Search (Analoui & Allebach, 1992) treats it as what it is — an
optimisation. Model the eye as a blur, then repeatedly ask of each pixel: does
swapping its pigment reduce the difference between the blurred render and the
blurred target? Keep the swaps that help. It was impractical for decades and
is not any more.

The trick that makes it cheap: because the metric is a blurred sum of squares,
the change from swapping one pixel can be evaluated in closed form from the
already-blurred error, with no re-blurring:

    dSSE  =  2 (e * g)[p] . d  +  |d|^2 * sum(g^2)

where d is the colour difference between the new pigment and the old. So a
whole pass over every pixel and every candidate pigment is a handful of array
operations.

Work in linear light, not sRGB: the eye integrates photons, and averaging
gamma-encoded values makes dithered mixtures come out too dark.

The objective has to be perceptual, and this is where the first attempt went
wrong. Plain squared error in linear RGB is dominated by luminance, so the
optimiser bought luminance accuracy by throwing away chroma — the same
"everything goes grey" failure as nearest-in-Lab matching, only pursued
deliberately and therefore worse. It duly minimised its objective while making
every cover measurably worse. The cost is now taken in an opponent basis
(luminance, red-green, blue-yellow) with the chroma axes weighted up, which
stays a quadratic form so the closed-form update still holds.
"""
import numpy as np

import eink


def _gauss(radius, sigma):
    x = np.arange(-radius, radius + 1)
    k = np.exp(-(x ** 2) / (2 * sigma ** 2))
    return k / k.sum()


def _sep_blur(a, k):
    r = len(k) // 2
    pad = np.pad(a, ((r, r), (r, r), (0, 0)), mode="edge")
    out = np.zeros_like(a)
    for i, w in enumerate(k):
        out += w * pad[i:i + a.shape[0], r:r + a.shape[1]]
    pad = np.pad(out, ((0, 0), (r, r), (0, 0)), mode="edge")
    out = np.zeros_like(a)
    for i, w in enumerate(k):
        out += w * pad[:, i:i + a.shape[1]]
    return out


def _to_linear(srgb):
    v = np.asarray(srgb, float) / 255.0
    return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4)


# Linear RGB -> opponent (luminance, red-green, blue-yellow).
_OPP = np.array([[0.2126, 0.7152, 0.0722],
                 [0.5000, -0.4000, -0.1000],
                 [0.1000, 0.3000, -0.4000]])


def _opponent(lin, chroma_weight):
    w = np.array([1.0, chroma_weight, chroma_weight])[:, None]
    return lin @ (_OPP * w).T


def refine(target_rgb, idx, passes=2, sigma=2.0, radius=4, chroma_weight=3.0):
    """
    Improve an existing index map. `idx` is whatever Floyd-Steinberg produced;
    this only ever accepts changes that lower the perceived error, so it cannot
    do worse than its starting point.
    """
    pal = _opponent(_to_linear(np.array(eink.PALETTE_RGB, float)), chroma_weight)
    tgt = _opponent(_to_linear(target_rgb), chroma_weight)
    k = _gauss(radius, sigma)
    g2 = float((np.outer(k, k) ** 2).sum())

    idx = np.asarray(idx).copy()
    cur = pal[idx]
    err = _sep_blur(cur - tgt, k)                            # blurred error

    h, w = idx.shape
    for _ in range(passes):
        changed = 0
        # A sublattice spaced so that no two pixels in it can interact: any
        # two are more than the blur's support apart, so their gains are
        # exactly independent and applying them together is safe. Spacing 2
        # was not enough — pixels two apart still overlap a radius-3 blur, and
        # the objective went UP on later passes. Re-decide for each offset,
        # because a decision taken before its neighbour moved is stale.
        step = 2 * radius + 1
        for oy in range(step):
            for ox in range(step):
                eg = _sep_blur(err, k)                       # e * g
                d = pal[None, None, :, :] - cur[:, :, None, :]
                dsse = 2.0 * (eg[:, :, None, :] * d).sum(-1) + g2 * (d ** 2).sum(-1)
                best = np.argmin(dsse, axis=2)
                gain = np.take_along_axis(dsse, best[:, :, None], 2)[:, :, 0]

                m = np.zeros((h, w), bool)
                m[oy::step, ox::step] = True
                m &= gain < -1e-9
                m &= best != idx
                if not m.any():
                    continue
                delta = np.zeros_like(cur)
                delta[m] = pal[best[m]] - cur[m]
                idx[m] = best[m]
                cur[m] = pal[best[m]]
                err += _sep_blur(delta, k)
                changed += int(m.sum())
        if changed == 0:
            break
    return idx
