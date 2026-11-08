#!/bin/sh
# tests/xld.sh [XLD.app]: end to end test of XLD (the original macOS audio converter, closed source, Objective-C, with Mach-O plug-ins and frameworks) in the pearOS image.
# A WAV file (1 second of noise, 16 bit stereo) goes to FLAC and back to WAV through XLD's own command line mode; the audio of the result has to be the same bytes (FLAC is lossless).
# Run in the desktop session of pearOS:  sh tests/xld.sh /mnt/XLD.app
APP="${1:-/mnt/XLD.app}"; WORK="${WORK:-/var/tmp/xldtest}"; MACHRUN="${MACHRUN:-/usr/local/bin/machrun}"; WRAP="${WRAP:-/usr/local/libexec/wrapapp.sh}"; pass=0; fail=0
ok() { echo "ok   $1"; pass=$((pass + 1)); }; bad() { echo "FAIL $1"; fail=$((fail + 1)); }
rm -rf "$WORK"; mkdir -p "$WORK"
# the WAV: 44 byte header (44100 Hz, 2 channels, 16 bit, 176400 bytes of audio) and noise
le32() { printf "\\$(printf '%03o' $(($1 & 255)))\\$(printf '%03o' $((($1 >> 8) & 255)))\\$(printf '%03o' $((($1 >> 16) & 255)))\\$(printf '%03o' $((($1 >> 24) & 255)))"; }
{ printf 'RIFF'; le32 $((36 + 176400)); printf 'WAVEfmt '; le32 16; printf '\001\000\002\000'; le32 44100; le32 176400; printf '\004\000\020\000'; printf 'data'; le32 176400; head -c 176400 /dev/urandom; } > "$WORK/in.wav"
sh "$WRAP" "$APP" "$WORK" "$MACHRUN" > /dev/null || { echo "FAIL XLD could not be wrapped"; exit 1; }
export WS_SOCKET=/var/run/WindowServer.sock XDG_SESSION_TYPE=wsd XDG_CURRENT_DESKTOP=Aqua XDG_SESSION_DESKTOP=Aqua LANG=C.UTF-8
[ -n "$MACHRUN_COMPAT" ] && export MACHRUN_COMPAT
ulimit -c 0; cd "$WORK/$(basename "$APP")" || exit 1
timeout -s KILL "${TIMEOUT:-90}" ./XLD --cmdline -f flac -o "$WORK/out.flac" "$WORK/in.wav" > "$WORK/log1" 2>&1
[ -s "$WORK/out.flac" ] && ok "WAV to FLAC" || bad "WAV to FLAC (see $WORK/log1)"
head -c 4 "$WORK/out.flac" | grep -q fLaC && ok "the output is a FLAC stream" || bad "the output is not a FLAC stream"
timeout -s KILL "${TIMEOUT:-90}" ./XLD --cmdline -f wav -o "$WORK/back.wav" "$WORK/out.flac" > "$WORK/log2" 2>&1
[ -s "$WORK/back.wav" ] && ok "FLAC to WAV" || bad "FLAC to WAV (see $WORK/log2)"
tail -c 176400 "$WORK/in.wav" > "$WORK/p1"; tail -c 176400 "$WORK/back.wav" > "$WORK/p2" 2>/dev/null
cmp -s "$WORK/p1" "$WORK/p2" && ok "the audio is the same, byte for byte" || bad "the audio differs"
echo "$pass passed, $fail failed"; [ "$fail" -eq 0 ]
