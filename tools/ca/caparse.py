import struct,sys
d=open('ca.bin','rb').read()
msgs=[];p=0
while p+4<=len(d):
    n=struct.unpack_from('<I',d,p)[0]; p+=4
    msgs.append(d[p:p+n]); p+=n

class R:
    def __init__(s,b): s.b=b; s.p=0
    def u8(s): v=s.b[s.p]; s.p+=1; return v
    def u16(s): v=struct.unpack_from('<H',s.b,s.p)[0]; s.p+=2; return v
    def u32(s): v=struct.unpack_from('<I',s.b,s.p)[0]; s.p+=4; return v
    def u64(s): v=struct.unpack_from('<Q',s.b,s.p)[0]; s.p+=8; return v
    def f64(s): v=struct.unpack_from('<d',s.b,s.p)[0]; s.p+=8; return v
    def raw(s,n): v=s.b[s.p:s.p+n]; s.p+=n; return v
    def left(s): return len(s.b)-s.p
def _dbg1():
    for n,m in enumerate(msgs):
        r=R(m[0x1c:])
        flags=r.u32(); ctx=r.u32(); ts=r.f64()
        ops=[]
        print('--- msg',n,len(m),'flags',flags,'ctx',ctx,'ts %.3f'%ts, 'rest bytes',r.left())
        print(r.b[r.p:r.p+64].hex())
    
    
def obj(r,req=0,depth=0):
    t=r.u8()
    if t==0: return None
    if t==0x1d: return kv(r)
    if t==0xff:
        i=r.u64(); g=r.u32(); return ('ref',i,g)
    if t==0xfe:
        i=r.u64(); g=r.u32(); o=obj(r,req,depth+1); return ('def',i,g,o)
    if t==0x1e: return layer(r,depth)
    if t==0x36:
        n=r.u32(); return ('string',r.raw(n))
    if t==0x3e:
        n=r.u32(); return ('vector',list(struct.unpack_from('<%dd'%n,r.raw(8*n))))
    if t==0x34:
        fl=r.u32()
        if fl&3==0:
            n=r.u64(); return ('shmem_inline',fl,r.raw(n) if n not in (0xffffffff,0xffffffffffffffff) else 'attached')
        a=r.u64(); b=r.u64(); return ('shmem_port',fl,a,b)
    if t==0x12: return filt(r)
    if t==0x17:				# CA::Render::Image::decode: i32, i8, i8 (planes), i32 w, i32 h, planes x u64 (row bytes), i8 shared?, then a Shmem object or inline data
        fl=r.u32(); fmt=r.u8(); np=r.u8(); w=r.u32(); h=r.u32()
        rows=[r.u64() for _ in range(np)]
        if r.u8(): data=obj(r,0x34,depth+1)
        else:
            n=r.u64(); data=r.raw(n) if n not in (0xffffffff,0xffffffffffffffff) else 'attached'
        return ('image',fl,fmt,w,h,rows,data)
    if t==0x1f:				# CA::Render::LayerHost: Object flags i32, the context to show (a port of the message: no bytes), i32, i8
        of=r.u32(); return ('layerhost',of,r.u32(),r.u8())	# the Object base reads an i32 (flags) first
    raise Exception('object type %d at %d'%(t,r.p))
def arr(r,retained,depth):
    tag=r.u8()
    if tag==0: return None
    assert tag==1
    fl=r.u32(); n=r.u32()
    if fl&1: return ('ids',[r.u64() for _ in range(n)])
    return ('objs',[obj(r,0,depth+1) for _ in range(n)])
def kv(r):
    raise Exception('keyvalue not decoded yet at %d'%r.p)
def cf(r):
    t=r.u8()
    if t==1: return None
    if t==2:
        f=r.u8()
        if not f: return None
        n=r.u64(); return r.raw(n).rstrip(b'\0').decode('latin1')
    if t==3:
        k=r.u8(); v=r.u64()
        return v if k==1 else struct.unpack('<q',struct.pack('<Q',v))[0]
    if t==4: return bool(r.u8())
    if t==5:
        n=r.u64(); return ('data',r.raw(n) if n not in (0xffffffff,0xffffffffffffffff) else 'attached')
    if t==6:
        n=r.u64(); return [cf(r) for _ in range(n)]
    if t==7:
        n=r.u64(); d={}
        for _ in range(n):
            k=cf(r); d[k]=cf(r)
        return d
    raise Exception('cf type %d at %d'%(t,r.p))
