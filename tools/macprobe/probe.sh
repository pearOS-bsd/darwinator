#!/bin/sh
# probe.sh: run on a Mac (Apple silicon or Intel).  It builds and runs probe.c (sizes, constants, sysctl values and the results of some calls: what darwinator assumes about Darwin) and, if you
# agree, traces the system calls of some applications with dtruss (needs sudo; on a Mac with SIP on, dtruss may refuse: then only probe.txt is made).  Nothing is sent anywhere and nothing outside
# the folder ./macprobe-out is written.  Send the file macprobe-out.tar.gz.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"; OUT="$PWD/macprobe-out"; mkdir -p "$OUT"
echo "== building the probe"
cc -o "$OUT/probe" "$HERE/probe.c" 2>"$OUT/build-errors.txt" || { echo "the probe did not build (see macprobe-out/build-errors.txt)"; }
[ -x "$OUT/probe" ] && { "$OUT/probe" > "$OUT/probe.txt" 2>&1; echo "probe.txt: $(wc -l < "$OUT/probe.txt") lines"; }
uname -a > "$OUT/uname.txt"; sw_vers > "$OUT/sw_vers.txt" 2>&1
printf "Trace the system calls of applications with dtruss (sudo; the applications are started for a few seconds)? [y/N] "; read -r ans
if [ "$ans" = y ] || [ "$ans" = Y ]; then
	for app in "$@"; do		# probe.sh /Applications/Keka.app /Applications/AppCleaner.app ...
		[ -d "$app" ] || continue
		name="$(basename "$app" .app)"; exe="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$app/Contents/Info.plist" 2>/dev/null)"
		[ -x "$app/Contents/MacOS/$exe" ] || continue
		echo "== dtruss $name"
		sudo dtruss -c -f "$app/Contents/MacOS/$exe" > "$OUT/dtruss-$name.txt" 2>&1 &
		pid=$!; sleep 8; sudo pkill -f "$app/Contents/MacOS/$exe" 2>/dev/null; kill $pid 2>/dev/null; wait 2>/dev/null
		echo "dtruss-$name.txt: $(wc -l < "$OUT/dtruss-$name.txt") lines"
	done
fi
tar czf macprobe-out.tar.gz -C "$PWD" macprobe-out && echo "done: send macprobe-out.tar.gz (the files in it are text; look at them first if you like)"
