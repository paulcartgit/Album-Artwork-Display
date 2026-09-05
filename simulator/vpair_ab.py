"""Did the five extra blended colours help? Score the index map, not the preview."""
import importlib, json
import numpy as np
from PIL import Image, ImageDraw
OLD=((2,3),(4,3))
NEW=((2,3),(4,3),(4,5),(2,5),(4,2),(4,1),(5,1))
def run(pairs, files):
    import eink; importlib.reload(eink)
    eink.VIRTUAL_PAIR=pairs
    eink.MATCH_COLORS=eink.EPD_COLORS+len(pairs)
    eink.MATCH_PAL=np.vstack([eink.PALETTE,
        np.array([(eink.PALETTE[a]+eink.PALETTE[b])/2.0 for a,b in pairs])])
    eink.MATCH_PAL_LAB=eink.rgb_to_lab(eink.MATCH_PAL)
    ch=np.sqrt(eink.MATCH_PAL_LAB[:,1]**2+eink.MATCH_PAL_LAB[:,2]**2)
    eink.ACHROMATIC=ch<5.0
    import fill_modes; importlib.reload(fill_modes)
    out={}
    for f in files:
        art=Image.open(f"gallery/{f}").convert("RGB")
        c=eink.enhance_for_eink(fill_modes.build(art,1,bg_style=1),1)
        _,i=eink.dither(c,1); out[f]=np.asarray(i).copy()
    return out
tone=json.load(open('/tmp/tone.json')); label={t['file']:t['label'] for t in tone}
files=[tone[i]['file'] for i in (0,1,3,8,20,35,50,60,75,88,97)]
a=run(OLD,files); b=run(NEW,files)
import eink
TRUE=np.array(eink.PALETTE_RGB,dtype=np.uint8)
print(f"{'cover':40s} {'chromatic %':>16s} {'white %':>16s}")
da,db=[],[]
for f in files:
    ca=np.bincount(a[f].ravel(),minlength=6); cb=np.bincount(b[f].ravel(),minlength=6)
    x,y=ca[2:].sum()/a[f].size*100, cb[2:].sum()/b[f].size*100
    wa,wb=ca[1]/a[f].size*100, cb[1]/b[f].size*100
    da.append(x); db.append(y)
    print(f"{label[f][:40]:40s} {x:5.1f} -> {y:5.1f}   {wa:5.1f} -> {wb:5.1f}  ({(a[f]!=b[f]).mean()*100:4.1f}% differ)")
print(f"\n  mean chromatic  {np.mean(da):5.1f} -> {np.mean(db):5.1f}")
TW,TH=150,250; cols=4; rows=(len(files)+cols-1)//cols
sheet=Image.new("RGB",(cols*(TW*2+14),rows*(TH+24)),"white"); d=ImageDraw.Draw(sheet)
for k,f in enumerate(files):
    x=(k%cols)*(TW*2+14); y=(k//cols)*(TH+24)
    d.text((x+2,y+6),label[f][:44],fill="black")
    for j,src in enumerate((a[f],b[f])):
        sheet.paste(Image.fromarray(TRUE[src]).resize((TW,TH),Image.LANCZOS),(x+j*(TW+4),y+18))
sheet.save('/tmp/vpair_sheet.png'); print("wrote /tmp/vpair_sheet.png (left=2 colours, right=7)")
