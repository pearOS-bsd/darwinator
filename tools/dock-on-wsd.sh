#!/bin/sh
# dock-on-wsd.sh: the real Dock on the screen of wsd (run in the FreeBSD VM, wsd already running: wsd -t -s /tmp/wsd.sock).
# Needs the Mac side files in /root/dnew (see stack-dock-ownlsd.sh) and tools/ca/{caparse,cadraw}.py with python3 + PIL + numpy on the machine that draws.
export WS_SOCKET=/tmp/wsd.sock
sysctl kern.darwinator.fast_sleep_ms=5      # IOSurface waits 241 s for IOSurfaceRoot otherwise (iconservicesagent)
rm -f /tmp/ca.bin /tmp/ca-ool-*
sh /root/run19.sh &
sleep 100
cd /tmp && python3 /root/darwinator/tools/ca/cadraw.py /tmp/ca.bin /tmp/cadock.ppm 1280 800 --ool=/tmp
/root/dnew/wsshow /tmp/cadock.ppm 0 0 5 &
