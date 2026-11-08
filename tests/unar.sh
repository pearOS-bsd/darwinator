#!/bin/sh
# tests/unar.sh [The Unarchiver.app]: the end to end test of The Unarchiver 3.11 (the original macOS application, Objective-C, no Swift) in the pearOS image.
# It makes archives of a small tree (zip, tar.gz, tar.bz2, tar.xz, and 7z when 7z is there), has the application open each one (what a double click does:
# application:openFile:), and checks that the application extracted it next to the archive with the same bytes.  Run in the desktop session of pearOS:
#   sh tests/unar.sh "/var/tmp/The Unarchiver.app"
APP="${1:-/var/tmp/The Unarchiver.app}"; WORK="${WORK:-/var/tmp/unartest}"; MACHRUN="${MACHRUN:-/usr/local/bin/machrun}"; pass=0; fail=0
ok() { echo "ok   $1"; pass=$((pass + 1)); }; bad() { echo "FAIL $1"; fail=$((fail + 1)); }
rm -rf "$WORK"; mkdir -p "$WORK/src/sub" "$WORK/cache"
echo "hello unarchiver" > "$WORK/src/hello.txt"; head -c 70000 /dev/urandom > "$WORK/src/rand.bin"; echo nested > "$WORK/src/sub/n.txt"
( cd "$WORK/src" && zip -qr ../t1.zip . 2>/dev/null; tar czf ../t2.tar.gz .; tar cjf ../t3.tar.bz2 . 2>/dev/null; tar cJf ../t4.tar.xz . 2>/dev/null
  command -v 7z >/dev/null && 7z a -bso0 -bsp0 ../t5.7z . >/dev/null 2>&1 )
# archives made elsewhere (zip and 7z tools are not in the image): PREBUILT=dir with t1.zip and t5.7z of the same tree (see tests/README or make them from src/)
[ -n "$PREBUILT" ] && cp "$PREBUILT"/t1.zip "$PREBUILT"/t5.7z "$WORK/" 2>/dev/null
sh /usr/local/libexec/wrapapp.sh "$APP" "$WORK/cache" "$MACHRUN" > /dev/null || { echo "FAIL the application could not be wrapped"; exit 1; }
export WS_SOCKET=/var/run/WindowServer.sock XDG_SESSION_TYPE=wsd XDG_CURRENT_DESKTOP=Aqua XDG_SESSION_DESKTOP=Aqua LANG=C.UTF-8 MALLOC_CONF=junk:false
[ -n "$MACHRUN_COMPAT" ] && export MACHRUN_COMPAT
ulimit -c 0
for a in t1.zip t2.tar.gz t3.tar.bz2 t4.tar.xz t5.7z; do
	[ -e "$WORK/$a" ] || { echo "skip $a (no tool to make it)"; continue; }
	d="${a%%.*}"
	( MACHRUN_OPENFILE="$WORK/$a" timeout -s KILL "${TIMEOUT:-45}" "$WORK/cache/$(basename "$APP")/$(basename "$APP" .app)" > "$WORK/$a.log" 2>&1 ) &
	for i in $(seq 1 45); do [ -f "$WORK/$d/rand.bin" ] && [ -f "$WORK/$d/sub/n.txt" ] && break; sleep 1; done
	sleep 1; pkill -KILL -f "$(basename "$APP" .app)\$" 2>/dev/null; wait
	if [ -f "$WORK/$d/hello.txt" ] && cmp -s "$WORK/src/hello.txt" "$WORK/$d/hello.txt" && cmp -s "$WORK/src/rand.bin" "$WORK/$d/rand.bin" && cmp -s "$WORK/src/sub/n.txt" "$WORK/$d/sub/n.txt"
	then ok "$a is extracted with the same bytes"; else bad "$a is not extracted correctly"; fi
done
echo "== $pass passed, $fail failed"
[ "$fail" -eq 0 ]
