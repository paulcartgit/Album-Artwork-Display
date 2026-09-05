import json, os, subprocess
import numpy as np
from PIL import Image
ROOT=os.path.abspath("../firmware")
def build(d):
    subprocess.run(["c++","-O2","-std=c++17","-D","NATIVE_TEST","-D",f"ERROR_CHROMA_DAMP={d}f",
        "-I",f"{ROOT}/test/mocks","-I",f"{ROOT}/src","-o",f"{ROOT}/tools/dither_cli",
        f"{ROOT}/tools/dither_cli.cpp"],check=True)
import native_dither, eink, fill_modes, fidelity, tone_select
from tone_map import compress
P=np.array(eink.PALETTE_RGB,dtype=np.uint8)
tone=json.load(open('/tmp/tone.json')); label={t['file']:t['label'] for t in tone}
FILES=["995beeb2.jpg","bb8149c0.jpg"]+[tone[i]['file'] for i in (0,8,20,35,50,75,97)]
srcs={f:np.asarray(fill_modes.build(Image.open(f'gallery/{f}').convert('RGB'),fill_modes.ADAPTIVE,bg_style=1)) for f in FILES}
def run():
    out={}
    for f in FILES:
        src=srcs[f]; best=None
        for s in tone_select.SCALES:
            for g in (False,True):
                c=compress(src,s)
                if g: c=native_dither.gamut_map(c,0.6)
                e=eink.enhance_for_eink(c,1); i=np.asarray(eink.dither(e,1)[1])
                m=fidelity.score(src,P[i]); k=m['dE']+tone_select.HUE_WEIGHT*m['hue']
                if best is None or k<best[0]: best=(k,m)
        out[f]=best[1]
    return out
build(1.0); a=run()
build(1.1); b=run()
print(f"{'cover':34s} {'dE 1.00->1.10':>16s} {'hue 1.00->1.10':>18s}")
win=0
for f in FILES:
    better = b[f]['dE']<=a[f]['dE'] and b[f]['hue']<=a[f]['hue']
    win += better
    print(f"{label[f][:34]:34s} {a[f]['dE']:6.2f} ->{b[f]['dE']:6.2f}   {a[f]['hue']:6.2f} ->{b[f]['hue']:6.2f}  {'better' if better else ''}")
print(f"\n  better on both axes: {win}/{len(FILES)} covers")
build(1.0)
