#!/bin/sh
# probe2.sh: run on a Mac.  Builds and runs probe2.c (posix_spawn, named semaphores, access_extended, statfs on the real system) and writes ./macprobe-out/probe2.txt.
# Nothing is sent anywhere.   sh probe2.sh   then   git add -f tools/macprobe/macprobe-out/probe2.txt (or the folder you ran it from) and push.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"; OUT="$PWD/macprobe-out"; mkdir -p "$OUT"
cc -Wno-deprecated-declarations -o "$OUT/probe2" "$HERE/probe2.c" 2>"$OUT/build-errors2.txt" || { echo "does not build:"; cat "$OUT/build-errors2.txt"; exit 1; }
"$OUT/probe2" > "$OUT/probe2.txt" 2>&1; { uname -a; sw_vers; } >> "$OUT/probe2.txt" 2>&1
cat "$OUT/probe2.txt"
