import numpy as np, struct, sys
from PIL import Image
W,H=2048,1024
rng=np.random.default_rng(7)
def fbm(w,h,octs,seed,wrapx=True):
    r=np.random.default_rng(seed); out=np.zeros((h,w))
    amp=1; tot=0
    for o in range(octs):
        gw,gh=2**(o+2),2**(o+1)+1
        g=r.random((gh,gw))
        # bilinear upsample with x wrap
        ys=np.linspace(0,gh-1,h); xs=np.linspace(0,gw,w,endpoint=False)
        y0=np.floor(ys).astype(int); y1=np.minimum(y0+1,gh-1); fy=(ys-y0)[:,None]
        x0=np.floor(xs).astype(int)%gw; x1=(x0+1)%gw; fx=(xs-np.floor(xs))[None,:]
        fy=fy*fy*(3-2*fy); fx=fx*fx*(3-2*fx)
        a=g[y0][:,x0]*(1-fx)+g[y0][:,x1]*fx; b=g[y1][:,x0]*(1-fx)+g[y1][:,x1]*fx
        out+=amp*(a*(1-fy)+b*fy); tot+=amp; amp*=0.55
    return out/tot
def make(name, zen, hor, below, cloud_lo, cloud_hi, mtn, mtn_hi, fogc, seed):
    v=(np.arange(H)+0.5)/H; el=(0.5-v)*np.pi   # elevation radians
    elc=el[:,None]*np.ones((1,W))
    img=np.zeros((H,W,3))
    t=np.clip(elc/(np.pi/2),0,1)
    sky=hor[None,None,:]*(1-t[...,None]**0.6)+zen[None,None,:]*(t[...,None]**0.6)
    # clouds: stretched noise, stronger near mid elevations
    n=fbm(W,H,7,seed)
    c=np.clip((n-0.38)*3.2,0,1)
    cloud=cloud_lo[None,None,:]*(1-n[...,None])+cloud_hi[None,None,:]*n[...,None]
    cw=c*np.clip(elc*6,0,1)[...]*np.clip(1-elc/1.4,0.3,1)
    sky=sky*(1-cw[...,None]*0.9)+cloud*cw[...,None]*0.9
    img[:]=sky
    # mountains: ridge heights per column, two layers
    x=np.arange(W)
    for layer,(scale,amp,base,col,s2) in enumerate([(1.0,0.16,0.012,mtn*1.1,seed+1),(1.0,0.09,0.004,mtn*0.92,seed+2)]):
        r=fbm(W,64,8,s2)[32]
        ridge=base+amp*np.clip((r-0.40)*2.5,0,1)**1.3
        mask=(elc>=0)&(elc<ridge[None,:])
        hshade=np.clip(elc/np.maximum(ridge[None,:],1e-3),0,1)
        sn=np.clip((hshade-0.75)*4,0,1)[...,None]
        mc=col[None,None,:]*(1-sn*0.7)+mtn_hi[None,None,:]*sn*0.7
        img[mask]=mc[mask]
    # fog: blend toward fog colour near horizon (above and below)
    f=np.exp(-np.abs(elc)/0.05)
    img=img*(1-f[...,None]*0.75)+fogc[None,None,:]*f[...,None]*0.75
    # below horizon: dark misty void fading
    b=np.clip(-elc/0.5,0,1)
    belowc=hor*(1-b[...,None])+below*b[...,None]
    img=np.where(elc[...,None]<0, belowc*(1-f[...,None]*0.6)+fogc*f[...,None]*0.6, img)
    img=np.clip(img,0,1)
    a=(img*255+rng.random(img.shape)*1.0).astype(np.uint8)
    Image.fromarray(a).save(name+'.png')
    rgba=np.concatenate([a,np.full((H,W,1),255,np.uint8)],2)
    with open('NorthgardVR_sky_'+name+'.raw','wb') as fp:
        fp.write(struct.pack('<II',W,H)); fp.write(rgba.tobytes())
C=lambda *x: np.array(x)/255.0
make('stormy', C(70,90,118), C(150,170,192), C(52,68,92), C(85,105,135), C(170,188,208), C(118,138,162), C(220,228,236), C(128,148,172), 3)
make('dusk', C(38,46,84), C(235,150,110), C(40,34,52), C(120,90,110), C(250,190,150), C(80,70,92), C(200,150,140), C(190,130,120), 11)
