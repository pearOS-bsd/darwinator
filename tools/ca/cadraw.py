#!/usr/bin/env python3
"""cadraw.py [ca.bin [out.ppm [W H]]]: draws the scene that a Dock sent (the stream of carender-stub) into a PPM.

Every context keeps the last state of its layers (set_object); the layers are drawn depth first from the roots (a layer that no
other layer lists as a sublayer): position is the centre (anchor 0.5), bounds give the size, the first four floats of the 0x58 byte
block are the background colour (RGBA), the next one the opacity.  Layers without a size or colour draw nothing.  What the Dock
does not send (images, the glass of the SDFLayer) is not drawn: the picture shows what there is.
With --ool=DIR the icons are the ones that the Dock really sent (out-of-line images kept by carender-stub in DIR).
With --watch the file is read again every second (run wsshow on the PPM to see it on the screen)."""
import struct, sys, time, os
here = os.path.dirname(os.path.abspath(__file__))

def load(path):
    src = open(os.path.join(here, 'caparse.py')).read().replace("open('ca.bin','rb')", "open(%r,'rb')" % path)
    g = {'__name__': 'caparse', 'sys': sys}
    sys.argv = ['x']
    exec(src, g)
    return g

def draw(path, out, W, H, icons=()):
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
    # the records that parse() could not follow (a layer with an Image has bytes that are not decoded yet): their pod (geometry) is still at a fixed place
    import re
    for m in g['msgs']:
        for mm in re.finditer(rb'\x02(.{4}\x29\x03\x05\x28)(.{4})\x1e(.{4})', m, re.S):
            i = struct.unpack_from('<Q', mm.group(1))[0]
            if i in layers: continue
            p = m[mm.end():mm.end() + 0x58]
            if len(p) < 0x58: continue
            d = struct.unpack_from('<6d', p, 0x28)
            layers[i] = dict(pos=(d[0], d[1]), size=(d[4], d[5]), col=struct.unpack_from('<5f', p, 0), subs=[], name='?')
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
    if icons:
        seen += overlay(layers, roots, img, W, H, icons)
    with open(out, 'wb') as f:
        f.write(b'P6 %d %d 255\n' % (W, H)); f.write(img)
    return len(layers), seen

def ool_icons(d):
    """The icons that the Dock really sent: the out-of-line data of its last commit that has any (carender-stub keeps them as <d>/ca-ool-<n>-<k>.bin):
    an RGBA half-float image of 128x128 (+ mip levels) or an 8 bit BGRA one; returned as PIL images, in the order of the descriptors."""
    import glob, re
    import numpy as np
    from PIL import Image
    fs = sorted(glob.glob(os.path.join(d, 'ca-ool-*-*.bin')))
    if not fs: return []
    last = max(re.search(r'ca-ool-(\d+)-', f).group(1) for f in fs)
    out = []
    for f in sorted(x for x in fs if '-%s-' % last in x):
        b = open(f, 'rb').read()
        if len(b) >= 128 * 128 * 8 and len(b) in (174592,):
            a = np.frombuffer(b[:128 * 128 * 8], dtype='<f2').reshape(128, 128, 4).astype(np.float32)
            a = np.nan_to_num(np.clip(a, 0, 1)); rgba = (a * 255).astype(np.uint8)
        elif len(b) >= 128 * 128 * 4:
            rgba = np.frombuffer(b[:128 * 128 * 4], dtype=np.uint8).reshape(128, 128, 4)[..., [2, 1, 0, 3]].copy()
        else: continue
        out.append(Image.fromarray(rgba, 'RGBA'))
    return out

