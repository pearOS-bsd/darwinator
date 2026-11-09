#!/bin/sh
# tests/all.sh: one command that checks every Mac application of the compatibility list on the pearOS image (run it in the desktop session of the image, as the user who is logged in):
#   macapp-install keka appcleaner xld unarchiver      (the network and a writable /Applications; the script installs what is missing)
#   sh tests/all.sh
# It prints a table (application, result) and ends with the number of failures.  Set ONLY="unarchiver appcleaner" to run some.  MACHRUN= another machrun.
HERE="$(cd "$(dirname "$0")" && pwd)"; APPS="${APPLICATIONS:-/Applications}"; ONLY="${ONLY:-unarchiver appcleaner xld keka}"; fails=0; rows=""
sysctl -n kern.coredump >/dev/null 2>&1 && sudo -n sysctl kern.coredump=0 >/dev/null 2>&1   # a crashing application would write a core file of 500 MB into /var (memory)
add() { rows="$rows
$(printf '%-14s %-6s %s' "$1" "$2" "$3")"; [ "$2" = ok ] || fails=$((fails + 1)); }
need() { [ -d "$APPS/$1" ] || macapp-install "$2" >/dev/null 2>&1; [ -d "$APPS/$1" ]; }
for a in $ONLY; do
	case "$a" in
	unarchiver)
		if need "The Unarchiver.app" unarchiver; then out="$(sh "$HERE/unar.sh" "$APPS/The Unarchiver.app" 2>&1)"; echo "$out" | grep -q "^== 5 passed" && add "The Unarchiver" ok "5 archive formats extracted" || add "The Unarchiver" FAIL "$(echo "$out" | grep -c '^FAIL') formats failed"
		else add "The Unarchiver" skip "not installed (no network?)"; fi ;;
	appcleaner)
		if need AppCleaner.app appcleaner && need Keka.app keka; then out="$(sh "$HERE/appcleaner.sh" "$APPS/AppCleaner.app" "$APPS/Keka.app" 2>&1)"; echo "$out" | grep -q "^== 13 passed\|13 passed, 0 failed" && add AppCleaner ok "finds and trashes the files of an application" || add AppCleaner FAIL "$(echo "$out" | grep -c '^FAIL') checks failed"
		else add AppCleaner skip "not installed (no network?)"; fi ;;
	xld)
		if need XLD.app xld; then out="$(sh "$HERE/xld.sh" "$APPS/XLD.app" 2>&1)"; echo "$out" | grep -q "4 passed, 0 failed\|== 4 passed" && add XLD ok "WAV to FLAC and back, same bytes" || add XLD FAIL "$(echo "$out" | grep -c '^FAIL') checks failed"
		else add XLD skip "not installed (no network?)"; fi ;;
	keka)
		if need Keka.app keka; then KEKA_DIR="$APPS/Keka.app/Contents/MacOS"; export KEKA_DIR; out="$(sh "$HERE/keka.sh" 2>&1)"; [ "$(echo "$out" | grep -c "^ok")" -gt 0 ] || out="FAIL keka.sh did not run: $out"; echo "$out" | grep -q "^FAIL" && add Keka FAIL "$(echo "$out" | grep -c '^FAIL') checks failed" || add Keka ok "command line tools extract and encrypt ($(echo "$out" | grep -c '^ok') checks)"
		else add Keka skip "not installed (no network?)"; fi ;;
	esac
done
echo "Application    Result What"; echo "$rows" | sed '/^$/d'
echo "== $fails failure(s)"; [ "$fails" -eq 0 ]
