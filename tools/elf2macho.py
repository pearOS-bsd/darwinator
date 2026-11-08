#!/usr/bin/env python3
"""elf2macho.py IN.elf OUT: turns a static, freestanding ELF64 executable (x86-64 or aarch64) into a Mach-O executable
for darwinator: every PT_LOAD becomes a segment (R/W/X from the ELF flags), the entry point goes into LC_UNIXTHREAD.
This is how the C test programs of tests/c are made on a machine without a Mac (clang compiles them for a bare ELF
target; no dyld, no libSystem: they call the kernel with the Darwin system call convention)."""
import struct, sys

PT_LOAD = 1
PAGE = 0x1000

def main(src, dst):
    d = open(src, "rb").read()
    if d[:4] != b"\x7fELF" or d[4] != 2 or d[5] != 1:
        sys.exit("not a little endian ELF64 file")
    e_type, e_machine = struct.unpack_from("<HH", d, 16)
    e_entry, e_phoff = struct.unpack_from("<QQ", d, 24)
    e_phentsize, e_phnum = struct.unpack_from("<HH", d, 54)
    arch = {62: "x86_64", 183: "arm64"}.get(e_machine)
    if arch is None:
        sys.exit("unsupported e_machine %d" % e_machine)
    segs = []
    for i in range(e_phnum):
        p_type, p_flags, p_offset, p_vaddr, _pa, p_filesz, p_memsz, _al = struct.unpack_from("<IIQQQQQQ", d, e_phoff + i * e_phentsize)
        if p_type == PT_LOAD and p_memsz:
            segs.append((p_flags, p_offset, p_vaddr, p_filesz, p_memsz))
    if not segs:
        sys.exit("no PT_LOAD segment")

    cputype, subtype = (0x01000007, 3) if arch == "x86_64" else (0x0100000C, 0)
    nthread = 42 if arch == "x86_64" else 68
    thread_size = 16 + nthread * 4
    sizeofcmds = 72 + sum(72 for _ in segs) + thread_size
    hdr_len = 32 + sizeofcmds
    cmds = struct.pack("<II16sQQQQIIII", 0x19, 72, b"__PAGEZERO", 0, 0x100000000 if segs[0][2] >= 0x100000000 else 0x1000, 0, 0, 0, 0, 0, 0)
    # __PAGEZERO must end below the first segment
    first = min(s[2] & ~(PAGE - 1) for s in segs)
    cmds = struct.pack("<II16sQQQQIIII", 0x19, 72, b"__PAGEZERO", 0, first if first > 0 else 0, 0, 0, 0, 0, 0, 0)
    body = b""
    off = (hdr_len + PAGE - 1) & ~(PAGE - 1)
    first_text = True
    for (flags, p_off, vaddr, filesz, memsz) in segs:
        prot = (1 if flags & 4 else 0) | (2 if flags & 2 else 0) | (4 if flags & 1 else 0)
        vmaddr = vaddr & ~(PAGE - 1)
        pre = vaddr - vmaddr
        data = b"\0" * pre + d[p_off:p_off + filesz]
        vmsize = (pre + memsz + PAGE - 1) & ~(PAGE - 1)
        name = b"__TEXT" if (prot & 4 and first_text) else b"__DATA"
        if prot & 4:
            first_text = False
        cmds += struct.pack("<II16sQQQQIIII", 0x19, 72, name, vmaddr, vmsize, off, len(data), prot, prot, 0, 0)
        body += b"\0" * (off - hdr_len - len(body)) + data
        off = (off + len(data) + PAGE - 1) & ~(PAGE - 1)
    state = bytearray(nthread * 4)
    struct.pack_into("<Q", state, 8 * (16 if arch == "x86_64" else 32), e_entry)
    cmds += struct.pack("<IIII", 5, thread_size, 4 if arch == "x86_64" else 6, nthread) + bytes(state)
    assert len(cmds) == sizeofcmds, (len(cmds), sizeofcmds)
    out = struct.pack("<IIIIIIII", 0xFEEDFACF, cputype, subtype, 2, len(segs) + 2, sizeofcmds, 1, 0) + cmds
    out += b"\0" * (((hdr_len + PAGE - 1) & ~(PAGE - 1)) - len(out)) + body[((hdr_len + PAGE - 1) & ~(PAGE - 1)) - hdr_len:]
    with open(dst, "wb") as f:
        f.write(out)
    import os
    os.chmod(dst, 0o755)

if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
