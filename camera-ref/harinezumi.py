import sys,numpy as np
from PIL import Image,ImageFilter,ImageEnhance
def curve(x,b,w,gamma,s):  # b=lift blacks down, w=white point, s=contrast S
    y=(x-b)/(w-b); y=np.clip(y,0,1); y=y**gamma
    y=y+ s*(y-0.5)*(1-abs(2*y-1)); return np.clip(y,0,1)
def process(p,out):
    im=Image.open(p).convert('RGB'); W,H=im.size
    # 1) soften + lower res feel: downsample then up
    small=im.resize((W//3,H//3),Image.BILINEAR).resize((W,H),Image.BILINEAR)
    im=Image.blend(im,small,0.55).filter(ImageFilter.GaussianBlur(0.8))
    a=np.asarray(im).astype(np.float32)/255
    # 2) per-channel contrast curve (crushed blacks, blown highs, warm/green cast)
    a[...,0]=curve(a[...,0],0.06,0.93,0.92,0.35)   # R
    a[...,1]=curve(a[...,1],0.05,0.95,0.90,0.38)   # G lifted -> green cast
    a[...,2]=curve(a[...,2],0.08,0.90,0.98,0.30)   # B crushed more
    a[...,1]*=1.03; a[...,2]*=1.02                  # cool-green push
    # 3) heavy saturation
    g=a.mean(2,keepdims=True); a=g+(a-g)*1.7; a=np.clip(a,0,1)
    # 4) halation/bloom: blur bright areas, screen back
    bloom=np.asarray(Image.fromarray((np.clip((a-0.7)*3,0,1)*255).astype('uint8')).filter(ImageFilter.GaussianBlur(6))).astype(np.float32)/255
    a=1-(1-a)*(1-bloom*0.5)
    # 5) vignette
    yy,xx=np.mgrid[0:H,0:W]; cx,cy=W/2,H/2
    r=np.sqrt(((xx-cx)/cx)**2+((yy-cy)/cy)**2); vig=np.clip(1-0.55*np.clip(r-0.5,0,1)**1.6,0,1)[...,None]
    a*=vig
    # 6) grain
    a+=np.random.normal(0,0.045,a.shape)*(0.5+0.5*(1-a))  # more in shadows
    a=np.clip(a,0,1)
    Image.fromarray((a*255).astype('uint8')).save(out,quality=88)
process(sys.argv[1],sys.argv[2])
