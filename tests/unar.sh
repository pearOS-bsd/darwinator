#!/bin/sh
# tests/unar.sh [The Unarchiver.app]: the end to end test of The Unarchiver 3.11 (the original macOS application, Objective-C, no Swift) in the pearOS image.
# It has the application open each archive of tests/data/unar (zip, tar.gz, tar.bz2, tar.xz, 7z: what a double click does, application:openFile:) and checks that the
# application extracted it next to the archive with the same bytes.  Run in the desktop session of pearOS:   sh tests/unar.sh "/Applications/The Unarchiver.app"
# (The archives are made on a host with GNU tar, zip and 7z: tar of the image (bsdtar) makes archives that crash the application.)
APP="${1:-/Applications/The Unarchiver.app}"; HERE="$(cd "$(dirname "$0")" && pwd)"; DATA="${DATA:-$HERE/data/unar}"
WORK="${WORK:-/var/tmp/unartest}"; MACHRUN="${MACHRUN:-/usr/local/bin/machrun}"; pass=0; fail=0
ok() { echo "ok   $1"; pass=$((pass + 1)); }; bad() { echo "FAIL $1"; fail=$((fail + 1)); }
rm -rf "$WORK"; mkdir -p "$WORK/cache"; cp -R "$DATA"/. "$WORK/"
sh /usr/local/libexec/wrapapp.sh "$APP" "$WORK/cache" "$MACHRUN" > /dev/null || { echo "FAIL the application could not be wrapped"; exit 1; }
export WS_SOCKET=/var/run/WindowServer.sock MALLOC_CONF=junk:false   # (not XDG_SESSION_TYPE=wsd etc., which the AppCleaner test sets)
[ -n "$MACHRUN_COMPAT" ] && export MACHRUN_COMPAT
ulimit -c 0
for a in t1.zip t2.tar.gz t3.tar.bz2 t4.tar.xz t5.7z; do
	d="${a%%.*}"; n=0; good=0
	while [ $n -lt "${ATTEMPTS:-3}" ] && [ $good -eq 0 ]; do
		n=$((n + 1)); rm -rf "$WORK/$d"
		# in the foreground for TIMEOUT seconds: the application stays open until it is killed
		MACHRUN_OPENFILE="$WORK/$a" timeout -s KILL "${TIMEOUT:-20}" "$WORK/cache/$(basename "$APP")/$(basename "$APP" .app)" > "$WORK/$a.log" 2>&1 < /dev/null
		if [ -f "$WORK/$d/hello.txt" ] && cmp -s "$WORK/src/hello.txt" "$WORK/$d/hello.txt" && cmp -s "$WORK/src/rand.bin" "$WORK/$d/rand.bin" && cmp -s "$WORK/src/sub/n.txt" "$WORK/$d/sub/n.txt"; then good=1; fi
	done
	[ $good -eq 1 ] && ok "$a is extracted with the same bytes (run $n)" || bad "$a is not extracted correctly ($n runs)"
done
echo "== $pass passed, $fail failed"
[ "$fail" -eq 0 ]
