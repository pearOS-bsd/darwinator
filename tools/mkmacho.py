#!/usr/bin/env python3
"""mkmacho.py: writes a tiny Mach-O executable that needs no toolchain and no dyld: it prints a line with
the Darwin write system call and exits with status 7 (exit system call).  For testing darwinator on a machine
that has no Mac.

  mkmacho.py --arch x86_64|arm64 [--entry main|unixthread] [--fat] -o FILE

--fat writes a universal file with both slices (the loader has to pick the one of the host)."""
import argparse, struct, sys

MSG = b"hello from a Mach-O file\n"
EXIT = 7
BASE = 0x100000000
CPU = {"x86_64": (0x01000007, 3), "arm64": (0x0100000C, 0)}

def code_x86_64(msg_off, code_off):
    # mov eax,0x2000004 ; mov edi,1 ; lea rsi,[rip+msg] ; mov edx,len ; syscall ; mov eax,0x2000001 ; mov edi,EXIT ; syscall
    c = b"\xb8" + struct.pack("<I", 0x2000004) + b"\xbf" + struct.pack("<I", 1)
    lea_end = code_off + len(c) + 7
    c += b"\x48\x8d\x35" + struct.pack("<i", msg_off - lea_end)
    c += b"\xba" + struct.pack("<I", len(MSG)) + b"\x0f\x05"
    c += b"\xb8" + struct.pack("<I", 0x2000001) + b"\xbf" + struct.pack("<I", EXIT) + b"\x0f\x05"
    return c

def code_arm64(msg_off, code_off):
    movz = lambda rd, imm: 0xD2800000 | (imm << 5) | rd
    adr_at = code_off + 8
    off = msg_off - adr_at
    adr = 0x10000000 | ((off & 3) << 29) | (((off >> 2) & 0x7FFFF) << 5) | 1
    ins = [movz(16, 4), movz(0, 1), adr, movz(2, len(MSG)), 0xD4001001,   # svc #0x80
           movz(16, 1), movz(0, EXIT), 0xD4001001]
    return b"".join(struct.pack("<I", i) for i in ins)

def thin(arch, entry):
    cputype, subtype = CPU[arch]
    page = 0x1000 if arch == "x86_64" else 0x4000
    seg = lambda name, vmaddr, vmsize, fileoff, filesize, prot, nsects: struct.pack(
        "<II16sQQQQIIII", 0x19, 72 + 80 * nsects, name, vmaddr, vmsize, fileoff, filesize, prot, prot, nsects, 0)
    if entry == "main":
        entry_cmd_size = 24
    elif arch == "x86_64":
        entry_cmd_size = 16 + 42 * 4
    else:
        entry_cmd_size = 16 + 68 * 4
    sizeofcmds = 72 + (72 + 80) + entry_cmd_size
    code_off = (32 + sizeofcmds + 15) & ~15
    code_len = len(code_x86_64(0, code_off)) if arch == "x86_64" else 32
    msg_off = code_off + code_len
    code = (code_x86_64 if arch == "x86_64" else code_arm64)(msg_off, code_off)
    assert len(code) == code_len
    text = code + MSG
    sect = struct.pack("<16s16sQQIIIIIIII", b"__text", b"__TEXT", BASE + code_off, len(text), code_off, 4, 0, 0,
                       0x80000400, 0, 0, 0)
    if entry == "main":
        ecmd = struct.pack("<IIQQ", 0x80000028, 24, code_off, 0)
    else:
        n = 42 if arch == "x86_64" else 68
        state = bytearray(n * 4)
        struct.pack_into("<Q", state, 8 * (16 if arch == "x86_64" else 32), BASE + code_off)
        ecmd = struct.pack("<IIII", 5, entry_cmd_size, 4 if arch == "x86_64" else 6, n) + bytes(state)
    cmds = seg(b"__PAGEZERO", 0, BASE, 0, 0, 0, 0) + seg(b"__TEXT", BASE, page, 0, page, 5, 1) + sect + ecmd
    assert len(cmds) == sizeofcmds
    hdr = struct.pack("<IIIIIIII", 0xFEEDFACF, cputype, subtype, 2, 3, sizeofcmds, 1, 0)
    img = hdr + cmds
    img += b"\0" * (code_off - len(img)) + text
    return img + b"\0" * (page - len(img))

def fat(slices):
    out = struct.pack(">II", 0xCAFEBABE, len(slices))
    off = 0x4000
    body = b""
    for arch, img in slices:
        cputype, subtype = CPU[arch]
        out += struct.pack(">IIIII", cputype, subtype, off + len(body), len(img), 14)
        body += img + b"\0" * (-len(img) % 0x4000)
    return out + b"\0" * (off - len(out)) + body

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arch", choices=sorted(CPU), default="x86_64")
    ap.add_argument("--entry", choices=["main", "unixthread"], default="main")
    ap.add_argument("--fat", action="store_true")
    ap.add_argument("-o", required=True)
    a = ap.parse_args()
    data = fat([(x, thin(x, a.entry)) for x in ("x86_64", "arm64")]) if a.fat else thin(a.arch, a.entry)
    with open(a.o, "wb") as f:
        f.write(data)
    import os; os.chmod(a.o, 0o755)

if __name__ == "__main__":
    main()
