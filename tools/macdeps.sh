#!/bin/sh
# macdeps.sh /path/of/the/program/on/the/freebsd/vm [loops]
# Runs a Mac program through the real dyld on the FreeBSD VM; each "Library not loaded: PATH" is fetched from the Mac VM (the path itself, or the same path in the cryptex of the OS)
# and put on the FreeBSD VM where the dyld looks second (/System/Volumes/Preboot/Cryptexes/OS/PATH), until the program passes the loading.  The libraries that were brought are
# listed in /root/macdeps.list on the FreeBSD VM (the list of what comes from the Mac and has to be replaced, one by one).  The files of the Mac stay on the VM: they are never committed.
# Needs: ssh to the FreeBSD VM (port 2222, key ~/fbsd-vm/id_ed25519) and to the Mac VM (port 2229, same key, user alex).
PROG="$1"; LOOPS="${2:-40}"
FB="ssh -i $HOME/fbsd-vm/id_ed25519 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -q -p 2222 root@localhost"
MAC="ssh -i $HOME/fbsd-vm/id_ed25519 -p 2229 -o IdentitiesOnly=yes -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -q alex@localhost"
i=0
while [ $i -lt "$LOOPS" ]; do
	i=$((i+1))
	miss=$($FB "cd /tmp; timeout -s KILL 10 $PROG </dev/null 2>&1 | sed -n 's/.*Library not loaded: \\(.*\\)/\\1/p' | head -1")
	[ -z "$miss" ] && { echo "no more libraries missing after $((i-1)) fetched"; exit 0; }
	echo "missing: $miss"
	dest="/System/Volumes/Preboot/Cryptexes/OS$miss"
	ok=0
	for src in "$miss" "/System/Cryptexes/OS$miss" "/System/Volumes/Preboot/Cryptexes/OS$miss"; do
		if $MAC "test -f '$src'"; then
			$FB "mkdir -p '$(dirname "$dest")'"
			$MAC "cat '$src'" | $FB "cat > '$dest'" && ok=1
			[ $ok = 1 ] && { echo "  from the Mac: $src"; $FB "echo '$miss' >> /root/macdeps.list"; break; }
		fi
	done
	[ $ok = 1 ] || { echo "  NOT found on the Mac: $miss"; exit 1; }
done
echo "stopped after $LOOPS libraries"
