import importlib, json
import numpy as np
from PIL import Image, ImageDraw
CURRENT=[(0x10,0x10,0x12),(0xD8,0xDA,0xD4),(0x30,0x66,0x58),(0x38,0x68,0xC0),(0x9C,0x30,0x2C),(0xC8,0xB8,0x30)]
MEASURED=[(0x0D,0x0A,0x10),(0xE0,0xE0,0xD9),(0x1F,0x6C,0x45),(0x00,0x5D,0xAB),(0xBD,0x0F,0x05),(0xFF,0xDA,0x1B)]
def idx_for(pal, files):
    import firmware_config as fc
    fc.PALETTE_RGB=[tuple(int(v) for v in c) for c in pal]
    import eink; importlib.reload(eink)
    import fill_modes; importlib.reload(fill_modes)
    out={}
    for f in files:
        art=Image.open(f"gallery/{f}").convert("RGB")
        c=fill_modes.build(art,1,bg_style=1)
        c=eink.enhance_for_eink(c,1)
        _,i=eink.dither(c,1); out[f]=np.asarray(i).copy()
    return out
tone=json.load(open('/tmp/tone.json')); label={t['file']:t['label'] for t in tone}
files=[tone[i]['file'] for i in (3,20,35,50,75,97)]
a=idx_for(CURRENT,files); b=idx_for(MEASURED,files)
# Paint BOTH with the measured pigments: that is what the panel physically shows.
TRUE=np.array(MEASURED,dtype=np.uint8)
TW,TH=170,283
sheet=Image.new("RGB",(3*(TW*2+16), 2*(TH+24)),"white"); d=ImageDraw.Draw(sheet)
for k,f in enumerate(files):
    x=(k%3)*(TW*2+16); y=(k//3)*(TH+24)
    d.text((x+2,y+6), label[f][:46], fill="black")
    for j,src in enumerate((a[f], b[f])):
        im=Image.fromarray(TRUE[src])
        sheet.paste(im.resize((TW,TH),Image.LANCZOS),(x+j*(TW+4), y+18))
sheet.save('/tmp/palette_sheet.png'); print("wrote /tmp/palette_sheet.png (left=old palette, right=new)")
