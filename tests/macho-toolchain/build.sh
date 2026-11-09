#!/bin/sh
# builds a dynamic Mach-O program and a Mach-O library on FreeBSD with clang and ld64.lld (llvm21 package), without any file of Apple's:
#   sh build.sh OUTDIR      -> OUTDIR/hello, OUTDIR/libhello.dylib
# the program is run with the dyld of a Mac (kern.darwinator.dyld=1) or, later, a dyld built from source
set -e
H="$(cd "$(dirname "$0")" && pwd)"; O="${1:-$H/out}"; mkdir -p "$O"
LD=/usr/local/llvm21/bin/ld64.lld; TGT=--target=x86_64-apple-macosx12.0
cc $TGT -O1 -ffreestanding -fno-builtin -fPIC -c "$H/hello.c" -o "$O/hello.o"
cc $TGT -O1 -ffreestanding -fno-builtin -c "$H/main.c" -o "$O/main.o"
$LD -arch x86_64 -platform_version macos 12.0 12.0 -dylib -install_name /usr/local/lib/libhello.dylib -o "$O/libhello.dylib" "$O/hello.o" -L"$H" -lSystem
$LD -arch x86_64 -platform_version macos 12.0 12.0 -e _main -o "$O/hello" "$O/main.o" -L"$O" -L"$H" -lhello -lSystem
file "$O/hello" "$O/libhello.dylib"
