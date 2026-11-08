#!/usr/bin/env python3
"""nibfix.py IN.nib OUT.nib: rewrites a compiled Interface Builder archive (keyedobjects.nib) so that GNUstep can decode it.

Fixes so far:
  * NSLocalizableString objects (a label with its localization key; the text is stored inline in the dictionary, which
    GNUstep's NSKeyedUnarchiver cannot decode) become the plain string.
usage: nibfix.py IN/keyedobjects.nib OUT/keyedobjects.nib   (or a .nib directory: its keyedobjects.nib)"""
import os, plistlib, sys

def fix(d):
    objs = d['$objects']
    n = 0
    for i, o in enumerate(objs):
        if isinstance(o, dict) and '$class' in o:
            cname = objs[o['$class'].data].get('$classname')
            if cname == 'NSLocalizableString':
                objs[i] = o.get('NS.string', o.get('NSDev', ''))
                n += 1
    return n

def main(src, dst):
    if os.path.isdir(src): src = os.path.join(src, 'keyedobjects.nib')
    if dst.endswith('.nib') and not os.path.exists(dst) or os.path.isdir(dst):
        os.makedirs(dst, exist_ok=True); dst = os.path.join(dst, 'keyedobjects.nib')
    d = plistlib.load(open(src, 'rb'))
    n = fix(d)
    with open(dst, 'wb') as f:
        plistlib.dump(d, f, fmt=plistlib.FMT_BINARY)
    print('%s: %d localizable strings replaced' % (dst, n))

if __name__ == '__main__':
    if len(sys.argv) != 3: sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
