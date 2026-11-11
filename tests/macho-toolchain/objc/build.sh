#!/bin/sh
# an Objective-C Mach-O program on FreeBSD (clang -fobjc-runtime=macosx, ld64.lld against a text stub of libobjc, no file of Apple's):  sh build.sh OUTDIR  ->  OUTDIR/hello
# run it with the real dyld (kern.darwinator.dyld=1): it prints "objc: Hello answer = 42"
set -e
H="$(cd "$(dirname "$0")" && pwd)"; O="${1:-$H/out}"; mkdir -p "$O"
cc --target=x86_64-apple-macosx12.0 -fobjc-runtime=macosx -fno-objc-arc -O1 -c "$H/hello.m" -o "$O/hello.o"
/usr/local/llvm21/bin/ld64.lld -arch x86_64 -platform_version macos 12.0 12.0 -e _main -o "$O/hello" "$O/hello.o" -L"$H" -L"$H/.." -lSystem -lobjc
file "$O/hello"
