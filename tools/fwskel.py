#!/usr/bin/env python3
"""python3 tools/fwskel.py ROOT [LIST]: the symlink skeleton of the frameworks under ROOT.

dyld looks a dlopen() path up in the file system first (realpath) and only then in the shared cache, so the alias paths
(Foo.framework/Foo, .../Versions/Current/..., CoreServices.framework/Frameworks/...) need their symlinks although the dylibs
need not exist.  LIST: install names, one per line (default tools/framework-paths.txt: what a Mac process with AppKit loaded).
Found with cfprefsd, which dlopen()s FSEvents by the alias path and calls a null function pointer when that fails."""
import os, sys
root = sys.argv[1] if len(sys.argv) > 1 else '/'
lst = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'framework-paths.txt')

def link(target, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if os.path.lexists(path):
        return
    os.symlink(target, path)

for line in open(lst):
    p = line.strip()
    parts = p.split('/')
    for i, c in enumerate(parts):
        if not c.endswith('.framework') or i + 2 >= len(parts) or parts[i + 1] != 'Versions':
            continue
        fw = '/'.join(parts[:i + 1]); ver = parts[i + 2]; name = c[:-len('.framework')]
        os.makedirs(root + fw + '/Versions/' + ver, exist_ok=True)
        link(ver, root + fw + '/Versions/Current')
        link('Versions/Current/' + name, root + fw + '/' + name)
        if i > 0 and parts[i - 1] == 'Frameworks' and parts[i - 3:i - 2] == ['Versions']:      # Outer.framework/Versions/A/Frameworks/Inner.framework
            outer = '/'.join(parts[:i - 3])
            link('Versions/Current/Frameworks', root + outer + '/Frameworks')
