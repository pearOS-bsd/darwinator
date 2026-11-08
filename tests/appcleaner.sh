#!/bin/sh
# tests/appcleaner.sh [AppCleaner.app [Target.app]]: the end to end test of AppCleaner 3.7 (the original macOS application) in the pearOS image.
# It leaves the files that an application leaves in ~/Library, opens the application in AppCleaner (what a drop on its window does), checks that AppCleaner lists them, presses "Remove" and checks that
# the files are in ~/.Trash and not in ~/Library any more.  Run in the desktop session of pearOS (a user who is logged in): sh tests/appcleaner.sh /var/tmp/AppCleaner.app /mnt/Keka.app
APP="${1:-/var/tmp/AppCleaner.app}"; TARGET="${2:-/mnt/Keka.app}"; ID="${TARGET_ID:-com.aone.keka}"; NAME="${TARGET_NAME:-Keka}"
L="$HOME/Library"; WORK="${WORK:-/var/tmp/actest}"; MACHRUN="${MACHRUN:-/usr/local/bin/machrun}"; pass=0; fail=0
ok() { echo "ok   $1"; pass=$((pass + 1)); }; bad() { echo "FAIL $1"; fail=$((fail + 1)); }
rm -rf "$WORK" "$HOME/.Trash"; mkdir -p "$WORK"
mkdir -p "$L/Preferences" "$L/Caches/$ID" "$L/Application Support/$NAME" "$L/Saved Application State/$ID.savedState"
echo p > "$L/Preferences/$ID.plist"; echo c > "$L/Caches/$ID/cache.db"; echo s > "$L/Application Support/$NAME/state.json"; echo x > "$L/Saved Application State/$ID.savedState/windows.plist"
sh /usr/local/libexec/wrapapp.sh "$APP" "$WORK" "$MACHRUN" > /dev/null || { echo "FAIL the application could not be wrapped"; exit 1; }
export WS_SOCKET=/var/run/WindowServer.sock XDG_SESSION_TYPE=wsd XDG_CURRENT_DESKTOP=Aqua XDG_SESSION_DESKTOP=Aqua LANG=C.UTF-8 MACHRUN_SHOWWINDOWS=1 MACHRUN_OPENFILE="$TARGET"
export MACHRUN_KVC=mainWindowController.uninstallViewController.bundles.relatedFiles.path MACHRUN_KVC_DELAY=45
export MACHRUN_INVOKE='mainWindowController.uninstallViewController remove:' MACHRUN_INVOKE_DELAY=55
[ -n "$MACHRUN_COMPAT" ] && export MACHRUN_COMPAT
ulimit -c 0; cd "$WORK/$(basename "$APP")" && timeout -s KILL "${TIMEOUT:-80}" ./"$(basename "$APP" .app)" > "$WORK/log" 2>&1
LIST="$(grep -a -A3 '^KVC ' "$WORK/log" | tr '\n' ' ')"
for f in "Preferences/$ID.plist" "Caches/$ID" "Application Support/$NAME" "Saved Application State/$ID.savedState"; do
	case "$LIST" in *"$L/$f"*) ok "AppCleaner lists $f";; *) bad "AppCleaner does not list $f";; esac
done
grep -aq '^INVOKE done' "$WORK/log" && ok "Remove was pressed" || bad "Remove was not pressed"
for f in "Preferences/$ID.plist" "Caches/$ID" "Application Support/$NAME" "Saved Application State/$ID.savedState"; do
	[ -e "$L/$f" ] && bad "$f is still in ~/Library" || ok "$f is gone from ~/Library"
done
for f in "$ID.plist" "$ID" "$NAME" "$ID.savedState"; do
	[ -e "$HOME/.Trash/$f" ] && ok "$f is in ~/.Trash" || bad "$f is not in ~/.Trash"
done
echo "$pass passed, $fail failed"; [ "$fail" -eq 0 ]
