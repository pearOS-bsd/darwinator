#!/bin/sh
# sh build.sh OUTDIR -> OUTDIR/svc-stub
set -e
H="$(cd "$(dirname "$0")" && pwd)"; O="${1:-$H/out}"; mkdir -p "$O"
cc --target=x86_64-apple-macosx12.0 -O1 -ffreestanding -fno-builtin -fno-stack-protector -c "$H/svc-stub.c" -o "$O/svc-stub.o"
/usr/local/llvm21/bin/ld64.lld -arch x86_64 -platform_version macos 12.0 12.0 -e _main -o "$O/svc-stub" "$O/svc-stub.o" -L"$H" -lSystem
