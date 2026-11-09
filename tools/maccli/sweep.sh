#!/bin/sh
# sweep.sh: runs every Mac command line tool of a list (list.txt: one path per line, relative to the current directory) through machrun as the user nobody, with no arguments, no input and 8 seconds,
# and writes a line per program to /root/results.tsv: name, class (STARTS, MISSING = the program called a symbol that machrun does not have, LOADERR, CRASH), the symbol or the message.
cd /root/macbins || exit 1
rm -rf /tmp/mrwork; mkdir /tmp/mrwork; chmod 777 /tmp/mrwork
: > /root/results.tsv
for f in $(cat list.txt); do
	case "$f" in */reboot|*/halt|*/shutdown|*/launchctl|*/sudo|*/login|*/su|*/passwd|*/newfs*|*/fsck*|*/diskutil|*/asr|*/dd|*/rm|*/srm|*/yes|*/cat) continue ;; esac
	out=$(cd /tmp/mrwork && su -m nobody -c "timeout -s KILL 8 /root/dy/dyld/machrun /root/macbins/$f </dev/null 2>&1" 2>&1 | head -c 600); st=$?
	cls=STARTS; det=""
	case "$out" in
	*"unimplemented symbol"*) cls=MISSING; det=$(echo "$out" | sed -n 's/.*unimplemented symbol \([^ ]*\).*/\1/p' | head -1) ;;
	*"Segmentation fault"*|*"signal 11"*|*"signal 10"*|*"Bus error"*) cls=CRASH; det=$(echo "$out" | head -1 | cut -c1-80) ;;
	*"machrun:"*) cls=LOADERR; det=$(echo "$out" | grep "machrun:" | head -1 | cut -c1-100) ;;
	esac
	printf '%s\t%s\t%s\n' "$f" "$cls" "$det" >> /root/results.tsv
done
echo DONE >> /root/results.tsv
