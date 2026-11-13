#!/bin/sh
# live.sh [port]: host side. Every 2 s fetches the Dock's stream (/tmp/ca.bin, /tmp/ca-ool-*) from the FreeBSD VM, draws it (cadraw.py, needs PIL + numpy)
# and puts the picture on the screen of wsd (wsshow reloads the PPM). Start wsshow once in the VM: WS_SOCKET=/tmp/wsd.sock /root/dnew/wsshow /tmp/cadock.ppm 0 0 5 &
P=${1:-2222}; K="-i $HOME/fbsd-vm/id_ed25519 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=15 -q"
D=$(mktemp -d); cd "$D" || exit 1; last=
while :; do
	scp -P $P $K 'root@localhost:/tmp/ca.bin' 'root@localhost:/tmp/ca-ool-*' . 2>/dev/null
	s=$(cksum ca.bin 2>/dev/null)
	if [ "$s" != "$last" ]; then
		last=$s
		python3 "$(dirname "$0")/cadraw.py" ca.bin out.ppm 1280 800 --ool=. >/dev/null && scp -P $P $K out.ppm root@localhost:/tmp/cadock.ppm
	fi
	sleep 2
done
