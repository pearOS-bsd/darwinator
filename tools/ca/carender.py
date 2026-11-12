import struct,sys
import caparse as c
from PIL import Image, ImageDraw
layers={};root=None
for n,m in enumerate(c.msgs):
    try: o,l=c.parse(m)
    except Exception as e: print('msg',n,'skipped:',e); continue
    for x in o:
        if x[0]=='layer_id': root=x[1]
        if x[0]=='set_object' and isinstance(x[3],dict):
            L=x[3]; d=struct.unpack_from('<6d',L['pod'],0x28)
            layers[x[1]]=dict(pos=(d[0],d[1]),size=(d[4],d[5]),subs=(L['subs'][1] if L['subs'] and L['subs'][0]=='ids' else []))
print(len(layers),'layers, root %x'%(root or 0))
img=Image.new('RGB',(1280,800),(36,36,36)); dr=ImageDraw.Draw(img)
def draw(i,ox,oy,depth):
    L=layers.get(i)
    if not L: return
    w,h=L['size']; x=ox+L['pos'][0]-w/2; y=oy+L['pos'][1]-h/2
    col=[(80,80,120),(90,140,90),(160,120,60),(150,70,70)][depth%4]
    dr.rectangle([x,y,x+w,y+h],outline=col)
    for s in L['subs']: draw(s,x,y,depth+1)
if root in layers: draw(root,0,0,0)
else:
    for i in layers: draw(i,0,0,0)
img.save('carender.png')
for i,L in list(layers.items())[:12]: print('%x'%i,L)
