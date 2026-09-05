import json, os, subprocess, importlib
import numpy as np
from PIL import Image
ROOT=os.path.abspath("../firmware")
def build(damp):
    subprocess.run(["c++","-O2","-std=c++17","-D","NATIVE_TEST",
        "-D",f"ERROR_CHROMA_DAMP={damp}f","-I",f"{ROOT}/test/mocks","-I",f"{ROOT}/src",
        "-o",f"{ROOT}/tools/dither_cli",f"{ROOT}/tools/dither_cli.cpp"],check=True)
import native_dither, eink, fill_modes, fidelity, tone_select
from tone_map import compress
P=np.array(eink.PALETTE_RGB,dtype=np.uint8)
tone=json.load(open('/tmp/tone.json'))
FILES=["995beeb2.jpg","bb8149c0.jpg"]+[tone[i]['file'] for i in (0,8,20,35,50,75,97)]
srcs={f:np.asarray(fill_modes.build(Image.open(f'gallery/{f}').convert('RGB'),fill_modes.ADAPTIVE,bg_style=1)) for f in FILES}
print(f"  {'damp':>6s} {'dE':>7s} {'hue':>7s} {'grain':>7s}")
for damp in (1.0,1.1,1.25,1.4):
    build(damp)
    d=[];h=[];n=[]
    for f in FILES:
        src=srcs[f]; best=None
        for s in tone_select.SCALES:
            for g in (False,True):
                c=compress(src,s)
                if g: c=native_dither.gamut_map(c,0.6)
                e=eink.enhance_for_eink(c,1); i=np.asarray(eink.dither(e,1)[1])
                m=fidelity.score(src,P[i]); k=m['dE']+tone_select.HUE_WEIGHT*m['hue']
                if best is None or k<best[0]: best=(k,m,fidelity.surviving_noise(P[i],e))
        d.append(best[1]['dE']); h.append(best[1]['hue']); n.append(best[2])
    print(f"  {damp:6.2f} {np.mean(d):7.2f} {np.mean(h):7.2f} {np.mean(n):7.2f}")
build(1.0)
