"""
Hue-preserving gamut mapping.

The dither matches each pixel to the nearest palette entry in Lab. When the
pixel is outside what the panel can reach, "nearest" trades away whatever is
cheapest in Lab distance — and that is usually HUE. It is why the KPop
sleeve's rose skin, #E8A2C5, lands on a drab #C6A8A7: grey is genuinely closer
in Lab than anything the panel can make at that hue, so the dither picks grey.

Every tone-mapping attempt so far has been a way of nudging pixels somewhere
the matcher would treat more kindly. This does the thing itself: move each
out-of-gamut colour onto the gamut boundary along its OWN hue line, spending
lightness and chroma, never hue. A pink that cannot be that pink becomes a
darker, duller pink instead of a grey-brown.

Global darkening is a crude special case of this — one lightness scale for the
whole frame, chosen by trial. This is per pixel and needs no trial.
"""
import numpy as np
from itertools import product

import eink

NL, NH = 33, 48          # finer than the diagnostic table; this one is applied


def _ceiling():
    P = np.array(eink.PALETTE_RGB, float)
    mixes = []
    for w in product(range(9), repeat=5):
        if sum(w) > 8:
            continue
        mixes.append((w[0]*P[0] + w[1]*P[2] + w[2]*P[3] +
                      w[3]*P[4] + w[4]*P[5] + (8-sum(w))*P[1]) / 8)
    M = eink.rgb_to_lab(np.array(mixes))
    Lb = np.clip((M[:, 0] / 100 * (NL - 1)).astype(int), 0, NL - 1)
    C = np.hypot(M[:, 1], M[:, 2])
    Hb = ((np.degrees(np.arctan2(M[:, 2], M[:, 1])) % 360) / 360 * NH).astype(int) % NH
    ceil = np.zeros((NL, NH))
    np.maximum.at(ceil, (Lb, Hb), C)
    # A hue the panel cannot hold at some lightness is still worth aiming at
    # from a neighbouring bin, so smooth across hue a little rather than
    # leaving holes that pixels fall into.
    ceil = np.maximum(ceil, 0.5 * (np.roll(ceil, 1, 1) + np.roll(ceil, -1, 1)))
    return ceil


CEILING = _ceiling()
_L_GRID = np.linspace(0, 100, NL)


def map_image(rgb, lightness_weight=0.6, native=True):
    """Delegates to the firmware's gamut.h unless native=False.

    The Python version below stays as the readable reference, but it is not
    what the panel runs: the two agreed on quality (dE 9.2 against 9.1) while
    differing by about 6 levels per pixel, which is exactly the kind of quiet
    disagreement that cost a day earlier.
    """
    if native:
        try:
            import native_dither
            return native_dither.gamut_map(rgb, lightness_weight)
        except Exception:
            pass
    return _map_image_python(rgb, lightness_weight)


def _map_image_python(rgb, lightness_weight=0.6):
    """
    Pull every pixel inside the gamut, keeping its hue.

    lightness_weight sets how willingly lightness is spent to keep chroma.
    Low values keep the picture bright and desaturate; high values hold colour
    and let the image go dark.
    """
    a = np.asarray(rgb, float)
    h, w = a.shape[:2]
    lab = eink.rgb_to_lab(a.reshape(-1, 3))
    L, A, B = lab[:, 0], lab[:, 1], lab[:, 2]
    C = np.hypot(A, B)
    ang = np.arctan2(B, A)
    hb = ((np.degrees(ang) % 360) / 360 * NH).astype(int) % NH

    # Candidate lightnesses for every pixel at once: cost of moving to each
    # grid lightness, plus the chroma that would have to be given up there.
    reach = CEILING[:, hb]                       # (NL, npix)
    reachable_C = np.minimum(C[None, :], reach)
    dL = _L_GRID[:, None] - L[None, :]
    dC = reachable_C - C[None, :]
    cost = (lightness_weight * dL) ** 2 + dC ** 2

    best = np.argmin(cost, axis=0)
    idx = np.arange(L.size)
    newL = _L_GRID[best]
    newC = reachable_C[best, idx]

    # Only pull pixels that were actually outside; leave the rest untouched so
    # in-gamut artwork is bit-for-bit unchanged.
    outside = C > CEILING[np.clip((L / 100 * (NL - 1)).astype(int), 0, NL - 1), hb]
    newL = np.where(outside, newL, L)
    newC = np.where(outside, newC, C)

    out = np.empty_like(lab)
    out[:, 0] = newL
    out[:, 1] = newC * np.cos(ang)
    out[:, 2] = newC * np.sin(ang)

    from tone_map import lab_to_rgb
    return lab_to_rgb(out).reshape(h, w, 3).astype(np.uint8)
