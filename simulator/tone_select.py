"""
Pick the lightness compression by TRYING it, not by predicting it.

Mirrors chooseLightnessScale() in firmware/src/image_pipeline.cpp and the
constants in firmware/src/tone_map.h.

Three separate proxy metrics have now mispredicted the real dithered result
(chromatic share, gamut excess, and unreachable-chroma cost). The panel takes
20-25s to refresh; a trial dither at quarter resolution costs a fraction of a
second. So dither the candidates, score each against the source, and keep the
winner. No model to be wrong.
"""
import numpy as np
from PIL import Image
import eink, fill_modes, fidelity
from tone_map import compress

import re, pathlib
_h = (pathlib.Path(__file__).resolve().parents[1] /
      "firmware" / "src" / "tone_map.h").read_text()
SCALES = tuple(float(v) for v in re.search(
    r"TONEMAP_SCALE\[TONEMAP_SCALES\]\s*=\s*\{([^}]*)\}", _h).group(1).replace("f", "").split(","))
HUE_WEIGHT = float(re.search(r"#define\s+TONEMAP_HUE_WEIGHT\s+([0-9.]+)f?", _h).group(1))
TRIAL_DIV = int(re.search(r"#define\s+TONEMAP_TRIAL_DIV\s+(\d+)", _h).group(1))


def choose(src, trial_div=None, verbose=False):
    trial_div = trial_div or TRIAL_DIV
    small = np.asarray(Image.fromarray(np.asarray(src, np.uint8))
                       .resize((src.shape[1]//trial_div, src.shape[0]//trial_div),
                               Image.LANCZOS))
    best = None
    for s in SCALES:
        cand = compress(small, s) if s < 1.0 else small
        _, i = eink.dither(eink.enhance_for_eink(cand, 1), 1)
        out = np.array(eink.PALETTE_RGB, dtype=np.uint8)[np.asarray(i)]
        m = fidelity.score(small, out)
        score = m['dE'] + HUE_WEIGHT * m['hue']
        if verbose:
            print(f"      x{s:.2f}  dE {m['dE']:5.1f}  hue {m['hue']:5.1f}  score {score:5.1f}")
        if best is None or score < best[0]:
            best = (score, s)
    return best[1]


