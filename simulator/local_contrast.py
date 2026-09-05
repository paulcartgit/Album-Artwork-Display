"""
Give back the local contrast that darkening takes away.

Compressing L* buys chroma — the pigments only hold colour low down, so pale
artwork has to come down to meet them. But it scales the local detail by the
same factor as the overall level, so the picture arrives darker AND flatter,
and flat is the part the eye objects to.

The two are separable. Split L* into a heavily blurred base and the detail
riding on it, then put the detail back at full strength while leaving the base
where the compression left it. The image keeps the lower average lightness
that made the colour reachable, and recovers the modelling that made it look
like something.

This is the standard move in HDR tone mapping, and it is licensed here by
something the fidelity metric already showed: a uniform lightness shift is
forgiven by a viewer who adapts to the panel, while flatness is not.
"""
import numpy as np

import eink
from tone_map import lab_to_rgb


def _blur(a, radius, passes=3):
    k = 2 * radius + 1
    for _ in range(passes):
        for axis in (0, 1):
            pad_width = [(0, 0), (0, 0)]
            pad_width[axis] = (radius, radius)
            pad = np.pad(a, pad_width, mode="edge")
            zeros = list(pad.shape)
            zeros[axis] = 1
            c = np.concatenate([np.zeros(zeros), np.cumsum(pad, axis=axis)], axis=axis)
            hi = np.take(c, np.arange(k, c.shape[axis]), axis=axis)
            lo = np.take(c, np.arange(0, c.shape[axis] - k), axis=axis)
            a = (hi - lo) / k
    return a


def restore(rgb, gain, radius=28):
    """
    gain is how much of the detail to give back. 1.0 changes nothing; 1/keep
    restores exactly what a compression by `keep` removed.
    """
    if gain <= 1.001:
        return np.asarray(rgb, np.uint8)
    a = np.asarray(rgb, float)
    h, w = a.shape[:2]
    lab = eink.rgb_to_lab(a.reshape(-1, 3))
    L = lab[:, 0].reshape(h, w)
    base = _blur(L, radius)
    lab[:, 0] = np.clip(base + gain * (L - base), 0, 100).reshape(-1)
    return lab_to_rgb(lab).reshape(h, w, 3).astype(np.uint8)
