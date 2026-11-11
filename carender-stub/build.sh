#!/bin/sh
# sh build.sh OUTDIR -> OUTDIR/carender-stub
set -e
H="$(cd "$(dirname "$0")" && pwd)"; O="${1:-$H/out}"; mkdir -p "$O"
cc --target=x86_64-apple-macosx12.0 -O1 -ffreestanding -fno-builtin -fno-stack-protector -c "$H/carender-stub.c" -o "$O/carender-stub.o"
/usr/local/llvm21/bin/ld64.lld -arch x86_64 -platform_version macos 12.0 12.0 -e _main -o "$O/carender-stub" "$O/carender-stub.o" -L"$H" -lSystem
