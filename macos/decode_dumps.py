import glob, array, struct, sys
from PIL import Image
def half(b): return struct.unpack('<e', b)[0]
def fl(v,mb):
    e=(v>>mb)&0x1f; m=v&((1<<mb)-1); val=(m/float(1<<mb)+(1.0 if e>0 else 0.0))*2.0**((e-15) if e>0 else -14)
    return max(0,min(255,int(val*255)))
def decode(f):
    parts=f.split('/')[-1].split('_'); w,h=map(int,parts[2].split('x')); bpp=int(parts[3][3:]); fmt=int(parts[4][3:]); data=open(f,'rb').read()
    n=w*h
    if fmt==0: return Image.frombytes('RGBA',(w,h),data[:n*4]).convert('RGB')
    if fmt==7:
        a=array.array('I'); a.frombytes(data[:n*4]); px=bytearray(n*3)
        for i,v in enumerate(a): px[3*i]=(v&0x3ff)>>2; px[3*i+1]=((v>>10)&0x3ff)>>2; px[3*i+2]=((v>>20)&0x3ff)>>2
        return Image.frombytes('RGB',(w,h),bytes(px))
    if fmt==21:
        a=array.array('I'); a.frombytes(data[:n*4]); px=bytearray(n*3)
        for i,v in enumerate(a): px[3*i]=fl(v&0x7ff,6); px[3*i+1]=fl((v>>11)&0x7ff,6); px[3*i+2]=fl((v>>22)&0x3ff,5)
        return Image.frombytes('RGB',(w,h),bytes(px))
    if bpp==8:  # RGBA16F
        px=bytearray(n*3)
        for i in range(n):
            for c in range(3): px[3*i+c]=max(0,min(255,int(half(data[i*8+c*2:i*8+c*2+2])*255)))
        return Image.frombytes('RGB',(w,h),bytes(px))
    if bpp==4 and fmt==39:  # R32F (or depth aspect): contrast-stretch between min and max
        a=array.array('f'); a.frombytes(data[:n*4]); lo=min(a); hi=max(a); rng=(hi-lo) or 1.0
        print("  R32F range", lo, hi)
        px=bytes(max(0,min(255,int((v-lo)/rng*255))) for v in a)
        return Image.frombytes('L',(w,h),px).convert('RGB')
    if bpp==2:  # R16F or RG8 or R16
        px=bytes(max(0,min(255,int(half(data[i*2:i*2+2])*255))) for i in range(n))
        return Image.frombytes('L',(w,h),px).convert('RGB')
    if bpp==4:  # generic: show raw bytes as RGBA
        return Image.frombytes('RGBA',(w,h),data[:n*4]).convert('RGB')
    if bpp==1: return Image.frombytes('L',(w,h),data[:n]).convert('RGB')
    return None
for f in sorted(glob.glob('/tmp/rtdump_*.raw')):
    if f[:-4]+'_crop.png' in glob.glob('/tmp/*_crop.png'): continue
    im=decode(f)
    if im is None: print("undecoded",f); continue
    w,h=im.size; im.crop((0,int(h*0.55),int(w*0.6),h)).save(f[:-4]+'_crop.png'); im.save(f[:-4]+'_full.png'); print("decoded",f.split('/')[-1],im.size,im.getextrema())
