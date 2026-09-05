"""Lightness compression helpers, importable without side effects."""
import numpy as np
import eink

def lab_to_rgb(lab):
    L,a,b=lab[:,0],lab[:,1],lab[:,2]
    fy=(L+16)/116; fx=fy+a/500; fz=fy-b/200
    def inv(t): return np.where(t**3>0.008856, t**3, (t-16/116)/7.787)
    X=inv(fx)*95.047; Y=inv(fy)*100.0; Z=inv(fz)*108.883
    X,Y,Z=X/100,Y/100,Z/100
    r= 3.2406*X -1.5372*Y -0.4986*Z
    g=-0.9689*X +1.8758*Y +0.0415*Z
    bb= 0.0557*X -0.2040*Y +1.0570*Z
    def enc(c):
        c=np.clip(c,0,1)
        return np.where(c<=0.0031308, c*12.92, 1.055*c**(1/2.4)-0.055)
    return np.clip(np.stack([enc(r),enc(g),enc(bb)],1)*255,0,255)

def compress(rgb, keep):
    """Scale L* toward the panel's range, leaving a* and b* untouched."""
    if keep >= 0.999:
        return np.asarray(rgb, np.uint8)   # exact no-op, no Lab round trip
    a=np.asarray(rgb,float); h,w,_=a.shape
    lab=eink.rgb_to_lab(a.reshape(-1,3))
    lab[:,0]*=keep
    return lab_to_rgb(lab).reshape(h,w,3).astype(np.uint8)

