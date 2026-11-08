#!/usr/bin/env python3
"""annotate-sel.py MACHO(thin x86_64) DISASSEMBLY: adds the selector name to the loads of selector references in a disassembly of a binary with chained fixups
(llvm-objdump does not know them there).  Usage: annotate-sel.py ac_x86 ac.dis > ac.annotated"""
import struct, re, sys
d = open(sys.argv[1], 'rb').read()
ncmds = struct.unpack_from('<I', d, 16)[0]; off = 32; segs = []
for _ in range(ncmds):
    cmd, sz = struct.unpack_from('<II', d, off)
    if cmd == 0x19:
        name = d[off+8:off+24].rstrip(b'\0').decode(); vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<QQQQ', d, off+24)
        segs.append((vmaddr, vmsize, fileoff, filesize))
    off += sz
def v2f(a):
    for vm, sz, fo, fs in segs:
        if vm <= a < vm + fs: return fo + a - vm
base = segs[1][0] if segs[0][0] == 0 else segs[0][0]
cache = {}
def sel(slot):
    if slot in cache: return cache[slot]
    f = v2f(slot)
    r = None
    if f is not None:
        raw = struct.unpack_from('<Q', d, f)[0]; t = raw & 0xfffffffff
        for cand in (base + t, t):
            g = v2f(cand)
            if g is not None and 0 < d[g] < 127:
                e = d.index(b'\0', g); s = d[g:e].decode('latin1')
                if re.match(r'^[A-Za-z_][A-Za-z0-9_:\.]*$', s): r = s; break
    cache[slot] = r; return r
pat = re.compile(r'^\s*([0-9a-f]+):\s+movq\s+(0x[0-9a-f]+)\(%rip\), %(r[a-z0-9]+)\s*$')
for line in open(sys.argv[2]):
    m = pat.match(line)
    if m:
        # next instruction address = address + length: movq rip-rel is 7 bytes
        a = int(m.group(1), 16) + 7; slot = a + int(m.group(2), 16); s = sel(slot)
        if s: line = line.rstrip('\n') + '  ## SEL ' + s + '\n'
    sys.stdout.write(line)
