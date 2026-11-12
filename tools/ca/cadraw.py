#!/usr/bin/env python3
"""cadraw.py [ca.bin [out.ppm [W H]]]: draws the scene that a Dock sent (the stream of carender-stub) into a PPM.

Every context keeps the last state of its layers (set_object); the layers are drawn depth first from the roots (a layer that no
other layer lists as a sublayer): position is the centre (anchor 0.5), bounds give the size, the first four floats of the 0x58 byte
block are the background colour (RGBA), the next one the opacity.  Layers without a size or colour draw nothing.  What the Dock
does not send (images, the glass of the SDFLayer) is not drawn: the picture shows what there is.
With --watch the file is read again every second (run wsshow on the PPM to see it on the screen)."""
import struct, sys, time, os
here = os.path.dirname(os.path.abspath(__file__))

def load(path):
    src = open(os.path.join(here, 'caparse.py')).read().replace("open('ca.bin','rb')", "open(%r,'rb')" % path)
    g = {'__name__': 'caparse', 'sys': sys}
    sys.argv = ['x']
    exec(src, g)
    return g

def draw(path, out, W, H):
    g = load(path)
    layers = {}
    for n, m in enumerate(g['msgs']):
        try: ops, left = g['parse'](m)
        except Exception: continue
        ctxt = None
        for x in ops:
            if x[0] == 'set_object' and isinstance(x[3], dict) and x[3].get('t') == 'layer':
                L = x[3]; p = L['pod']
                d = struct.unpack_from('<6d', p, 0x28)
                col = struct.unpack_from('<5f', p, 0)
                subs = L['subs'][1] if L['subs'] and L['subs'][0] == 'ids' else []
                layers[x[1]] = dict(pos=(d[0], d[1]), size=(d[4], d[5]), col=col, subs=subs,
                                    name=L['o'][1].decode('latin1') if L['o'] and L['o'][0] == 'string' else '')
    child = set(s for L in layers.values() for s in L['subs'])
    roots = [i for i in layers if i not in child]
    img = bytearray(b'\x1c\x1c\x1e' * (W * H))
    def fill(x0, y0, x1, y1, c, a):
        x0 = max(0, int(x0)); y0 = max(0, int(y0)); x1 = min(W, int(x1)); y1 = min(H, int(y1))
        for y in range(y0, y1):
            row = y * W * 3
            for x in range(x0, x1):
                o = row + x * 3
                for k in range(3):
                    img[o + k] = int(img[o + k] * (1 - a) + c[k] * 255 * a)
    seen = []
    def walk(i, ox, oy, depth):
        L = layers.get(i)
        if not L: return
        w, h = L['size']
        x = ox + L['pos'][0] - w / 2; y = oy + L['pos'][1] - h / 2
        r, g_, b, a, op = L['col']
        if w > 0 and h > 0 and a > 0 and op > 0:
            fill(x, y, x + w, y + h, (r, g_, b), a * min(op, 1))
            seen.append((L['name'], int(x), int(y), int(w), int(h)))
        for s in L['subs']: walk(s, x, y, depth + 1)
    for i in roots: walk(i, 0, 0, 0)
    with open(out, 'wb') as f:
        f.write(b'P6 %d %d 255\n' % (W, H)); f.write(img)
    return len(layers), seen

if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if a != '--watch']
    watch = '--watch' in sys.argv
    path = args[0] if args else 'ca.bin'; out = args[1] if len(args) > 1 else '/tmp/cadock.ppm'
    W = int(args[2]) if len(args) > 2 else 1280; H = int(args[3]) if len(args) > 3 else 800
    last = -1
    while True:
        sz = os.path.getsize(path) if os.path.exists(path) else 0
        if sz != last:
            last = sz
            n, seen = draw(path, out + '.tmp', W, H); os.replace(out + '.tmp', out)
            print('%d layers; drawn: %s' % (n, seen), flush=True)
        if not watch: break
        time.sleep(1)
