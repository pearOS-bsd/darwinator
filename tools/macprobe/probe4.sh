#!/bin/sh
# probe4.sh: run on an Intel Mac (on Apple silicon it only says so).  Writes ./macprobe-out/probe4.txt (also shown).  Nothing is sent anywhere.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"; OUT="$PWD/macprobe-out"; mkdir -p "$OUT"
cc -Wno-deprecated-declarations -o "$OUT/probe4" "$HERE/probe4.c" 2>"$OUT/build-errors4.txt" || { echo "does not build:"; cat "$OUT/build-errors4.txt"; exit 1; }
"$OUT/probe4" > "$OUT/probe4.txt" 2>&1; { uname -a; sw_vers; } >> "$OUT/probe4.txt" 2>&1; cat "$OUT/probe4.txt"