def atom(r):
    b=r.u8()
    if b<0xfe: return b
    if b==0xfe: return r.u16()
    n=r.u64(); return ('atomstr',r.raw(n))
def filt(r):
    fl=r.u32()
    if fl>>8&1: return ('filter_shared',fl,atom(r))
    k=r.u8()
    assert k==1,'filter kind %d'%k
    a=atom(r); name=obj(r)
    kv=arr(r,False,0)
    return ('filter',fl,a,name,kv)
def layer(r,depth):
    fl=r.u32()
    pod=r.raw(0x58)
    contents=obj(r)
    subs=arr(r,True,depth)
    v=r.u64()
    o=obj(r)
    ext=r.u8()
    e=None
    if ext:
        e={}
        e['sub']=obj(r)
        for k in ['n3a','n3e_a','n3e_b','n3e_c','n3e_d','n3e_e']: e[k]=obj(r)
        e['c']=obj(r)
        for k in ['n3e_f','n3e_g','n27','n12']: e[k]=obj(r)
        e['a12a']=arr(r,False,depth); e['a12b']=arr(r,False,depth)
        for k in ['n23','n26','n27b']: e[k]=obj(r)
        e['a2c']=arr(r,False,depth)
        e['n40']=obj(r); e['n42']=obj(r)
        e['aremote']=arr(r,False,depth)
        e['b1']=r.raw(16); e['b2']=r.raw(16); e['i1']=r.u64(); e['i2']=r.u64()
        e['b3']=r.raw(0x14); e['b4']=r.raw(8)
        e['i32a']=[r.u32() for _ in range(4)]; e['b5']=r.raw(0x14); e['i32b']=[r.u32() for _ in range(5)]
        e['b6']=r.raw(0x14); e['i32c']=[r.u32() for _ in range(5)]; e['b7']=r.raw(0x14)
        e['i32d']=r.u32(); e['vm']=r.u64(); e['o1']=obj(r); e['o2']=obj(r); e['enum']=r.u8()
    return dict(t='layer',fl=fl,pod=pod,contents=contents,subs=subs,v=v,o=o,ext=ext,e=e)
def parse(m,verbose=True):
    st=0x1c
    if struct.unpack_from('<I',m,0)[0]&0x80000000: st=0x1c+12*struct.unpack_from('<I',m,0x18)[0]
    r=R(m[st:]); flags=r.u32(); ctx=r.u32(); ts=r.f64()
    out=[]
    while r.left()>0:
        op=r.u8()
        if op==0: out.append(('end',)); break
        elif op==1: out.append(('delete',r.u64(),r.u32()))
        elif op==2: out.append(('set_object',r.u64(),r.u32(),obj(r),r.u32()))
        elif op==3: out.append(('update_layer',r.u64()))
        elif op==7: out.append(('layer_id',r.u64()))
        elif op==9:
            k=r.u8(); e=r.u8(); out.append(('colorspace',k,e))
        elif op in (0x1a,0x1c,0x1d,0x1e): out.append((hex(op),r.u64()))
        elif op==8: out.append(('ctxopts',cf(r)))
        elif op==10: out.append(('annotation',cf(r)))
        elif op in (0xb,0xc): out.append((hex(op),r.u64()))
        elif op==0xd: out.append(('level',r.u32()))
        elif op==0x20: out.append(('op20',r.u32(),r.u8()))
        elif op==0x25: out.append(('port',))
        elif op==0x1f: out.append(('op1f',r.u8()))
        else:
            out.append(('UNKNOWN op',op,r.b[r.p-1:r.p+40].hex())); break
    return out,r.left()
def _dbg2():
    for n,m in enumerate(msgs):
        try:
            o,left=parse(m)
        except Exception as e:
            o,left=(['EXC '+str(e)],-1)
        print(n,left,[ (x[0] if isinstance(x,tuple) else x) for x in o][:12])
    
    
if __name__=='__main__' and len(sys.argv)>1:
    n=int(sys.argv[1]); o,l=parse(msgs[n])
    for x in o:
        if x[0]=='set_object' and isinstance(x[3],dict):
            L=x[3]; print('layer id %x slot %d fl %d pod %s subs %s ext %d'%(x[1],x[2],L['fl'],L['pod'].hex(),L['subs'],L['ext']))
        else: print(x if len(str(x))<200 else str(x)[:200])
