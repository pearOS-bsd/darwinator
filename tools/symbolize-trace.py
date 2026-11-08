#!/usr/bin/env python3
"""symbolize-trace.py LIBROOT < trace: replaces the "<symbol+offset>" of a backtrace made by dyld/libexctrace.so with the real
function of the library's full symbol table (the dynamic symbol table of a shared library only has the exported names).
LIBROOT is a directory with the same library files as the machine that made the trace (e.g. the root of the image build)."""
import bisect, os, re, subprocess, sys

root = sys.argv[1]
syms = []
if len(sys.argv) > 2:
    for l in open(sys.argv[2]):
        a, _, n = l.rstrip().partition(' ')
        if n: syms.append((int(a, 16), n))
    syms.sort()
symaddrs = [a for a, _ in syms]
cache = {}
def tables(lib):
    if lib in cache: return cache[lib]
    path = root + lib
    dyn = {}; full = []
    if os.path.exists(path):
        for l in subprocess.run(['nm', '-D', path], capture_output=True, text=True).stdout.splitlines():
            p = l.split()
            if len(p) == 3 and p[1] in 'TtWwDdBb': dyn[p[2]] = int(p[0], 16)
        for l in subprocess.run(['nm', '-n', path], capture_output=True, text=True).stdout.splitlines():
            p = l.split(None, 2)
            if len(p) == 3 and p[1] in 'TtWw': full.append((int(p[0], 16), p[2]))
    cache[lib] = (dyn, full, [a for a, _ in full])
    return cache[lib]

for line in sys.stdin:
    mf = re.match(r'fp (0x[0-9a-f]+)', line)
    if mf:
        a = int(mf.group(1), 16)
        i = bisect.bisect_right(symaddrs, a) - 1
        if syms and i >= 0 and 0 <= a - syms[i][0] < 0x4000: print('   %s +%#x (program)' % (syms[i][1], a - syms[i][0]))
        else: print('   frame %s' % mf.group(1))
        continue
    m = re.match(r'(0x[0-9a-f]+) <([^+>]+)\+(0x[0-9a-f]+)> at (\S+)', line)
    if not m: print(line.rstrip()); continue
    _, sym, off, lib = m.groups()
    dyn, full, addrs = tables(lib)
    if sym in dyn and full:
        a = dyn[sym] + int(off, 16)
        i = bisect.bisect_right(addrs, a) - 1
        if i >= 0: print('%-60s +%#x   %s' % (full[i][1], a - addrs[i] if False else a - full[i][0], os.path.basename(lib))); continue
    print(line.rstrip())
