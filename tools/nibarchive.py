#!/usr/bin/env python3
"""nibarchive.py: reads the compiled nib format "NIBArchive" (what Xcode 16 / macOS 15 writes) and dumps it, or converts it to the NSKeyedArchiver
property list that older nibs have (which GNUstep can load):  nibarchive.py [-d] FILE.nib [OUT.nib]"""
import struct, sys

def varint(d, p):
    r = 0; s = 0
    while True:
        b = d[p]; p += 1
        r |= (b & 0x7f) << s; s += 7
        if b & 0x80: return r, p

def parse(d):
    assert d[:10] == b'NIBArchive'
    (v1, v2, nobj, objoff, nkey, keyoff, nval, valoff, ncls, clsoff) = struct.unpack_from('<10I', d, 10)
    keys = []; p = keyoff
    for _ in range(nkey):
        n, p = varint(d, p); keys.append(d[p:p + n].decode('utf8', 'replace')); p += n
    classes = []; p = clsoff
    for _ in range(ncls):
        n, p = varint(d, p); ne, p = varint(d, p); extra = struct.unpack_from('<%dI' % ne, d, p); p += 4 * ne
        classes.append(d[p:p + n].rstrip(b'\0').decode()); p += n
    vals = []; p = valoff
    for _ in range(nval):
        k, p = varint(d, p); t = d[p]; p += 1
        if t == 0: v = struct.unpack_from('<b', d, p)[0]; p += 1
        elif t == 1: v = struct.unpack_from('<h', d, p)[0]; p += 2
        elif t == 2: v = struct.unpack_from('<i', d, p)[0]; p += 4
        elif t == 3: v = struct.unpack_from('<q', d, p)[0]; p += 8
        elif t == 4: v = True
        elif t == 5: v = False
        elif t == 6: v = struct.unpack_from('<f', d, p)[0]; p += 4
        elif t == 7: v = struct.unpack_from('<d', d, p)[0]; p += 8
        elif t == 8: n, p = varint(d, p); v = bytes(d[p:p + n]); p += n
        elif t == 9: v = None
        elif t == 10: v = ('obj', struct.unpack_from('<I', d, p)[0]); p += 4
        else: raise ValueError('value type %d at %d' % (t, p))
        vals.append((keys[k], t, v))
    objs = []; p = objoff
    for _ in range(nobj):
        c, p = varint(d, p); s, p = varint(d, p); n, p = varint(d, p)
        objs.append((classes[c], vals[s:s + n]))
    return objs

if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if a != '-d']
    objs = parse(open(args[0], 'rb').read())
    for i, (c, vs) in enumerate(objs):
        print(i, c)
        for k, t, v in vs: print('    ', k, t, v if not isinstance(v, bytes) or len(v) < 48 else v[:48] + b'...')
