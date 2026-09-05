"""
Local lightness compression: darken only the parts that need it.

A single scale for the whole image is a compromise. On the KPop sleeve the
pale figures gain a lot from being darkened, while the already-dark areas gain
nothing and just lose shadow detail. What actually decides it is per-pixel:
how much of this pixel's chroma is unreachable at its own lightness and hue.

The panel's gamut is strongly hue-dependent — at L*75 yellow can reach chroma
77 and purple can reach 0 — so the reachable ceiling is a 2-D table over
lightness and hue, built from the palette itself.

The scale map is heavily blurred before it is applied. An abrupt change in
lightness between neighbouring regions reads as a halo, and worse, can invert
local contrast: a bright area darkened next to an untouched one looks like a
stain. Blurring trades exactness for something that looks like lighting.
"""
import numpy as np
from itertools import product

import eink

NL, NH = 21, 24          # lightness x hue bins for the gamut ceiling
BLUR_PASSES = 3
BLUR_RADIUS = 24         # px, at 480x800
LIGHTNESS_COST = 0.35    # how dearly a lost L* unit is paid for


def _reachable_ceiling():
    """Max chroma the panel can average to, per (lightness, hue) bin."""
    P = np.array(eink.PALETTE_RGB, float)
    mixes = []
    for w in product(range(9), repeat=5):
        if sum(w) > 8:
            continue
        mixes.append((w[0]*P[0] + w[1]*P[2] + w[2]*P[3] +
                      w[3]*P[4] + w[4]*P[5] + (8-sum(w))*P[1]) / 8)
    M = eink.rgb_to_lab(np.array(mixes))
    L = np.clip((M[:, 0] / 100 * (NL - 1)).astype(int), 0, NL - 1)
    C = np.hypot(M[:, 1], M[:, 2])
    H = ((np.degrees(np.arctan2(M[:, 2], M[:, 1])) % 360) / 360 * NH).astype(int) % NH
    ceiling = np.zeros((NL, NH))
    np.maximum.at(ceiling, (L, H), C)
    return ceiling


CEILING = _reachable_ceiling()


def _box_blur(a, radius, passes=BLUR_PASSES):
    """Separable box blur, repeated — three passes approximate a Gaussian."""
    k = 2 * radius + 1
    for _ in range(passes):
        for axis in (0, 1):
            pad_width = [(0, 0), (0, 0)]
            pad_width[axis] = (radius, radius)
            pad = np.pad(a, pad_width, mode="edge")
            # Leading zero so the running sum has one more entry than the
            # padded array; without it the result comes back a row short.
            zero_shape = list(pad.shape)
            zero_shape[axis] = 1
            c = np.concatenate([np.zeros(zero_shape), np.cumsum(pad, axis=axis)],
                               axis=axis)
            hi = np.take(c, np.arange(k, c.shape[axis]), axis=axis)
            lo = np.take(c, np.arange(0, c.shape[axis] - k), axis=axis)
            a = (hi - lo) / k
    return a


def scale_map(rgb, scales=(1.0, 0.9, 0.8, 0.7)):
    """Per-pixel lightness scale, smoothed. 1.0 means leave alone."""
    a = np.asarray(rgb, float)
    h, w = a.shape[:2]
    lab = eink.rgb_to_lab(a.reshape(-1, 3))
    L, C = lab[:, 0], np.hypot(lab[:, 1], lab[:, 2])
    hb = ((np.degrees(np.arctan2(lab[:, 2], lab[:, 1])) % 360) / 360 * NH).astype(int) % NH

    # Minimise what is left over, rather than demanding a perfect fit. The
    # first version took the gentlest scale that brought chroma within reach
    # and left everything else at 1.0 — which excluded exactly the pixels that
    # gain most. The KPop sleeve's hair is hue 320, where the ceiling is zero
    # at every lightness, so it fit nowhere and was never darkened, and the
    # local pass scored worse on that cover than a single global scale.
    # Darkening still moves those pixels closer even though it cannot land
    # them.
    best_cost = None
    chosen = np.full(L.shape, 1.0)
    for s in scales:
        lb = np.clip((np.clip(L * s, 0, 100) / 100 * (NL - 1)).astype(int), 0, NL - 1)
        excess = np.maximum(0.0, C - CEILING[lb, hb])
        # Giving up lightness is a real cost, so it has to be paid for.
        cost = excess + LIGHTNESS_COST * L * (1.0 - s)
        if best_cost is None:
            best_cost, chosen = cost, np.full(L.shape, s)
        else:
            better = cost < best_cost
            best_cost = np.where(better, cost, best_cost)
            chosen = np.where(better, s, chosen)

    return _box_blur(chosen.reshape(h, w), BLUR_RADIUS)


def apply(rgb, smap):
    a = np.asarray(rgb, float)
    h, w = a.shape[:2]
    lab = eink.rgb_to_lab(a.reshape(-1, 3))
    lab[:, 0] *= smap.reshape(-1)
    from tone_map import lab_to_rgb
    return lab_to_rgb(lab).reshape(h, w, 3).astype(np.uint8)
