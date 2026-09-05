import json, subprocess, os
import numpy as np
from PIL import Image
import native_dither, eink, fill_modes, fidelity, tone_select
from tone_map import compress
P=np.array(eink.PALETTE_RGB,dtype=np.uint8)
tone=json.load(open('/tmp/tone.json')); label={t['file']:t['label'] for t in tone}
FILES=["995beeb2.jpg","bb8149c0.jpg"]+[tone[i]['file'] for i in (0,8,20,35,50,75,97)]
def full(src):
    """The shipped decision: 3 tone scales x gamut on/off, best score wins."""
    best=None
    for s in tone_select.SCALES:
        for g in (False,True):
            c=compress(src,s)
            if g: c=native_dither.gamut_map(c,0.6)
            e=eink.enhance_for_eink(c,1); i=np.asarray(eink.dither(e,1)[1])
            m=fidelity.score(src,P[i]); k=m['dE']+tone_select.HUE_WEIGHT*m['hue']
            if best is None or k<best[0]: best=(k,m,fidelity.surviving_noise(P[i],e))
    return best[1],best[2]
d=[];h=[];n=[]
print(f"{'cover':34s} {'dE':>6s} {'hue':>6s} {'grain':>7s}")
for f in FILES:
    src=np.asarray(fill_modes.build(Image.open(f'gallery/{f}').convert('RGB'),fill_modes.ADAPTIVE,bg_style=1))
    m,g=full(src); d.append(m['dE']); h.append(m['hue']); n.append(g)
    print(f"{label[f][:34]:34s} {m['dE']:6.1f} {m['hue']:6.1f} {g:7.1f}")
print(f"\n  {'MEAN':32s} {np.mean(d):6.2f} {np.mean(h):6.2f} {np.mean(n):7.2f}")
