#!/bin/sh
# sh build.sh OUTDIR -> OUTDIR/timertest (a dispatch timer, for the real dyld)
set -e
H="$(cd "$(dirname "$0")" && pwd)"; O="${1:-$H/out}"; mkdir -p "$O"
cc --target=x86_64-apple-macosx12.0 -fblocks -O1 -ffreestanding -fno-builtin -fno-stack-protector ${TIMERTEST_CFLAGS} -c "$H/timertest.c" -o "$O/timertest.o"
/usr/local/llvm21/bin/ld64.lld -arch x86_64 -platform_version macos 12.0 12.0 -e _main -o "$O/timertest" "$O/timertest.o" -L"$H" -lSystem
