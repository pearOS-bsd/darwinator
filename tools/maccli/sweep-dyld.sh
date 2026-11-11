#!/bin/sh
# sweep-dyld.sh: runs every Mac command line tool of ./list.txt through the real dyld of the Mac (kern.darwinator.dyld=1, darwinator.ko loaded, the dyld cache in place) as nobody, with no
# arguments and no input, 8 seconds each.  One line per program in /root/results-dyld.tsv: name, class (EXIT0, EXITn, SIGnn, TIMEOUT), the unimplemented system calls that the kernel logged, the first line of the output.
cd /root/macbins || exit 1
LIST="${LIST:-list.txt}"; RES="${RES:-/root/results-dyld.tsv}"
sysctl kern.darwinator.dyld=1 kern.darwinator.debug=1 >/dev/null
rm -rf /tmp/mrwork; mkdir /tmp/mrwork; chmod 777 /tmp/mrwork
: > "$RES"
for f in $(cat "$LIST"); do
	case "$f" in */reboot|*/halt|*/shutdown|*/launchctl|*/sudo|*/login|*/su|*/passwd|*/newfs*|*/fsck*|*/diskutil|*/asr|*/dd|*/rm|*/srm|*/yes|*/cat|*/kill|*/killall|*/pkill) continue ;; esac
	dmesg -c >/dev/null 2>&1
	out=$(cd /tmp/mrwork && su -m nobody -c "timeout -s KILL 8 /root/macbins/$f </dev/null 2>&1; echo RC=\$?" 2>&1 | head -c 700)
	rc=$(echo "$out" | sed -n 's/.*RC=\([0-9]*\).*/\1/p' | tail -1)
	cls=EXIT$rc; [ "$rc" = 0 ] && cls=EXIT0
	[ "$rc" = 137 ] && cls=TIMEOUT
	[ "$rc" -ge 129 ] 2>/dev/null && [ "$rc" != 137 ] && cls=SIG$((rc-128))
	un=$(dmesg | sed -n 's/.*unimplemented system call \(0x[0-9a-f]*\) (\(.*\))/\1:\2/p' | sort -u | tr '\n' ',')
	bs=$(dmesg | sed -n 's/.*message to the bootstrap port: id \(0x[0-9a-f]*\).*/\1/p' | sort -u | tr '\n' ',')
	first=$(echo "$out" | grep -v "^RC=" | head -1 | cut -c1-100)
	printf '%s\t%s\t%s\t%s\t%s\n' "$f" "$cls" "$un" "$first" "$bs" >> "$RES"
done
echo DONE >> "$RES"
