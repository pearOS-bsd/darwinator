#!/usr/bin/env python3
"""ca_scene.py: a prototype of the CoreAnimation compositor of the render server (the part that will move into wsd as C).

It takes the commit stream that carender-stub keeps (ca.bin + ca-ool-<n>-<k>.bin), follows the layers of every context (set_object replaces a layer, delete drops it),
gives every Image of a layer its pixels (the n-th image with attached data of a message takes the n-th out-of-line descriptor of that message) and paints the tree.

    python3 ca_scene.py CADIR OUT.png [W H]        (CADIR holds ca.bin and the ca-ool files)

What is drawn: background colour + opacity of a layer (the first floats of its 0x58 block), its Image (BGRA8 or RGBA half float, first plane), children on top.
Not yet: transforms, masks, filters (BackdropLayer), the other object types.  Layers are placed as cadraw.py does (centre position, size, y up).
"""
import os, re, struct, sys, glob
here = os.path.dirname(os.path.abspath(__file__))


def load(path):
    src = open(os.path.join(here, 'caparse.py')).read().replace("open('ca.bin','rb')", "open(%r,'rb')" % path)
    g = {'__name__': 'caparse', 'sys': sys}
    sys.argv = ['x']
    exec(src, g)
    return g


def ool_descriptors(m):
    """The descriptors of a message (list of 'ool' / 'port')."""
    out = []
    if struct.unpack_from('<I', m, 0)[0] & 0x80000000:
        st = 0x1c
        for _ in range(struct.unpack_from('<I', m, 0x18)[0]):
            if m[st + 11] == 1: out.append('ool'); st += 16
            else: out.append('port'); st += 12
    return out


