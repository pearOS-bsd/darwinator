#!/bin/sh
# builds the test program and the library (build.sh: clang + ld64.lld of the llvm21 package) and runs them with the dyld of a Mac (needs /usr/lib/dyld and the cache, kern.darwinator.dyld=1):
#   sh tests/macho-toolchain/run.sh          (as root, darwinator.ko loaded)
set -e
H="$(cd "$(dirname "$0")" && pwd)"; O="$(mktemp -d)"
sh "$H/build.sh" "$O" >/dev/null
mkdir -p /usr/local/lib && cp "$O/libhello.dylib" /usr/local/lib/
sysctl kern.darwinator.dyld=1 >/dev/null
out="$("$O/hello" 2>&1)"; st=$?
echo "$out"
[ $st -eq 0 ] && echo "$out" | grep -q "hello from libhello, a Mach-O program built on FreeBSD" && echo "ok   a Mach-O program and library built on FreeBSD run through dyld" || { echo "FAIL (status $st)"; exit 1; }
