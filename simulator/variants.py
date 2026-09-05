import hashlib, io, json, time, urllib.request, urllib.parse
import numpy as np
from PIL import Image, ImageDraw
import eink, fill_modes, fidelity, tone_select, native_dither
from tone_map import compress

UA={"User-Agent":"NowPlayingFrame/1.0 (e-ink album frame; cover variant selection)"}
P=np.array(eink.PALETTE_RGB,dtype=np.uint8)

def fetch(url, timeout=25):
    return urllib.request.urlopen(urllib.request.Request(url,headers=UA),timeout=timeout).read()

def score(art):
    src=np.asarray(fill_modes.build(art, fill_modes.ADAPTIVE, bg_style=1))
    best=None
    for s in tone_select.SCALES:
        for g in (False,True):
            c=compress(src,s)
            if g: c=native_dither.gamut_map(c,0.6)
            e=eink.enhance_for_eink(c,1)
            i=np.asarray(eink.dither(e,1)[1])
            m=fidelity.score(src,P[i])
            k=m['dE']+tone_select.HUE_WEIGHT*m['hue']
            if best is None or k<best[0]: best=(k,m,P[i],e)
    k,m,img,e=best
    return k, m, img, fidelity.surviving_noise(img,e)

for artist,album in (("The Beatles","Help!"),("Miles Davis","Kind of Blue")):
    q=urllib.parse.quote(f'artist:"{artist}" AND release:"{album}"')
    try:
        d=json.loads(fetch(f"https://musicbrainz.org/ws/2/release/?query={q}&fmt=json&limit=25"))
    except Exception as ex:
        print(f"\n  {album}: MusicBrainz query failed ({ex})"); continue
    seen={}; rows=[]
    for r in d.get("releases",[]):
        if len(rows)>=6: break
        # Cover Art Archive redirects to archive.org, which is often slow and
        # frequently has no art for a given release. Both are normal; skip and
        # move on rather than letting one stall the lot.
        try:
            raw=fetch(f"https://coverartarchive.org/release/{r['id']}/front-500",timeout=12)
        except Exception:
            continue
        h=hashlib.md5(raw).hexdigest()
        if h in seen: continue
        seen[h]=1
        try: art=Image.open(io.BytesIO(raw)).convert("RGB")
        except Exception: continue
        k,m,img,noise=score(art)
        rows.append((k,m,noise,r.get('date','?'),r.get('country','?'),art,img))
        time.sleep(0.3)
    if not rows: print(f"\n  {album}: no cover art found"); continue
    rows.sort(key=lambda x:x[0])
    print(f"\n  {artist} — {album}: {len(rows)} distinct covers")
    for k,m,noise,date,cc,_,_ in rows:
        print(f"     score {k:6.2f}   dE {m['dE']:5.1f}  hue {m['hue']:5.1f}  grain {noise:5.1f}   {str(date):>10s} {cc}")
    spread=rows[-1][0]-rows[0][0]
    print(f"     spread between best and worst: {spread:.2f}")
    W,H=150,250
    sh=Image.new("RGB",(len(rows)*(W+6),H*2+30),"white"); dr=ImageDraw.Draw(sh)
    for i,(k,m,noise,date,cc,art,img) in enumerate(rows):
        dr.text((i*(W+6)+2,2),f"{k:.1f} {date}"[:20],fill="black")
        sh.paste(art.resize((W,W),Image.LANCZOS),(i*(W+6),14))
        sh.paste(Image.fromarray(img).resize((W,H),Image.LANCZOS),(i*(W+6),W+20))
    sh.save(f"/tmp/variants_{album.replace(' ','_').replace('!','')}.png")
