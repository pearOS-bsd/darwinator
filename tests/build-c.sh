#!/bin/sh
# builds tests/c/machsh.c as a Mach-O program for the architecture of this machine (FreeBSD: clang and lld of the base system):
#   sh tests/build-c.sh OUTFILE [x86_64|arm64]
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; OUT="$1"
ARCH="${2:-$(case "$(uname -p)" in amd64) echo x86_64 ;; *) echo arm64 ;; esac)}"
case "$ARCH" in x86_64) T=x86_64-unknown-none-elf ;; arm64) T=aarch64-unknown-none-elf ;; esac
TMP="$(mktemp -d)"
cc --target=$T -ffreestanding -fno-builtin -fPIE -fno-stack-protector -fno-asynchronous-unwind-tables -O1 -mno-red-zone \
    -c "$HERE/c/machsh.c" -o "$TMP/machsh.o" 2>/dev/null || cc --target=$T -ffreestanding -fno-builtin -fPIE -fno-stack-protector -fno-asynchronous-unwind-tables -O1 -c "$HERE/c/machsh.c" -o "$TMP/machsh.o"
ld.lld -static -nostdlib -e _start --image-base=0x100000000 -z norelro -z max-page-size=0x1000 --build-id=none -o "$TMP/machsh.elf" "$TMP/machsh.o"
python3 "$HERE/../tools/elf2macho.py" "$TMP/machsh.elf" "$OUT"
rm -rf "$TMP"
