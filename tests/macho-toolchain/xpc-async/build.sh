#!/bin/sh
# sh build.sh OUTDIR -> OUTDIR/asynctest (an XPC client with an asynchronous reply, for the real dyld)
set -e
H="$(cd "$(dirname "$0")" && pwd)"; O="${1:-$H/out}"; mkdir -p "$O"
cc --target=x86_64-apple-macosx12.0 -fblocks -O1 -ffreestanding -fno-builtin -fno-stack-protector ${ASYNCTEST_CFLAGS} -c "$H/asynctest.c" -o "$O/asynctest.o"
/usr/local/llvm21/bin/ld64.lld -arch x86_64 -platform_version macos 12.0 12.0 -e _main -o "$O/asynctest" "$O/asynctest.o" -L"$H" -lSystem