def overlay(layers, roots, img, W, H, icons):
    """What the Dock does not send yet (its tiles have no contents): the glass of the bar and the application icons that the preferences name,
    placed on the tile layers (52x52 inside a 56x56 slot, in the order of the slots).  CA's y goes up, the screen's down."""
    from PIL import Image, ImageDraw
    im = Image.frombytes('RGB', (W, H), bytes(img)).convert('RGBA')
    ov = Image.new('RGBA', (W, H), (0, 0, 0, 0)); dr = ImageDraw.Draw(ov)
    best = None
    for r_ in roots:
        tiles = []; bars = []
        def walk(i, ox, oy):
            L = layers.get(i)
            if not L: return
            w, h = L['size']; x = ox + L['pos'][0] - w / 2; y = oy + L['pos'][1] - h / 2
            if round(w) == 52 and round(h) == 52: tiles.append((x, y, w, h))
            elif w > 100 and h > 60 and w < W / 2: bars.append((x, y, w, h))
            for s_ in L['subs']: walk(s_, x, y)
        walk(r_, 0, 0)
        if tiles and (best is None or len(tiles) > len(best[0])): best = (tiles, bars)
    tiles, bars = best if best else ([], [])
    if True:
        # the slots arrive as roots of size 0 (the link to the bar is a later reference): a slot's centre is (x, y); its 52x52 tile is at (x-26, y-33)
        ftiles = []; pend = None
        for i_, L in layers.items():      # a tile layer (52 wide) is followed by the slot (size 0) that it belongs to
            w_, h_ = L['size']
            if round(h_) == 52 and round(w_) in (52, 21): pend = (w_, h_)
            elif w_ == 0 and h_ == 0 and pend and 40 < L['pos'][1] < 60:
                ftiles.append((L['pos'][0] - 26, L['pos'][1] - 33, pend[0], 52)); pend = None
        ftiles.sort()
        if ftiles:
            tiles = ftiles; bars = [((W - 266) / 2, 10, 266, 72)]
    # the real Dock of this system: 56x56 slots (the tile 52x52 is a little below the middle), the bar 266x72 centred in the screen
    slots = sorted((L['pos'][0], L['pos'][1]) for L in layers.values() if tuple(round(v) for v in L['size']) == (56, 56) and L['pos'][0] > 100)
    if slots:
        tiles = [(cx - 26, cy - 7 - 26, 52, 52) for cx, cy in slots]
        bl = [L for L in layers.values() if round(L['size'][0]) > 200 and round(L['size'][1]) == 72 and L['pos'][0] > 100]
        bl.sort(key=lambda L: -L['pos'][0])
        if bl: bars = [(bl[0]['pos'][0] - bl[0]['size'][0] / 2, bl[0]['pos'][1] - 36, bl[0]['size'][0], 72)]
    out = []
    if bars:
        x, y, w, h = max(bars, key=lambda b: b[2] * b[3])
        dr.rounded_rectangle([x, H - (y + h), x + w, H - y], radius=18, fill=(255, 255, 255, 70), outline=(255, 255, 255, 120))
        out.append(('bar', int(x), int(H - (y + h)), int(w), int(h)))
    tiles.sort()
    for (x, y, w, h), path in zip(tiles, icons):
        try: ic = (path if hasattr(path, 'resize') else Image.open(path)).convert('RGBA').resize((int(w), int(h)), Image.LANCZOS)
        except Exception as e:
            out.append(('icon-error', path, str(e))); continue
        ov.paste(ic, (int(x), int(H - (y + h))), ic)
        out.append(('tile', 'ool' if hasattr(path, 'resize') else os.path.basename(os.path.dirname(os.path.dirname(os.path.dirname(path)))), int(x), int(H - (y + h))))
    im = Image.alpha_composite(im, ov).convert('RGB')
    img[:] = im.tobytes()
    return out

if __name__ == '__main__':
    icons = [a[8:].split(',') for a in sys.argv[1:] if a.startswith('--icons=')]
    icons = icons[0] if icons else []
    ool = [a[6:] for a in sys.argv[1:] if a.startswith('--ool=')]
    if ool: icons = ool_icons(ool[0]) or icons
    args = [a for a in sys.argv[1:] if a != '--watch' and not a.startswith('--icons=') and not a.startswith('--ool=')]
    watch = '--watch' in sys.argv
    path = args[0] if args else 'ca.bin'; out = args[1] if len(args) > 1 else '/tmp/cadock.ppm'
    W = int(args[2]) if len(args) > 2 else 1280; H = int(args[3]) if len(args) > 3 else 800
    last = -1
    while True:
        sz = os.path.getsize(path) if os.path.exists(path) else 0
        if sz != last:
            last = sz
            n, seen = draw(path, out + '.tmp', W, H, icons); os.replace(out + '.tmp', out)
            print('%d layers; drawn: %s' % (n, seen), flush=True)
        if not watch: break
        time.sleep(1)
