import hashlib, io, json, time, urllib.request, urllib.parse
import numpy as np
from PIL import Image, ImageDraw
UA={"User-Agent":"NowPlayingFrame/1.0 (e-ink album frame; cover variant selection)"}
def fetch(u,t=12): return urllib.request.urlopen(urllib.request.Request(u,headers=UA),timeout=t).read()

def signature(img, n=16):
    """Coarse colour+structure fingerprint: what the sleeve IS, not how it prints."""
    a=np.asarray(img.convert("RGB").resize((n,n),Image.LANCZOS),float)/255.0
    # normalise per channel so a colour cast or a darker scan does not count
    for c in range(3):
        ch=a[...,c]; s=ch.std()
        a[...,c]=(ch-ch.mean())/(s if s>1e-4 else 1.0)
    return a.reshape(-1)

def similarity(a,b):
    x,y=signature(a),signature(b)
    return float(np.dot(x,y)/(np.linalg.norm(x)*np.linalg.norm(y)+1e-9))

def covers(artist,album,limit=10):
    q=urllib.parse.quote(f'artist:"{artist}" AND release:"{album}"')
    d=json.loads(fetch(f"https://musicbrainz.org/ws/2/release/?query={q}&fmt=json&limit={limit}",25))
    out=[];seen=set()
    for r in d.get("releases",[]):
        try: raw=fetch(f"https://coverartarchive.org/release/{r['id']}/front-500")
        except Exception: continue
        h=hashlib.md5(raw).hexdigest()
        if h in seen: continue
        seen.add(h)
        try: out.append((r.get('date','?'),Image.open(io.BytesIO(raw)).convert("RGB")))
        except Exception: pass
        if len(out)>=6: break
    return out

help_=covers("The Beatles","Help!")
print(f"  Help!: {len(help_)} distinct covers")
ref=help_[0][1]
for date,im in help_:
    print(f"    {str(date):>10s}  similarity to first {similarity(ref,im):+.3f}")
time.sleep(1.2)
other=covers("The Beatles","Abbey Road",6)
print(f"\n  DIFFERENT album (Abbey Road) against the Help! reference:")
for date,im in other[:4]:
    print(f"    {str(date):>10s}  similarity {similarity(ref,im):+.3f}")
W=120
sh=Image.new("RGB",((len(help_)+len(other[:4]))*(W+4),W+18),"white"); d=ImageDraw.Draw(sh)
for i,(dt,im) in enumerate(help_+other[:4]):
    d.text((i*(W+4)+2,2),f"{similarity(ref,im):+.2f}",fill="black")
    sh.paste(im.resize((W,W),Image.LANCZOS),(i*(W+4),14))
sh.save('/tmp/gate.png'); print("\n  wrote /tmp/gate.png")
