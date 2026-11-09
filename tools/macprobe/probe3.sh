#!/bin/sh
# probe3.sh: on a Mac.  sh tools/macprobe/probe3.sh  -> macprobe-out/probe3.txt
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"; OUT="$PWD/macprobe-out"; mkdir -p "$OUT"
cc -Wno-deprecated-declarations -o "$OUT/probe3" "$HERE/probe3.c" || exit 1
"$OUT/probe3" > "$OUT/probe3.txt" 2>&1; cat "$OUT/probe3.txt"
