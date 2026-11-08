#!/bin/sh
# tests/dylib-ffmpeg.sh FRAMEWORKS_DIR: the loader of Mach-O dylibs with real libraries: the FFmpeg dylibs of IINA (libavformat, libavcodec, libavutil and what they need: re-exports, @rpath,
# thread-local variables, initializers) open a Matroska file, find its video stream and decode two frames (a 320x240 test picture).  FRAMEWORKS_DIR is IINA.app/Contents/Frameworks.
# Needs ffmpeg (to make the test file) and the built dyld/machrun.   Usage: sh tests/dylib-ffmpeg.sh /path/IINA.app/Contents/Frameworks
FW="${1:?FRAMEWORKS_DIR}"; MR="${MACHRUN:-$(dirname "$0")/../dyld/machrun}"; T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
ffmpeg -loglevel error -y -f lavfi -i testsrc=duration=2:size=320x240:rate=25 -c:v mpeg4 -an "$T/v.mkv" || { echo "FAIL ffmpeg could not make the test file"; exit 1; }
cat > "$T/s.txt" <<S
\$1 = calloc(1, 8)
\$2 = avformat_open_input(\$1, "$T/v.mkv", 0, 0)
\$3 = peek(\$1, 0)
\$4 = avformat_find_stream_info(\$3, 0)
\$17 = peek(\$3, 48)
\$18 = peek(\$17, 0)
\$5 = peek(\$18, 16)
\$6 = avcodec_find_decoder(12)
\$7 = avcodec_alloc_context3(\$6)
\$8 = avcodec_parameters_to_context(\$7, \$5)
\$9 = avcodec_open2(\$7, \$6, 0)
\$10 = av_packet_alloc()
\$11 = av_frame_alloc()
\$12 = av_read_frame(\$3, \$10)
\$13 = avcodec_send_packet(\$7, \$10)
\$14 = avcodec_receive_frame(\$7, \$11)
\$15 = peek32(\$11, 104)
\$16 = peek32(\$11, 108)
S
OUT="$("$MR" -L "$FW/libavformat.63.dylib" -L "$FW/libavcodec.63.dylib" -L "$FW/libavutil.61.dylib" -x "$T/s.txt" 2>&1)"
pass=0; fail=0
chk() { if echo "$OUT" | grep -q "$1"; then echo "ok   $2"; pass=$((pass + 1)); else echo "FAIL $2"; fail=$((fail + 1)); fi; }
chk '^\$avformat_open_input -> 0$' "avformat_open_input opens the file"
chk '^\$avformat_find_stream_info -> 0$' "the stream is found"
chk '^\$avcodec_open2 -> 0$' "the mpeg4 decoder opens"
chk '^\$avcodec_receive_frame -> 0$' "a frame is decoded"
chk '^\$peek32 -> 0x140$' "its width is 320"
chk '^\$peek32 -> 0xf0$' "its height is 240"
[ "$fail" -ne 0 ] && echo "$OUT" | grep -v GSPrivate | tail -25
echo "$pass passed, $fail failed"; [ "$fail" -eq 0 ]