class Scene:
    def __init__(self, cadir):
        self.dir = cadir
        self.g = load(os.path.join(cadir, 'ca.bin'))
        self.layers = {}      # id -> dict(pos,size,col,subs,name,contents)
        self.images = {}      # image object id -> (w, h, bytes per pixel, pixels bytes)
        self.ool_n = 0        # index of the next message that had out-of-line data (carender-stub numbers them)
        for m in self.g['msgs']:
            self.message(m)

    def ool_file(self, n, k):
        p = os.path.join(self.dir, 'ca-ool-%02d-%d.bin' % (n, k))
        return open(p, 'rb').read() if os.path.exists(p) else None

    def message(self, m):
        descs = ool_descriptors(m)
        n = None
        if struct.unpack_from('<I', m, 0)[0] & 0x80000000:		# carender-stub numbers the complex messages (any descriptor), the file name has the index of the descriptor
            n = self.ool_n; self.ool_n += 1
        try:
            ops, left = self.g['parse'](m)
        except Exception:
            return
        ool_k = [i for i, d in enumerate(descs) if d == 'ool']
        taken = 0
        for x in ops:
            if x[0] == 'delete':
                self.layers.pop(x[1], None)
            elif x[0] == 'set_object' and isinstance(x[3], dict) and x[3].get('t') == 'layer':
                L = x[3]
                d = struct.unpack_from('<6d', L['pod'], 0x28)
                col = struct.unpack_from('<5f', L['pod'], 0)
                old = self.layers.get(x[1])
                subs = L['subs'][1] if L['subs'] and L['subs'][0] == 'ids' else (old['subs'] if old and not L['subs'] else [])		# no array in the record: the sublayers stay
                name = L['o'][1].decode('latin1') if L['o'] and L['o'][0] == 'string' else ''
                img = None
                c = L['contents']
                if c is not None:
                    obj = c
                    oid = None
                    if obj[0] == 'def': oid = obj[1]; obj = obj[3]
                    elif obj[0] == 'ref': oid = obj[1]; obj = None
                    if obj is not None and obj[0] == 'image':
                        w, h, rows, data = obj[3], obj[4], obj[5], obj[6]
                        if isinstance(data, tuple) and data[0] == 'shmem_inline' and data[2] == 'attached' and n is not None and taken < len(ool_k):
                            px = self.ool_file(n, ool_k[taken]); taken += 1
                            if px: self.images[oid] = (w, h, rows[0] // max(w, 1), px)
                    img = self.images.get(oid)
                self.layers[x[1]] = dict(pos=(d[0], d[1]), size=(d[4], d[5]), col=col, subs=subs, name=name, img=img)

    def roots(self):
        child = set(s for L in self.layers.values() for s in L['subs'])
        r = [i for i in self.layers if i not in child]
        return sorted(r, key=lambda i: -(self.layers[i]['size'][0] * self.layers[i]['size'][1]))		# the biggest (the wallpaper) first: it is the back

    def render(self, W, H):
        import numpy as np
        from PIL import Image
        canvas = Image.new('RGBA', (W, H), (28, 28, 30, 255))

        def to_pil(img):
            w, h, bpp, px = img
            if bpp == 8:
                a = np.frombuffer(px[:w * h * 8], dtype='<f2').reshape(h, w, 4).astype(np.float32)
                a = (np.nan_to_num(np.clip(a, 0, 1)) * 255).astype(np.uint8)
                return Image.fromarray(a, 'RGBA')
            a = np.frombuffer(px[:w * h * 4], dtype=np.uint8).reshape(h, w, 4)[..., [2, 1, 0, 3]].copy()
            return Image.fromarray(a, 'RGBA')

        drawn = []

        def blit(im, box):		# alpha_composite with clipping at the edges of the canvas
            x0, y0 = box
            sx, sy = max(0, -x0), max(0, -y0)
            if sx >= im.width or sy >= im.height or x0 >= W or y0 >= H: return
            im = im.crop((sx, sy, im.width, im.height))
            canvas.alpha_composite(im, (max(x0, 0), max(y0, 0)))

        def walk(i, ox, oy, op):
            L = self.layers.get(i)
            if not L: return
            w, h = L['size']
            x = ox + L['pos'][0] - w / 2
            y = oy + L['pos'][1] - h / 2
            r, g_, b, a, o = L['col']
            if not any(L['col']): a = 0; o = 1.0		# the first 0x28 bytes of a layer are a mask of the properties that are set + their values: all zero = defaults (opacity 1, no background)
            op2 = op * (min(o, 1) if o > 0 else 0)
            if w > 0 and h > 0 and op2 > 0:
                box = (int(x), int(H - (y + h)))
                if a > 0:
                    ov = Image.new('RGBA', (max(int(w), 1), max(int(h), 1)), (int(r * 255), int(g_ * 255), int(b * 255), int(a * op2 * 255)))
                    blit(ov, box)
                if L['img']:
                    try:
                        im = to_pil(L['img']).resize((max(int(w), 1), max(int(h), 1)), Image.LANCZOS)
                        if op2 < 1:
                            al = im.getchannel('A').point(lambda v: int(v * op2)); im.putalpha(al)
                        blit(im, box)
                        drawn.append(('image', L['name'], box, (int(w), int(h))))
                    except Exception as e:
                        drawn.append(('image-error', str(e)))
                drawn.append((L['name'], box, (int(w), int(h))))
            for s in L['subs']: walk(s, x, y, op2 if op2 > 0 else op)
        for i in self.roots(): walk(i, 0, 0, 1.0)
        return canvas, drawn


if __name__ == '__main__':
    d = sys.argv[1]; out = sys.argv[2]
    W = int(sys.argv[3]) if len(sys.argv) > 3 else 1280; H = int(sys.argv[4]) if len(sys.argv) > 4 else 800
    s = Scene(d)
    img, drawn = s.render(W, H)
    img.convert('RGB').save(out)
    print(len(s.layers), 'layers,', sum(1 for L in s.layers.values() if L['img']), 'with an image;', len([x for x in drawn if x[0] == 'image']), 'images drawn')
