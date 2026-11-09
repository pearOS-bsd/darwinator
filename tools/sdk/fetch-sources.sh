#!/bin/sh
# fetches the open sources (Apple's apple-oss-distributions on GitHub, APSL 2.0 and other licenses: each file keeps its notice; nothing of this goes into this repository) into WORK (default /root/oss):
#   sh tools/sdk/fetch-sources.sh [WORK]
W="${1:-/root/oss}"; mkdir -p "$W"; cd "$W" || exit 1
for r in dyld:dyld-1378 xnu:xnu-12377.121.6 Libc:Libc-1752.120.2 libplatform:libplatform-375.120.2 libdispatch:libdispatch-1542.100.32 libpthread:libpthread-539.100.4 CarbonHeaders: bootstrap_cmds:; do
	n="${r%%:*}"; t="${r#*:}"; [ -d "$n" ] && continue
	if [ -n "$t" ]; then git clone -q --depth 1 --branch "$t" "https://github.com/apple-oss-distributions/$n"; else git clone -q --depth 1 "https://github.com/apple-oss-distributions/$n"; fi
done
